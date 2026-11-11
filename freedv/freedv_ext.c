// freedv_ext.c -- FreeDV add-on for the sBitx. See freedv_ext.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <codec2/freedv_api.h>
#include <codec2/modem_stats.h>
#include "freedv_ext.h"

#define FIR_TAPS 121                 // anti-alias filter for the 96 kHz <-> 8 kHz conversion
#define DEC 12                       // 96000 / 8000
#define FIFO 16384                   // samples at 8 kHz, power of two

// Level of the modem signal handed to the transmitter. The SSB transmit path
// clips its input at 4% of full scale (voice_clip_level in sbitx.c) and the
// modem signal must never reach that clip: with the clipping option on, the
// codec2 peaks are about 16000, which this puts at about 60% of the clip level.
#define TX_SCALE_100 3000.0
static volatile double tx_scale_now = TX_SCALE_100;   // changed by the TX level setting
#define TX_SCALE tx_scale_now
// Target level (rms of 16-bit samples) the decoder is fed with.
#define RX_TARGET_RMS 3000.0

struct fifo { short buf[FIFO]; unsigned head, tail; };
static unsigned f_fill(struct fifo *f){ return f->head - f->tail; }
static void f_put(struct fifo *f, short v){
	if (f->head - f->tail >= FIFO - 1) f->tail++;
	f->buf[f->head++ & (FIFO - 1)] = v;
}
static int f_get(struct fifo *f, short *v){
	if (f->head == f->tail) return 0;
	*v = f->buf[f->tail++ & (FIFO - 1)];
	return 1;
}

struct chain {                         // one direction: 96k -> 8k -> codec -> 8k -> 96k
	float dec_hist[FIR_TAPS];          // decimator history (newest first)
	int dec_phase;
	float int_hist[FIR_TAPS / DEC + 2];
	int int_phase;
	struct fifo in8, out8;
	int primed;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct freedv *fdv = NULL;
static int mode_id = -1;
static char mode_name[8] = "OFF";
static int n_speech, n_modem, n_max_modem, n_max_speech;
static struct chain rxc, txc;
static float fir[FIR_TAPS];
static int fir_ready = 0;
static volatile int sync_flag = 0;
static volatile float snr_est = 0.0f;
static double rx_level = 1.0e7;        // slow average of the incoming audio level (int32 rms)


// ---- setup values and what the screen shows --------------------------------
static char cfg_call[16] = "";
static int cfg_tx_pct = 100, cfg_clip = 1, cfg_loaded = 0;
static float cfg_squelch = -2.0f;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static struct freedv_ext_stats snap;
static volatile int stats_req = 0;
static char rx_text[64];
static volatile float tx_db_now = -99.0f, rx_db_now = -99.0f;
static const char *tx_call_ptr = NULL;
static float radex_foff_val = 0.0f;

static void cfg_path(char *p, size_t n){
	const char *h = getenv("HOME");
	snprintf(p, n, "%s/sbitx/data/freedv_setup.txt", h ? h : ".");
}
static void cfg_save(void){
	char p[300]; cfg_path(p, sizeof(p));
	FILE *f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "callsign=%s\ntxlevel=%d\nsquelch=%.1f\nclip=%d\n", cfg_call, cfg_tx_pct, cfg_squelch, cfg_clip);
	fclose(f);
}
static void cfg_load(void){
	if (cfg_loaded) return;
	cfg_loaded = 1;
	char p[300], ln[100]; cfg_path(p, sizeof(p));
	FILE *f = fopen(p, "r");
	if (!f) return;
	while (fgets(ln, sizeof(ln), f)){
		if (!strncmp(ln, "callsign=", 9)){
			ln[strcspn(ln, "\r\n")] = 0;
			strncpy(cfg_call, ln + 9, sizeof(cfg_call) - 1);
		}
		else if (!strncmp(ln, "txlevel=", 8)) cfg_tx_pct = atoi(ln + 8);
		else if (!strncmp(ln, "squelch=", 8)) cfg_squelch = (float)atof(ln + 8);
		else if (!strncmp(ln, "clip=", 5)) cfg_clip = atoi(ln + 5);
	}
	fclose(f);
	if (cfg_tx_pct < 20 || cfg_tx_pct > 100) cfg_tx_pct = 100;
	tx_scale_now = TX_SCALE_100 * cfg_tx_pct / 100.0;
}

