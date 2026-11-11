// fdvapp.c -- sBitx FreeDV: a complete stand-alone FreeDV program.
//
// It does not touch the sBitx program at all. It talks to the radio the way
// the other digital programs do:
//   * receive audio  <- the radio's ALSA loopback card (the radio must be in
//                       DIGITAL mode, USB sideband)
//   * transmit audio -> the radio's second loopback card
//   * PTT / frequency through the radio's Hamlib-style socket (127.0.0.1:4532)
//   * your microphone and speakers are any ALSA devices (e.g. the USB mic with
//     PTT button and the USB speakers)
// Modes: FreeDV 700D, 700E, 1600 (libcodec2) and RADE / RADE V2 (libradex.so,
// built by rade/build_rade.sh). Window: spectrum, waterfall, constellation,
// SNR history, sync, text, levels, setup, test loopback, WAV decode.
//
// Build:  make        Run:  ./sbitx_freedv      (options: --test, --rx-wav file)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <alsa/asoundlib.h>
#include <fftw3.h>
#include <codec2/freedv_api.h>
#include <codec2/modem_stats.h>

#define APP_NAME "sBitx FreeDV"
#define FS 48000                 // sample rate of every sound card we open
#define CHUNK 480                // 10 ms at 48 kHz
#define NFFT 2048                // spectrum: 8 kHz / 2048 = 3.9 Hz per bin
#define NBIN 1024                // bins to 4 kHz
#define WF_W 512                 // waterfall pixel columns (0..4 kHz)
#define WF_H 120                 // waterfall rows
#define HIST_N 120               // SNR history points (500 ms each = 60 s)

// ---------------------------------------------------------------- modes ----
enum { M_700D, M_700E, M_1600, M_RADE, M_RAD2, M_COUNT };
static const char *mode_names[M_COUNT] = {"700D", "700E", "1600", "RADE", "RADE V2"};
static const int c2_ids[3] = {FREEDV_MODE_700D, FREEDV_MODE_700E, FREEDV_MODE_1600};
#define IS_RADE(m) ((m) == M_RADE || (m) == M_RAD2)

// -------------------------------------------------------------- settings ---
static struct {
	char rxdev[64], txdev[64], micdev[64], spkdev[64];     // ALSA names
	char rigaddr[64]; int rigport;
	char callsign[16];
	int txlevel;            // 10..100 %, 100 = peaks at 90% of full scale
	int spkvol;             // 0..200 %
	float squelch;          // dB
	int squelch_on, clip_on, mode;
	int fullscreen_start;
} cfg = {"plughw:1,1", "plughw:2,0", "default", "default", "127.0.0.1", 4532,
         "", 50, 100, -2.0f, 1, 1, M_700D, 0};

static char cfg_path[300];
static void cfg_file(void){
	const char *h = getenv("HOME");
	snprintf(cfg_path, sizeof(cfg_path), "%s/sbitx/data/freedv_app.txt", h ? h : ".");
}
static void cfg_save(void){
	FILE *f = fopen(cfg_path, "w");
	if (!f) return;
	fprintf(f, "rxdev=%s\ntxdev=%s\nmicdev=%s\nspkdev=%s\nrigaddr=%s\nrigport=%d\ncallsign=%s\n"
		"txlevel=%d\nspkvol=%d\nsquelch=%.1f\nsquelch_on=%d\nclip_on=%d\nmode=%d\n",
		cfg.rxdev, cfg.txdev, cfg.micdev, cfg.spkdev, cfg.rigaddr, cfg.rigport, cfg.callsign,
		cfg.txlevel, cfg.spkvol, cfg.squelch, cfg.squelch_on, cfg.clip_on, cfg.mode);
	fclose(f);
}
static void cfg_load(void){
	cfg_file();
	FILE *f = fopen(cfg_path, "r");
	char ln[200];
	if (!f) return;
	while (fgets(ln, sizeof(ln), f)){
		ln[strcspn(ln, "\r\n")] = 0;
		char *v = strchr(ln, '=');
		if (!v) continue;
		*v++ = 0;
#define S(k, d) if (!strcmp(ln, k)) snprintf(d, sizeof(d), "%s", v);
		S("rxdev", cfg.rxdev) S("txdev", cfg.txdev) S("micdev", cfg.micdev) S("spkdev", cfg.spkdev)
		S("rigaddr", cfg.rigaddr) S("callsign", cfg.callsign)
		if (!strcmp(ln, "rigport")) cfg.rigport = atoi(v);
		if (!strcmp(ln, "txlevel")) cfg.txlevel = atoi(v);
		if (!strcmp(ln, "spkvol")) cfg.spkvol = atoi(v);
		if (!strcmp(ln, "squelch")) cfg.squelch = (float)atof(v);
		if (!strcmp(ln, "squelch_on")) cfg.squelch_on = atoi(v);
		if (!strcmp(ln, "clip_on")) cfg.clip_on = atoi(v);
		if (!strcmp(ln, "mode")) cfg.mode = atoi(v);
	}
	fclose(f);
	if (cfg.txlevel < 10 || cfg.txlevel > 100) cfg.txlevel = 50;
	if (cfg.spkvol < 0 || cfg.spkvol > 200) cfg.spkvol = 100;
	if (cfg.mode < 0 || cfg.mode >= M_COUNT) cfg.mode = M_700D;
}

// ------------------------------------------------------------ small tools --
#define FIFO_N 65536
struct fifo { short buf[FIFO_N]; _Atomic unsigned head, tail; };
static unsigned fl(struct fifo *f){ return atomic_load(&f->head) - atomic_load(&f->tail); }
static void fput(struct fifo *f, short v){
	unsigned h = atomic_load(&f->head);
	if (h - atomic_load(&f->tail) >= FIFO_N - 1) return;          // full: drop the new sample
	f->buf[h & (FIFO_N - 1)] = v;
	atomic_store(&f->head, h + 1);
}
static int fget(struct fifo *f, short *v){
	unsigned t = atomic_load(&f->tail);
	if (t == atomic_load(&f->head)) return 0;
	*v = f->buf[t & (FIFO_N - 1)];
	atomic_store(&f->tail, t + 1);
	return 1;
}
static void fdrain(struct fifo *f){ atomic_store(&f->tail, atomic_load(&f->head)); }   // consumer side only

