// ainr.c -- AI noise reduction (RNNoise) for the sBitx RX audio path.
// See ainr.h for the interface.
//
// Data flow, per audio block (normally 1024 samples at 96 kHz):
//
//   speaker audio (96k) --lowpass+decimate--> 48k --> input ring
//                                                        |
//                                          worker thread: RNNoise, 480-sample frames
//                                                        |
//   mix <-- lowpass+interpolate <-- 48k <-- output ring (primed with silence)
//    ^
//    +-- original audio, delayed by exactly the same amount as the wet path
//
// The output ring starts with AINR_PRIME48 samples of silence, which gives the
// worker two blocks of slack: as long as it finishes each 10 ms frame within
// about 20 ms, nothing is ever missed. If it does fall behind, the missing
// stretch is filled with the (time-aligned) original audio and the matching
// late samples are discarded, so the wet path never drifts out of step with
// the dry path and the radio never goes silent or glitches.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include "rnnoise.h"
#include "ainr.h"

#define AINR_FRAME        480          // RNNoise frame: 10 ms at 48 kHz
#define AINR_MAX_BLOCK    4096         // largest 96 kHz block we accept
#define AINR_RING         32768        // power of two, 48 kHz samples
#define AINR_RING_MASK    (AINR_RING - 1)
#define AINR_PRIME48      1024         // silence pre-loaded into the output ring (~21 ms)
#define AINR_RNN_DELAY48  959          // RNNoise's own algorithmic delay at 48 kHz (~20 ms, measured)
#define AINR_TAPS         63           // anti-alias / anti-image lowpass, odd
#define AINR_FIR_GD       ((AINR_TAPS - 1) / 2)
// total delay of the wet path in 96 kHz samples: decimation FIR + silence
// prime + RNNoise delay + interpolation FIR. The dry path is delayed by the same.
#define AINR_DRY_DELAY    (2 * (AINR_FIR_GD) + 2 * (AINR_PRIME48 + AINR_RNN_DELAY48))
#define AINR_MAKEUP_MAX   3.0f         // most make-up gain (3.0 = +9.5 dB)
// RNNoise was trained on speech at normal levels. Weak speech (a distant
// station) looks like noise to it and is removed with the noise, so the audio
// is first brought to a steady level for the network, and the loudness is put
// back afterwards. Both gains move slowly, so nothing pumps.
#define AINR_TARGET_RMS   3000.0f      // wanted level at the network input (about -21 dBFS)
#define AINR_IN_GAIN_MAX  10.0f        // most pre-gain (+20 dB)
#define AINR_GHIST        32           // blocks of pre-gain history
#define AINR_XFADE        384          // 4 ms crossfade from live to cleaned audio
#define AINR_FLOOR_DB_STEP 0.9         // dB of original audio left in per level step
#define AINR_SCALE        (1.0f / 65536.0f)   // int32 audio -> RNNoise's +/-32768 range

static struct {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	int   started;
	volatile int model_ready;
	volatile int failed;
	volatile int stop;

	// shared between audio thread and worker (protected by lock)
	float    in_ring[AINR_RING];
	float    out_ring[AINR_RING];
	uint64_t in_wr, in_rd, out_wr, out_rd;
	uint64_t discard;        // late wet samples to throw away after an underrun
	unsigned gen;            // bumped on every reset so the worker drops stale work