// FreeDV text: one character at a time in each direction
static void rx_txt_cb(void *st, char c){
	(void)st;
	if (c < 32 || c > 126) return;
	size_t n = strlen(rx_text);
	if (n >= sizeof(rx_text) - 1){ memmove(rx_text, rx_text + 1, n - 1); n--; }
	rx_text[n] = c; rx_text[n + 1] = 0;
}
static char tx_txt_cb(void *st){
	(void)st;
	static int pos = 0;
	size_t n = strlen(cfg_call);
	if (!n) return ' ';
	if (pos >= (int)n + 1) pos = 0;
	char c = (pos < (int)n) ? cfg_call[pos] : ' ';
	pos++;
	return c;
}

static float to_db(double v, double full){ return (float)(20.0 * log10((v + 1.0) / full)); }

void freedv_ext_set_callsign(const char *call){
	cfg_load();
	int j = 0;
	for (int i = 0; call && call[i] && j < 15; i++){
		char c = call[i];
		if (c >= 'a' && c <= 'z') c -= 32;
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '/' || c == '-') cfg_call[j++] = c;
	}
	cfg_call[j] = 0;
	cfg_save();
}
const char *freedv_ext_get_callsign(void){ cfg_load(); return cfg_call; }
void freedv_ext_set_tx_level(int pct){
	cfg_load();
	if (pct < 20) pct = 20;
	if (pct > 100) pct = 100;
	cfg_tx_pct = pct;
	tx_scale_now = TX_SCALE_100 * pct / 100.0;
	cfg_save();
}
int freedv_ext_get_tx_level(void){ cfg_load(); return cfg_tx_pct; }
void freedv_ext_set_squelch(float db){
	cfg_load();
	if (db < -6) db = -6;
	if (db > 6) db = 6;
	cfg_squelch = db;
	cfg_save();
	pthread_mutex_lock(&lock);
	if (fdv) freedv_set_snr_squelch_thresh(fdv, db);
	pthread_mutex_unlock(&lock);
}
float freedv_ext_get_squelch(void){ cfg_load(); return cfg_squelch; }
void freedv_ext_set_clip(int on){
	cfg_load();
	cfg_clip = on ? 1 : 0;
	cfg_save();
	pthread_mutex_lock(&lock);
	if (fdv) freedv_set_clip(fdv, cfg_clip);
	pthread_mutex_unlock(&lock);
}
int freedv_ext_get_clip(void){ cfg_load(); return cfg_clip; }

// Called by the screen about ten times a second. The audio thread fills the
// snapshot (so the decoder is never touched from two threads at once).
void freedv_ext_get_stats(struct freedv_ext_stats *out){
	stats_req = 1;
	pthread_mutex_lock(&stats_lock);
	*out = snap;
	pthread_mutex_unlock(&stats_lock);
	out->active = freedv_ext_active();
	strncpy(out->mode, mode_name, sizeof(out->mode) - 1);
	out->mode[sizeof(out->mode) - 1] = 0;
	out->sync = sync_flag;
	out->snr = snr_est;
	out->rx_db = rx_db_now;
	out->tx_db = tx_db_now;
}

// audio thread: fill the snapshot from the codec2 modem
static void take_snapshot(void){
	static struct MODEM_STATS ms;                    // large: kept off the stack
	struct freedv_ext_stats t;
	memset(&t, 0, sizeof(t));
	if (fdv){
		freedv_get_modem_extended_stats(fdv, &ms);
		t.foff = ms.foff;
		t.clock_ppm = ms.clock_offset;
		t.sync_metric = ms.sync_metric;
		t.has_text = 1;
		int nc = ms.Nc > 0 ? ms.Nc : 0, n = 0;
		double e = 0;
		for (int r = ms.nr - 1; r >= 0 && n < 400; r--)
			for (int c = 0; c < nc && n < 400; c++, n++){
				t.sx[n] = ms.rx_symbols[r][c].real;
				t.sy[n] = ms.rx_symbols[r][c].imag;
				e += (double)t.sx[n] * t.sx[n] + (double)t.sy[n] * t.sy[n];
			}
		t.nsym = n;
		if (n > 8){
			float k = (float)(1.0 / (2.0 * sqrt(e / n) + 1e-9));
			for (int i = 0; i < n; i++){ t.sx[i] *= k; t.sy[i] *= k; }
			t.has_scatter = 1;
		}
		strncpy(t.rxtext, rx_text, sizeof(t.rxtext) - 1);
	}
	else {
		t.foff = radex_foff_val;
	}
	pthread_mutex_lock(&stats_lock);
	snap = t;
	pthread_mutex_unlock(&stats_lock);
}

