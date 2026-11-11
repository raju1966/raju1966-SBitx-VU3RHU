// usbaudio.h -- optional USB speaker and USB microphone for the sBitx.
//
// An add-on: the stock codec path is left exactly as it is. With the USB
// speaker on, the received audio is ALSO played on a USB sound device; with
// the USB microphone on, the transmit (and VOX) audio comes from a USB sound
// device instead of the radio's own mic input. Each side runs on its own
// thread with a small buffer, so a slow or unplugged USB device can never
// stall the radio's audio.
#ifndef USBAUDIO_H
#define USBAUDIO_H
#include <stdint.h>

// 1 = turn on, 0 = off. Returns 1 if the device opened (or it is off), 0 if it
// could not be opened (the radio carries on normally without it).
int usb_speaker_enable(int on);
int usb_mic_enable(int on);

int usb_speaker_active(void);    // 1 while audio is really going to the USB speaker
int usb_mic_active(void);        // 1 while audio is really coming from the USB mic

// Called from the audio callback. 96 kHz mono int32, the radio's internal format.
// gain_x1000: speaker gain in thousandths (1000 = unity).
void usb_speaker_write(const int32_t *samples, int n_samples, int gain_x1000);
// Replaces samples[] with the USB mic audio (zeros if it is not arriving).
void usb_mic_read(int32_t *samples, int n_samples);

// Name of the device found, for messages ("" if none).
const char *usb_audio_device_name(void);
#endif