	// audio-thread only
	int      active;
	int64_t  last_ms;
	float    fir[AINR_TAPS];
	float    dec_hist[AINR_TAPS - 1];
	float    int_hist[AINR_TAPS - 1];
	int32_t  dry[AINR_DRY_DELAY];
	int      dry_pos;
	unsigned long underruns, blocks;
	int64_t  last_report_ms;
	double   e_dry, e_mix, wet_floor;   // loudness make-up tracking
	float    gain;
	double   in_env;                    // input power tracked for the pre-gain
	float    in_gain;
	int      res_left, xf_left;         // resume: live audio while the cleaning path fills, then a short crossfade
	float    ghist[AINR_GHIST];         // pre-gain used for each recent block
	int      ghist_pos;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static int64_t now_ms(void){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void weights_path(char *buf, size_t len){
	const char *env = getenv("AINR_WEIGHTS");
	if (env && *env){
		snprintf(buf, len, "%s", env);
		return;
	}
	const char *home = getenv("HOME");
	snprintf(buf, len, "%s/sbitx/data/rnnoise_weights.bin", home ? home : ".");
}

// ---------------------------------------------------------------- worker --

static void *ainr_worker(void *arg){
	(void)arg;
	char path[512];
	weights_path(path, sizeof(path));

	RNNModel *model = rnnoise_model_from_filename(path);
	if (!model){
		printf("[AINR] cannot load RNNoise weights from %s -- AI noise reduction disabled\n", path);
		g.failed = 1;
		return NULL;
	}
	DenoiseState *st = rnnoise_create(model);
	if (!st){
		printf("[AINR] rnnoise_create failed -- AI noise reduction disabled\n");
		g.failed = 1;
		return NULL;
	}
	printf("[AINR] RNNoise model loaded from %s\n", path);
	g.model_ready = 1;

	float in[AINR_FRAME], out[AINR_FRAME];
	unsigned my_gen = 0;

	for (;;){
		unsigned gen_taken;
		pthread_mutex_lock(&g.lock);
		while (!g.stop && g.in_wr - g.in_rd < AINR_FRAME)
			pthread_cond_wait(&g.cond, &g.lock);
		if (g.stop){
			pthread_mutex_unlock(&g.lock);
			break;
		}
		for (int i = 0; i < AINR_FRAME; i++)
			in[i] = g.in_ring[(g.in_rd + i) & AINR_RING_MASK];
		g.in_rd += AINR_FRAME;
		gen_taken = g.gen;
		pthread_mutex_unlock(&g.lock);

		if (gen_taken != my_gen){
			// the audio side was reset: start RNNoise's memory fresh too
			rnnoise_init(st, model);
			my_gen = gen_taken;
		}

		rnnoise_process_frame(st, out, in);

		pthread_mutex_lock(&g.lock);
		if (g.gen == gen_taken){
			int start = 0;
			if (g.discard > 0){
				start = (g.discard < AINR_FRAME) ? (int)g.discard : AINR_FRAME;
				g.discard -= start;
			}
			for (int i = start; i < AINR_FRAME; i++)
				g.out_ring[(g.out_wr++) & AINR_RING_MASK] = out[i];
		}
		pthread_mutex_unlock(&g.lock);
	}

	rnnoise_destroy(st);
	rnnoise_model_free(model);
	return NULL;
}

// ------------------------------------------------------------ audio side --

static void design_fir(void){
	// Blackman-windowed sinc, cutoff 11 kHz at 96 kHz: well above any audio the
	// speaker path carries, well below the 24 kHz Nyquist of the 48 kHz stage.
	const double fc = 11000.0 / 96000.0;
	double sum = 0;
	for (int i = 0; i < AINR_TAPS; i++){
		double m = i - AINR_FIR_GD;
		double s = (m == 0) ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
		double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (AINR_TAPS - 1))
		              + 0.08 * cos(4.0 * M_PI * i / (AINR_TAPS - 1));
		g.fir[i] = (float)(s * w);
		sum += s * w;
	}
	for (int i = 0; i < AINR_TAPS; i++)
		g.fir[i] = (float)(g.fir[i] / sum);
}

// Called from the audio thread only.
static void ainr_reset(void){
	pthread_mutex_lock(&g.lock);
	g.gen++;
	g.in_rd = g.in_wr = 0;
	memset(g.out_ring, 0, sizeof(float) * AINR_PRIME48);
	g.out_rd = 0;
	g.out_wr = AINR_PRIME48;
	g.discard = 0;
	pthread_mutex_unlock(&g.lock);

	memset(g.dec_hist, 0, sizeof(g.dec_hist));
	memset(g.int_hist, 0, sizeof(g.int_hist));
	memset(g.dry, 0, sizeof(g.dry));
	g.dry_pos = 0;
	g.e_dry = g.e_mix = 0;
	g.wet_floor = 0;
	g.gain = 1.0f;
	g.in_env = 0;
	g.in_gain = 1.0f;
	for (int q = 0; q < AINR_GHIST; q++) g.ghist[q] = 1.0f;
	g.ghist_pos = 0;
	g.res_left = AINR_DRY_DELAY;     // the cleaned path needs this long to produce its first audio
	g.xf_left = 0;
}

int ainr_ready(void){
	return g.model_ready && g.active;
}

void ainr_process(int32_t *s, int n, int level){
	if (level <= 0){
		g.active = 0;
		return;
	}
	if (g.failed || n <= 0 || (n & 1) || n > AINR_MAX_BLOCK)
		return;

	if (!g.started){
		g.started = 1;
		design_fir();
		pthread_create(&g.thread, NULL, ainr_worker, NULL);
	}
	if (!g.model_ready)
		return;                  // still loading: audio passes through

	int64_t now = now_ms();
	if (!g.active || now - g.last_ms > 300){
		ainr_reset();            // just switched on, or audio paused (TX, other mode)
		g.active = 1;
	}
	g.last_ms = now;

	int half = n / 2;
	static int32_t live96[AINR_MAX_BLOCK];
	memcpy(live96, s, sizeof(int32_t) * n);
	static float xbuf[AINR_TAPS - 1 + AINR_MAX_BLOCK];
	static float x48[AINR_MAX_BLOCK / 2];
	static float w48[AINR_MAX_BLOCK / 2];
	static float ubuf[AINR_TAPS - 1 + AINR_MAX_BLOCK];
	static float wet96[AINR_MAX_BLOCK];
	static int32_t dry96[AINR_MAX_BLOCK];
	int i, k;

	// --- original audio, delayed to line up with the wet path -----------------
	for (i = 0; i < n; i++){
		dry96[i] = g.dry[g.dry_pos];
		g.dry[g.dry_pos] = s[i];
		if (++g.dry_pos >= AINR_DRY_DELAY)
			g.dry_pos = 0;
	}

	// --- decimate 96k -> 48k ------------------------------------------------
	memcpy(xbuf, g.dec_hist, sizeof(float) * (AINR_TAPS - 1));
	for (i = 0; i < n; i++)
		xbuf[AINR_TAPS - 1 + i] = (float)s[i] * AINR_SCALE;
	for (i = 0; i < half; i++){
		const float *p = &xbuf[2 * i];
		float acc = 0;
		for (k = 0; k < AINR_TAPS; k++)
			acc += g.fir[k] * p[AINR_TAPS - 1 - k];
		x48[i] = acc;
	}
	memcpy(g.dec_hist, &xbuf[n], sizeof(float) * (AINR_TAPS - 1));

	// --- steady input level for the network ---------------------------------
	{
		double pw = 0;
		for (i = 0; i < half; i++) pw += (double)x48[i] * x48[i];
		pw /= half;
		if (g.in_env == 0) g.in_env = pw;
		else if (pw > g.in_env) g.in_env += 0.3 * (pw - g.in_env);    // loud: follow quickly
		else g.in_env += 0.0015 * (pw - g.in_env);                    // quiet: about 7 s, so noise alone never raises the gain
		float want = AINR_TARGET_RMS / (float)sqrt(g.in_env + 1.0);
		if (want > AINR_IN_GAIN_MAX) want = AINR_IN_GAIN_MAX;
		if (want < 1.0f) want = 1.0f;
		float g0 = g.in_gain, g1;
		if (want < g0) g1 = g0 + 0.5f * (want - g0);       // too loud: come down quickly
		else           g1 = g0 + 0.02f * (want - g0);      // too quiet: rise slowly
		for (i = 0; i < half; i++)
			x48[i] *= g0 + (g1 - g0) * (float)(i + 1) / half;
		g.in_gain = g1;
		g.ghist[g.ghist_pos] = g1;
		g.ghist_pos = (g.ghist_pos + 1) % AINR_GHIST;
	}

	// --- hand to the worker, collect what it has finished -----------------------
	int missing = 0;
	pthread_mutex_lock(&g.lock);
	if (g.in_wr - g.in_rd + half > AINR_RING - AINR_FRAME){
		// worker is hopelessly behind (e.g. starved of CPU): drop the backlog
		pthread_mutex_unlock(&g.lock);
		ainr_reset();
		return;
	}
	for (i = 0; i < half; i++)
		g.in_ring[(g.in_wr++) & AINR_RING_MASK] = x48[i];
	uint64_t avail = g.out_wr - g.out_rd;
	int take = (avail >= (uint64_t)half) ? half : (int)avail;
	for (i = 0; i < take; i++)
		w48[i] = g.out_ring[(g.out_rd++) & AINR_RING_MASK];
	missing = half - take;
	if (missing > 0){
		for (i = take; i < half; i++)
			w48[i] = 0;
		g.discard += missing;     // these will arrive late; skip them when they do
	}
	pthread_cond_signal(&g.cond);
	pthread_mutex_unlock(&g.lock);

	g.blocks++;
	if (missing > 0){
		g.underruns++;
#ifdef AINR_DEBUG
		printf("[AINR] underrun: %d of %d samples missing\n", missing, half);
#endif
	}

	// --- interpolate 48k -> 96k ------------------------------------------------
	memcpy(ubuf, g.int_hist, sizeof(float) * (AINR_TAPS - 1));
	for (i = 0; i < half; i++){
		ubuf[AINR_TAPS - 1 + 2 * i]     = 2.0f * w48[i];
		ubuf[AINR_TAPS - 1 + 2 * i + 1] = 0.0f;
	}
	for (i = 0; i < n; i++){
		const float *p = &ubuf[i];
		float acc = 0;
		for (k = 0; k < AINR_TAPS; k++)
			acc += g.fir[k] * p[AINR_TAPS - 1 - k];
		wet96[i] = acc * 65536.0f;
	}
	memcpy(g.int_hist, &ubuf[n], sizeof(float) * (AINR_TAPS - 1));

	// Undo the pre-gain. The wet audio is about AINR_DRY_DELAY samples late, so
	// the gain that was applied to it is the one from that many samples ago.
	{
		int back = (AINR_DRY_DELAY + n / 2) / n;
		if (back >= AINR_GHIST) back = AINR_GHIST - 1;
		static float prev_inv = 1.0f;
		int idx = ((g.ghist_pos - 1 - back) % AINR_GHIST + AINR_GHIST) % AINR_GHIST;
		float inv = 1.0f / g.ghist[idx];
		for (i = 0; i < n; i++)
			wet96[i] *= prev_inv + (inv - prev_inv) * (float)(i + 1) / n;
		prev_inv = inv;
	}

	// A stretch the worker did not deliver in time: use the original there.
	if (missing > 0){
		int from = n - 2 * missing;
		if (from < 0) from = 0;
		for (i = from; i < n; i++)
			wet96[i] = (float)dry96[i];
	}

	// --- mix, then give back the loudness the noise reduction took away -------
	// The cleaned audio plus a little of the original, which keeps the sound
	// natural. The original is left in at -0.9 dB per level step, so level 10 is
	// 9 dB below it and level 20 is 18 dB below it. Speech stays at full level.
	double kdry = pow(10.0, -AINR_FLOOR_DB_STEP * level / 20.0);
	double norm = 1.0 / (1.0 + kdry);
	static int32_t mix96[AINR_MAX_BLOCK];
	double ed = 0, em = 0, ew = 0;
	for (i = 0; i < n; i++){
		double v = (wet96[i] + kdry * dry96[i]) * norm;
		mix96[i] = (int32_t)v;
		ed += (double)dry96[i] * dry96[i];
		em += v * v;
		ew += (double)wet96[i] * wet96[i];
	}
	ed /= n; em /= n; ew /= n;
	// Speech is present when the cleaned audio is well above its own noise floor
	// (the floor follows the quietest recent blocks and creeps up slowly).
	if (g.wet_floor == 0 || ew < g.wet_floor) g.wet_floor = ew;
	else g.wet_floor *= 1.0005;
	int speech = (ew > 4.0 * g.wet_floor) && (ew > 1.0e10);
	if (speech){
		g.e_dry += 0.05 * (ed - g.e_dry);
		g.e_mix += 0.05 * (em - g.e_mix);
		if (g.e_mix > 1.0){
			float target = (float)sqrt(g.e_dry / g.e_mix);
			if (target < 1.0f) target = 1.0f;
			if (target > AINR_MAKEUP_MAX) target = AINR_MAKEUP_MAX;
			g.gain += 0.03f * (target - g.gain);      // slow, so it never pumps
		}
	}
	else
		g.gain += 0.0005f * (1.0f - g.gain);          // noise only: drift back down very slowly
	for (i = 0; i < n; i++){
		double v = (double)mix96[i] * g.gain;
		// gentle limiter above 60% of full scale so the extra gain cannot clip
		const double knee = 1.2e9, top = 2.0e9;
		double a = fabs(v);
		if (a > knee) a = knee + (top - knee) * tanh((a - knee) / (top - knee));
		double outv = v < 0 ? -a : a;
		// Right after switching on, or coming back from transmit, the cleaned path is
		// still empty. Play the live audio meanwhile instead of 40 ms of silence, then
		// fade over to the cleaned audio.
		if (g.res_left > 0){
			outv = live96[i];
			if (--g.res_left == 0) g.xf_left = AINR_XFADE;
		}
		else if (g.xf_left > 0){
			double t = 1.0 - (double)g.xf_left / AINR_XFADE;
			outv = live96[i] * (1.0 - t) + outv * t;
			g.xf_left--;
		}
		s[i] = (int32_t)outv;
	}

	if (now - g.last_report_ms > 60000 && g.underruns){
		printf("[AINR] %lu of %lu blocks late in the last minute (original audio used for those)\n",
			g.underruns, g.blocks);
		g.underruns = 0;
		g.blocks = 0;
		g.last_report_ms = now;
	}
	else if (g.last_report_ms == 0)
		g.last_report_ms = now;
}