static double now_s(void){
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static short clip16(float v){ return (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v); }

// low-pass FIR (windowed sinc), unity gain at DC
static float *make_lp(int taps, double fc_norm){
	float *h = malloc(taps * sizeof(float));
	double sum = 0;
	for (int i = 0; i < taps; i++){
		double m = i - (taps - 1) / 2.0;
		double s = (m == 0) ? 2 * fc_norm : sin(2 * M_PI * fc_norm * m) / (M_PI * m);
		h[i] = (float)(s * (0.54 - 0.46 * cos(2 * M_PI * i / (taps - 1))));
		sum += h[i];
	}
	for (int i = 0; i < taps; i++) h[i] /= (float)sum;
	return h;
}
struct decim { int M, taps, ph; float *h, *hist; };
static void decim_init(struct decim *d, int M){
	free(d->h); free(d->hist);
	d->M = M; d->taps = 16 * M + 1; d->ph = 0;
	d->h = make_lp(d->taps, 0.42 / M);               // pass to ~0.42 of the new Nyquist x2
	d->hist = calloc(d->taps, sizeof(float));
}
static int decim_push(struct decim *d, float x, float *y){
	memmove(d->hist + 1, d->hist, (d->taps - 1) * sizeof(float));
	d->hist[0] = x;
	if (++d->ph < d->M) return 0;
	d->ph = 0;
	float a = 0;
	for (int k = 0; k < d->taps; k++) a += d->h[k] * d->hist[k];
	*y = a;
	return 1;
}
struct interp { int L, taps, nh; float *h, *hist; };
static void interp_init(struct interp *p, int L){
	free(p->h); free(p->hist);
	p->L = L; p->nh = 16; p->taps = L * p->nh;
	p->h = make_lp(p->taps, 0.42 / L);
	p->hist = calloc(p->nh + 1, sizeof(float));
}
static void interp_push(struct interp *p, float x, float *out){
	memmove(p->hist + 1, p->hist, (p->nh - 1) * sizeof(float));
	p->hist[0] = x;
	for (int k = 0; k < p->L; k++){
		float a = 0;
		for (int j = 0; j < p->nh; j++) a += p->h[k + p->L * j] * p->hist[j];
		out[k] = a * p->L;
	}
}

// -------------------------------------------------------------- RADE lib ---
static struct {
	void *lib;
	void *(*open)(int v2);
	void (*close)(void *h);
	int (*nin)(void *h);
	int (*rx)(void *h, const short *in8, int nin, short *speech);
	int (*sync)(void *h);
	float (*snr)(void *h);
	void (*tx_reset)(void *h);
	int (*tx)(void *h, const short *pcm16, short *out);
	float (*foff)(void *h);
	int (*eoo)(void *h, short *out);
} rl;

static int rade_lib_load(void){
	if (rl.lib) return 1;
	const char *home = getenv("HOME");
	char p[300];
	const char *try1[] = {"./libradex.so", "./rade/libradex.so", NULL};
	for (int i = 0; try1[i] && !rl.lib; i++) rl.lib = dlopen(try1[i], RTLD_NOW | RTLD_LOCAL);
	if (!rl.lib && home){
		snprintf(p, sizeof(p), "%s/sbitx_freedv/rade/libradex.so", home);
		rl.lib = dlopen(p, RTLD_NOW | RTLD_LOCAL);
	}
	if (!rl.lib && home){
		snprintf(p, sizeof(p), "%s/sbitx/rade/libradex.so", home);
		rl.lib = dlopen(p, RTLD_NOW | RTLD_LOCAL);
	}
	if (!rl.lib) rl.lib = dlopen("libradex.so", RTLD_NOW | RTLD_LOCAL);
	if (!rl.lib) return 0;
#define SYM(f, n) rl.f = dlsym(rl.lib, n)
	SYM(open, "radex_open"); SYM(close, "radex_close"); SYM(nin, "radex_nin"); SYM(rx, "radex_rx");
	SYM(sync, "radex_sync"); SYM(snr, "radex_snr"); SYM(tx_reset, "radex_tx_reset"); SYM(tx, "radex_tx");
	SYM(foff, "radex_foff"); SYM(eoo, "radex_tx_eoo");
	if (!rl.open || !rl.close || !rl.nin || !rl.rx || !rl.sync || !rl.snr || !rl.tx_reset || !rl.tx){
		dlclose(rl.lib); rl.lib = NULL;
		return 0;
	}
	return 1;
}

// ------------------------------------------------------------ shared state --
static pthread_mutex_t codec_lock = PTHREAD_MUTEX_INITIALIZER;
static struct freedv *fdv = NULL;
static void *rade_h = NULL;
static int cur_mode = -1;
static volatile int speech_rate = 8000;             // rate of the speech the codec handles
static int n_speech = 0, n_modem = 0;               // codec2 frame sizes
static char status_msg[160] = "";

static struct fifo rx8, mic_f, txm8, spk_f;         // modem in, microphone, modem out, speech out
static volatile int running = 0;
static volatile int test_mode = 0, rx_wav_active = 0;
static volatile int ptt_req = 0, tune_req = 0;
static volatile int tx_state_pub = 0;               // 0 off, 1 keying, 2 on air, 3 ending
static volatile int tx_active = 0;                  // the TX sound thread should be sending
static volatile int tx_feed = 0;                    // microphone goes to the encoder
static volatile int ok_rx = 0, ok_tx = 0, ok_mic = 0, ok_spk = 0, rig_ok = 0;
static volatile double rig_freq = 0;
static volatile float rx_db = -99, mic_db = -99, spk_db = -99, tx_db = -99;
static volatile int sync_now = 0;
static volatile float snr_now = 0, foff_now = 0, clock_now = 0, metric_now = 0;
static volatile int has_scatter = 0, has_text = 0;
static volatile int test_snr_db = 20;
static volatile long frames_ok = 0, resyncs = 0;
static volatile int rec_on = 0;
static pthread_t th_rx, th_spk, th_mic, th_txp, th_work, th_wav;
static volatile int threads_up = 0;
static char wav_path[300] = "";

// spectrum source (written by the worker only)
static float sring[4096];
static _Atomic unsigned swr = 0;
static void spec_push(short v){
	unsigned w = atomic_load(&swr);
	sring[w & 4095] = v / 32768.0f;
	atomic_store(&swr, w + 1);
}

// scatter / text snapshot
static pthread_mutex_t snap_lock = PTHREAD_MUTEX_INITIALIZER;
static float sc_x[400], sc_y[400];
static int sc_n = 0;
static char rx_text[64] = "";

static void set_status(const char *fmt, ...){
	va_list ap; va_start(ap, fmt);
	vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
	va_end(ap);
}

// ------------------------------------------------------------- the radio ---
static int rig_fd = -1;
static void rig_close(void){ if (rig_fd >= 0) close(rig_fd); rig_fd = -1; rig_ok = 0; }
static int rig_connect(void){
	if (rig_fd >= 0) return 1;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return 0;
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(cfg.rigport);
	if (!inet_aton(cfg.rigaddr, &a.sin_addr)){ close(fd); return 0; }
	fcntl(fd, F_SETFL, O_NONBLOCK);
	int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
	if (r < 0 && errno != EINPROGRESS){ close(fd); return 0; }
	struct pollfd p = {fd, POLLOUT, 0};
	if (poll(&p, 1, 300) <= 0){ close(fd); return 0; }
	int err = 0; socklen_t el = sizeof(err);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
	if (err){ close(fd); return 0; }
	fcntl(fd, F_SETFL, 0);
	struct timeval tv = {0, 300000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	rig_fd = fd;
	return 1;
}
static int rig_cmd(const char *cmd, char *resp, int n){
	if (!rig_connect()){ rig_ok = 0; return -1; }
	if (send(rig_fd, cmd, strlen(cmd), MSG_NOSIGNAL) < 0){ rig_close(); return -1; }
	char tmp[128];
	int got = recv(rig_fd, tmp, sizeof(tmp) - 1, 0);
	if (got <= 0){ rig_close(); return -1; }
	tmp[got] = 0;
	if (resp && n > 0){ strncpy(resp, tmp, n - 1); resp[n - 1] = 0; }
	rig_ok = 1;
	return got;
}

// --------------------------------------------------------------- the codec --
static void codec_close_locked(void){
	if (fdv){ freedv_close(fdv); fdv = NULL; }
	if (rade_h){ rl.close(rade_h); rade_h = NULL; }
	cur_mode = -1;
}

static char tx_txt_char(void *st){
	static int pos = 0;
	size_t n = strlen(cfg.callsign);
	if (!n) return ' ';
	if (pos > (int)n) pos = 0;
	char c = pos < (int)n ? cfg.callsign[pos] : ' ';
	pos++;
	return c;
}
static void rx_txt_char(void *st, char c){
	if (c < 32 || c > 126) return;
	pthread_mutex_lock(&snap_lock);
	size_t n = strlen(rx_text);
	if (n >= sizeof(rx_text) - 1){ memmove(rx_text, rx_text + 1, n - 1); n--; }
	rx_text[n] = c; rx_text[n + 1] = 0;
	pthread_mutex_unlock(&snap_lock);
}

// open mode m; returns 1 on success
static int codec_open(int m){
	pthread_mutex_lock(&codec_lock);
	codec_close_locked();
	int ok = 0;
	if (m >= 0 && m <= M_1600){
		struct freedv *f = freedv_open(c2_ids[m]);
		if (f){
			freedv_set_verbose(f, 0);
			freedv_set_clip(f, cfg.clip_on);
			freedv_set_tx_bpf(f, 1);
			freedv_set_squelch_en(f, cfg.squelch_on);
			freedv_set_snr_squelch_thresh(f, cfg.squelch);
			freedv_set_callback_txt(f, rx_txt_char, tx_txt_char, NULL);
			fdv = f;
			n_speech = freedv_get_n_speech_samples(f);
			n_modem = freedv_get_n_nom_modem_samples(f);
			speech_rate = 8000;
			ok = 1;
		}
		else set_status("Could not open FreeDV %s", mode_names[m]);
	}
	else if (IS_RADE(m)){
		if (!rade_lib_load())
			set_status("RADE is not installed: run rade/build_rade.sh first");
		else {
			rade_h = rl.open(m == M_RAD2);
			if (rade_h){ speech_rate = 16000; ok = 1; }
			else set_status("Could not open RADE");
		}
	}
	cur_mode = ok ? m : -1;
	pthread_mutex_lock(&snap_lock);
	rx_text[0] = 0; sc_n = 0;
	pthread_mutex_unlock(&snap_lock);
	has_scatter = 0; has_text = (m >= 0 && m <= M_1600);
	sync_now = 0; snr_now = 0; foff_now = 0;
	fdrain(&rx8); fdrain(&mic_f); fdrain(&txm8); fdrain(&spk_f);
	pthread_mutex_unlock(&codec_lock);
	return ok;
}

// --------------------------------------------------------------- audio -----
static snd_pcm_t *pcm_open(const char *dev, int capture){
	snd_pcm_t *h = NULL;
	if (snd_pcm_open(&h, dev, capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(h, hw);
	snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S32_LE) < 0){ snd_pcm_close(h); return NULL; }
	unsigned rate = FS;
	snd_pcm_hw_params_set_rate_near(h, hw, &rate, 0);
	if (snd_pcm_hw_params_set_channels(h, hw, 2) < 0){ snd_pcm_close(h); return NULL; }
	snd_pcm_uframes_t per = CHUNK * 2, buf = CHUNK * 8;
	snd_pcm_hw_params_set_period_size_near(h, hw, &per, 0);
	snd_pcm_hw_params_set_buffer_size_near(h, hw, &buf);
	if (snd_pcm_hw_params(h, hw) < 0){ snd_pcm_close(h); return NULL; }
	return h;
}

static double rms_to_db(double r){ return 20.0 * log10(r + 1e-9); }

// receive audio from the radio (loopback) -> 8 kHz modem samples
static void *rx_thread(void *arg){
	struct decim dc = {0};
	decim_init(&dc, FS / 8000);
	static int32_t buf[CHUNK * 2];
	double lvl = 1e-3;
	while (running){
		snd_pcm_t *h = pcm_open(cfg.rxdev, 1);
		if (!h){
			ok_rx = 0;
			for (int i = 0; i < 20 && running; i++) usleep(100000);
			continue;
		}
		ok_rx = 1;
		while (running){
			int n = snd_pcm_readi(h, buf, CHUNK);
			if (n < 0){
				if (snd_pcm_recover(h, n, 1) < 0) break;
				continue;
			}
			double e = 0;
			for (int i = 0; i < n; i++){
				double x = buf[2 * i] / 2147483648.0;
				e += x * x;
			}
			double rms = sqrt(e / (n ? n : 1)) + 1e-9;
			lvl += (rms > lvl ? 0.2 : 0.01) * (rms - lvl);          // slow level tracking
			rx_db = (float)rms_to_db(lvl);
			double gain = (3000.0 / 32768.0) / lvl;                  // decoder wants ~3000 rms
			if (gain > 2000.0) gain = 2000.0;
			if (test_mode || rx_wav_active || tx_active) continue;   // the radio's audio is not wanted now
			for (int i = 0; i < n; i++){
				float y;
				if (decim_push(&dc, (float)(buf[2 * i] / 2147483648.0 * gain * 32768.0), &y))
					fput(&rx8, clip16(y));
			}
		}
		snd_pcm_close(h);
		ok_rx = 0;
	}
	free(dc.h); free(dc.hist);
	return NULL;
}

// decoded speech -> speakers
static void *spk_thread(void *arg){
	struct interp ip = {0};
	int sr = 0;
	static int32_t out[CHUNK * 2];
	while (running){
		snd_pcm_t *h = pcm_open(cfg.spkdev, 0);
		if (!h){
			ok_spk = 0;
			for (int i = 0; i < 20 && running; i++) usleep(100000);
			continue;
		}
		ok_spk = 1;
		int primed = 0;
		while (running){
			if (sr != speech_rate){ sr = speech_rate; interp_init(&ip, FS / sr); primed = 0; }
			int need = CHUNK / ip.L;                         // speech samples for 10 ms
			if (!primed && fl(&spk_f) >= (unsigned)(need * 3)) primed = 1;
			double e = 0;
			float vol = cfg.spkvol / 100.0f;
			for (int i = 0; i < need; i++){
				short v = 0;
				if (primed && !fget(&spk_f, &v)){ primed = 0; v = 0; }
				float o[16];
				interp_push(&ip, v, o);
				for (int k = 0; k < ip.L; k++){
					float s = o[k] * vol * 65536.0f;
					if (s > 2.0e9f) s = 2.0e9f;
					if (s < -2.0e9f) s = -2.0e9f;
					out[2 * (i * ip.L + k)] = out[2 * (i * ip.L + k) + 1] = (int32_t)s;
				}
				e += (double)v * v;
			}
			spk_db = (float)rms_to_db(sqrt(e / need) / 32768.0);
			int n = snd_pcm_writei(h, out, need * ip.L);
			if (n < 0 && snd_pcm_recover(h, n, 1) < 0) break;
		}
		snd_pcm_close(h);
		ok_spk = 0;
	}
	free(ip.h); free(ip.hist);
	return NULL;
}

// microphone -> speech samples for the encoder
static void *mic_thread(void *arg){
	struct decim dc = {0};
	int sr = 0;
	static int32_t buf[CHUNK * 2];
	while (running){
		snd_pcm_t *h = pcm_open(cfg.micdev, 1);
		if (!h){
			ok_mic = 0;
			for (int i = 0; i < 20 && running; i++) usleep(100000);
			continue;
		}
		ok_mic = 1;
		while (running){
			if (sr != speech_rate){ sr = speech_rate; decim_init(&dc, FS / sr); }
			int n = snd_pcm_readi(h, buf, CHUNK);
			if (n < 0){
				if (snd_pcm_recover(h, n, 1) < 0) break;
				continue;
			}
			double e = 0;
			for (int i = 0; i < n; i++){
				double x = buf[2 * i] / 2147483648.0;
				e += x * x;
				float y;
				if (decim_push(&dc, (float)(x * 32768.0 * 1.0), &y) && tx_feed)
					fput(&mic_f, clip16(y));
			}
			mic_db = (float)rms_to_db(sqrt(e / (n ? n : 1)));
		}
		snd_pcm_close(h);
		ok_mic = 0;
	}
	free(dc.h); free(dc.hist);
	return NULL;
}

// modem signal -> the radio (loopback). Only runs while transmitting.
static void *txp_thread(void *arg){
	struct interp ip;
	memset(&ip, 0, sizeof(ip));
	interp_init(&ip, FS / 8000);
	static int32_t out[CHUNK * 2];
	snd_pcm_t *h = NULL;
	while (running){
		if (!tx_active || test_mode){
			if (h){ snd_pcm_drop(h); snd_pcm_close(h); h = NULL; ok_tx = 0; }
			tx_db = -99;
			usleep(5000);
			continue;
		}
		if (!h){
			h = pcm_open(cfg.txdev, 0);
			if (!h){ ok_tx = 0; set_status("Cannot open the radio's transmit audio (%s)", cfg.txdev); usleep(300000); continue; }
			ok_tx = 1;
		}
		int need = CHUNK / ip.L;
		float g = cfg.txlevel / 100.0f * 0.9f * 2147483647.0f / 16000.0f;   // 100% = peaks at 90% of full scale
		float pk = 0;
		for (int i = 0; i < need; i++){
			short v = 0;
			fget(&txm8, &v);                                  // silence if the encoder is behind
			float o[16];
			interp_push(&ip, v, o);
			for (int k = 0; k < ip.L; k++){
				float s = o[k] * g;
				if (s > 2.0e9f) s = 2.0e9f;
				if (s < -2.0e9f) s = -2.0e9f;
				out[2 * (i * ip.L + k)] = out[2 * (i * ip.L + k) + 1] = (int32_t)s;
				if (fabsf(s) > pk) pk = fabsf(s);
			}
		}
		tx_db = (float)rms_to_db(pk / 2147483648.0);
		int n = snd_pcm_writei(h, out, need * ip.L);
		if (n < 0 && snd_pcm_recover(h, n, 1) < 0){ snd_pcm_close(h); h = NULL; }
	}
	if (h) snd_pcm_close(h);
	free(ip.h); free(ip.hist);
	return NULL;
}

// ------------------------------------------------------------- the worker --
static void take_snapshot(void){
	static struct MODEM_STATS ms;
	if (!fdv) return;
	freedv_get_modem_extended_stats(fdv, &ms);
	foff_now = ms.foff; clock_now = ms.clock_offset; metric_now = ms.sync_metric;
	float tx[400], ty[400];
	int n = 0, nc = ms.Nc > 0 ? ms.Nc : 0;
	double e = 0;
	for (int r = ms.nr - 1; r >= 0 && n < 400; r--)
		for (int c = 0; c < nc && n < 400; c++, n++){
			tx[n] = ms.rx_symbols[r][c].real; ty[n] = ms.rx_symbols[r][c].imag;
			e += (double)tx[n] * tx[n] + (double)ty[n] * ty[n];
		}
	if (n > 8){
		float k = (float)(1.0 / (2.0 * sqrt(e / n) + 1e-9));
		pthread_mutex_lock(&snap_lock);
		for (int i = 0; i < n; i++){ sc_x[i] = tx[i] * k; sc_y[i] = ty[i] * k; }
		sc_n = n;
		pthread_mutex_unlock(&snap_lock);
		has_scatter = 1;
	}
}

static FILE *rec_fp = NULL;
static unsigned rec_bytes = 0;
static void wav_header(FILE *f, int rate, unsigned bytes){
	unsigned br = rate * 2, riff = 36 + bytes, fmt = 16, sr = rate;
	unsigned short pcm = 1, ch = 1, ba = 2, bps = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
	fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}
static void rec_speech(const short *p, int n){
	if (rec_on && !rec_fp){
		char name[300];
		time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
		const char *h = getenv("HOME");
		snprintf(name, sizeof(name), "%s/freedv_%04d%02d%02d_%02d%02d%02d.wav", h ? h : ".",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		rec_fp = fopen(name, "wb");
		rec_bytes = 0;
		if (rec_fp){ wav_header(rec_fp, speech_rate, 0); set_status("Recording speech to %s", name); }
		else rec_on = 0;
	}
	if (rec_fp && rec_on){ fwrite(p, 2, n, rec_fp); rec_bytes += n * 2; }
	if (rec_fp && !rec_on){
		fseek(rec_fp, 0, SEEK_SET); wav_header(rec_fp, speech_rate, rec_bytes);
		fclose(rec_fp); rec_fp = NULL;
		set_status("Recording saved in your home folder");
	}
}

static void put_speech(const short *p, int n){
	for (int i = 0; i < n; i++) fput(&spk_f, p[i]);
	rec_speech(p, n);
	double e = 0;
	for (int i = 0; i < n; i++) e += (double)p[i] * p[i];
	if (n && e > 0) frames_ok++;
}

static unsigned rng = 12345;
static float gauss(void){
	float s = 0;
	for (int i = 0; i < 6; i++){ rng = rng * 1664525u + 1013904223u; s += (rng >> 8) / 16777216.0f; }
	return (s - 3.0f) * 1.414f;
}

static void *worker(void *arg){
	static short in[2048], speech[16 * 160 + 4096], modem[8192], pcm16[160];
	double t_sync = 0, last_snap = 0, last_rig = 0, t_key = 0, t_end = 0, tx_t0 = 0;
	int txs = 0, last_sync = 0, last_mode = -1;
	long tone_n = 0;
	double sig_rms = 3000;

	while (running){
		int did = 0;
		double t = now_s();
		int want = (ptt_req || tune_req);
		// mode change requested by the window
		if (last_mode != cfg.mode && !(txs != 0)){
			if (!codec_open(cfg.mode)) { if (!status_msg[0]) set_status("Mode %s not available", mode_names[cfg.mode]); }
			else set_status("%s ready", mode_names[cfg.mode]);
			last_mode = cfg.mode;
		}

		// ---- transmit sequencer ----
		switch (txs){
		case 0:
			if (want && cur_mode >= 0){
				if (!test_mode){
					char r[64] = "";
					if (rig_cmd("T 1\n", r, sizeof(r)) < 0){
						set_status("Cannot reach the radio at %s:%d (is the sBitx running?)", cfg.rigaddr, cfg.rigport);
						ptt_req = tune_req = 0;
						break;
					}
				}
				fdrain(&mic_f); fdrain(&txm8);
				pthread_mutex_lock(&codec_lock);
				if (rade_h) rl.tx_reset(rade_h);
				pthread_mutex_unlock(&codec_lock);
				txs = 1; t_key = t; tx_state_pub = 1;
			}
			break;
		case 1:
			if (t - t_key >= 0.15){
				txs = 2; tx_active = 1; tx_feed = !tune_req; tx_t0 = t; tx_state_pub = 2; tone_n = 0;
			}
			break;
		case 2:
			if (!want || t - tx_t0 > 180.0){
				ptt_req = tune_req = 0;
				tx_feed = 0;
				if (rade_h && rl.eoo){                      // RADE: send the end-of-over frame
					short eoo[8192];
					pthread_mutex_lock(&codec_lock);
					int n = rl.eoo(rade_h, eoo);
					pthread_mutex_unlock(&codec_lock);
					for (int i = 0; i < n && i < 8192; i++) fput(&txm8, eoo[i]);
				}
				txs = 3; tx_state_pub = 3; t_end = 0;
			}
			break;
		case 3:
			if (fl(&txm8) == 0){
				if (t_end == 0) t_end = t;
				if (t - t_end >= 0.30){
					if (!test_mode) rig_cmd("T 0\n", NULL, 0);
					tx_active = 0; txs = 0; tx_state_pub = 0;
				}
			}
			break;
		}

		// ---- transmit: encode ----
		if (txs == 2){
			if (tune_req){
				while (fl(&txm8) < 2400){
					for (int i = 0; i < 80; i++){
						fput(&txm8, (short)(15000.0 * sin(2 * M_PI * 1500.0 * (tone_n++) / 8000.0)));
					}
					did = 1;
				}
			}
			else {
				pthread_mutex_lock(&codec_lock);
				if (fdv){
					while (fl(&mic_f) >= (unsigned)n_speech && n_speech <= 2048 && fl(&txm8) < 8000){
						for (int i = 0; i < n_speech; i++){ short v = 0; fget(&mic_f, &v); in[i] = v; }
						freedv_tx(fdv, modem, in);
						for (int i = 0; i < n_modem; i++) fput(&txm8, modem[i]);
						did = 1;
					}
				}
				else if (rade_h){
					while (fl(&mic_f) >= 160 && fl(&txm8) < 8000){
						for (int i = 0; i < 160; i++){ short v = 0; fget(&mic_f, &v); pcm16[i] = v; }
						int n = rl.tx(rade_h, pcm16, modem);
						for (int i = 0; i < n; i++) fput(&txm8, modem[i]);
						did = 1;
					}
				}
				pthread_mutex_unlock(&codec_lock);
			}
		}

		// ---- the modem signal goes round in the test mode; the spectrum shows what is sent ----
		if (test_mode && tx_active){
			short v;
			double es = 0; int cnt = 0;
			float nz = 0;
			while (fget(&txm8, &v)){
				es += (double)v * v; cnt++;
				float x = v;
				if (cnt == 1){ sig_rms = 0.995 * sig_rms + 0.005 * 3000; }
				nz = (float)(3000.0 / pow(10.0, test_snr_db / 20.0) * 0.8);   // about 3 kHz of noise
				x += nz * gauss();
				fput(&rx8, clip16(x));
				spec_push(v);
			}
			(void)es; (void)nz;
		}
		else if (tx_active){
			// spectrum of what we are sending is taken in the encoder side (peek)
			unsigned tl = atomic_load(&txm8.tail), hd = atomic_load(&txm8.head);
			int c = 0;
			for (unsigned i = tl; i != hd && c < 128; i++, c++) spec_push(txm8.buf[i & (FIFO_N - 1)]);
		}

		// ---- receive: decode ----
		if (!tx_active || test_mode){
			pthread_mutex_lock(&codec_lock);
			if (fdv){
				for (;;){
					int nin = freedv_nin(fdv);
					if (nin > 2000 || fl(&rx8) < (unsigned)nin) break;
					for (int i = 0; i < nin; i++){ short v = 0; fget(&rx8, &v); in[i] = v; if (!test_mode || !tx_active) spec_push(v); }
					int nout = freedv_rx(fdv, speech, in);
					int s; float snr;
					freedv_get_modem_stats(fdv, &s, &snr);
					sync_now = s; snr_now = snr;
					if (s && !last_sync) resyncs++;
					last_sync = s;
					if (nout > 0) put_speech(speech, nout);
					did = 1;
				}
				if (t - last_snap > 0.1){ last_snap = t; take_snapshot(); }
			}
			else if (rade_h){
				for (int guard = 0; guard < 8; guard++){
					int nin = rl.nin(rade_h);
					if (nin <= 0 || nin > 2000 || fl(&rx8) < (unsigned)nin) break;
					for (int i = 0; i < nin; i++){ short v = 0; fget(&rx8, &v); in[i] = v; spec_push(v); }
					int nout = rl.rx(rade_h, in, nin, speech);
					int s = rl.sync(rade_h);
					sync_now = s;
					snr_now = rl.snr(rade_h);
					if (rl.foff) foff_now = rl.foff(rade_h);
					if (s && !last_sync) resyncs++;
					last_sync = s;
					if (nout > 0) put_speech(speech, nout);
					did = 1;
				}
			}
			pthread_mutex_unlock(&codec_lock);
		}
		else {
			fdrain(&rx8);
		}

		// ---- radio frequency, about every 2 s while idle ----
		if (!test_mode && txs == 0 && t - last_rig > 2.0){
			last_rig = t;
			char r[64] = "";
			if (rig_cmd("f\n", r, sizeof(r)) > 0) rig_freq = atof(r);
		}
		(void)t_sync;
		if (!did) usleep(2000);
	}
	if (rig_fd >= 0 && tx_active) rig_cmd("T 0\n", NULL, 0);
	return NULL;
}

// a recorded WAV (any of 8, 16 or 48 kHz, 16-bit mono) is played into the receiver
static void *wav_thread(void *arg){
	FILE *f = fopen(wav_path, "rb");
	if (!f){ set_status("Cannot open %s", wav_path); rx_wav_active = 0; return NULL; }
	unsigned char hd[44];
	if (fread(hd, 1, 44, f) != 44 || memcmp(hd, "RIFF", 4)){ fclose(f); rx_wav_active = 0; set_status("Not a WAV file"); return NULL; }
	unsigned rate; memcpy(&rate, hd + 24, 4);
	unsigned short ch; memcpy(&ch, hd + 22, 2);
	int M = rate == 8000 ? 1 : rate == 16000 ? 2 : rate == 48000 ? 6 : 0;
	if (!M || ch != 1){ fclose(f); rx_wav_active = 0; set_status("WAV must be 16-bit mono, 8, 16 or 48 kHz"); return NULL; }
	struct decim dc = {0};
	if (M > 1) decim_init(&dc, M);
	short buf[960];
	set_status("Decoding %s", wav_path);
	rx_wav_active = 1;
	double t0 = now_s(); long sent = 0;
	size_t n;
	while (running && rx_wav_active && (n = fread(buf, 2, 960, f)) > 0){
		for (size_t i = 0; i < n; i++){
			float y;
			if (M == 1) fput(&rx8, buf[i]);
			else if (decim_push(&dc, buf[i], &y)) fput(&rx8, clip16(y));
		}
		sent += n / M;
		while (running && rx_wav_active && sent / 8000.0 > now_s() - t0 + 0.05) usleep(5000);
	}
	fclose(f);
	free(dc.h); free(dc.hist);
	rx_wav_active = 0;
	set_status("WAV finished");
	return NULL;
}

static void app_start(void){
	if (running) return;
	running = 1;
	pthread_create(&th_rx, NULL, rx_thread, NULL);
	pthread_create(&th_spk, NULL, spk_thread, NULL);
	pthread_create(&th_mic, NULL, mic_thread, NULL);
	pthread_create(&th_txp, NULL, txp_thread, NULL);
	pthread_create(&th_work, NULL, worker, NULL);
	threads_up = 1;
}
static void app_stop(void){
	if (!running) return;
	ptt_req = tune_req = 0;
	// let the sequencer unkey the radio
	for (int i = 0; i < 200 && tx_state_pub != 0; i++) usleep(10000);
	running = 0;
	rx_wav_active = 0;
	pthread_join(th_work, NULL);
	pthread_join(th_rx, NULL); pthread_join(th_spk, NULL);
	pthread_join(th_mic, NULL); pthread_join(th_txp, NULL);
	threads_up = 0;
	ok_rx = ok_tx = ok_mic = ok_spk = 0;
	pthread_mutex_lock(&codec_lock);
	codec_close_locked();
	pthread_mutex_unlock(&codec_lock);
	rig_close();
}

// =============================================================== the window =
static GtkWidget *win, *w_spec, *w_wf, *w_sc, *w_hist, *w_panel, *w_status;
static GtkWidget *b_start, *b_ptt, *b_tune, *b_test, *c_mode, *k_sq, *k_clip, *s_txlvl, *s_vol, *l_txlvl, *l_vol;
static fftwf_plan fplan;
static float *fin, *fout_c;               // fftw buffers (fout_c holds NFFT/2+1 complex)
static float spec_db[NBIN], spec_avg[NBIN];
static float floor_db = -90;
static cairo_surface_t *wf_surf = NULL;
static unsigned char *wf_pix;
static float hist_snr[HIST_N]; static int hist_sync[HIST_N]; static int hist_n = 0;
static int building = 0;
static float hann[NFFT];

static void rgb_for(float v, unsigned char *p){          // v 0..1 -> blue/cyan/yellow/red, as BGRA
	float r, g, b;
	if (v < 0.25f){ r = 0; g = 0; b = 0.25f + v * 3; }
	else if (v < 0.5f){ r = 0; g = (v - 0.25f) * 4; b = 1; }
	else if (v < 0.75f){ r = (v - 0.5f) * 4; g = 1; b = 1 - (v - 0.5f) * 4; }
	else { r = 1; g = 1 - (v - 0.75f) * 4; b = 0; }
	p[0] = (unsigned char)(b * 255); p[1] = (unsigned char)(g * 255); p[2] = (unsigned char)(r * 255); p[3] = 255;
}

static int ui_tick_count = 0;
static gboolean ui_tick(gpointer d){
	// ---- spectrum ----
	unsigned w = atomic_load(&swr);
	if (w >= NFFT){
		for (int i = 0; i < NFFT; i++) fin[i] = sring[(w - NFFT + i) & 4095] * hann[i];
		fftwf_execute(fplan);
		float mn = 1e9;
		for (int k = 0; k < NBIN; k++){
			float re = fout_c[2 * k], im = fout_c[2 * k + 1];
			float db = 10.0f * log10f(re * re + im * im + 1e-12f);
			spec_avg[k] += 0.4f * (db - spec_avg[k]);
			spec_db[k] = spec_avg[k];
		}
		// noise floor: mean of the lowest quarter of the bins
		float tmp[64]; int nb = 0;
		for (int k = 0; k < NBIN; k += 16) tmp[nb++] = spec_db[k];
		for (int i = 0; i < nb; i++) for (int j = i + 1; j < nb; j++) if (tmp[j] < tmp[i]){ float x = tmp[i]; tmp[i] = tmp[j]; tmp[j] = x; }
		float s = 0; for (int i = 0; i < nb / 4; i++) s += tmp[i];
		mn = s / (nb / 4);
		floor_db += 0.2f * (mn - floor_db);
	}
	// ---- waterfall row every other tick ----
	if ((ui_tick_count & 1) == 0 && wf_surf){
		int stride = cairo_image_surface_get_stride(wf_surf);
		memmove(wf_pix + stride, wf_pix, stride * (WF_H - 1));
		for (int x = 0; x < WF_W; x++){
			float a = (spec_db[2 * x] + spec_db[2 * x + 1]) * 0.5f;
			float v = (a - (floor_db - 3.0f)) / 60.0f;
			if (v < 0) v = 0;
			if (v > 1) v = 1;
			rgb_for(v, wf_pix + 4 * x);
		}
		cairo_surface_mark_dirty(wf_surf);
	}
	// ---- SNR history, 2 per second ----
	if (ui_tick_count % 10 == 0){
		if (hist_n < HIST_N){ hist_snr[hist_n] = snr_now; hist_sync[hist_n] = sync_now; hist_n++; }
		else {
			memmove(hist_snr, hist_snr + 1, (HIST_N - 1) * sizeof(float));
			memmove(hist_sync, hist_sync + 1, (HIST_N - 1) * sizeof(int));
			hist_snr[HIST_N - 1] = snr_now; hist_sync[HIST_N - 1] = sync_now;
		}
		gtk_widget_queue_draw(w_hist);
	}
	ui_tick_count++;
	gtk_widget_queue_draw(w_spec);
	gtk_widget_queue_draw(w_wf);
	gtk_widget_queue_draw(w_sc);
	gtk_widget_queue_draw(w_panel);

	// ---- the buttons follow the radio state ----
	building = 1;
	int on_air = (tx_state_pub == 2);
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b_ptt)) != (ptt_req != 0))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_ptt), ptt_req != 0);
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b_tune)) != (tune_req != 0))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_tune), tune_req != 0);
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b_start)) != (running != 0))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_start), running != 0);
	building = 0;
	(void)on_air;

	// ---- status bar ----
	if (ui_tick_count % 5 == 0){
		char t[400], fr[32] = "--";
		if (rig_ok && rig_freq > 0) snprintf(fr, sizeof(fr), "%.3f MHz", rig_freq / 1e6);
		snprintf(t, sizeof(t), "%s | Radio: %s %s | RX %s  SPK %s  MIC %s  TX %s | %s",
			running ? (sync_now ? "SYNC" : "searching") : "stopped",
			test_mode ? "TEST (no radio)" : (rig_ok ? "connected" : "not connected"), test_mode ? "" : fr,
			ok_rx ? "OK" : "--", ok_spk ? "OK" : "--", ok_mic ? "OK" : "--", ok_tx ? "OK" : "--",
			status_msg);
		gtk_label_set_text(GTK_LABEL(w_status), t);
	}
	return TRUE;
}

