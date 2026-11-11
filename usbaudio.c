#define _GNU_SOURCE
// usbaudio.c -- optional USB speaker and USB microphone. See usbaudio.h.
//
// Device choice: the first ALSA card whose name contains "USB" (from
// /proc/asound/cards) is used for both. To pick other devices, create
// $HOME/sbitx/data/usb_audio.txt with lines such as
//      speaker=plughw:2,0
//      mic=plughw:3,0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <alsa/asoundlib.h>
#include "usbaudio.h"

#define RING 16384                       // samples at 48 kHz, power of two
#define CHUNK 480                        // 10 ms at 48 kHz

struct ring {
	int32_t buf[RING];
	atomic_uint head, tail;              // head: written by producer, tail: by consumer
};

static struct ring spk_ring, mic_ring;
static pthread_t spk_thread, mic_thread;
static volatile int spk_run = 0, mic_run = 0, spk_ok = 0, mic_ok = 0;
static char dev_name[64] = "";
static char spk_dev[64], mic_dev[64];

static unsigned ring_fill(struct ring *r){
	return atomic_load(&r->head) - atomic_load(&r->tail);
}
static void ring_put(struct ring *r, int32_t v){
	unsigned h = atomic_load(&r->head);
	if (h - atomic_load(&r->tail) >= RING - 1)
		atomic_fetch_add(&r->tail, 1);    // full: drop the oldest
	r->buf[h & (RING - 1)] = v;
	atomic_store(&r->head, h + 1);
}
static int ring_get(struct ring *r, int32_t *v){
	unsigned t = atomic_load(&r->tail);
	if (t == atomic_load(&r->head))
		return 0;
	*v = r->buf[t & (RING - 1)];
	atomic_store(&r->tail, t + 1);
	return 1;
}

static void find_devices(void){
	char line[256];
	FILE *f;
	spk_dev[0] = mic_dev[0] = 0;

	f = fopen("/proc/asound/cards", "r");
	if (f){
		while (fgets(line, sizeof(line), f)){
			int idx;
			// lines look like " 2 [Device         ]: USB-Audio - USB PnP Sound Device"
			// a USB sound device is recognised by its name containing "USB", or
			// by the usbid file the kernel creates only for USB audio cards
			char usbid[80];
			int is_usb = 0;
			if (sscanf(line, " %d [", &idx) == 1){
				snprintf(usbid, sizeof(usbid), "/proc/asound/card%d/usbid", idx);
				is_usb = strstr(line, "USB") != NULL || access(usbid, R_OK) == 0;
			}
			if (is_usb){
				snprintf(spk_dev, sizeof(spk_dev), "plughw:%d,0", idx);
				snprintf(mic_dev, sizeof(mic_dev), "plughw:%d,0", idx);
				char *p = strstr(line, "]:");
				snprintf(dev_name, sizeof(dev_name), "%s", p ? p + 3 : "USB audio");
				dev_name[strcspn(dev_name, "\r\n")] = 0;
				break;
			}
		}
		fclose(f);
	}

	char path[300];
	snprintf(path, sizeof(path), "%s/sbitx/data/usb_audio.txt", getenv("HOME") ? getenv("HOME") : ".");
	f = fopen(path, "r");
	if (f){
		while (fgets(line, sizeof(line), f)){
			line[strcspn(line, "\r\n")] = 0;
			if (!strncmp(line, "speaker=", 8)) snprintf(spk_dev, sizeof(spk_dev), "%s", line + 8);
			if (!strncmp(line, "mic=", 4)) snprintf(mic_dev, sizeof(mic_dev), "%s", line + 4);
		}
		fclose(f);
	}
}

// The Pi desktop's sound server (PulseAudio) grabs a USB sound card the moment it
// is plugged in, which makes the radio's own open fail with "busy". Suspending
// the USB sinks/sources makes it let go of the card.
static void release_desktop_audio(snd_pcm_stream_t stream){
	if (stream == SND_PCM_STREAM_PLAYBACK)
		system("for s in $(pactl list short sinks 2>/dev/null | awk '{print $2}' | grep -i usb); do pactl suspend-sink $s 1; done >/dev/null 2>&1");
	else
		system("for s in $(pactl list short sources 2>/dev/null | awk '{print $2}' | grep -i usb); do pactl suspend-source $s 1; done >/dev/null 2>&1");
}

static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t stream, int channels){
	snd_pcm_t *h;
	// A USB card that has just been plugged in, or has gone to sleep, often
	// refuses the first open; keep trying for about 3 seconds.
	int e = -1;
	for (int tries = 0; tries < 12; tries++){
		e = snd_pcm_open(&h, dev, stream, 0);
		if (e >= 0 || e == -ENOENT && tries >= 4)
			break;
		if (e == -EBUSY && (tries == 0 || tries == 6)){
			release_desktop_audio(stream);
			usleep(300000);
		}
		usleep(250000);
	}
	if (e < 0){
		printf("[USB] cannot open %s: %s%s\n", dev, snd_strerror(e),
			e == -EBUSY ? " (another program is using it, for example PulseAudio)" : "");
		return NULL;
	}
	// 48 kHz, 32-bit, interleaved; "plughw" converts to whatever the device really does
	if (snd_pcm_set_params(h, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
		channels, 48000, 1, 120000) < 0){
		printf("[USB] %s did not accept 48 kHz, %d channels\n", dev, channels);
		snd_pcm_close(h);
		return NULL;
	}
	return h;
}