static void fir_init(void){
	if (fir_ready) return;
	double fc = 3600.0 / 96000.0, sum = 0.0;
	for (int i = 0; i < FIR_TAPS; i++){
		double m = i - (FIR_TAPS - 1) / 2.0;
		double s = (m == 0) ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
		double w = 0.54 - 0.46 * cos(2 * M_PI * i / (FIR_TAPS - 1));
		fir[i] = (float)(s * w);
		sum += fir[i];
	}
	for (int i = 0; i < FIR_TAPS; i++) fir[i] /= sum;     // unity gain at DC
	fir_ready = 1;
}

// 96 kHz int32 -> 8 kHz shorts (scale: int32 units per short unit)
static void decimate(struct chain *c, const int32_t *x, int n, double scale, struct fifo *out){
	for (int i = 0; i < n; i++){
		memmove(c->dec_hist + 1, c->dec_hist, (FIR_TAPS - 1) * sizeof(float));
		c->dec_hist[0] = (float)(x[i] / scale);
		if (++c->dec_phase >= DEC){
			c->dec_phase = 0;
			float acc = 0;
			for (int k = 0; k < FIR_TAPS; k++) acc += fir[k] * c->dec_hist[k];
			if (acc > 32767) acc = 32767;
			if (acc < -32768) acc = -32768;
			f_put(out, (short)acc);
		}
	}
}

// 8 kHz shorts -> 96 kHz int32. Plays silence until about 20 ms are buffered
// (and again after an underrun) so the output is never choppy.
static void interpolate(struct chain *c, struct fifo *in, int32_t *y, int n, double scale){
	int ninhist = FIR_TAPS / DEC + 1;
	if (!c->primed && f_fill(in) >= 160)
		c->primed = 1;
	for (int i = 0; i < n; i++){
		if (c->int_phase == 0){
			short v = 0;
			if (c->primed && !f_get(in, &v)){
				c->primed = 0;                 // ran dry: rebuffer
				v = 0;
			}
			memmove(c->int_hist + 1, c->int_hist, (ninhist - 1) * sizeof(float));
			c->int_hist[0] = (float)v;
		}
		float acc = 0;
		for (int j = 0; j < ninhist; j++){
			int k = c->int_phase + DEC * j;
			if (k < FIR_TAPS) acc += fir[k] * c->int_hist[j];
		}
		double o = acc * DEC * scale;
		if (o > 2.0e9) o = 2.0e9;
		if (o < -2.0e9) o = -2.0e9;
		y[i] = (int32_t)o;
		if (++c->int_phase >= DEC) c->int_phase = 0;
	}
}

static int mode_from_name(const char *name){
	if (!strcmp(name, "700D")) return FREEDV_MODE_700D;
	if (!strcmp(name, "700E")) return FREEDV_MODE_700E;
	if (!strcmp(name, "1600")) return FREEDV_MODE_1600;
	return -1;
}


// ============================================================================
// RADE -- the Radio Autoencoder, the latest FreeDV (neural-network speech
// coding, much better than 700D/700E in weak signals). The RADE and Opus code
// lives in libradex.so (built by rade/build_rade.sh from the open-source RADE
// library) and is loaded here at run time, so the radio builds and runs
// exactly as before when it is not installed.
//
// The neural networks are too slow to run inside the audio callback, so a
// worker thread does the coding: the callback only converts between the
// radio's 96 kHz audio and 8 kHz FIFOs, and the thread works on the FIFOs.
// ============================================================================
#define RADE_RX_TARGET_RMS 3000.0     // level the RADE receiver is fed with (16-bit rms)
#define RADE_SPK_SCALE 24000.0        // decoded speech: 16-bit -> radio audio level

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
	float (*foff)(void *h);              // optional (older libradex.so may lack it)
} rx_lib;

static void *rade_h = NULL;
static pthread_t rade_th;
static volatile int rade_run = 0;
static volatile int rade_on = 0;
static volatile int rade_tx_reset_req = 0;

