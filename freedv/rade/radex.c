// radex.c -- thin wrapper that lets the sBitx use RADE (the latest FreeDV)
// without any RADE or Opus headers inside the radio's own source files.
//
// It is built into libradex.so by rade/build_rade.sh and loaded at run time
// by freedv_ext.c (dlopen). If the library is not there the radio simply
// reports that RADE is not installed; every other mode keeps working.
//
// Based on the RADE library's own examples (rade_tx_wav.c / rade_rx_wav.c,
// David Rowe, BSD 2-clause) and its public API (rade_api.h).
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "rade_api.h"
#include "rade_dsp.h"
#include "lpcnet.h"
#include "fargan.h"
#include "arch.h"
#include "cpu_support.h"

#define EXPORT __attribute__((visibility("default")))
#define NTF RADE_NB_TOTAL_FEATURES            // 36 floats per 10 ms speech frame
#define FRAME LPCNET_FRAME_SIZE               // 160 speech samples at 16 kHz = 10 ms

struct radex {
	struct rade *r;
	int v2;
	int arch;
	LPCNetEncState *enc;
	FARGANState far;
	int far_ready, cont_frames;
	float cont[5 * NTF];
	int n_feat_in, frames_per_mf, nin_max, n_tx_out;
	float *feat_tx, *feat_rx;
	RADE_COMP *tx_out, *rx_in;
	int feat_idx;
	float last_snr;
};

static int initialised = 0;

EXPORT int radex_version(void){ return 1; }

EXPORT void *radex_open(int v2){
	if (!initialised){ rade_initialize(); initialised = 1; }
	struct radex *x = calloc(1, sizeof(*x));
	if (!x) return NULL;
	x->v2 = v2;
	x->arch = opus_select_arch();
	x->enc = lpcnet_encoder_create();
	int flags = RADE_VERBOSE_0 | (v2 ? RADE_MODE_V2 : 0);
	char model[] = "";
	x->r = rade_open(model, flags);
	if (!x->enc || !x->r){
		if (x->enc) lpcnet_encoder_destroy(x->enc);
		if (x->r) rade_close(x->r);
		free(x);
		return NULL;
	}
	x->n_feat_in = rade_n_features_in_out(x->r);
	x->frames_per_mf = x->n_feat_in / NTF;
	x->nin_max = rade_nin_max(x->r);
	x->n_tx_out = rade_n_tx_out(x->r);
	x->feat_tx = calloc(x->n_feat_in, sizeof(float));
	x->feat_rx = calloc(x->n_feat_in, sizeof(float));
	x->tx_out = calloc(x->n_tx_out, sizeof(RADE_COMP));
	x->rx_in = calloc(x->nin_max, sizeof(RADE_COMP));
	fargan_init(&x->far);
	return x;
}

EXPORT void radex_close(void *h){
	struct radex *x = h;
	if (!x) return;
	rade_close(x->r);
	lpcnet_encoder_destroy(x->enc);
	free(x->feat_tx); free(x->feat_rx); free(x->tx_out); free(x->rx_in);
	free(x);
}

// ---- receive ---------------------------------------------------------------
// How many 8 kHz modem samples the next radex_rx() call needs.
EXPORT int radex_nin(void *h){ struct radex *x = h; return rade_nin(x->r); }

// in8: nin real samples at 8 kHz as 16-bit values. Writes decoded speech
// (16 kHz, 16-bit) into speech[] -- a multiple of 160 samples, often zero --
// and returns the count. speech[] must hold at least 16 * 160 samples.
EXPORT int radex_rx(void *h, const short *in8, int nin, short *speech){
	struct radex *x = h;
	if (nin > x->nin_max) nin = x->nin_max;
	for (int i = 0; i < nin; i++){
		x->rx_in[i].real = in8[i] * (2.0f / RADE_INT16_SCALE);   // real signal, see rade_api.h
		x->rx_in[i].imag = 0.0f;
	}
	float eoo[512];
	int has_eoo = 0;
	int n_out = rade_rx(x->r, x->feat_rx, &has_eoo, x->v2 ? NULL : eoo, x->rx_in);
	if (rade_sync(x->r)) x->last_snr = rade_snrdB_3k_est(x->r);
	int total = 0;
	if (n_out > 0){
		int frames = n_out / NTF;
		for (int f = 0; f < frames; f++){
			float *feat = &x->feat_rx[f * NTF];
			if (!x->far_ready){                        // FARGAN needs 5 frames to warm up
				memcpy(&x->cont[x->cont_frames * NTF], feat, NTF * sizeof(float));
				if (++x->cont_frames >= 5){
					float packed[5 * NB_FEATURES], zeros[FARGAN_CONT_SAMPLES];
					for (int i = 0; i < 5; i++)
						memcpy(&packed[i * NB_FEATURES], &x->cont[i * NTF], NB_FEATURES * sizeof(float));
					memset(zeros, 0, sizeof(zeros));
					fargan_cont(&x->far, zeros, packed);
					x->far_ready = 1;
				}
				continue;
			}
			if (total + FRAME > 16 * FRAME) break;
			float fpcm[FRAME];
			fargan_synthesize(&x->far, fpcm, feat);
			for (int s = 0; s < FRAME; s++){
				float v = fpcm[s] * 32768.0f;
				if (v > 32767.0f) v = 32767.0f;
				if (v < -32767.0f) v = -32767.0f;
				speech[total + s] = (short)floorf(v + 0.5f);
			}
			total += FRAME;
		}
	}
	return total;
}

EXPORT int radex_sync(void *h){ struct radex *x = h; return rade_sync(x->r); }
EXPORT float radex_snr(void *h){ struct radex *x = h; return x->last_snr; }
EXPORT float radex_foff(void *h){ struct radex *x = h; return rade_freq_offset(x->r); }

// ---- transmit --------------------------------------------------------------
EXPORT void radex_tx_reset(void *h){ struct radex *x = h; x->feat_idx = 0; }

// pcm16: exactly 160 speech samples at 16 kHz (10 ms). Returns 0 until a whole
// modem frame has been collected (V1: 12 speech frames = 120 ms), then writes
// the modem signal (8 kHz, real part, 16-bit) into out[] and returns its length.
// out[] must hold at least 4096 samples.
EXPORT int radex_tx(void *h, const short *pcm16, short *out){
	struct radex *x = h;
	opus_int16 pcm[FRAME];
	for (int i = 0; i < FRAME; i++) pcm[i] = pcm16[i];
	lpcnet_compute_single_frame_features(x->enc, pcm, &x->feat_tx[x->feat_idx * NTF], x->arch);
	if (++x->feat_idx < x->frames_per_mf) return 0;
	x->feat_idx = 0;
	int n = rade_tx(x->r, x->tx_out, x->feat_tx);
	if (n > 4096) n = 4096;
	for (int i = 0; i < n; i++){
		float v = x->tx_out[i].real * RADE_INT16_SCALE;
		if (v > 32767.0f) v = 32767.0f;
		if (v < -32767.0f) v = -32767.0f;
		out[i] = (short)floorf(v + 0.5f);
	}
	return n;
}