// A USB sound card often starts muted or very quiet. Switch its playback on and
// set it to 90 % so the speaker is audible whatever its mixer was left at.
static void usb_card_unmute(const char *dev){
	int card = -1;
	const char *c = strchr(dev, ':');
	if (c) card = atoi(c + 1);
	if (card < 0) return;
	char name[24];
	snprintf(name, sizeof(name), "hw:%d", card);
	snd_mixer_t *m = NULL;
	if (snd_mixer_open(&m, 0) < 0) return;
	if (snd_mixer_attach(m, name) < 0 || snd_mixer_selem_register(m, NULL, NULL) < 0 || snd_mixer_load(m) < 0){
		snd_mixer_close(m);
		return;
	}
	for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)){
		if (!snd_mixer_selem_is_active(e) || !snd_mixer_selem_has_playback_volume(e) && !snd_mixer_selem_has_playback_switch(e))
			continue;
		// The microphone monitor ("Mic", "Sidetone", "Line", "Capture" playback)
		// plays the mic back through the speaker and causes whistling: keep it OFF.
		const char *en = snd_mixer_selem_get_name(e);
		int monitor = strcasestr(en, "mic") || strcasestr(en, "sidetone") ||
			strcasestr(en, "line") || strcasestr(en, "capture") || strcasestr(en, "aux");
		if (monitor){
			if (snd_mixer_selem_has_playback_switch(e))
				snd_mixer_selem_set_playback_switch_all(e, 0);
			if (snd_mixer_selem_has_playback_volume(e)){
				long mn, mx;
				snd_mixer_selem_get_playback_volume_range(e, &mn, &mx);
				snd_mixer_selem_set_playback_volume_all(e, mn);
			}
			printf("[USB] mixer control '%s' kept OFF (monitor)\n", en);
			continue;
		}
		if (snd_mixer_selem_has_playback_switch(e))
			snd_mixer_selem_set_playback_switch_all(e, 1);
		if (snd_mixer_selem_has_playback_volume(e)){
			long mn, mx;
			snd_mixer_selem_get_playback_volume_range(e, &mn, &mx);
			snd_mixer_selem_set_playback_volume_all(e, mn + (mx - mn) * 90 / 100);
		}
		printf("[USB] mixer control '%s' unmuted, 90%%\n", en);
	}
	snd_mixer_close(m);
}

static void *speaker_loop(void *arg){
	snd_pcm_t *h = open_pcm(spk_dev, SND_PCM_STREAM_PLAYBACK, 2);
	int32_t out[CHUNK * 2];
	if (!h){
		printf("[USB] could not open speaker %s\n", spk_dev);
		spk_run = 0;
		return NULL;
	}
	usb_card_unmute(spk_dev);
	printf("[USB] speaker on %s (%s)\n", spk_dev, dev_name);
	spk_ok = 1;
	while (spk_run){
		if (ring_fill(&spk_ring) < CHUNK){
			usleep(2000);
			continue;
		}
		for (int i = 0; i < CHUNK; i++){
			int32_t v = 0;
			ring_get(&spk_ring, &v);
			out[2*i] = out[2*i+1] = v;
		}
		snd_pcm_sframes_t n = snd_pcm_writei(h, out, CHUNK);
		if (n < 0)
			snd_pcm_recover(h, (int)n, 1);
	}
	spk_ok = 0;
	snd_pcm_drop(h);
	snd_pcm_close(h);
	return NULL;
}

// Same for the microphone side: switch the card's capture on and set it to 70 %.
static void usb_card_capture_on(const char *dev){
	int card = -1;
	const char *c = strchr(dev, ':');
	if (c) card = atoi(c + 1);
	if (card < 0) return;
	char name[24];
	snprintf(name, sizeof(name), "hw:%d", card);
	snd_mixer_t *m = NULL;
	if (snd_mixer_open(&m, 0) < 0) return;
	if (snd_mixer_attach(m, name) < 0 || snd_mixer_selem_register(m, NULL, NULL) < 0 || snd_mixer_load(m) < 0){
		snd_mixer_close(m);
		return;
	}
	for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)){
		if (!snd_mixer_selem_is_active(e))
			continue;
		int has_sw = snd_mixer_selem_has_capture_switch(e);
		int has_vol = snd_mixer_selem_has_capture_volume(e);
		if (!has_sw && !has_vol)
			continue;
		if (has_sw)
			snd_mixer_selem_set_capture_switch_all(e, 1);
		if (has_vol){
			long mn, mx;
			snd_mixer_selem_get_capture_volume_range(e, &mn, &mx);
			snd_mixer_selem_set_capture_volume_all(e, mn + (mx - mn) * 70 / 100);
		}
		printf("[USB] microphone control '%s' on, 70%%\n", snd_mixer_selem_get_name(e));
	}
	snd_mixer_close(m);
}

