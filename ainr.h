// ainr.h -- AI noise reduction (RNNoise) for the sBitx receive audio path.
//
// ainr_process() sits in the RX audio chain on the 96 kHz mono int32 speaker
// buffer. It downsamples to 48 kHz, runs RNNoise on a separate worker thread
// (so a slow frame can never stall the audio callback), upsamples, and mixes
// the cleaned audio with a time-aligned copy of the original according to
// level (0 = off, 20 = fully cleaned).
//
// It is always safe to call: until the model has loaded, or if the model file
// is missing, or whenever the worker falls behind, the original audio passes
// through untouched.
#ifndef AINR_H
#define AINR_H

#include <stdint.h>

// level: 0 = bypass (returns immediately), 1..20 = amount of RNNoise in the mix.
void ainr_process(int32_t *samples, int n_samples, int level);

// 1 once the model is loaded and active, 0 otherwise (not loaded / load failed).
int ainr_ready(void);

#endif