// 2:1 filter (16 kHz <-> 8 kHz), 31 taps
#define HB 31
static float hb[HB];
static void hb_init(void){
	double sum = 0;
	for (int i = 0; i < HB; i++){
		double m = i - (HB - 1) / 2.0, fc = 3600.0 / 16000.0;
		double s = (m == 0) ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
		hb[i] = (float)(s * (0.54 - 0.46 * cos(2 * M_PI * i / (HB - 1))));
		sum += hb[i];
	}
	for (int i = 0; i < HB; i++) hb[i] /= sum;
}
struct hbstate { float h[HB]; int ph; };
static float hb_push(struct hbstate *st, float v){
	memmove(st->h + 1, st->h, (HB - 1) * sizeof(float));
	st->h[0] = v;
	float a = 0;
	for (int k = 0; k < HB; k++) a += hb[k] * st->h[k];
	return a;
}

static void *rade_worker(void *arg){
	(void)arg;
	static short in8[1024], speech[16 * 160], modem[4096];
	struct hbstate down = {{0}, 0}, up = {{0}, 0};
	short pcm16[160];
	int npcm = 0;
	while (rade_run){
		int did = 0;
		// receive: modem audio (8 kHz) -> speech
		for (int guard = 0; guard < 8; guard++){
			int nin = rx_lib.nin(rade_h);
			if (nin <= 0 || nin > 1024 || f_fill(&rxc.in8) < (unsigned)nin) break;
			for (int i = 0; i < nin; i++){ short v = 0; f_get(&rxc.in8, &v); in8[i] = v; }
			int n = rx_lib.rx(rade_h, in8, nin, speech);
			for (int i = 0; i < n; i++){
				float y = hb_push(&down, speech[i]);
				if (down.ph ^= 1) continue;               // keep every second sample
				if (y > 32767) y = 32767;
				if (y < -32768) y = -32768;
				f_put(&rxc.out8, (short)y);
			}
			sync_flag = rx_lib.sync(rade_h);
			snr_est = rx_lib.snr(rade_h);
			if (rx_lib.foff) radex_foff_val = rx_lib.foff(rade_h);
			did = 1;
		}
		// transmit: microphone (8 kHz) -> speech frames (16 kHz) -> modem audio
		if (rade_tx_reset_req){
			rade_tx_reset_req = 0;
			rx_lib.tx_reset(rade_h);
			npcm = 0;
			memset(&up, 0, sizeof(up));
		}
		while (f_fill(&txc.in8) >= 80){
			for (int i = 0; i < 80; i++){
				short v = 0; f_get(&txc.in8, &v);
				float a = 2.0f * hb_push(&up, v);         // sample, then a zero: 8 -> 16 kHz
				float b = 2.0f * hb_push(&up, 0);
				pcm16[npcm++] = (short)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
				pcm16[npcm++] = (short)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
			}
			npcm = 0;
			int n = rx_lib.tx(rade_h, pcm16, modem);
			for (int i = 0; i < n; i++) f_put(&txc.out8, modem[i]);
			did = 1;
		}
		if (!did) usleep(1500);
	}
	return NULL;
}

static void rade_stop(void){
	if (!rade_on) return;
	rade_on = 0;
	rade_run = 0;
	pthread_join(rade_th, NULL);
	rx_lib.close(rade_h);
	rade_h = NULL;
}