static void txt(cairo_t *cr, double x, double y, double sz, const char *s){
	cairo_set_font_size(cr, sz); cairo_move_to(cr, x, y); cairo_show_text(cr, s);
}
static void bar(cairo_t *cr, double x, double y, double w, double h, double f, double r, double g, double b){
	if (f < 0) f = 0;
	if (f > 1) f = 1;
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12); cairo_rectangle(cr, x, y, w, h); cairo_fill(cr);
	cairo_set_source_rgb(cr, r, g, b); cairo_rectangle(cr, x, y, w * f, h); cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.7, 0.7, 0.7); cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + .5, y + .5, w - 1, h - 1); cairo_stroke(cr);
}

static gboolean draw_spec(GtkWidget *w, cairo_t *cr, gpointer d){
	int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
	cairo_set_source_rgb(cr, 0, 0, 0); cairo_paint(cr);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	double top = floor_db + 55.0, bot = floor_db - 5.0;
	// grid: every 500 Hz, and every 10 dB
	cairo_set_line_width(cr, 1);
	for (int f = 0; f <= 4000; f += 500){
		double x = (double)f / 4000.0 * W;
		cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
		cairo_move_to(cr, x + .5, 0); cairo_line_to(cr, x + .5, H - 12); cairo_stroke(cr);
		char t[12]; sprintf(t, "%d", f);
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		if (f < 4000) txt(cr, x + 2, H - 1, 10, t);
	}
	for (int i = 1; i < 6; i++){
		double y = (H - 12.0) * i / 6.0;
		cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
		cairo_move_to(cr, 0, y + .5); cairo_line_to(cr, W, y + .5); cairo_stroke(cr);
	}
	cairo_set_source_rgb(cr, 1.0, 0.9, 0.2);
	cairo_set_line_width(cr, 1.5);
	for (int k = 0; k < NBIN; k += 2){
		double x = (double)k / NBIN * W;
		double v = (spec_db[k] - bot) / (top - bot);
		if (v < 0) v = 0;
		if (v > 1) v = 1;
		double y = (H - 12.0) * (1.0 - v);
		if (k == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
	}
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	txt(cr, 4, 12, 11, tx_state_pub == 2 ? "Spectrum (transmit signal, Hz)" : "Spectrum (receive audio, Hz)");
	return FALSE;
}

static gboolean draw_wf(GtkWidget *w, cairo_t *cr, gpointer d){
	int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
	cairo_set_source_rgb(cr, 0, 0, 0); cairo_paint(cr);
	if (wf_surf){
		cairo_save(cr);
		cairo_scale(cr, (double)W / WF_W, (double)H / WF_H);
		cairo_set_source_surface(cr, wf_surf, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	return FALSE;
}

static gboolean draw_sc(GtkWidget *w, cairo_t *cr, gpointer d){
	int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
	cairo_set_source_rgb(cr, 0, 0, 0); cairo_paint(cr);
	cairo_set_source_rgb(cr, 0.35, 0.35, 0.35); cairo_set_line_width(cr, 1);
	cairo_move_to(cr, W / 2, 0); cairo_line_to(cr, W / 2, H);
	cairo_move_to(cr, 0, H / 2); cairo_line_to(cr, W, H / 2); cairo_stroke(cr);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	if (has_scatter && cur_mode >= 0 && cur_mode <= M_1600){
		double sc = (W < H ? W : H) / 2.0 * 0.9;
		pthread_mutex_lock(&snap_lock);
		cairo_set_source_rgba(cr, 0.2, 1.0, 0.3, 0.8);
		for (int i = 0; i < sc_n; i++){
			cairo_arc(cr, W / 2 + sc_x[i] * sc, H / 2 - sc_y[i] * sc, 1.8, 0, 6.2832); cairo_fill(cr);
		}
		pthread_mutex_unlock(&snap_lock);
	}
	else {
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		txt(cr, 8, H / 2 - 6, 12, !running ? "stopped" : (IS_RADE(cur_mode) ? "no plot for RADE" : "waiting for signal"));
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	txt(cr, 5, 13, 11, "Constellation");
	return FALSE;
}

static gboolean draw_hist(GtkWidget *w, cairo_t *cr, gpointer d){
	int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
	cairo_set_source_rgb(cr, 0.03, 0.03, 0.03); cairo_paint(cr);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	// -5 .. +25 dB
	cairo_set_line_width(cr, 1);
	for (int db = -5; db <= 25; db += 10){
		double y = H - (db + 5.0) / 30.0 * H;
		cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
		cairo_move_to(cr, 0, y + .5); cairo_line_to(cr, W, y + .5); cairo_stroke(cr);
		char t[8]; sprintf(t, "%+d", db);
		cairo_set_source_rgb(cr, 0.7, 0.7, 0.7); txt(cr, 2, y - 2, 9, t);
	}
	cairo_set_line_width(cr, 2);
	cairo_set_source_rgb(cr, 0.3, 0.9, 0.4);
	int first = 1;
	for (int i = 0; i < hist_n; i++){
		if (!hist_sync[i]){ first = 1; continue; }
		double x = (double)i / (HIST_N - 1) * W;
		double v = (hist_snr[i] + 5.0) / 30.0;
		if (v < 0) v = 0;
		if (v > 1) v = 1;
		double y = H - v * H;
		if (first){ cairo_move_to(cr, x, y); first = 0; } else cairo_line_to(cr, x, y);
	}
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	txt(cr, W - 120, 12, 10, "SNR, last 60 s (dB)");
	return FALSE;
}

static gboolean draw_panel(GtkWidget *w, cairo_t *cr, gpointer d){
	int W = gtk_widget_get_allocated_width(w);
	char t[96];
	cairo_set_source_rgb(cr, 0.08, 0.08, 0.08); cairo_paint(cr);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	int tx = (tx_state_pub == 2);
	if (!running) cairo_set_source_rgb(cr, .4, .4, .4);
	else if (tx || tx_state_pub == 1 || tx_state_pub == 3) cairo_set_source_rgb(cr, 1.0, 0.2, 0.1);
	else if (sync_now) cairo_set_source_rgb(cr, 0.1, 0.9, 0.2);
	else cairo_set_source_rgb(cr, 0.9, 0.7, 0.1);
	cairo_arc(cr, 18, 18, 11, 0, 6.2832); cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	txt(cr, 36, 25, 16, !running ? "STOPPED" : tx_state_pub ? "TRANSMIT" : (sync_now ? "SYNC" : "SEARCHING"));
	txt(cr, W - 75, 25, 14, cur_mode >= 0 ? mode_names[cur_mode] : "--");
	if (sync_now && !tx) snprintf(t, sizeof(t), "SNR %+.1f dB", snr_now); else strcpy(t, "SNR  --");
	txt(cr, 8, 52, 14, t);
	bar(cr, 8, 58, W - 16, 10, (snr_now + 5.0) / 30.0, 0.2, 0.8, 0.3);
	if (sync_now) snprintf(t, sizeof(t), "Freq offset %+.0f Hz", foff_now); else strcpy(t, "Freq offset  --");
	txt(cr, 8, 86, 12, t);
	if (sync_now && has_text) snprintf(t, sizeof(t), "Clock %+.0f ppm   Sync %.0f%%", clock_now, metric_now * 100); else strcpy(t, "Clock  --");
	txt(cr, 8, 102, 12, t);
	snprintf(t, sizeof(t), "Frames %ld   Re-syncs %ld", frames_ok, resyncs);
	txt(cr, 8, 118, 12, t);
	snprintf(t, sizeof(t), "RX in %.0f dB", rx_db); txt(cr, 8, 138, 11, t);
	bar(cr, 8, 142, W - 16, 8, (rx_db + 60) / 60.0, 0.3, 0.6, 1.0);
	snprintf(t, sizeof(t), "Mic %.0f dB", mic_db); txt(cr, 8, 164, 11, t);
	bar(cr, 8, 168, W - 16, 8, (mic_db + 60) / 60.0, 1.0, 0.6, 0.2);
	snprintf(t, sizeof(t), "Speech %.0f dB", spk_db); txt(cr, 8, 190, 11, t);
	bar(cr, 8, 194, W - 16, 8, (spk_db + 60) / 60.0, 0.6, 0.9, 0.6);
	snprintf(t, sizeof(t), "TX peak %.0f dB", tx_db); txt(cr, 8, 216, 11, t);
	bar(cr, 8, 220, W - 16, 8, (tx_db + 40) / 40.0, 1.0, 0.3, 0.3);
	cairo_set_source_rgb(cr, 1, 1, 1);
	txt(cr, 8, 246, 11, "Received text:");
	cairo_set_source_rgb(cr, 1.0, 0.9, 0.3);
	pthread_mutex_lock(&snap_lock);
	strncpy(t, (cur_mode >= 0 && IS_RADE(cur_mode)) ? "(not with RADE)" : rx_text, sizeof(t) - 1);
	pthread_mutex_unlock(&snap_lock);
	t[sizeof(t) - 1] = 0;
	txt(cr, 8, 264, 14, t);
	return FALSE;
}

// ------------------------------------------------------------ the controls --
static void on_start(GtkToggleButton *b, gpointer d){
	if (building) return;
	if (gtk_toggle_button_get_active(b)) app_start(); else app_stop();
}
static void on_ptt(GtkToggleButton *b, gpointer d){
	if (building) return;
	if (!running){ gtk_toggle_button_set_active(b, FALSE); set_status("Press Start first"); return; }
	ptt_req = gtk_toggle_button_get_active(b);
	if (ptt_req) tune_req = 0;
}
static void on_tune(GtkToggleButton *b, gpointer d){
	if (building) return;
	if (!running){ gtk_toggle_button_set_active(b, FALSE); set_status("Press Start first"); return; }
	tune_req = gtk_toggle_button_get_active(b);
	if (tune_req) ptt_req = 0;
}
static void on_test(GtkToggleButton *b, gpointer d){
	if (building) return;
	test_mode = gtk_toggle_button_get_active(b);
	fdrain(&rx8); fdrain(&txm8);
	set_status(test_mode ? "TEST: press PTT and speak -- the signal goes round inside the program (no radio)" : "Test off");
}
static void on_mode(GtkComboBox *c, gpointer d){
	if (building) return;
	cfg.mode = gtk_combo_box_get_active(c);
	cfg_save();
	status_msg[0] = 0;
	if (!running) set_status("%s selected -- press Start", mode_names[cfg.mode]);
}
static void on_sq(GtkToggleButton *b, gpointer d){
	cfg.squelch_on = gtk_toggle_button_get_active(b); cfg_save();
	pthread_mutex_lock(&codec_lock); if (fdv) freedv_set_squelch_en(fdv, cfg.squelch_on); pthread_mutex_unlock(&codec_lock);
}
static void on_clip(GtkToggleButton *b, gpointer d){
	cfg.clip_on = gtk_toggle_button_get_active(b); cfg_save();
	pthread_mutex_lock(&codec_lock); if (fdv) freedv_set_clip(fdv, cfg.clip_on); pthread_mutex_unlock(&codec_lock);
}
static void on_txlvl(GtkRange *r, gpointer d){
	cfg.txlevel = (int)gtk_range_get_value(r);
	char t[40]; sprintf(t, "TX level %d%%", cfg.txlevel); gtk_label_set_text(GTK_LABEL(l_txlvl), t);
	if (!building) cfg_save();
}
static void on_vol(GtkRange *r, gpointer d){
	cfg.spkvol = (int)gtk_range_get_value(r);
	char t[40]; sprintf(t, "Speaker %d%%", cfg.spkvol); gtk_label_set_text(GTK_LABEL(l_vol), t);
	if (!building) cfg_save();
}
static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d){
	if (e->keyval == GDK_KEY_F11){
		static int fs = 0;
		fs = !fs;
		if (fs) gtk_window_fullscreen(GTK_WINDOW(win)); else gtk_window_unfullscreen(GTK_WINDOW(win));
		return TRUE;
	}
	if (e->keyval == GDK_KEY_space && !GTK_IS_ENTRY(gtk_window_get_focus(GTK_WINDOW(win)))){
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_ptt), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b_ptt)));
		return TRUE;
	}
	return FALSE;
}
static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d){
	app_stop();
	cfg_save();
	return FALSE;
}

