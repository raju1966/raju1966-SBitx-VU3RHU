// freedv_ext.h -- FreeDV digital voice (codec2) as an add-on for the sBitx.
//
// FreeDV turns speech into a digital modem signal that is sent through the
// radio's ordinary SSB transmitter and receiver, so this module only sits
// around the audio: on receive it takes the demodulated 96 kHz audio from the
// receiver, decodes it and puts speech back in its place; on transmit it takes
// the microphone audio, encodes it and hands the modem tones to the SSB
// transmitter in place of the microphone. Nothing in the sBitx's RF chain is
// changed. Uses the open-source codec2 library (libcodec2).
#ifndef FREEDV_EXT_H
#define FREEDV_EXT_H
#include <stdint.h>

// name: "OFF", "700D", "700E" or "1600". Returns 1 on success.
int freedv_ext_set_mode(const char *name);
int freedv_ext_active(void);                  // 1 while a FreeDV mode is on

// Receive: replaces the audio in place with decoded speech.
void freedv_ext_rx(int32_t *audio96k, int n_samples);
// Transmit: replaces the microphone audio in place with the modem signal.
void freedv_ext_tx(int32_t *mic96k, int n_samples);
// Call once when the radio goes to transmit, so no old audio is sent.
void freedv_ext_tx_start(void);

int freedv_ext_sync(void);                    // 1 when the receiver has locked on
float freedv_ext_snr(void);                   // estimated SNR in dB (3 kHz bandwidth)
const char *freedv_ext_mode_name(void);

// ---- data for the FreeDV window on the screen -----------------------------
struct freedv_ext_stats {
	int active;               // a FreeDV mode is on
	char mode[8];             // "700D" ... "RADE"
	int sync;                 // receiver locked on
	float snr;                // dB (3 kHz)
	float foff;               // frequency offset, Hz
	float clock_ppm;          // sample-clock difference, ppm (codec2 modes)
	float sync_metric;        // 0..1 (codec2 modes)
	int has_scatter;          // 1 when sx/sy hold a constellation
	int nsym;                 // points in sx/sy
	float sx[400], sy[400];   // constellation, scaled to about +/-1
	int has_text;             // the mode can carry text (not RADE)
	char rxtext[64];          // received text (callsign) -- latest characters
	float rx_db, tx_db;       // audio level into the decoder / from the microphone, dB
};
void freedv_ext_get_stats(struct freedv_ext_stats *s);

// ---- setup (saved in data/freedv_setup.txt) -------------------------------
void freedv_ext_set_callsign(const char *call);   // sent as text, up to 15 characters
const char *freedv_ext_get_callsign(void);
void freedv_ext_set_tx_level(int percent);        // 20..100, 100 = the normal drive
int freedv_ext_get_tx_level(void);
void freedv_ext_set_squelch(float snr_db);        // codec2 modes: mute below this SNR
float freedv_ext_get_squelch(void);
void freedv_ext_set_clip(int on);                 // codec2 modes: peak clipping of the transmit signal
int freedv_ext_get_clip(void);
#endif