static int rade_start(int v2){
	if (!rx_lib.lib){
		const char *paths[] = {"./libradex.so", "./rade/libradex.so", "libradex.so", NULL};
		char home[300];
		const char *h = getenv("HOME");
		for (int i = 0; paths[i] && !rx_lib.lib; i++)
			rx_lib.lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
		if (!rx_lib.lib && h){
			snprintf(home, sizeof(home), "%s/sbitx/rade/libradex.so", h);
			rx_lib.lib = dlopen(home, RTLD_NOW | RTLD_LOCAL);
		}
		if (!rx_lib.lib){
			printf("[RADE] libradex.so not found: run rade/build_rade.sh (%s)\n", dlerror());
			return 0;
		}
		rx_lib.open = dlsym(rx_lib.lib, "radex_open");
		rx_lib.close = dlsym(rx_lib.lib, "radex_close");
		rx_lib.nin = dlsym(rx_lib.lib, "radex_nin");
		rx_lib.rx = dlsym(rx_lib.lib, "radex_rx");
		rx_lib.sync = dlsym(rx_lib.lib, "radex_sync");
		rx_lib.snr = dlsym(rx_lib.lib, "radex_snr");
		rx_lib.tx_reset = dlsym(rx_lib.lib, "radex_tx_reset");
		rx_lib.tx = dlsym(rx_lib.lib, "radex_tx");
		rx_lib.foff = dlsym(rx_lib.lib, "radex_foff");
		if (!rx_lib.open || !rx_lib.close || !rx_lib.nin || !rx_lib.rx || !rx_lib.sync ||
			!rx_lib.snr || !rx_lib.tx_reset || !rx_lib.tx){
			printf("[RADE] libradex.so is incomplete\n");
			dlclose(rx_lib.lib); rx_lib.lib = NULL;
			return 0;
		}
		hb_init();
	}
	rade_stop();
	void *nh = rx_lib.open(v2);
	if (!nh){ printf("[RADE] could not open the RADE codec\n"); return 0; }

	pthread_mutex_lock(&lock);
	struct freedv *old = fdv;                         // leave any codec2 mode
	fdv = NULL;
	mode_id = -1;
	memset(&rxc, 0, sizeof(rxc));
	memset(&txc, 0, sizeof(txc));
	sync_flag = 0; snr_est = 0;
	strncpy(mode_name, v2 ? "RAD2" : "RADE", sizeof(mode_name) - 1);
	rade_h = nh;
	rade_run = 1;
	pthread_mutex_unlock(&lock);
	if (old) freedv_close(old);

	if (pthread_create(&rade_th, NULL, rade_worker, NULL)){
		rade_run = 0; rx_lib.close(rade_h); rade_h = NULL;
		return 0;
	}
	rade_on = 1;
	printf("[RADE] %s started\n", mode_name);
	return 1;
}

// receive-side callback for RADE (see freedv_ext_rx)
static void rade_rx_cb(int32_t *audio, int n){
	double e = 0;
	for (int i = 0; i < n; i++) e += (double)audio[i] * audio[i];
	double rms = sqrt(e / n) + 1.0;
	if (rms > rx_level) rx_level += (rms - rx_level) * 0.2;
	else rx_level += (rms - rx_level) * 0.01;
	decimate(&rxc, audio, n, rx_level / RADE_RX_TARGET_RMS, &rxc.in8);
	rx_db_now = to_db(rx_level, 1.07e9);
	if (stats_req){ stats_req = 0; take_snapshot(); }
	interpolate(&rxc, &rxc.out8, audio, n, RADE_SPK_SCALE);
}

static void rade_tx_cb(int32_t *mic, int n){
	{ double e = 0; for (int i = 0; i < n; i++) e += (double)mic[i] * mic[i];
	  tx_db_now = to_db(sqrt(e / n), 1.07e9); }
	decimate(&txc, mic, n, 32768.0, &txc.in8);
	interpolate(&txc, &txc.out8, mic, n, TX_SCALE);
}

int freedv_ext_set_mode(const char *name){
	fir_init();
	if (!strcmp(name, "RADE")) return rade_start(0);
	if (!strcmp(name, "RAD2")) return rade_start(1);
	rade_stop();
	int id = mode_from_name(name);
	struct freedv *nf = NULL;

	if (id >= 0){
		nf = freedv_open(id);                  // done outside the lock: it takes a moment
		if (!nf){
			printf("[FreeDV] could not open mode %s\n", name);
			return 0;
		}
		freedv_set_verbose(nf, 0);
		cfg_load();
		freedv_set_clip(nf, cfg_clip);         // keeps the peak-to-average ratio of the transmit signal low
		freedv_set_tx_bpf(nf, 1);
		freedv_set_squelch_en(nf, 1);
		freedv_set_snr_squelch_thresh(nf, cfg_squelch);
		freedv_set_callback_txt(nf, rx_txt_cb, tx_txt_cb, NULL);   // callsign text, both ways
		rx_text[0] = 0;
	}

	pthread_mutex_lock(&lock);
	struct freedv *old = fdv;
	fdv = nf;
	mode_id = id;
	strncpy(mode_name, id >= 0 ? name : "OFF", sizeof(mode_name) - 1);
	if (nf){
		n_speech = freedv_get_n_speech_samples(nf);
		n_modem = freedv_get_n_nom_modem_samples(nf);
		n_max_modem = freedv_get_n_max_modem_samples(nf);
		n_max_speech = freedv_get_n_max_speech_samples(nf);
	}
	memset(&rxc, 0, sizeof(rxc));
	memset(&txc, 0, sizeof(txc));
	sync_flag = 0;
	snr_est = 0;
	pthread_mutex_unlock(&lock);

	if (old)
		freedv_close(old);
	if (nf)
		printf("[FreeDV] mode %s: %d speech / %d modem samples per frame at 8 kHz\n",
			name, n_speech, n_modem);
	return 1;
}