// ---- settings window ----
static void fill_devs(GtkComboBoxText *cb, int capture, const char *cur){
	gtk_combo_box_text_append_text(cb, "default");
	int card = -1;
	while (snd_card_next(&card) >= 0 && card >= 0){
		char name[16]; snprintf(name, sizeof(name), "hw:%d", card);
		snd_ctl_t *ctl;
		if (snd_ctl_open(&ctl, name, 0) < 0) continue;
		int dev = -1;
		while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0){
			snd_pcm_info_t *info; snd_pcm_info_alloca(&info);
			snd_pcm_info_set_device(info, dev);
			snd_pcm_info_set_subdevice(info, 0);
			snd_pcm_info_set_stream(info, capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK);
			if (snd_ctl_pcm_info(ctl, info) < 0) continue;
			char e[96]; snprintf(e, sizeof(e), "plughw:%d,%d", card, dev);
			gtk_combo_box_text_append_text(cb, e);
		}
		snd_ctl_close(ctl);
	}
	GtkWidget *en = gtk_bin_get_child(GTK_BIN(cb));
	gtk_entry_set_text(GTK_ENTRY(en), cur);
}
static void cards_text(char *out, int n){
	out[0] = 0;
	int card = -1;
	while (snd_card_next(&card) >= 0 && card >= 0){
		char *nm = NULL;
		snd_card_get_name(card, &nm);
		char l[100]; snprintf(l, sizeof(l), "card %d: %s\n", card, nm ? nm : "?");
		if (strlen(out) + strlen(l) < (size_t)n - 1) strcat(out, l);
		free(nm);
	}
}
static void on_settings(GtkMenuItem *mi, gpointer d){
	GtkWidget *dlg = gtk_dialog_new_with_buttons("Settings", GTK_WINDOW(win), GTK_DIALOG_MODAL,
		"Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_OK, NULL);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6); gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid);
	GtkWidget *cb[4], *rig, *port, *call;
	const char *lab[4] = {"Radio receive audio (from sBitx)", "Radio transmit audio (to sBitx)", "Microphone", "Speakers"};
	const char *cur[4] = {cfg.rxdev, cfg.txdev, cfg.micdev, cfg.spkdev};
	for (int i = 0; i < 4; i++){
		gtk_grid_attach(GTK_GRID(grid), gtk_label_new(lab[i]), 0, i, 1, 1);
		cb[i] = gtk_combo_box_text_new_with_entry();
		fill_devs(GTK_COMBO_BOX_TEXT(cb[i]), (i == 0 || i == 2), cur[i]);
		gtk_widget_set_hexpand(cb[i], TRUE);
		gtk_grid_attach(GTK_GRID(grid), cb[i], 1, i, 1, 1);
	}
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Radio address / port"), 0, 4, 1, 1);
	GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	rig = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(rig), cfg.rigaddr);
	port = gtk_entry_new(); gtk_entry_set_width_chars(GTK_ENTRY(port), 6);
	char pb[16]; sprintf(pb, "%d", cfg.rigport); gtk_entry_set_text(GTK_ENTRY(port), pb);
	gtk_box_pack_start(GTK_BOX(hb), rig, TRUE, TRUE, 0); gtk_box_pack_start(GTK_BOX(hb), port, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), hb, 1, 4, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Your callsign (sent as text)"), 0, 5, 1, 1);
	call = gtk_entry_new(); gtk_entry_set_max_length(GTK_ENTRY(call), 15); gtk_entry_set_text(GTK_ENTRY(call), cfg.callsign);
	gtk_grid_attach(GTK_GRID(grid), call, 1, 5, 1, 1);
	char cards[600]; cards_text(cards, sizeof(cards));
	char info[800]; snprintf(info, sizeof(info), "Sound cards on this computer:\n%sThe sBitx loopback cards are normally card 1 and card 2:\n  receive = plughw:1,1    transmit = plughw:2,0", cards);
	GtkWidget *il = gtk_label_new(info); gtk_label_set_xalign(GTK_LABEL(il), 0);
	gtk_grid_attach(GTK_GRID(grid), il, 0, 6, 2, 1);
	gtk_widget_show_all(dlg);
	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK){
		char *p[4] = {cfg.rxdev, cfg.txdev, cfg.micdev, cfg.spkdev};
		int changed = 0;
		for (int i = 0; i < 4; i++){
			const char *v = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(cb[i]))));
			if (strcmp(v, p[i])) changed = 1;
			snprintf(p[i], 64, "%s", v);
		}
		snprintf(cfg.rigaddr, sizeof(cfg.rigaddr), "%s", gtk_entry_get_text(GTK_ENTRY(rig)));
		cfg.rigport = atoi(gtk_entry_get_text(GTK_ENTRY(port)));
		const char *c = gtk_entry_get_text(GTK_ENTRY(call));
		int j = 0;
		for (int i = 0; c[i] && j < 15; i++){
			char ch = c[i]; if (ch >= 'a' && ch <= 'z') ch -= 32;
			if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || ch == '/' || ch == '-') cfg.callsign[j++] = ch;
		}
		cfg.callsign[j] = 0;
		rig_close();
		cfg_save();
		if (changed && running) set_status("Audio devices changed: press Stop, then Start");
	}
	gtk_widget_destroy(dlg);
}
static void on_decode_wav(GtkMenuItem *mi, gpointer d){
	if (!running){ set_status("Press Start first, then open a WAV file"); return; }
	GtkWidget *dlg = gtk_file_chooser_dialog_new("Decode a recorded FreeDV signal (WAV)", GTK_WINDOW(win),
		GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, NULL);
	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT){
		char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
		if (f){
			rx_wav_active = 0;
			if (th_wav) pthread_join(th_wav, NULL);
			snprintf(wav_path, sizeof(wav_path), "%s", f);
			g_free(f);
			rx_wav_active = 1;
			pthread_create(&th_wav, NULL, wav_thread, NULL);
		}
	}
	gtk_widget_destroy(dlg);
}
static void on_rec(GtkCheckMenuItem *mi, gpointer d){ rec_on = gtk_check_menu_item_get_active(mi); }
static void on_quit(GtkMenuItem *mi, gpointer d){ app_stop(); cfg_save(); gtk_main_quit(); }
static void on_about(GtkMenuItem *mi, gpointer d){
	GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
		"%s\n\nHow to use it\n"
		"1. sBitx: select DIGITAL mode (USB), switch TX EQ, mic compressor, noise\n"
		"    reduction, RX EQ and audio boost OFF.\n"
		"2. Pick the mode, press Start.\n"
		"3. Receive: the spectrum/waterfall show the signal; SYNC turns green when\n"
		"    the decoder locks. Speech comes from your speakers.\n"
		"4. Transmit: press PTT (or the space bar), speak, press again to stop.\n"
		"    Use Tune into a dummy load first and set TX level / sBitx DRIVE so the\n"
		"    ALC stays quiet.\n"
		"5. Test (toolbar) runs a full transmit-to-receive loop inside the program\n"
		"    with no radio, so you can hear and see how a mode behaves.\n\n"
		"Uses the open-source codec2 (FreeDV) and RADE libraries.", APP_NAME);
	gtk_dialog_run(GTK_DIALOG(dlg));
	gtk_widget_destroy(dlg);
}