static void *mic_loop(void *arg){
	snd_pcm_t *h = open_pcm(mic_dev, SND_PCM_STREAM_CAPTURE, 1);
	int32_t in[CHUNK];
	if (!h){
		printf("[USB] could not open microphone %s\n", mic_dev);
		mic_run = 0;
		return NULL;
	}
	usb_card_capture_on(mic_dev);
	printf("[USB] microphone on %s (%s)\n", mic_dev, dev_name);
	snd_pcm_prepare(h);
	mic_ok = 1;
	while (mic_run){
		snd_pcm_sframes_t n = snd_pcm_readi(h, in, CHUNK);
		if (n < 0){
			snd_pcm_recover(h, (int)n, 1);
			continue;
		}
		for (int i = 0; i < n; i++)
			ring_put(&mic_ring, in[i] / 2);     // /2: same level as the codec path, which halves its samples
	}
	mic_ok = 0;
	snd_pcm_drop(h);
	snd_pcm_close(h);
	return NULL;
}

int usb_speaker_enable(int on){
	if (on && !spk_run){
		for (int t = 0; t < 12; t++){            // card may still be appearing: look for ~3 s
			find_devices();
			if (spk_dev[0]) break;
			usleep(250000);
		}
		if (!spk_dev[0]){
			printf("[USB] no USB sound device found (checked /proc/asound/cards)\n");
			return 0;
		}
		atomic_store(&spk_ring.head, 0); atomic_store(&spk_ring.tail, 0);
		spk_run = 1;
		pthread_create(&spk_thread, NULL, speaker_loop, NULL);
		for (int i = 0; i < 450 && spk_run && !spk_ok; i++)    // up to 1 s for it to open
			usleep(10000);
		if (!spk_ok){
			spk_run = 0;
			pthread_join(spk_thread, NULL);
			return 0;
		}
	}
	else if (!on && spk_run){
		spk_run = 0;
		pthread_join(spk_thread, NULL);
	}
	return 1;
}

int usb_mic_enable(int on){
	if (on && !mic_run){
		for (int t = 0; t < 12; t++){
			find_devices();
			if (mic_dev[0]) break;
			usleep(250000);
		}
		if (!mic_dev[0]){
			printf("[USB] no USB sound device found (checked /proc/asound/cards)\n");
			return 0;
		}
		atomic_store(&mic_ring.head, 0); atomic_store(&mic_ring.tail, 0);
		mic_run = 1;
		pthread_create(&mic_thread, NULL, mic_loop, NULL);
		for (int i = 0; i < 450 && mic_run && !mic_ok; i++)
			usleep(10000);
		if (!mic_ok){
			mic_run = 0;
			pthread_join(mic_thread, NULL);
			return 0;
		}
	}
	else if (!on && mic_run){
		mic_run = 0;
		pthread_join(mic_thread, NULL);
	}
	return 1;
}

int usb_speaker_active(void){ return spk_run && spk_ok; }
int usb_mic_active(void){ return mic_run && mic_ok; }
const char *usb_audio_device_name(void){ return dev_name; }

// 96 kHz -> 48 kHz by averaging pairs (the radio's audio is below 4 kHz, so no aliasing)
void usb_speaker_write(const int32_t *samples, int n_samples, int gain_x1000){
	if (!usb_speaker_active())
		return;
	static int64_t peak = 0;
	static int blocks = 0;
	for (int i = 0; i + 1 < n_samples; i += 2){
		int64_t v = ((int64_t)samples[i] + samples[i+1]) / 2;
		v = v * gain_x1000 / 1000;
		if (llabs(v) > peak) peak = llabs(v);
		if (v > 2147483000LL) v = 2147483000LL;
		if (v < -2147483000LL) v = -2147483000LL;
		ring_put(&spk_ring, (int32_t)v);
	}
	if (++blocks >= 500){      // about every 5 seconds
		printf("[USB] speaker audio peak %.1f%% of full scale\n", 100.0 * peak / 2147483647.0);
		peak = 0; blocks = 0;
	}
}

// 48 kHz -> 96 kHz by linear interpolation. Keeps the buffer between 20 and 80 ms:
// the USB clock and the radio's clock are never exactly equal, so the extra is
// dropped (or a sample is repeated) rather than letting the delay creep.
void usb_mic_read(int32_t *samples, int n_samples){
	static int32_t last = 0;
	if (!usb_mic_active()){
		memset(samples, 0, n_samples * sizeof(int32_t));
		return;
	}
	unsigned fill = ring_fill(&mic_ring);
	int32_t dummy;
	while (fill > 3840){ ring_get(&mic_ring, &dummy); fill--; }
	for (int i = 0; i < n_samples; i += 2){
		int32_t v;
		if (!ring_get(&mic_ring, &v))
			v = last;                            // underrun: hold
		samples[i] = (int32_t)(((int64_t)last + v) / 2);
		if (i + 1 < n_samples)
			samples[i+1] = v;
		last = v;
	}
}