int freedv_ext_active(void){ return fdv != NULL || rade_on; }
int freedv_ext_sync(void){ return sync_flag; }
float freedv_ext_snr(void){ return snr_est; }
const char *freedv_ext_mode_name(void){ return mode_name; }

void freedv_ext_tx_start(void){
	if (rade_on){
		// the worker thread owns the FIFOs: ask it to start clean
		txc.in8.tail = txc.in8.head;
		txc.out8.tail = txc.out8.head;
		txc.primed = 0;
		rade_tx_reset_req = 1;
		return;
	}
	pthread_mutex_lock(&lock);
	memset(&txc, 0, sizeof(txc));
	pthread_mutex_unlock(&lock);
}

void freedv_ext_rx(int32_t *audio, int n){
	if (rade_on){ rade_rx_cb(audio, n); return; }
	if (pthread_mutex_trylock(&lock))
		return;                                  // being switched: leave the audio alone this block
	if (!fdv){ pthread_mutex_unlock(&lock); return; }

	// slow level tracking, so the decoder always sees about the same level
	double e = 0;
	for (int i = 0; i < n; i++) e += (double)audio[i] * audio[i];
	double rms = sqrt(e / n) + 1.0;
	if (rms > rx_level) rx_level += (rms - rx_level) * 0.2;
	else rx_level += (rms - rx_level) * 0.01;
	double in_scale = rx_level / RX_TARGET_RMS;
	double out_scale = rx_level / RX_TARGET_RMS;

	decimate(&rxc, audio, n, in_scale, &rxc.in8);

	static short modem_in[4096], speech_out[4096];
	for (;;){
		int nin = freedv_nin(fdv);
		if (nin > 4000 || f_fill(&rxc.in8) < (unsigned)nin)
			break;
		for (int i = 0; i < nin; i++){
			short v = 0; f_get(&rxc.in8, &v); modem_in[i] = v;
		}
		int nout = freedv_rx(fdv, speech_out, modem_in);
		for (int i = 0; i < nout; i++) f_put(&rxc.out8, speech_out[i]);
		int s; float snr;
		freedv_get_modem_stats(fdv, &s, &snr);
		sync_flag = s; snr_est = snr;
	}
	rx_db_now = to_db(rx_level, 1.07e9);
	if (stats_req){ stats_req = 0; take_snapshot(); }
	interpolate(&rxc, &rxc.out8, audio, n, out_scale);
	pthread_mutex_unlock(&lock);
}

void freedv_ext_tx(int32_t *mic, int n){
	if (rade_on){ rade_tx_cb(mic, n); return; }
	if (pthread_mutex_trylock(&lock)){
		memset(mic, 0, n * sizeof(int32_t));
		return;
	}
	if (!fdv){ pthread_mutex_unlock(&lock); return; }

	{ double e = 0; for (int i = 0; i < n; i++) e += (double)mic[i] * mic[i];
	  tx_db_now = to_db(sqrt(e / n), 1.07e9); }
	decimate(&txc, mic, n, 32768.0, &txc.in8);     // full-scale mic (about 1.07e9) -> 16-bit

	static short speech_in[4096], mod_out[8192];
	while (f_fill(&txc.in8) >= (unsigned)n_speech && n_speech <= 4096 && n_max_modem <= 8192){
		for (int i = 0; i < n_speech; i++){
			short v = 0; f_get(&txc.in8, &v); speech_in[i] = v;
		}
		freedv_tx(fdv, mod_out, speech_in);
		for (int i = 0; i < n_modem; i++) f_put(&txc.out8, mod_out[i]);
	}
	interpolate(&txc, &txc.out8, mic, n, TX_SCALE);
	pthread_mutex_unlock(&lock);
}