static void css(void){
	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_data(p,
		"window { background-color: #1e1e1e; }"
		"label { color: #ffffff; }"
		"menubar, menubar > menuitem, menu, menu menuitem { background-color: #2a2a2a; color: #ffffff; }"
		"menu menuitem label, menubar menuitem label { color: #ffffff; }"
		"button { min-height: 30px; background-image: none; background-color: #000000; color: #ffffff;"
		"  border: 2px solid #c8c8c8; border-radius: 4px; padding: 0 8px; }"
		"button label { color: #ffffff; font-weight: bold; }"
		"button:checked { background-color: #1f6f2f; }"
		"button#ptt:checked { background-color: #b01010; }"
		"button#tune:checked { background-color: #a05a00; }"
		"button#test:checked { background-color: #2a4a9a; }"
		"entry, combobox entry { background-color: #000000; color: #ffffff; border: 2px solid #c8c8c8; }"
		"scale trough { min-height: 8px; background-color: #d8d8d8; border: 1px solid #000000; }"
		"scale slider { min-width: 22px; min-height: 22px; background-image: none; background-color: #ffffff;"
		"  border: 2px solid #000000; border-radius: 6px; }"
		"checkbutton label { color: #ffffff; }"
		"#statusbar { background-color: #000000; padding: 3px 6px; }"
		"dialog, messagedialog { background-color: #1e1e1e; }",
		-1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static GtkWidget *mk_item(GtkWidget *menu, const char *label, GCallback cb){
	GtkWidget *it = gtk_menu_item_new_with_label(label);
	g_signal_connect(it, "activate", cb, NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
	return it;
}

static void build_ui(void){
	css();
	for (int i = 0; i < NFFT; i++) hann[i] = 0.5f - 0.5f * cosf(2 * M_PI * i / (NFFT - 1));
	fin = fftwf_malloc(sizeof(float) * NFFT);
	fout_c = fftwf_malloc(sizeof(float) * 2 * (NFFT / 2 + 1));
	fplan = fftwf_plan_dft_r2c_1d(NFFT, fin, (fftwf_complex *)fout_c, FFTW_ESTIMATE);
	wf_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WF_W, WF_H);
	wf_pix = cairo_image_surface_get_data(wf_surf);
	memset(wf_pix, 0, cairo_image_surface_get_stride(wf_surf) * WF_H);
	for (int i = 0; i < WF_H * WF_W; i++) wf_pix[4 * i + 3] = 255;
	for (int i = 0; i < NBIN; i++) spec_avg[i] = -90;

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), APP_NAME);
	gtk_window_set_default_size(GTK_WINDOW(win), 790, 440);
	g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), NULL);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_container_add(GTK_CONTAINER(win), vbox);

	// menu bar
	GtkWidget *mb = gtk_menu_bar_new();
	GtkWidget *mf = gtk_menu_new(), *mo = gtk_menu_new(), *mh = gtk_menu_new();
	GtkWidget *fi = gtk_menu_item_new_with_label("File"), *oi = gtk_menu_item_new_with_label("Options"), *hi = gtk_menu_item_new_with_label("Help");
	mk_item(mf, "Decode a WAV file...", G_CALLBACK(on_decode_wav));
	GtkWidget *rc = gtk_check_menu_item_new_with_label("Record received speech");
	g_signal_connect(rc, "toggled", G_CALLBACK(on_rec), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(mf), rc);
	mk_item(mf, "Quit", G_CALLBACK(on_quit));
	mk_item(mo, "Settings...", G_CALLBACK(on_settings));
	mk_item(mh, "How to use / About", G_CALLBACK(on_about));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(fi), mf); gtk_menu_item_set_submenu(GTK_MENU_ITEM(oi), mo); gtk_menu_item_set_submenu(GTK_MENU_ITEM(hi), mh);
	gtk_menu_shell_append(GTK_MENU_SHELL(mb), fi); gtk_menu_shell_append(GTK_MENU_SHELL(mb), oi); gtk_menu_shell_append(GTK_MENU_SHELL(mb), hi);
	gtk_box_pack_start(GTK_BOX(vbox), mb, FALSE, FALSE, 0);

	// toolbar
	GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	gtk_container_set_border_width(GTK_CONTAINER(tb), 3);
	b_start = gtk_toggle_button_new_with_label("Start");
	g_signal_connect(b_start, "toggled", G_CALLBACK(on_start), NULL);
	c_mode = gtk_combo_box_text_new();
	for (int i = 0; i < M_COUNT; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_mode), mode_names[i]);
	gtk_combo_box_set_active(GTK_COMBO_BOX(c_mode), cfg.mode);
	g_signal_connect(c_mode, "changed", G_CALLBACK(on_mode), NULL);
	b_ptt = gtk_toggle_button_new_with_label("PTT"); gtk_widget_set_name(b_ptt, "ptt");
	g_signal_connect(b_ptt, "toggled", G_CALLBACK(on_ptt), NULL);
	b_tune = gtk_toggle_button_new_with_label("Tune"); gtk_widget_set_name(b_tune, "tune");
	g_signal_connect(b_tune, "toggled", G_CALLBACK(on_tune), NULL);
	b_test = gtk_toggle_button_new_with_label("Test"); gtk_widget_set_name(b_test, "test");
	g_signal_connect(b_test, "toggled", G_CALLBACK(on_test), NULL);
	k_sq = gtk_check_button_new_with_label("Squelch");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(k_sq), cfg.squelch_on);
	g_signal_connect(k_sq, "toggled", G_CALLBACK(on_sq), NULL);
	k_clip = gtk_check_button_new_with_label("Clip");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(k_clip), cfg.clip_on);
	g_signal_connect(k_clip, "toggled", G_CALLBACK(on_clip), NULL);
	l_txlvl = gtk_label_new("TX level"); l_vol = gtk_label_new("Speaker");
	s_txlvl = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10, 100, 5);
	gtk_scale_set_draw_value(GTK_SCALE(s_txlvl), FALSE); gtk_widget_set_size_request(s_txlvl, 90, -1);
	g_signal_connect(s_txlvl, "value-changed", G_CALLBACK(on_txlvl), NULL);
	s_vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 200, 10);
	gtk_scale_set_draw_value(GTK_SCALE(s_vol), FALSE); gtk_widget_set_size_request(s_vol, 90, -1);
	g_signal_connect(s_vol, "value-changed", G_CALLBACK(on_vol), NULL);
	building = 1;
	gtk_range_set_value(GTK_RANGE(s_txlvl), cfg.txlevel);
	gtk_range_set_value(GTK_RANGE(s_vol), cfg.spkvol);
	building = 0;
	GtkWidget *w1[] = {b_start, c_mode, b_ptt, b_tune, b_test, k_sq, k_clip};
	for (unsigned i = 0; i < sizeof(w1) / sizeof(w1[0]); i++) gtk_box_pack_start(GTK_BOX(tb), w1[i], FALSE, FALSE, 0);
	GtkWidget *sv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(sv), l_txlvl, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(sv), s_txlvl, FALSE, FALSE, 0);
	GtkWidget *sv2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(sv2), l_vol, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(sv2), s_vol, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tb), sv, FALSE, FALSE, 4); gtk_box_pack_start(GTK_BOX(tb), sv2, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);
	{ char t[40]; sprintf(t, "TX level %d%%", cfg.txlevel); gtk_label_set_text(GTK_LABEL(l_txlvl), t);
	  sprintf(t, "Speaker %d%%", cfg.spkvol); gtk_label_set_text(GTK_LABEL(l_vol), t); }

	// main area
	GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_container_set_border_width(GTK_CONTAINER(hb), 3);
	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	w_spec = gtk_drawing_area_new(); gtk_widget_set_size_request(w_spec, 420, 120);
	w_wf = gtk_drawing_area_new(); gtk_widget_set_size_request(w_wf, 420, 110);
	w_hist = gtk_drawing_area_new(); gtk_widget_set_size_request(w_hist, 420, 70);
	g_signal_connect(w_spec, "draw", G_CALLBACK(draw_spec), NULL);
	g_signal_connect(w_wf, "draw", G_CALLBACK(draw_wf), NULL);
	g_signal_connect(w_hist, "draw", G_CALLBACK(draw_hist), NULL);
	gtk_box_pack_start(GTK_BOX(left), w_spec, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(left), w_wf, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(left), w_hist, FALSE, FALSE, 0);
	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	w_sc = gtk_drawing_area_new(); gtk_widget_set_size_request(w_sc, 250, 120);
	w_panel = gtk_drawing_area_new(); gtk_widget_set_size_request(w_panel, 250, 275);
	g_signal_connect(w_sc, "draw", G_CALLBACK(draw_sc), NULL);
	g_signal_connect(w_panel, "draw", G_CALLBACK(draw_panel), NULL);
	gtk_box_pack_start(GTK_BOX(right), w_sc, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(right), w_panel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hb), left, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hb), right, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hb, TRUE, TRUE, 0);

	w_status = gtk_label_new("");
	gtk_widget_set_name(w_status, "statusbar");
	gtk_label_set_xalign(GTK_LABEL(w_status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(w_status), PANGO_ELLIPSIZE_END);
	gtk_box_pack_end(GTK_BOX(vbox), w_status, FALSE, FALSE, 0);

	g_timeout_add(50, ui_tick, NULL);
	gtk_widget_show_all(win);
	if (cfg.fullscreen_start) gtk_window_fullscreen(GTK_WINDOW(win));
}

int main(int argc, char **argv){
	int auto_start = 0, auto_test = 0;
	const char *auto_wav = NULL, *snap = NULL;
	cfg_load();
	for (int i = 1; i < argc; i++){
		if (!strcmp(argv[i], "--test")) auto_test = 1;
		else if (!strcmp(argv[i], "--start")) auto_start = 1;
		else if (!strcmp(argv[i], "--rx-wav") && i + 1 < argc) auto_wav = argv[++i];
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc){
			for (int m = 0; m < M_COUNT; m++) if (!strcmp(argv[i + 1], mode_names[m])) cfg.mode = m;
			i++;
		}
		else if (!strcmp(argv[i], "--dev") && i + 1 < argc){            // all four devices (used for testing)
			snprintf(cfg.rxdev, 64, "%s", argv[i + 1]); snprintf(cfg.txdev, 64, "%s", argv[i + 1]);
			snprintf(cfg.micdev, 64, "%s", argv[i + 1]); snprintf(cfg.spkdev, 64, "%s", argv[i + 1]);
			i++;
		}
		else if (!strcmp(argv[i], "--fullscreen")) cfg.fullscreen_start = 1;
		else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc){ g_timeout_add_seconds(atoi(argv[i + 1]), (GSourceFunc)gtk_main_quit, NULL); i++; }
	}
	(void)snap;
	gtk_init(&argc, &argv);
	build_ui();
	if (auto_start || auto_test || auto_wav){
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_start), TRUE);
	}
	if (auto_test) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_test), TRUE);
	if (auto_wav){
		snprintf(wav_path, sizeof(wav_path), "%s", auto_wav);
		rx_wav_active = 1;
		pthread_create(&th_wav, NULL, wav_thread, NULL);
	}
	gtk_main();
	app_stop();
	return 0;
}
