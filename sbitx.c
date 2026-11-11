#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <linux/types.h>
#include <linux/limits.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <errno.h>
#include "sdr.h"
#include "ainr.h"
#include "usbaudio.h"
#ifdef SBITX_FREEDV
#include "freedv_ext.h"
#endif
#include "sdr_ui.h"
#include "sound.h"
#include "i2cbb.h"
#include "si5351.h"
#include "ini.h"

#define DEBUG 0

char audio_card[32];
static int tx_shift = 512;

FILE *pf_debug = NULL;

//this is for processing FT8 decodes 
//unsigned int	wallclock = 0;

#define TX_LINE 4
#define TX_POWER 27
#define BAND_SELECT 5
#define LPF_A 5
#define LPF_B 6
#define LPF_C 10
#define LPF_D 11

#define SBITX_DE (0)
#define SBITX_V2 (1)

int sbitx_version = SBITX_V2;
int fwdpower, vswr;
float fft_bins[MAX_BINS]; // spectrum ampltiudes  
int spectrum_plot[MAX_BINS];
fftw_complex *fft_spectrum;
fftw_plan plan_spectrum;
float spectrum_window[MAX_BINS];
void set_rx1(int frequency);
void tr_switch(int tx_on);

// Wisdom Defines for the FFTW and FFTWF libraries
// Options for WISDOM_MODE from least to most rigorous are FFTW_ESTIMATE, FFTW_MEASURE, FFTW_PATIENT, and FFTW_EXHAUSTIVE
// The FFTW_ESTIMATE mode seems to make completely incorrect Wisdom plan choices sometimes, and is not recommended.
// Wisdom plans found in an existing Wisdom file will negate the need for time consuming Wisdom plan calculations
// if the Wisdom plans in the file were generated at the same or more rigorous level.
#define WISDOM_MODE FFTW_MEASURE
#define PLANTIME -1		// spend no more than plantime seconds finding the best FFT algorithm. -1 turns the platime cap off.
char wisdom_file[] = "sbitx_wisdom.wis";

fftw_complex *fft_out;		// holds the incoming samples in freq domain (for rx as well as tx)
fftw_complex *fft_in;			// holds the incoming samples in time domain (for rx as well as tx) 
fftw_complex *fft_m;			// holds previous samples for overlap and discard convolution 
fftw_plan plan_fwd, plan_tx;
int bfo_freq = 40035000;
int freq_hdr = -1;
int si570_xtal = 0;

static double volume 	= 100.0;
static int tx_drive = 40;
static int rx_gain = 100;
static int fdv_voice_mode(void);     // FreeDV active on a voice sideband (defined further down)
// Speech-oriented receive filters (NRVL, FMNT, FVLY, NBVL) must not touch FreeDV or FT8:
// they treat the steady digital tones as noise and cut them.
#define RX_DSP_BYPASS() (fdv_voice_mode() || (rx_list && rx_list->mode == MODE_FT8))
static int rx_vol = 100;
// With the USB speaker active the radio's own earphone output is muted; the
// volume control then sets the level sent to the USB speaker instead.
static int usb_spk_on = 0;
#define RX_MASTER_VOL (usb_speaker_active() ? 0 : rx_vol)
static int tx_gain = 100;
static int tx_compress = 0;
static double spectrum_speed = 0.3;
static int in_tx = 0;
static int rx_tx_ramp = 0;
static int sidetone = 2000000000;
struct vfo tone_a, tone_b, am_carrier; //these are audio tone generators
static int tx_use_line = 0;
struct rx *rx_list = NULL;
struct rx *tx_list = NULL;
struct filter *tx_filter;	//convolution filter
static double tx_amp = 0.0;
double alc_level = 1.0;   // was 'static' -- now exposed so sbitx_gtk.c can display it

// --- Notch filter (new feature, not in original base) ---
int notch_enabled = 0;
double notch_freq_hz = 700.0;   // center frequency of the notch, in Hz within the passband
int notch_width_hz = 60;        // bandwidth of the notch, in Hz. Now implemented as a
                                 // time-domain IIR biquad (see apply_notch_filter()),
                                 // not FFT-bin-zeroing -- so this is genuinely continuous,
                                 // any Hz value works exactly as set, no ~47Hz quantization.

// --- Contour (new feature) ---
// The inverse of Notch: boosts (rather than cuts) a region of the audio
// around a chosen center frequency. Same RBJ biquad family as the notch,
// but the peaking-filter variant (with a gain term) instead of the pure
// null variant. "Wider spread" per request -- its Hz range and zone
// boundaries are roughly double the notch's, for a broader, gentler bump
// rather than a sharp peak.
int contour_enabled = 0;
double contour_freq_hz = 1200.0;
int contour_width_hz = 150;
double contour_gain_db = 18.0;  // boost amount at the peak - raised again (was 12dB) for more prominence

// --- Formant-Aware DSP (re-added feature) ---
// Rabiner & Gold's source-filter model treats the vocal tract as an
// all-pole filter; its resonances (formants) carry vowel identity and
// voice timbre. Plain per-bin spectral subtraction (the NR stage above)
// doesn't know where those resonances are, so it can shave energy off
// formant peaks exactly as readily as it does noise-filled valleys. This
// stage estimates the vocal-tract envelope each frame via LPC
// (autocorrelation + Levinson-Durbin recursion), then uses that envelope
// to reduce NR's subtraction near formant peaks and increase it in the
// gaps between them (valley_atten), where musical-noise artifacts
// concentrate.
// Separate on/off flag, distinct from the 0-10 level -- 0 is now a valid
// "on, at lowest intensity" setting, not the same as being off.
int formant_protect_enabled = 0;
double valley_atten = 0.0;
int formant_valley_enabled = 0;  // separate from formant_protect_enabled -- fully independent toggle
int formant_valley_level = 1;    // 1-20, non-formant (valley) bin suppression amount
#define LPC_ORDER 10
static double lpc_envelope[MAX_BINS] = {0};

// --- Dynamic Noise Blanker (new feature) ---
// Time-domain impulse detector, operating on raw RX IF samples before any
// FFT processing. Targets bursty/impulsive noise (switching supplies, LED
// drivers, ignition noise) that frequency-domain spectral subtraction
// cannot fix, since that noise type is broadband and instantaneous rather
// than a stable per-bin noise floor.
// Separate on/off flag, same reasoning as above.
int noise_blanker_enabled = 0;
double nb_threshold = 0.0;
#define NB_ALPHA     0.001       // slow-adapting rolling average, tracks the noise floor not transients
#define NB_MAX_HOLD  8           // cap on consecutive samples blanked, so a sustained loud signal isn't muted

// --- Baseline Dynamic Noise Reduction (new feature) ---
// Frequency-domain spectral subtraction against a per-bin noise floor that
// is continuously re-estimated ("dynamic sampling" of the baseline noise).
// nr_threshold genuinely drives the subtraction curve below -- this is a
// deliberate fix for a real bug found in a third-party fork, where the
// equivalent slider was computed but never actually consumed by the DSP.
// Separate on/off flag, same reasoning as above.
int baseline_nr_enabled = 0;
double nr_threshold = 0.0;    // how far above the noise floor (in SNR ratio) a signal must be to escape subtraction
// AI noise reduction (RNNoise, see ainr.c): 0 = off, 1..20 = how much of the
// cleaned audio is mixed in (20 = fully cleaned). Applies to received SSB/AM
// audio only; set through the "ai_nr" command from the AINR menu box.
int ai_nr_level = 0;
#define NR_RESIDUAL    0.25   // fraction of the estimated noise magnitude always retained -- prevents "musical noise"/metallic artifacts (raised from 0.15)
#define NR_ALPHA_RISE  0.90   // slow rise: a loud signal shouldn't quickly get mistaken for a higher noise floor
#define NR_ALPHA_FALL  0.70   // faster fall: a dropping noise floor should be tracked reasonably promptly
static int tr_relay = 0;
static int rx_pitch = 700; //used only to offset the lo for CW,CWR
static int bridge_compensation = 100;
static double voice_clip_level = 0.04;
static int in_calibration = 1; // this turns off alc, clipping et al

static int multicast_socket = -1;

#define MUTE_MAX 2 
static int mute_count = 50;
// blocks (about 10 ms each) after a switch back to receive during which the AGC
// climbs back up fast; see agc2()
static int agc_post_tx = 0;

FILE *pf_record;
int16_t record_buffer[1024];
int32_t modulation_buff[MAX_BINS];


/* the power gain of the tx varies widely from 
band to band. these data structures help in flattening 
the gain */

struct power_settings {
	int f_start;
	int f_stop;
	int	max_watts;
	double scale;
};

struct power_settings band_power[] ={
	{ 3500000,  4000000, 37, 0.002},
	{ 7000000,  7300009, 40, 0.0015},
	{10000000, 10200000, 35, 0.0019},
	{14000000, 14300000, 35, 0.0025},
	{18000000, 18200000, 20, 0.0023},
	{21000000, 21450000, 20, 0.003},
	{24800000, 25000000, 20, 0.0034},
	{28000000, 29700000, 20, 0.0037}  
};

#define CMD_TX (2)
#define CMD_RX (3)
#define TUNING_SHIFT (0)
#define MDS_LEVEL (-135)

struct Queue qremote;

void radio_tune_to(u_int32_t f){
	if (rx_list->mode == MODE_CW)
  	si5351bx_setfreq(2, f + bfo_freq - 24000 + TUNING_SHIFT - rx_pitch);
	else if (rx_list->mode == MODE_CWR)
  	si5351bx_setfreq(2, f + bfo_freq - 24000 + TUNING_SHIFT + rx_pitch);
	else
  	si5351bx_setfreq(2, f + bfo_freq - 24000 + TUNING_SHIFT);

//  printf("Setting radio rx_pitch %d\n", rx_pitch);
}

void fft_init(){
	// int mem_needed;

	//printf("initializing the fft\n");
	fflush(stdout);

	// mem_needed = sizeof(fftw_complex) * MAX_BINS;

	fft_m = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS/2);
	fft_in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	fft_spectrum = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);

	memset(fft_spectrum, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_in, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_out, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_m, 0, sizeof(fftw_complex) * MAX_BINS/2);

	fftw_set_timelimit(PLANTIME);
	fftwf_set_timelimit(PLANTIME);
	int e = fftw_import_wisdom_from_filename(wisdom_file);
	if (e == 0)
	{
		printf("Generating Wisdom File...\n");
	}
	plan_fwd = fftw_plan_dft_1d(MAX_BINS, fft_in, fft_out, FFTW_FORWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
	plan_spectrum = fftw_plan_dft_1d(MAX_BINS, fft_in, fft_spectrum, FFTW_FORWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
	fftw_export_wisdom_to_filename(wisdom_file);

	//zero up the previous 'M' bins
	for (int i= 0; i < MAX_BINS/2; i++){
		__real__ fft_m[i]  = 0.0;
		__imag__ fft_m[i]  = 0.0;
	}

	make_hann_window(spectrum_window, MAX_BINS);
}

void fft_reset_m_bins(){
	//zero up the previous 'M' bins
	memset(fft_in, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_out, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_m, 0, sizeof(fftw_complex) * MAX_BINS/2);
	memset(fft_spectrum, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(tx_list->fft_time, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(tx_list->fft_freq, 0, sizeof(fftw_complex) * MAX_BINS);
/*	for (int i= 0; i < MAX_BINS/2; i++){
		__real__ fft_m[i]  = 0.0;
		__imag__ fft_m[i]  = 0.0;
	}
*/
}

int mag2db(double mag){
	int m = abs(mag) * 10000000;
	
	int c = 31;
	int p = 0x80000000;
	while(c > 0){
		if (p & m)
			break;
		c--;
		p = p >> 1;
	}
	return c;
}

void set_spectrum_speed(int speed){
	spectrum_speed = speed;
	for (int i = 0; i < MAX_BINS; i++)
		fft_bins[i] = 0;
}

void spectrum_reset(){
	for (int i = 0; i < MAX_BINS; i++)
		fft_bins[i] = 0;
}

void spectrum_update(){
	//we are only using the lower half of the bins, 
	//so this copies twice as many bins, 
	//it can be optimized. leaving it here just in case 
	//someone wants to try I Q channels 
	//in hardware

	// this has been hand optimized to lower
	//the inordinate cpu usage
	for (int i = 1269; i < 1803; i++){

		fft_bins[i] = ((1.0 - spectrum_speed) * fft_bins[i]) + 
			(spectrum_speed * cabs(fft_spectrum[i]));

		int y = power2dB(cnrmf(fft_bins[i])); 
		spectrum_plot[i] = y;
	}
}
/*
static int create_mcast_socket(){
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[MAX_BUFFER_SIZE];

    // Create a UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Error creating mcast socket");
				return -1;
    }

    // Set up the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(MULTICAST_PORT);

    // Bind the socket to the server address
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Error binding mcast socket");
        close(sockfd);
				return -1;
    }

    // Set up the multicast group membership
    struct ip_mreq mreq;
    inet_pton(AF_INET, MULTICAST_ADDR, &(mreq.imr_multiaddr.s_addr));
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
        perror("Error adding multicast group membership");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Listening for multicast on  %s:%d...\n", MULTICAST_ADDR, PORT);
		return socketfd;
}
*/

int remote_audio_output(int16_t *samples){
	int length = q_length(&qremote);
	for (int i = 0; i < length; i++){
		samples[i] = q_read(&qremote) / 32786;
	}
	return length;
}

static int prev_lpf = -1;
void set_lpf_40mhz(int frequency){
	int lpf = 0;

	if (frequency < 5500000)
		lpf = LPF_D;
	else if (frequency < 10500000)		
		lpf = LPF_C;
	else if (frequency < 18500000)		
		lpf = LPF_B;
	else if (frequency < 30000000) 
		lpf = LPF_A; 

	if (lpf == prev_lpf){
#if DEBUG > 0		
		puts("LPF not changed");
#endif		
		return;
	}
	
#if DEBUG > 0
	printf("##################Setting LPF to %d\n", lpf);
#endif

  digitalWrite(LPF_A, LOW);
  digitalWrite(LPF_B, LOW);
  digitalWrite(LPF_C, LOW);
  digitalWrite(LPF_D, LOW);

#if DEBUG > 0
  printf("################ setting %d high\n", lpf);
#endif 
  digitalWrite(lpf, HIGH); 
	prev_lpf = lpf;
}


void set_rx1(int frequency){
	if (frequency == freq_hdr)
		return;
	radio_tune_to(frequency);
	freq_hdr = frequency;
	set_lpf_40mhz(frequency);
}

void set_volume(double v){
	volume = v;	
}

FILE *wav_start_writing(const char* path)
{
    char subChunk1ID[4] = { 'f', 'm', 't', ' ' };
    uint32_t subChunk1Size = 16; // 16 for PCM
    uint16_t audioFormat = 1; // PCM = 1
    uint16_t numChannels = 1;
    uint16_t bitsPerSample = 16;
    uint32_t sampleRate = 12000;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t byteRate = sampleRate * blockAlign;

    char subChunk2ID[4] = { 'd', 'a', 't', 'a' };
    uint32_t subChunk2Size = 0Xffffffff; //num_samples * blockAlign;

    char chunkID[4] = { 'R', 'I', 'F', 'F' };
    uint32_t chunkSize = 4 + (8 + subChunk1Size) + (8 + subChunk2Size);
    char format[4] = { 'W', 'A', 'V', 'E' };

    FILE* f = fopen(path, "w");

    // NOTE: works only on little-endian architecture
    fwrite(chunkID, sizeof(chunkID), 1, f);
    fwrite(&chunkSize, sizeof(chunkSize), 1, f);
    fwrite(format, sizeof(format), 1, f);

    fwrite(subChunk1ID, sizeof(subChunk1ID), 1, f);
    fwrite(&subChunk1Size, sizeof(subChunk1Size), 1, f);
    fwrite(&audioFormat, sizeof(audioFormat), 1, f);
    fwrite(&numChannels, sizeof(numChannels), 1, f);
    fwrite(&sampleRate, sizeof(sampleRate), 1, f);
    fwrite(&byteRate, sizeof(byteRate), 1, f);
    fwrite(&blockAlign, sizeof(blockAlign), 1, f);
    fwrite(&bitsPerSample, sizeof(bitsPerSample), 1, f);

    fwrite(subChunk2ID, sizeof(subChunk2ID), 1, f);
    fwrite(&subChunk2Size, sizeof(subChunk2Size), 1, f);
		
		return f;
}

void wav_record(int32_t *samples, int count){
	int16_t *w;
	int32_t *s;
	int i = 0, j = 0;
	int decimation_factor = 96000 / 12000; 

	if (!pf_record)
		return;

	w = record_buffer;
	while(i < count){
		*w++ = *samples / 32786;
		samples += decimation_factor;
		i += decimation_factor;	
		j++;
	}
	fwrite(record_buffer, j, sizeof(int16_t), pf_record);
}

/*
The sound process is called by the duplex sound system for each block of samples
In this demo, we read and equivalent block from the file instead of processing from
the input I and Q signals.
*/

int32_t in_i[MAX_BINS];
int32_t in_q[MAX_BINS];
int32_t	out_i[MAX_BINS];
int32_t out_q[MAX_BINS];
short is_ready = 0;

void tx_init(int frequency, short mode, int bpf_low, int bpf_high){

	//we assume that there are 96000 samples / sec, giving us a 48khz slice
	//the tuning can go up and down only by 22 KHz from the center_freq

	tx_filter = filter_new(1024, 1025);
//	filter_tune(tx_filter, (1.0 * bpf_low)/96000.0, (1.0 * bpf_high)/96000.0 , 5);
}

struct rx *add_tx(int frequency, short mode, int bpf_low, int bpf_high){

	//we assume that there are 96000 samples / sec, giving us a 48khz slice
	//the tuning can go up and down only by 22 KHz from the center_freq

	struct rx *r = malloc(sizeof(struct rx));
	r->low_hz = bpf_low;
	r->high_hz = bpf_high;
	r->tuned_bin = 512; 

	//create fft complex arrays to convert the frequency back to time
	r->fft_time = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	r->fft_freq = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	
	int e = fftw_import_wisdom_from_filename(wisdom_file);
	if (e == 0)
	{
		printf("Generating Wisdom File...\n");
	}	
	r->plan_rev = fftw_plan_dft_1d(MAX_BINS, r->fft_freq, r->fft_time, FFTW_BACKWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
	fftw_export_wisdom_to_filename(wisdom_file);
	
	r->output = 0;
	r->next = NULL;
	r->mode = mode;
	
	r->filter = filter_new(1024, 1025);
	filter_tune(r->filter, (1.0 * bpf_low)/96000.0, (1.0 * bpf_high)/96000.0 , 5);

	if (abs(bpf_high - bpf_low) < 1000){
		r->agc_speed = 10;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}
	else {
		r->agc_speed = 10;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}

	//the modems drive the tx at 12000 Hz, this has to be upconverted
	//to the radio's sampling rate

  r->next = tx_list;
  tx_list = r;
}

struct rx *add_rx(int frequency, short mode, int bpf_low, int bpf_high){

	//we assume that there are 96000 samples / sec, giving us a 48khz slice
	//the tuning can go up and down only by 22 KHz from the center_freq

	struct rx *r = malloc(sizeof(struct rx));
	r->low_hz = bpf_low;
	r->high_hz = bpf_high;
	r->tuned_bin = 512; 
	r->agc_gain = 0.0;

	//create fft complex arrays to convert the frequency back to time
	r->fft_time = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	r->fft_freq = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	
	int e = fftw_import_wisdom_from_filename(wisdom_file);
	if (e == 0)
	{
		printf("Generating Wisdom File...\n");
	}	
	r->plan_rev = fftw_plan_dft_1d(MAX_BINS, r->fft_freq, r->fft_time, FFTW_BACKWARD, WISDOM_MODE);  // Was FFTW_ESTIMATE N3SB
	fftw_export_wisdom_to_filename(wisdom_file);
	
	r->output = 0;
	r->next = NULL;
	r->mode = mode;
	
	r->filter = filter_new(1024, 1025);
	filter_tune(r->filter, (1.0 * bpf_low)/96000.0, (1.0 * bpf_high)/96000.0 , 5);

	if (abs(bpf_high - bpf_low) < 1000){
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
    r->signal_avg = 0;
	}
	else {
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
    r->signal_avg = 0;
	}

	// the modems are driven by 12000 samples/sec
	// the queue is for 20 seconds, 5 more than 15 sec needed for the FT8

	r->next = rx_list;
	rx_list = r;

}



int count = 0;

double agc2(struct rx *r){
	int i;
  double signal_strength, agc_gain_should_be;

	//do nothing if agc is off
  if (r->agc_speed == -1){
	  for (i=0; i < MAX_BINS/2; i++)
			__imag__ (r->fft_time[i+(MAX_BINS/2)]) *=10000000;
    return 10000000;
  }

  // While the radio is still muting after a switch back to receive, the AGC must
  // not react to the switching click: hold the gain and just apply it.
  if (mute_count > 0 && r->agc_gain > 0.0){
	  for (i=0; i < MAX_BINS/2; i++)
			__imag__ (r->fft_time[i+(MAX_BINS/2)]) *= r->agc_gain;
    return 100000000000 / r->agc_gain;
  }

  //find the peak signal amplitude
  signal_strength = 0.0;
	for (i=0; i < MAX_BINS/2; i++){
		double s = cimag(r->fft_time[i+(MAX_BINS/2)]) * 1000;
		if (signal_strength < s) 
			signal_strength = s;
	}
	//also calculate the moving average of the signal strength
  r->signal_avg = (r->signal_avg * 0.93) + (signal_strength * 0.07);
	if (signal_strength == 0)
		agc_gain_should_be = 10000000;
	else
		agc_gain_should_be = 100000000000/signal_strength;
	r->signal_strength = signal_strength;
//	printf("Agc temp, g:%g, s:%g, f:%g ", r->agc_gain, signal_strength, agc_gain_should_be);

	double agc_ramp = 0.0;

  // climb up the agc quickly if the signal is louder than before 
	if (agc_gain_should_be < r->agc_gain){
		r->agc_gain = agc_gain_should_be;
		//reset the agc to hang count down 
    r->agc_loop = r->agc_speed;
  }
	else if (agc_post_tx > 0){
		// Just back from transmit: any click or leakage at the switch makes the
		// AGC drop at once, and the normal recovery takes 3 to 5 seconds, which
		// is heard as the receive audio coming back slowly. Climb back up in
		// about a third of a second instead (a louder signal still pulls the
		// gain down instantly, as always), and ignore the hang time.
		agc_ramp = (agc_gain_should_be - r->agc_gain) / 32.0;
	}
	else if (r->agc_loop <= 0){
		agc_ramp = (agc_gain_should_be - r->agc_gain) / (MAX_BINS/2);	
	}
 
	if (agc_ramp != 0){
  	for (i = 0; i < MAX_BINS/2; i++){
	  	__imag__ (r->fft_time[i+(MAX_BINS/2)]) *= r->agc_gain;
		}
		r->agc_gain += agc_ramp;		
	}
	else 
  	for (i = 0; i < MAX_BINS/2; i++)
	  	__imag__ (r->fft_time[i+(MAX_BINS/2)]) *= r->agc_gain;

  r->agc_loop--;
	if (agc_post_tx > 0) agc_post_tx--;

	//printf("%d:s meter: %d %d %d \n", count++, (int)r->agc_gain, (int)r->signal_strength, r->agc_loop);
  return 100000000000 / r->agc_gain;  
}

void my_fftw_execute(fftw_plan f){
	fftw_execute(f);
}

static int32_t rx_am_avg = 0;

// Estimate the vocal-tract spectral envelope for the current block of real
// RX samples, via the autocorrelation method + Levinson-Durbin recursion
// (the standard LPC technique described by Rabiner & Gold). Fills
// lpc_envelope[MAX_BINS] with a smooth magnitude-response curve whose
// peaks mark the formant locations for this frame.
void compute_formant_envelope(int32_t *samples, int n_samples){
	double frame[MAX_BINS];
	double r_ac[LPC_ORDER + 1];
	double a[LPC_ORDER + 1], a_prev[LPC_ORDER + 1];
	double lpc_coeffs[LPC_ORDER];
	int i, j, k;
	double err;

	if (n_samples > MAX_BINS)
		n_samples = MAX_BINS;

	// Hamming window before autocorrelation -- standard practice, reduces
	// edge artifacts from treating a finite block as if periodic.
	for (i = 0; i < n_samples; i++){
		double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (n_samples - 1));
		frame[i] = ((double)samples[i] / 2000000000.0) * w;
	}

	for (i = 0; i <= LPC_ORDER; i++){
		r_ac[i] = 0.0;
		for (j = 0; j < n_samples - i; j++)
			r_ac[i] += frame[j] * frame[j + i];
	}

	if (r_ac[0] < 1e-12){
		// silence/near-silence this frame -- flatten the envelope so NR
		// falls back to its normal, unprotected behavior
		for (k = 0; k < MAX_BINS; k++)
			lpc_envelope[k] = 1.0;
		return;
	}

	for (i = 0; i <= LPC_ORDER; i++)
		a[i] = 0.0;
	err = r_ac[0];

	for (i = 1; i <= LPC_ORDER; i++){
		double acc = r_ac[i];
		for (j = 1; j < i; j++)
			acc -= a[j] * r_ac[i - j];
		double refl = acc / err;

		for (j = 0; j <= LPC_ORDER; j++)
			a_prev[j] = a[j];
		a[i] = refl;
		for (j = 1; j < i; j++)
			a[j] = a_prev[j] - refl * a_prev[i - j];

		err *= (1.0 - refl * refl);
		if (err <= 1e-9)
			err = 1e-9;
	}

	for (k = 0; k < LPC_ORDER; k++)
		lpc_coeffs[k] = a[k + 1];

	// Evaluate the all-pole filter's magnitude response |H(w)| = 1/|A(w)|
	// at each FFT bin frequency -- this is the formant envelope.
	double sum_env = 0.0;
	for (k = 0; k < MAX_BINS; k++){
		double w = 2.0 * M_PI * k / MAX_BINS;
		double re = 1.0, im = 0.0;
		for (j = 1; j <= LPC_ORDER; j++){
			re -= lpc_coeffs[j - 1] * cos(j * w);
			im += lpc_coeffs[j - 1] * sin(j * w);
		}
		double denom = sqrt(re * re + im * im);
		if (denom < 1e-6)
			denom = 1e-6;
		lpc_envelope[k] = 1.0 / denom;
		sum_env += lpc_envelope[k];
	}

	double avg_env = sum_env / MAX_BINS;
	if (avg_env > 1e-9)
		for (k = 0; k < MAX_BINS; k++)
			lpc_envelope[k] /= avg_env;

	// On noise the envelope the LPC finds changes from block to block, so the
	// FMNT peaks and FVLY valleys would jump about and the noise would warble
	// ("gurgle"). Speech formants move much more slowly than that, so the
	// envelope is smoothed over about 40 ms before it is used.
	static double env_s[MAX_BINS];
	static int env_s_init = 0;
	if (!env_s_init){
		for (k = 0; k < MAX_BINS; k++)
			env_s[k] = lpc_envelope[k];
		env_s_init = 1;
	}
	else{
		for (k = 0; k < MAX_BINS; k++){
			env_s[k] = 0.85 * env_s[k] + 0.15 * lpc_envelope[k];
			lpc_envelope[k] = env_s[k];
		}
	}
}

void rx_am(int32_t *input_rx,  int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, int n_samples)
{
	int i, j = 0;
	double i_sample, q_sample;
	//STEP 1: first add the previous M samples to
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	//STEP 2: then add the new set of samples
	// m is the index into incoming samples, starting at zero
	// i is the index into the time samples, picking from 
	// the samples added in the previous step
	int m = 0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){
		i_sample = (1.0  *input_rx[j])/200000000.0;
		q_sample = 0;

		j++;

		__real__ fft_m[m] = i_sample;
		__imag__ fft_m[m] = q_sample;

		__real__ fft_in[i]  = i_sample;
		__imag__ fft_in[i]  = q_sample;
		m++;
	}

	// STEP 3: convert the time domain samples to  frequency domain
	my_fftw_execute(plan_fwd);

	//STEP 3B: this is a side line, we use these frequency domain
	// values to paint the spectrum in the user interface
	// I discovered that the raw time samples give horrible spectrum
	// and they need to be multiplied wiht a window function 
	// they use a separate fft plan
	// NOTE: the spectrum update has nothing to do with the actual
	// signal processing. If you are not showing the spectrum or the
	// waterfall, you can skip these steps
	for (i = 0; i < MAX_BINS; i++)
		__real__ fft_in[i] *= spectrum_window[i];
	my_fftw_execute(plan_spectrum);

	// the spectrum display is updated
	spectrum_update();
	
	struct rx *r = rx_list;

	//STEP 4: we rotate the bins around by r-tuned_bin
	for (i = 0; i < MAX_BINS; i++){
		int b =  i + r->tuned_bin;
		if (b >= MAX_BINS)
			b = b - MAX_BINS;
		if (b < 0)
			b = b + MAX_BINS;
		r->fft_freq[i] = fft_out[b]; 
//		r->fft_freq[i] = fft_out[i];
	}

	// STEP 6: apply the filter to the signal,
	// in frequency domain we just multiply the filter
	// coefficients with the frequency domain samples
	for (i = 0; i < MAX_BINS; i++)
		r->fft_freq[i] *= r->filter->fir_coeff[i];

	//STEP 7: convert back to time domain	
	my_fftw_execute(r->plan_rev);
	//STEP 8 : AGC
	agc2(r);

	//do an independent am detection (this takes 12 khz of b/w)
	for (i= MAX_BINS/2; i < MAX_BINS; i++){
		int32_t sample;
		sample = abs(r->fft_time[i]) * 1000000;
		rx_am_avg = (rx_am_avg* 5 + sample)/6;
		//keep transmit buffer empty
		output_speaker[i] = sample;
//		output_speaker[i] = abs(input_rx[i]);
		output_tx[i] = 0;
	}
//	for (i = 0; i < n_samples; i++)
//		output_speaker[i] = rx_am_avg = ((rx_am_avg * 9) + abs(input_rx[i]))/10;
}

//TODO : optimize the memory copy and moves to use the memcpy
// --- DSP noise-reduction readout (new; display only, changes no audio) ---
// The spectrum on screen is tapped before NRVL / FMNT / FVLY run (see the
// note in rx_linear), so it can't show what they do. This measures it
// directly instead: the noise power in the passband bins of r->fft_freq
// just BEFORE those frequency-domain stages and just AFTER them, and
// publishes the difference in tenths of a dB for the GTK side to draw.
//
// Paired measurement, on purpose: bins louder than DSP_NR_NOISE_CEILING
// times the MEDIAN bin power (as it was BEFORE the DSP) are treated as
// signals and left out; every other bin counts as noise. The same bins
// are then measured again afterwards. Two earlier versions were wrong,
// as found by checking this readout against the real drop in output
// noise: comparing a low percentile tracks the bottom of the noise rather
// than its power, and picking "the quietest 60% of bins" is biased in
// both directions, because the spectral subtraction cuts quiet and loud
// noise bins by different amounts (it read 0 dB when the output had
// dropped 0.55 dB, and 10 dB when it had dropped 7 dB). What you hear is
// the power of ALL the noise, so everything that isn't clearly a signal
// has to be counted. The result is smoothed over about half a second.
//
// Covers NRVL, FVLY and FMNT only (the frequency-domain stages). NBVL
// acts earlier and is already inside the noise floor the display shows.
// Reads 0 when none of them is on, when the passband is too narrow to
// measure (a tight CW filter), or when signals fill most of the passband.
#define DSP_NR_MAX_BINS 256
#define DSP_NR_MIN_BINS 8
#ifndef DSP_NR_NOISE_CEILING
#define DSP_NR_NOISE_CEILING 10.0      // x the median bin power, i.e. +10 dB: above this a bin is a signal
#endif
#define DSP_NR_EMA_ALPHA 0.03

volatile int dsp_nr_reduction_x10 = 0;      // tenths of a dB, >= 0
static double dsp_nr_ema = 0.0;
static int dsp_nr_have_ema = 0;
static double dsp_pre_power[DSP_NR_MAX_BINS];

static int dsp_cmp_double(const void *a, const void *b){
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

// Which bins of r->fft_freq hold the passband audio. The stored edges are
// negative for LSB at startup (add_rx(..., -3000, -300)) and positive once
// the UI has set them, so use absolute values and order them.
static int dsp_nr_bins(struct rx *r, int *first, int *count, int *stride){
	double hz_per_bin = 96000.0 / MAX_BINS;
	double a = fabs((double)r->low_hz), b = fabs((double)r->high_hz);
	double lo_hz = a < b ? a : b, hi_hz = a < b ? b : a;
	int lo = (int)ceil(lo_hz / hz_per_bin);
	int hi = (int)floor(hi_hz / hz_per_bin);

	if (lo < 1) lo = 1;
	if (hi > MAX_BINS/2 - 1) hi = MAX_BINS/2 - 1;
	if (hi - lo + 1 < DSP_NR_MIN_BINS)
		return 0;

	// LSB / CWR keep their audio in the upper half of the rotated spectrum
	if (r->mode == MODE_LSB || r->mode == MODE_CWR){
		int t = lo;
		lo = MAX_BINS - hi;
		hi = MAX_BINS - t;
	}

	int n = hi - lo + 1;
	*stride = (n + DSP_NR_MAX_BINS - 1) / DSP_NR_MAX_BINS;   // very wide passbands: sample every nth bin
	*count = n / *stride;
	*first = lo;
	return 1;
}

static void dsp_nr_snapshot(struct rx *r, int first, int count, int stride, double *out){
	int i;
	for (i = 0; i < count; i++){
		double re = creal(r->fft_freq[first + i * stride]);
		double im = cimag(r->fft_freq[first + i * stride]);
		out[i] = re * re + im * im;
	}
}

static void dsp_nr_update(struct rx *r, int first, int count, int stride){
	double sorted[DSP_NR_MAX_BINS];
	int i, idx;

	memcpy(sorted, dsp_pre_power, count * sizeof(double));
	qsort(sorted, count, sizeof(double), dsp_cmp_double);
	idx = count / 2;
	double limit = DSP_NR_NOISE_CEILING * sorted[idx];     // median-based, so a few signals can't drag it up

	double sum_pre = 0.0, sum_post = 0.0;
	for (i = 0; i < count; i++){
		if (dsp_pre_power[i] > limit)
			continue;           // a signal, not noise: same bins before and after, so it just isn't counted
		double re = creal(r->fft_freq[first + i * stride]);
		double im = cimag(r->fft_freq[first + i * stride]);
		sum_pre += dsp_pre_power[i];
		sum_post += re * re + im * im;
	}
	if (sum_pre < 1e-20)
		return;                 // silence: nothing meaningful to compare, hold the last reading

	double red = 10.0 * log10(sum_pre / (sum_post + 1e-30));
	if (!dsp_nr_have_ema){
		dsp_nr_ema = red;
		dsp_nr_have_ema = 1;
	}
	else
		dsp_nr_ema += DSP_NR_EMA_ALPHA * (red - dsp_nr_ema);

	double v = dsp_nr_ema;
	if (v < 0.0) v = 0.0;
	if (v > 40.0) v = 40.0;
	dsp_nr_reduction_x10 = (int)(v * 10.0 + 0.5);
}

static void dsp_nr_idle(void){
	dsp_nr_have_ema = 0;
	dsp_nr_ema = 0.0;
	dsp_nr_reduction_x10 = 0;
}

// NRMMSE_BEGIN
// ---------------------------------------------------------------------
// NRVL noise reduction engine: MMSE log-spectral-amplitude estimator
// (Ephraim & Malah 1985) with decision-directed a-priori SNR and a
// minimum-statistics noise tracker (Martin 2001).
//
//  * The noise level of every bin is the MINIMUM of its smoothed power
//    over the last ~1.4 s (8 sub-windows of 16 frames), times a bias
//    factor. Speech never pulls it up, however long a word lasts, and a
//    changing noise floor is followed within a second or two. (The old
//    NRVL used a rise/fall average of the magnitude, which treated
//    speech as noise and so barely subtracted anything.)
//  * The gain of every bin comes from the estimated signal-to-noise
//    ratio, carried over from the previous frame (decision-directed),
//    which removes the "bubbling / musical noise" of plain spectral
//    subtraction. The gain is also smoothed across 3 neighbouring bins.
//  * The level control (0..20) sets how deep noise may be pushed down
//    (floor -8 dB at 0 to -26 dB at 20).
//    Measured on 40 m recordings (speech 7 dB over the noise): about -9 dB of
//    steady noise at level 5, -14 dB at 10 and -25 dB at 20. The speech loses
//    2 to 3 dB at level 5, 3 to 4 dB at 10 and 5 to 7 dB at 20, so the
//    speech-to-noise ratio improves 6 to 7 dB at 5, 10 to 11 dB at 10 and
//    18 to 20 dB at 20.
//
// Only bins within +-NRM_MAX_HZ of the carrier are processed; the
// receive filter removes everything outside that anyway.
// ---------------------------------------------------------------------
#define NRM_MAX_HZ   6000.0
#define NRM_ALPHA_DD 0.93
#define NRM_ALPHA_S  0.85
#define NRM_SUBLEN   16
#define NRM_SUBN     8
#define NRM_BIAS     1.6
#define NRM_XI_MIN   0.003

struct nrm_state {
	double P[MAX_BINS];          // smoothed power
	double Pmin_cur[MAX_BINS];   // minimum within the current sub-window
	double Phist[NRM_SUBN][MAX_BINS];
	double clean2[MAX_BINS];     // previous frame's estimated clean power
	int sub_count, sub_idx, frames;
	int init;
};
static struct nrm_state nrm;

static double nrm_e1(double x){
	if (x < 1.0){
		return -log(x) - 0.57721566 + x * (0.99999193 + x * (-0.24991055 +
			x * (0.05519968 + x * (-0.00976004 + x * 0.00107857))));
	}
	return exp(-x) / x * (x*x + 2.334733*x + 0.250621) / (x*x + 3.330657*x + 1.681534);
}

static void nrm_reset(void){
	memset(&nrm, 0, sizeof(nrm));
}

// X: the MAX_BINS complex bins of this frame (modified in place).
// level: 0..20.  Bins are FFT order (0 = carrier, upper half = negative freq).
static void nrm_process(double complex *X, double level){
	int span = (int)(NRM_MAX_HZ / (96000.0 / MAX_BINS));
	int k, j, idx[2*512 + 4], n = 0;
	static double G[MAX_BINS], Gt[MAX_BINS];
	double gmin = pow(10.0, -(8.0 + 0.9 * level) / 20.0);

	for (k = 0; k <= span && n < 1030; k++) idx[n++] = k;
	for (k = MAX_BINS - span; k < MAX_BINS && n < 1030; k++) idx[n++] = k;

	int first = !nrm.init;
	int roll = 0;
	if (!first && ++nrm.sub_count >= NRM_SUBLEN)
		roll = 1;

	for (j = 0; j < n; j++){
		k = idx[j];
		double pw = creal(X[k])*creal(X[k]) + cimag(X[k])*cimag(X[k]) + 1e-30;

		if (first){
			nrm.P[k] = pw;
			nrm.Pmin_cur[k] = pw;
			for (int u = 0; u < NRM_SUBN; u++) nrm.Phist[u][k] = pw;
			nrm.clean2[k] = pw * 0.1;
		}
		else
			nrm.P[k] = NRM_ALPHA_S * nrm.P[k] + (1.0 - NRM_ALPHA_S) * pw;

		if (nrm.P[k] < nrm.Pmin_cur[k])
			nrm.Pmin_cur[k] = nrm.P[k];

		double mn = nrm.Pmin_cur[k];
		for (int u = 0; u < NRM_SUBN; u++)
			if (nrm.Phist[u][k] < mn) mn = nrm.Phist[u][k];

		// during the first sub-windows the minimum is taken over few frames
		// and is biased low; the history was seeded with the first frame, so
		// it settles on its own as real minima replace it.
		double lam = mn * NRM_BIAS + 1e-30;

		double gamma = pw / lam;
		double xi = NRM_ALPHA_DD * nrm.clean2[k] / lam +
			(1.0 - NRM_ALPHA_DD) * (gamma > 1.0 ? gamma - 1.0 : 0.0);
		if (xi < NRM_XI_MIN) xi = NRM_XI_MIN;
		double v = xi / (1.0 + xi) * gamma;
		if (v < 1e-6) v = 1e-6;
		double g = xi / (1.0 + xi) * exp(0.5 * nrm_e1(v));
		if (g > 1.0) g = 1.0;
		if (level > 8.0)
			g = pow(g, 1.0 + (level - 8.0) / 6.0);   // above 8 the cut deepens: level 20 squares the gain
		if (g < gmin) g = gmin;
		nrm.clean2[k] = g * g * pw;
		// time smoothing of the applied gain: opens fast (speech onsets stay
		// crisp), closes slowly (random gain dips between blocks are what
		// is heard as a gurgle / warble)
		if (first) Gt[k] = g;
		else if (g > Gt[k]) Gt[k] += 0.7 * (g - Gt[k]);
		else Gt[k] += 0.22 * (g - Gt[k]);
		G[k] = Gt[k];
	}

	if (roll){
		for (j = 0; j < n; j++){
			k = idx[j];
			nrm.Phist[nrm.sub_idx][k] = nrm.Pmin_cur[k];
			nrm.Pmin_cur[k] = nrm.P[k];
		}
		nrm.sub_idx = (nrm.sub_idx + 1) % NRM_SUBN;
		nrm.sub_count = 0;
	}
	nrm.init = 1;

	// smooth the gain over five neighbouring bins, then apply it
	for (j = 0; j < n; j++){
		k = idx[j];
		int km = (j > 0 && idx[j-1] == k - 1) ? k - 1 : k;
		int kp = (j < n-1 && idx[j+1] == k + 1) ? k + 1 : k;
		int kmm = (km != k && j > 1 && idx[j-2] == k - 2) ? k - 2 : km;
		int kpp = (kp != k && j < n-2 && idx[j+2] == k + 2) ? k + 2 : kp;
		double gs = 0.1 * G[kmm] + 0.2 * G[km] + 0.4 * G[k] + 0.2 * G[kp] + 0.1 * G[kpp];
		X[k] *= gs;
	}
}
// NRMMSE_END

void rx_linear(int32_t *input_rx,  int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, int n_samples)
{
	int i, j = 0;
	double i_sample, q_sample;

	// formant-aware DSP: refresh the envelope estimate for this block of
	// real RX samples before any further processing. Needed by either
	// FMNT (peak boost) or FVLY (valley suppression) independently.
	if (formant_protect_enabled || formant_valley_enabled)
		compute_formant_envelope(input_rx, n_samples);

	//STEP 1: first add the previous M samples to
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	//STEP 2: then add the new set of samples
	// m is the index into incoming samples, starting at zero
	// i is the index into the time samples, picking from 
	// the samples added in the previous step
	int m = 0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){
		i_sample = (1.0  *input_rx[j])/200000000.0;
		q_sample = 0;

		j++;

		__real__ fft_m[m] = i_sample;
		__imag__ fft_m[m] = q_sample;

		__real__ fft_in[i]  = i_sample;
		__imag__ fft_in[i]  = q_sample;
		m++;
	}

	// STEP 3: convert the time domain samples to  frequency domain
	my_fftw_execute(plan_fwd);

	//STEP 3B: this is a side line, we use these frequency domain
	// values to paint the spectrum in the user interface
	// I discovered that the raw time samples give horrible spectrum
	// and they need to be multiplied wiht a window function 
	// they use a separate fft plan
	// NOTE: the spectrum update has nothing to do with the actual
	// signal processing. If you are not showing the spectrum or the
	// waterfall, you can skip these steps
	for (i = 0; i < MAX_BINS; i++)
		__real__ fft_in[i] *= spectrum_window[i];
	my_fftw_execute(plan_spectrum);

	// the spectrum display is updated
	spectrum_update();

	// ... back to the actual processing, after spectrum update  

	// we may add another sub receiver within the pass band later,
	// hence, the linkced list of receivers here
	// at present, we handle just the first receiver
	struct rx *r = rx_list;

	//STEP 4: we rotate the bins around by r-tuned_bin
	int shift = r->tuned_bin;
	if (r->mode == MODE_AM)
		shift = 0;
	for (i = 0; i < MAX_BINS; i++){
		int b =  i + shift;
		if (b >= MAX_BINS)
			b = b - MAX_BINS;
		if (b < 0)
			b = b + MAX_BINS;
		r->fft_freq[i] = fft_out[b];
	}


	// STEP 5:zero out the other sideband
	if (r->mode == MODE_LSB || r->mode == MODE_CWR)
		for (i = 0; i < MAX_BINS/2; i++){
			__real__ r->fft_freq[i] = 0;
			__imag__ r->fft_freq[i] = 0;	
		}
	else if (r->mode != MODE_AM)  
		for (i = MAX_BINS/2; i < MAX_BINS; i++){
			__real__ r->fft_freq[i] = 0;
			__imag__ r->fft_freq[i] = 0;	
		}

	// STEP 5B: notch filter -- REMOVED from here. It's now a time-domain
	// IIR biquad (see apply_notch_filter(), called from sound_process()
	// before this function runs), which has no bin-resolution limit --
	// the FFT-bin-zeroing approach that used to live here was confirmed
	// via diagnostic logging to quantize to ~47Hz steps regardless of the
	// requested width, since that's this FFT's fixed bin resolution.

	// DSP readout, 'before' side: snapshot the passband bins as they are now,
	// ahead of the frequency-domain noise stages below (display only).
	int dsp_first = 0, dsp_count = 0, dsp_stride = 1, dsp_measure = 0;
	if ((baseline_nr_enabled || formant_protect_enabled || formant_valley_enabled) &&
		dsp_nr_bins(r, &dsp_first, &dsp_count, &dsp_stride)){
		dsp_nr_snapshot(r, dsp_first, dsp_count, dsp_stride, dsp_pre_power);
		dsp_measure = 1;
	}

	// STEP 5C: NRVL noise reduction -- MMSE log-spectral-amplitude gain with
	// minimum-statistics noise tracking (see nrm_process() above). nr_threshold
	// (0-20) is the NRVL level.
	{
		static int nrm_was_on = 0;
		if (baseline_nr_enabled && !RX_DSP_BYPASS()){  // not on FreeDV: it would damage the modem signal
			if (!nrm_was_on)
				nrm_reset();       // start fresh each time it is switched on
			nrm_process(r->fft_freq, nr_threshold);
		}
		nrm_was_on = baseline_nr_enabled && !RX_DSP_BYPASS();
	}

	// STEP 5D: Real-Time Formant Enhancer -- FMNT (peak boost) and FVLY
	// (valley suppression) are fully independent, separately-toggled
	// features that happen to share the same underlying per-bin
	// classification (peak vs valley, from the live LPC envelope). Either
	// can be on with the other off.
	//   - FMNT: PEAK bins (lpc_envelope[k] > 1.0) get boosted, scaled by
	//     valley_atten (0-20). 400-700Hz is excluded from this boost
	//     specifically: that range typically holds F1, the strongest
	//     resonance in most voiced speech, and boosting it is a
	//     well-known cause of a "boomy"/"chesty" character. F2/F3
	//     (roughly 900Hz-3.5kHz) still get boosted normally.
	//   - FVLY: VALLEY bins (everywhere else, i.e. NOT a formant peak)
	//     get suppressed, scaled by formant_valley_level (1-20) -- the
	//     same residual-floor principle Baseline NR already uses (retain
	//     a minimum fraction rather than fully zeroing) to avoid
	//     musical-noise artifacts from over-suppression.
	if ((formant_protect_enabled || formant_valley_enabled) && !RX_DSP_BYPASS()){
		static double sum_before_fmnt = 0, sum_after_fmnt = 0;
		static time_t last_fmnt_log = 0;
		int k;
		double hz_per_bin = 96000.0 / MAX_BINS;

		for (k = 0; k < MAX_BINS; k++){
			double mag = cabs(r->fft_freq[k]);

			double bin_freq_hz = (k <= MAX_BINS/2) ? (k * hz_per_bin) : ((k - MAX_BINS) * hz_per_bin);
			double abs_freq_hz = fabs(bin_freq_hz);

			double formant_factor = lpc_envelope[k];
			if (formant_factor > 2.0)
				formant_factor = 2.0;

			double gain = 1.0;

			if (formant_factor > 1.0){
				// Peak bin -- boost only if FMNT is on, unless inside the
				// F1-exclusion zone.
				if (formant_protect_enabled && (abs_freq_hz < 400.0 || abs_freq_hz > 700.0)){
					double peak_strength = formant_factor - 1.0;
					if (peak_strength > 1.0)
						peak_strength = 1.0;
					gain = 1.0 + (valley_atten * 0.15) * peak_strength;  // level 20, strongest peak -> 4x
				}
				// else: FMNT off, or inside 400-700Hz -- no boost, gain stays 1.0
			}
			else{
				// Valley bin -- suppress only if FVLY is on, with a
				// residual floor to avoid musical-noise artifacts.
				if (formant_valley_enabled){
					double valley_depth = 1.0 - formant_factor;   // 0 (right at average) to 1 (deep valley)
					double suppress_amount = (formant_valley_level * 0.04) * valley_depth;  // level 20, deepest valley -> 0.8
					gain = 1.0 - suppress_amount;
					if (gain < 0.15)
						gain = 0.15;   // residual floor, same principle as NR_RESIDUAL
				}
				// else: FVLY off -- no suppression, gain stays 1.0
			}

			// smooth the gain over time (about 3 blocks): on noise alone the
			// envelope wanders from block to block and an unsmoothed gain
			// is heard as gurgle
			static double gsm[MAX_BINS];
			static int gsm_init = 0;
			if (!gsm_init){ for (int q = 0; q < MAX_BINS; q++) gsm[q] = 1.0; gsm_init = 1; }
			gsm[k] += 0.3 * (gain - gsm[k]);
			gain = gsm[k];

			double new_mag = mag * gain;
			sum_before_fmnt += mag;
			sum_after_fmnt += new_mag;

			double phase = carg(r->fft_freq[k]);
			r->fft_freq[k] = new_mag * cexp(I * phase);
		}

		// DIAGNOSTIC: prints once per second while either FMNT or FVLY is
		// on -- real measurable magnitude before/after.
		time_t now_fmnt = time(NULL);
		if (now_fmnt != last_fmnt_log){
			printf("[FMNT DIAG] fmnt=%s(%.1f) fvly=%s(%d) sum_before=%.1f sum_after=%.1f\n",
				formant_protect_enabled ? "ON" : "OFF", valley_atten,
				formant_valley_enabled ? "ON" : "OFF", formant_valley_level,
				sum_before_fmnt, sum_after_fmnt);
			sum_before_fmnt = 0;
			sum_after_fmnt = 0;
			last_fmnt_log = now_fmnt;
		}
	}

	// DSP readout, 'after' side: same bins again now that NRVL / FVLY / FMNT
	// have run, and publish how much quieter the noise got.
	if (dsp_measure)
		dsp_nr_update(r, dsp_first, dsp_count, dsp_stride);
	else
		dsp_nr_idle();

	// STEP 6: apply the filter to the signal,
	// in frequency domain we just multiply the filter
	// coefficients with the frequency domain samples
	for (i = 0; i < MAX_BINS; i++)
		r->fft_freq[i] *= r->filter->fir_coeff[i];

	//STEP 7: convert back to time domain	
	my_fftw_execute(r->plan_rev);

	//STEP 8 : AGC
	agc2(r);
	
	//STEP 9: send the output back to where it needs to go
	int is_digital = 0;

	if (rx_list->output == 0){
		if (r->mode == MODE_AM)
			for (i= 0; i < MAX_BINS/2; i++){
				double sample_d = cabs(r->fft_time[i+(MAX_BINS/2)]);
				// Clipping protection: this path is now fed by the new
				// Formant Enhancer's magnitude boost upstream, which could
				// otherwise push values past int32_t range here with no
				// existing guard -- same class of issue found and fixed
				// in Contour/Notch.
				if (sample_d > 2000000000.0) sample_d = 2000000000.0;
				if (sample_d < -2000000000.0) sample_d = -2000000000.0;
				int32_t sample = (int32_t)sample_d;
				//keep transmit buffer empty
				output_speaker[i] = sample;
				output_tx[i] = 0;
			}
		else
			for (i= 0; i < MAX_BINS/2; i++){
				double sample_d = cimag(r->fft_time[i+(MAX_BINS/2)]);
				if (sample_d > 2000000000.0) sample_d = 2000000000.0;
				if (sample_d < -2000000000.0) sample_d = -2000000000.0;
				int32_t sample = (int32_t)sample_d;
				//keep transmit buffer empty
				output_speaker[i] = sample;
				output_tx[i] = 0;
			}

		//push the samples to the remote audio queue, decimated to 16000 samples/sec
		for (i = 0; i < MAX_BINS/2; i += 6)
			q_write(&qremote, output_speaker[i]);

	}

	if (mute_count){
		memset(output_speaker, 0, MAX_BINS/2 * sizeof(int32_t));
		mute_count--;
	}

	//push the data to any potential modem 
	modem_rx(rx_list->mode, output_speaker, MAX_BINS/2);
}

void read_power(){
	uint8_t response[4];
	int16_t vfwd, vref;
	char buff[20];

	if (!in_tx)
		return;
	if(i2cbb_read_i2c_block_data(0x8, 0, 4, response) == -1)
		return;

	vfwd = vref = 0;

	memcpy(&vfwd, response, 2);
	memcpy(&vref, response+2, 2);
//	printf("%d:%d\n", vfwd, vref);
	if (vref >= vfwd)
		vswr = 100;
	else
		vswr = (10*(vfwd + vref))/(vfwd-vref);

	//here '400' is the scaling factor as our ref power output is 40 watts
	//this calculates the power as 1/10th of a watt, 400 = 40 watts
	int fwdvoltage =  (vfwd * 40)/bridge_compensation;
	fwdpower = (fwdvoltage * fwdvoltage)/400;

	int rf_v_p2p = (fwdvoltage * 126)/400;
	if (rf_v_p2p > 135 && !in_calibration){
		alc_level *= 135.0 / (1.0 * rf_v_p2p);
		printf("ALC tripped, to %d percent\n", (int)(100 * alc_level));
	}
/*	else if (alc_level < 0.95){
		printf("alc releasing to ");
		alc_level *= 1.02;
	}
*/
//	printf("alc: %g\n", alc_level);
}

static int tx_process_restart = 0;

// --- Voice Recorder (new feature) ---
// 5 channels for pre-recorded CQ calls (or any other TX audio), each up to
// 1 minute at the confirmed 96kHz mic sample rate. Playback actually
// transmits: it substitutes the recorded buffer for live input_mic
// samples at unity gain and runs it through the full normal TX chain
// (MIC Compression, filtering, etc.) exactly as if it were live speech --
// then a monitor copy is written to output_speaker afterward, since
// tx_process() deliberately zeroes it for voice modes ("don't echo the
// voice modes").
//
// Timed keying sequence, per spec:
//   - First press: radio returns to normal RX/TX operation, runs normally
//     for 30ms, then keys and transmission begins.
//   - Every subsequent press (2nd, 3rd, 4th...), whether or not still
//     transmitting from the previous one: unkeys immediately back to
//     normal RX/TX operation, runs normally for 10ms, then re-keys and
//     restarts the recording from the beginning. Same 10ms pattern for
//     every repeat -- this is the repeated-CQ-call workflow, not a
//     toggle-off.
// A double-tap (explicit "play=OFF") always fully stops and unkeys,
// regardless of which phase it's in, and resets so the next press is
// treated as a fresh first press.
#define REC_CHANNELS 5
#define REC_MAX_SECONDS 60
#define REC_MAX_SAMPLES (REC_MAX_SECONDS * 96000)
#define PLAY_FIRST_PRESS_DELAY_SAMPLES 2880   // 30ms at 96kHz
#define PLAY_REPEAT_PRESS_DELAY_SAMPLES 960   // 10ms at 96kHz

static int32_t rec_buffer[REC_CHANNELS][REC_MAX_SAMPLES];
int rec_length[REC_CHANNELS] = {0};              // actual recorded samples per channel

// Save a voice-recorder channel as a mono 32-bit WAV (96 kHz) in ~/sbitx/audio so the
// recording can be copied off the radio and analysed.
static void rec_export_wav(int ch){
	if (ch < 0 || ch >= REC_CHANNELS || rec_length[ch] <= 0)
		return;
	char path[300], stamp[32];
	time_t now = time(NULL);
	struct tm *tmv = localtime(&now);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tmv);
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/sbitx/audio/mic_chan%d_%s.wav", home ? home : ".", ch, stamp);
	FILE *f = fopen(path, "wb");
	if (!f){
		printf("[REC] cannot save %s\n", path);
		return;
	}
	uint32_t n = rec_length[ch], bytes = n * 4, rate = 96000, brate = 96000 * 4, v;
	uint16_t u;
	int32_t peak = 0;
	for (uint32_t i = 0; i < n; i++){
		int32_t x = rec_buffer[ch][i];
		if (x < 0) x = -x;
		if (x > peak) peak = x;
	}
	fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
	u = 1; fwrite(&u, 2, 1, f);          // PCM
	u = 1; fwrite(&u, 2, 1, f);          // mono
	fwrite(&rate, 4, 1, f);
	fwrite(&brate, 4, 1, f);
	u = 4; fwrite(&u, 2, 1, f);          // block align
	u = 32; fwrite(&u, 2, 1, f);         // bits per sample
	fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
	fwrite(rec_buffer[ch], 4, n, f);
	fclose(f);
	printf("[REC] saved %s  (%.1f s, peak %.1f%% of full scale)\n", path, n / 96000.0, 100.0 * peak / 2147483647.0);

	// If a USB stick is plugged in (mounted under /media/pi or /media/<user>), put a copy on it too.
	const char *user = getenv("USER");
	char root[2][80];
	snprintf(root[0], sizeof(root[0]), "/media/pi");
	snprintf(root[1], sizeof(root[1]), "/media/%s", user ? user : "pi");
	int copied = 0;
	for (int r = 0; r < 2 && !copied; r++){
		DIR *d = opendir(root[r]);
		if (!d) continue;
		struct dirent *de;
		while (!copied && (de = readdir(d))){
			if (de->d_name[0] == '.') continue;
			char dest[400];
			snprintf(dest, sizeof(dest), "%s/%s/mic_chan%d_%s.wav", root[r], de->d_name, ch, stamp);
			FILE *in = fopen(path, "rb"), *out = fopen(dest, "wb");
			if (in && out){
				char buf[65536];
				size_t k;
				while ((k = fread(buf, 1, sizeof(buf), in)) > 0)
					fwrite(buf, 1, k, out);
				fflush(out);
				fsync(fileno(out));
				copied = 1;
				printf("[REC] copied to USB: %s\n", dest);
			}
			if (in) fclose(in);
			if (out) fclose(out);
		}
		closedir(d);
	}
	if (!copied)
		printf("[REC] no USB stick found; the file is only in ~/sbitx/audio\n");
}
int rec_channel_selected = 0;                    // 0-4
int rec_volume_level = 10;                       // 0-20, playback volume (10 = unity gain, matches "1 to 20 scale" convention with an explicit unity midpoint)
int rec_active = 0;                              // currently recording?
int play_active = 0;                             // true throughout the entire cycle (countdown + transmitting)
int play_keyed = 0;                              // true only during actual transmission (between key and unkey)
static int play_has_fired_once = 0;              // false = next press is a fresh first press (30ms); true = a repeat (10ms)
static int play_countdown = 0;                   // samples remaining before key/restart
static int rec_write_pos = 0;
static int play_read_pos = 0;
static int32_t play_monitor_buf[MAX_BINS];       // holds the block just fed to input_mic, for the post-tx_process monitor copy

extern int mic_comp_enabled;
extern double mic_comp_level;
void apply_mic_compression(int32_t *samples, int n_samples);

// --- 6-band graphic equalizer (new feature) ---
// Two fully independent chains, RX and TX, each with its own on/off, six
// gains and its own filter state -- they're genuinely different signal
// paths and usually want different settings. Each band is a peaking
// biquad (the same RBJ cookbook form already proven in Contour), the six
// cascaded in series. Centers span the SSB voice passband rather than a
// full-range music spread. Gains are whole dB, -EQ_MAX_DB..+EQ_MAX_DB.
//
// The settings arrive from the GTK popups as "eq_rx_on", "eq_rx_b0".."b5"
// (and the eq_tx_ equivalents). They're stored as ordinary hidden fields
// on the GTK side, so they persist across restarts and are re-sent here
// at startup by the normal settings-restore path.
#define EQ_BANDS 6
#define EQ_MAX_DB 20
#define EQ_Q 1.5      // roughly one octave of bandwidth per band

static const double eq_center_hz[EQ_BANDS] = {300.0, 600.0, 1000.0, 1500.0, 2000.0, 2800.0};

struct eq_chain {
	volatile int enabled;
	volatile int gain_db[EQ_BANDS];
	volatile int dirty;     // coefficients need recomputing
	volatile int reset;     // clear filter state at the start of the next block
	double b0[EQ_BANDS], b1[EQ_BANDS], b2[EQ_BANDS], a1[EQ_BANDS], a2[EQ_BANDS];
	double z1[EQ_BANDS], z2[EQ_BANDS];
};

// dirty starts at 1 so coefficients are always computed before first use --
// otherwise an enabled-but-never-adjusted chain would run with all-zero
// coefficients and output silence.
static struct eq_chain eq_rx = { .dirty = 1 };
static struct eq_chain eq_tx = { .dirty = 1 };

static void eq_update_coeffs(struct eq_chain *c){
	const double fs = 96000.0;
	int i;
	for (i = 0; i < EQ_BANDS; i++){
		double A = pow(10.0, c->gain_db[i] / 40.0);
		double w0 = 2.0 * M_PI * eq_center_hz[i] / fs;
		double alpha = sin(w0) / (2.0 * EQ_Q);
		double cw = cos(w0);
		double a0 = 1.0 + alpha / A;
		c->b0[i] = (1.0 + alpha * A) / a0;
		c->b1[i] = (-2.0 * cw) / a0;
		c->b2[i] = (1.0 - alpha * A) / a0;
		c->a1[i] = (-2.0 * cw) / a0;
		c->a2[i] = (1.0 - alpha / A) / a0;
	}
}

static void apply_eq(struct eq_chain *c, int32_t *samples, int n_samples){
	int i, b;

	if (!c->enabled)
		return;

	if (c->reset){
		c->reset = 0;
		memset(c->z1, 0, sizeof(c->z1));
		memset(c->z2, 0, sizeof(c->z2));
	}
	if (c->dirty){
		// Cleared BEFORE computing: if a gain changes while this runs, it
		// re-flags dirty and gets picked up on the next block instead of
		// being lost.
		c->dirty = 0;
		eq_update_coeffs(c);
	}

	int any = 0;
	for (b = 0; b < EQ_BANDS; b++)
		if (c->gain_db[b])
			any = 1;
	if (!any)
		return;   // all bands flat -- nothing to do

	for (i = 0; i < n_samples; i++){
		double x = (double)samples[i];
		for (b = 0; b < EQ_BANDS; b++){
			double y = c->b0[b] * x + c->z1[b];
			c->z1[b] = c->b1[b] * x - c->a1[b] * y + c->z2[b];
			c->z2[b] = c->b2[b] * x - c->a2[b] * y;
			x = y;
		}
		// Same overflow protection as every other stage that adds gain.
		if (x > 2000000000.0) x = 2000000000.0;
		if (x < -2000000000.0) x = -2000000000.0;
		samples[i] = (int32_t)x;
	}
}

// Live spectrum while transmitting. The RX path stops updating the display
// bins in TX, so the spectrum and waterfall used to freeze. This is called
// after the transmit filter, so it shows what is actually sent (the voice
// passband, not the mic's raw noise). Audio frequency f is drawn f Hz above
// the dial frequency for USB, below it for LSB (both sides for AM), with the
// same smoothing and dB scale as the receive spectrum.
static void tx_spectrum_update(int lsb, int am){
	// Plot what the transmit filter really lets through: upper sideband audio
	// sits in the low FFT bins (shown right of the centre line), lower sideband
	// audio in the high bins (shown left of the centre line). AM shows both sides alike.
	for (int i = 1269; i < 1803; i++){
		int idx;
		// NOTE: the screen draws a LOWER bin number further to the RIGHT
		// (x = width - ...), so positive audio frequencies (USB) map to bins
		// below 1536 and negative ones (LSB) to bins above 1536.
		if (am)
			idx = abs(i - 1536);
		else if (i <= 1536)
			idx = 1536 - i;
		else
			idx = MAX_BINS - (i - 1536);
		double mag = (idx >= 0 && idx < MAX_BINS) ? cabs(fft_out[idx]) : 0.0;
		if (mag < 0.01) mag = 0.01;
		fft_bins[i] = ((1.0 - spectrum_speed) * fft_bins[i]) + (spectrum_speed * mag);
		spectrum_plot[i] = power2dB(cnrmf(fft_bins[i]));
	}
}

void tx_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	int i;
	double i_sample, q_sample, i_carrier;

	struct rx *r = tx_list;

	//fix the burst at the start of transmission
	if (tx_process_restart){
    fft_reset_m_bins();
		tx_process_restart = 0;
	} 

	if (mute_count && (r->mode == MODE_USB || r->mode == MODE_LSB 
		|| r->mode == MODE_AM)){
		memset(input_mic, 0, n_samples * sizeof(int32_t));
		mute_count--;
	}

	// TX equalizer first, then compression -- the compressor should react
	// to the tone-shaped signal, not the raw mic. Returns immediately if
	// the TX EQ is off.
	if (!fdv_voice_mode())
		apply_eq(&eq_tx, input_mic, n_samples);   // not on FreeDV: the modem signal must stay untouched

	if (mic_comp_enabled && !fdv_voice_mode()){
		apply_mic_compression(input_mic, n_samples);
	}
	//first add the previous M samples
	for (i = 0; i < MAX_BINS/2; i++)
		fft_in[i]  = fft_m[i];

	int m = 0;
	int j = 0;

	//double max = -10.0, min = 10.0;
	//gather the samples into a time domain array 
	for (i= MAX_BINS/2; i < MAX_BINS; i++){

		if (r->mode == MODE_2TONE)
			i_sample = (1.0 * (vfo_read(&tone_a) 
										+ vfo_read(&tone_b) )) / 50000000000.0;
		else if (r->mode == MODE_CALIBRATE)
			i_sample = (1.0 * (vfo_read(&tone_a))) / 30000000000.0;
		else if (r->mode == MODE_CW || r->mode == MODE_CWR || r->mode == MODE_FT8)
			i_sample = modem_next_sample(r->mode) / 3;
		else if (r->mode == MODE_AM){
			//double modulation = (1.0 * vfo_read(&tone_a)) / 1073741824.0;
	  		double modulation = (1.0 * input_mic[j]) / 200000000.0;
			if (modulation < -1.0)
				modulation = -1.0;
			i_carrier= (1.0  * vfo_read(&am_carrier))/ 50000000000.0 ; 
	  		i_sample =  (1.0 + modulation) * i_carrier;
		}
		else 
	  		i_sample = (1.0 * input_mic[j]) / 2000000000.0;
		
		//clip the overdrive to prevent damage up the processing chain, PA
		if (r->mode == MODE_USB || r->mode == MODE_LSB || r->mode == MODE_AM){
			if (i_sample < (-1.0 * voice_clip_level))
				i_sample = -1.0 * voice_clip_level;
			else if (i_sample > voice_clip_level)
				i_sample = voice_clip_level;
		}

		//don't echo the voice modes
		if (r->mode == MODE_USB || r->mode == MODE_LSB || r->mode == MODE_AM 
			|| r->mode == MODE_NBFM)
			output_speaker[j] = 0;
		else
			output_speaker[j] = i_sample * sidetone;
	  	q_sample = 0;

	  	j++;

	  	__real__ fft_m[m] = i_sample;
	  	__imag__ fft_m[m] = q_sample;

	  	__real__ fft_in[i]  = i_sample;
	  	__imag__ fft_in[i]  = q_sample;
	  	m++;
	}

	//push the samples to the remote audio queue, decimated to 16000 samples/sec
	for (i = 0; i < MAX_BINS/2; i += 6)
		q_write(&qremote, output_speaker[i]);

	//convert to frequency
	fftw_execute(plan_fwd);

	// NOTE: fft_out holds the fft output (in freq domain) of the 
	// incoming mic samples 
	// the naming is unfortunate

	// apply the filter
	// The SWR sweep / calibration carrier is a +700 Hz tone on the upper
	// sideband side. In LSB the tx filter only passes the lower sideband and
	// would remove it completely (0.0 W), so the test carrier skips the filter.
	if (r->mode != MODE_CALIBRATE)
		for (i = 0; i < MAX_BINS; i++)
			fft_out[i] *= tx_filter->fir_coeff[i];

	tx_spectrum_update(r->mode == MODE_LSB || r->mode == MODE_CWR, r->mode == MODE_AM);

	// the usb extends from 0 to MAX_BINS/2 - 1, 
	// the lsb extends from MAX_BINS - 1 to MAX_BINS/2 (reverse direction)
	// zero out the other sideband

	// TBD: Something strange is going on, this should have been the otherway

	if (r->mode == MODE_LSB || r->mode == MODE_CWR)
		// zero out the LSB
		for (i = 0; i < MAX_BINS/2; i++){
			__real__ fft_out[i] = 0;
			__imag__ fft_out[i] = 0;	
		}
	else if (r->mode != MODE_AM)
		// zero out the USB
		for (i = MAX_BINS/2; i < MAX_BINS; i++){
			__real__ fft_out[i] = 0;
			__imag__ fft_out[i] = 0;	
		}

	//now rotate to the tx_bin 
	//rememeber the AM is already a carrier modulated at 24 KHz
	int shift = tx_shift;
	if (r->mode == MODE_AM)
		shift = 0;
	for (i = 0; i < MAX_BINS; i++){
		int b = i + shift;
		if (b >= MAX_BINS)
			b = b - MAX_BINS;
		if (b < 0)
			b = b + MAX_BINS;
		r->fft_freq[b] = fft_out[i];
	}

	// the spectrum display is updated
	//spectrum_update();

	//convert back to time domain	
	fftw_execute(r->plan_rev);
	int min = 10000000;
	int max = -10000000;
	float scale = volume;
	for (i= 0; i < MAX_BINS/2; i++){
		double s = creal(r->fft_time[i+(MAX_BINS/2)]);
		output_tx[i] = s * scale * tx_amp * alc_level;
		if (min > output_tx[i])
			min = output_tx[i];
		if (max < output_tx[i])
			max = output_tx[i];	
			//output_tx[i] = 0;
	}
//	printf("min %d, max %d\n", min, max);

	read_power();
	sdr_modulation_update(output_tx, MAX_BINS/2, tx_amp);	
}

// Sample-and-hold impulse noise blanker. Tracks a slow-adapting rolling
// average of sample magnitude (representing the "normal" signal level) and
// replaces any sample that spikes well above it with the last known-good
// sample, rather than zeroing it (zeroing itself produces an audible click;
// holding the previous value is a much gentler substitution). The rolling
// average is deliberately NOT updated while actively blanking, so a
// sustained burst of impulses doesn't drag the average up and hide itself.
void apply_noise_blanker(int32_t *samples, int n_samples){
	static int32_t last_good = 0;
	static double avg_mag = 0.0;
	static int hold_count = 0;
	static int blanked_count = 0;
	static time_t last_log = 0;
	int i;

	for (i = 0; i < n_samples; i++){
		double mag = fabs((double)samples[i]);

		if (hold_count == 0)
			avg_mag = (1.0 - NB_ALPHA) * avg_mag + NB_ALPHA * mag;

		// Inverted so higher NBVL (1-10) means MORE aggressive, matching
		// NRVL/VALY's convention -- confirmed via direct formula check
		// that NB was previously running the opposite direction (raw
		// nb_threshold as the multiplier meant a HIGHER number required a
		// BIGGER spike to trigger, i.e. was actually gentler).
		// 12x the average at level 1 down to 4x at level 20. Never below 4x: at
		// 4x a Gaussian noise sample is blanked about once in 700, so only real
		// spikes go. The old 11 - level/2 fell to 1x-3x above level 16 and
		// blanked ordinary noise, which was heard as crackle / gurgle.
		double effective_multiplier = 12.0 - (nb_threshold * 0.4);
		if (effective_multiplier < 4.0) effective_multiplier = 4.0;
		if (avg_mag > 0 && mag > avg_mag * effective_multiplier && hold_count < NB_MAX_HOLD){
			samples[i] = last_good;
			hold_count++;
			blanked_count++;
		}
		else{
			last_good = samples[i];
			hold_count = 0;
		}
	}

	// DIAGNOSTIC: prints once per second while NB is on, showing real,
	// measurable proof of whether this is actually running and catching
	// anything -- not just claiming to.
	time_t now = time(NULL);
	if (now != last_log){
		printf("[NB DIAG] threshold=%.1f avg_mag=%.1f blanked_samples_this_sec=%d\n",
			nb_threshold, avg_mag, blanked_count);
		blanked_count = 0;
		last_log = now;
	}
}

/*
	This is called each time there is a block of signal samples ready 
	either from the mic or from the rx IF 
*/	
// Time-domain IIR notch filter (RBJ audio cookbook formula), operating on
// the RECOVERED audio (output_speaker) after demodulation -- not the raw
// pre-demod IF. This matters: NFREQ has always meant "Hz within the
// demodulated audio", and applying a filter before demodulation would
// target a completely different, wrong frequency (the IF, not the audio).
//
// Unlike the previous FFT-bin-zeroing approach (confirmed via diagnostic
// logging to quantize to ~47Hz steps regardless of the requested width,
// since that's this radio's fixed FFT bin resolution), a biquad notch has
// no such limit -- Q = center_freq / bandwidth gives genuinely continuous,
// arbitrary Hz control.
double notch_b0=1, notch_b1=0, notch_b2=0, notch_a1=0, notch_a2=0;
double notch_z1 = 0, notch_z2 = 0;

void compute_notch_coeffs(){
	double fs = 96000.0;
	double f0 = notch_freq_hz;
	double bw = notch_width_hz;
	if (bw < 1) bw = 1;
	double w0 = 2.0 * M_PI * f0 / fs;
	double Q = f0 / bw;
	if (Q < 0.1) Q = 0.1;
	double alpha = sin(w0) / (2.0 * Q);

	double b0 = 1.0, b1 = -2.0 * cos(w0), b2 = 1.0;
	double a0 = 1.0 + alpha, a1 = -2.0 * cos(w0), a2 = 1.0 - alpha;

	notch_b0 = b0 / a0;
	notch_b1 = b1 / a0;
	notch_b2 = b2 / a0;
	notch_a1 = a1 / a0;
	notch_a2 = a2 / a0;
}

void apply_notch_filter(int32_t *samples, int n_samples){
	static double last_freq = -1, last_width = -1;
	static double sum_before = 0, sum_after = 0;
	static time_t last_notch_log = 0;
	int i;

	if (notch_freq_hz != last_freq || notch_width_hz != last_width){
		compute_notch_coeffs();
		last_freq = notch_freq_hz;
		last_width = notch_width_hz;
	}

	for (i = 0; i < n_samples; i++){
		double x = (double)samples[i];
		double y = notch_b0 * x + notch_z1;
		notch_z1 = notch_b1 * x - notch_a1 * y + notch_z2;
		notch_z2 = notch_b2 * x - notch_a2 * y;
		sum_before += fabs(x);
		sum_after += fabs(y);
		if (y > 2000000000.0) y = 2000000000.0;
		if (y < -2000000000.0) y = -2000000000.0;
		samples[i] = (int32_t)y;
	}

	// DIAGNOSTIC: prints once per second while NOTCH is on. Alongside the
	// existing before/after magnitude, this now also computes the
	// filter's actual theoretical attenuation AT THE EXACT NOTCH
	// FREQUENCY, evaluated directly from the live biquad coefficients
	// (H(z) at z=e^jw0) -- this answers "how deep is the null" precisely,
	// independent of whether a strong test tone happens to be present at
	// that exact moment, which is something ear-based listening alone
	// cannot reliably judge.
	time_t now = time(NULL);
	if (now != last_notch_log){
		double w0 = 2.0 * M_PI * notch_freq_hz / 96000.0;
		double num_re = notch_b0 + notch_b1 * cos(w0) + notch_b2 * cos(2*w0);
		double num_im = -notch_b1 * sin(w0) - notch_b2 * sin(2*w0);
		double den_re = 1.0 + notch_a1 * cos(w0) + notch_a2 * cos(2*w0);
		double den_im = -notch_a1 * sin(w0) - notch_a2 * sin(2*w0);
		double mag_num = sqrt(num_re*num_re + num_im*num_im);
		double mag_den = sqrt(den_re*den_re + den_im*den_im);
		double response = (mag_den > 1e-12) ? mag_num / mag_den : 0.0;
		double depth_db = (response > 1e-12) ? 20.0 * log10(response) : -200.0;

		printf("[NOTCH DIAG] freq=%.0fHz width=%dHz DEPTH_AT_F0=%.1fdB sum_before=%.1f sum_after=%.1f\n",
			notch_freq_hz, notch_width_hz, depth_db, sum_before, sum_after);
		sum_before = 0;
		sum_after = 0;
		last_notch_log = now;
	}
}

// Contour: the peaking-filter variant of the same RBJ biquad family as the
// notch, with a gain term (A = 10^(gain_dB/40)) so it boosts instead of
// cutting. Same Q = center_freq/bandwidth relationship as the notch.
double contour_b0=1, contour_b1=0, contour_b2=0, contour_a1=0, contour_a2=0;
double contour_z1 = 0, contour_z2 = 0;

void compute_contour_coeffs(){
	double fs = 96000.0;
	double f0 = contour_freq_hz;
	double bw = contour_width_hz;
	if (bw < 1) bw = 1;
	double w0 = 2.0 * M_PI * f0 / fs;
	double Q = f0 / bw;
	if (Q < 0.1) Q = 0.1;
	// Capped at 4.0: beyond this, a peaking filter starts to ring/resonate
	// rather than sound like a gentle presence boost -- the whole point of
	// Contour is to make that region sound BETTER, not add an artificial,
	// harsh-sounding peak. High CFREQ + narrow CWIDTH would otherwise push
	// Q well past what still sounds pleasant.
	if (Q > 4.0) Q = 4.0;
	double A = pow(10.0, contour_gain_db / 40.0);
	double alpha = sin(w0) / (2.0 * Q);

	double b0 = 1.0 + alpha * A;
	double b1 = -2.0 * cos(w0);
	double b2 = 1.0 - alpha * A;
	double a0 = 1.0 + alpha / A;
	double a1 = -2.0 * cos(w0);
	double a2 = 1.0 - alpha / A;

	contour_b0 = b0 / a0;
	contour_b1 = b1 / a0;
	contour_b2 = b2 / a0;
	contour_a1 = a1 / a0;
	contour_a2 = a2 / a0;
}

void apply_contour_filter(int32_t *samples, int n_samples){
	static double last_freq = -1, last_width = -1;
	static double sum_before = 0, sum_after = 0;
	static time_t last_contour_log = 0;
	int i;

	if (contour_freq_hz != last_freq || contour_width_hz != last_width){
		compute_contour_coeffs();
		last_freq = contour_freq_hz;
		last_width = contour_width_hz;
	}

	for (i = 0; i < n_samples; i++){
		double x = (double)samples[i];
		double y = contour_b0 * x + contour_z1;
		contour_z1 = contour_b1 * x - contour_a1 * y + contour_z2;
		contour_z2 = contour_b2 * x - contour_a2 * y;
		sum_before += fabs(x);
		sum_after += fabs(y);
		if (y > 2000000000.0) y = 2000000000.0;
		if (y < -2000000000.0) y = -2000000000.0;
		samples[i] = (int32_t)y;
	}

	// DIAGNOSTIC: prints once per second while CONTOUR is on -- real
	// before/after magnitude, should show sum_after clearly HIGHER than
	// sum_before if the boost is genuinely being applied.
	time_t now = time(NULL);
	if (now != last_contour_log){
		printf("[CONTOUR DIAG] freq=%.0fHz width=%dHz gain=%.1fdB b0=%.4f b1=%.4f b2=%.4f a1=%.4f a2=%.4f sum_before=%.1f sum_after=%.1f (after should be HIGHER if boost is working)\n",
			contour_freq_hz, contour_width_hz, contour_gain_db, contour_b0, contour_b1, contour_b2, contour_a1, contour_a2,
			sum_before, sum_after);
		sum_before = 0;
		sum_after = 0;
		last_contour_log = now;
	}
}

// --- MIC Compression (new feature) ---
// Standard feedforward audio compressor on the TX mic path: an envelope
// follower (fast attack, slower release) tracks signal level, and once it
// crosses a threshold, gain reduction is applied proportional to the
// compression ratio, with makeup gain to restore average loudness.
// Separate on/off flag, distinct from the 0-10 level (0 = lowest setting,
// still on; not the same as being off).
int mic_comp_enabled = 0;
double mic_comp_level = 0.0;

void apply_mic_compression(int32_t *samples, int n_samples){
	static double envelope = 0.0;
	int i;

	if (!mic_comp_enabled)
		return;

	double threshold = 2000000000.0 * (1.0 - mic_comp_level * 0.03);
	double ratio = 1.0 + mic_comp_level * 0.25;      // rescaled for 0-20 range (was 0.5 for 0-10) -- level 20 -> 6:1, matching old level 10
	double makeup_gain = 1.0 + mic_comp_level * 0.075;  // rescaled for 0-20 range (was 0.15 for 0-10)

	for (i = 0; i < n_samples; i++){
		double x = (double)samples[i];
		double mag = fabs(x);
		double alpha = (mag > envelope) ? 0.3 : 0.01;  // fast attack, slow release
		envelope = alpha * mag + (1.0 - alpha) * envelope;

		double gain = 1.0;
		if (envelope > threshold && threshold > 0){
			double excess_db = 20.0 * log10(envelope / threshold);
			double compressed_db = excess_db / ratio;
			gain = pow(10.0, (compressed_db - excess_db) / 20.0);
		}

		double y = x * gain * makeup_gain;
		if (y > 2000000000.0) y = 2000000000.0;
		if (y < -2000000000.0) y = -2000000000.0;
		samples[i] = (int32_t)y;
	}
}

// --- Audio Boost (new feature) ---
// A straightforward additional RX audio gain stage, for when the overall
// received volume needs a lift beyond the normal AUDIO/volume control --
// e.g. a weak station that's otherwise readable. Separate on/off flag,
// same reasoning as above.
int audio_boost_enabled = 0;
double audio_boost_level = 0.0;

void apply_audio_boost(int32_t *samples, int n_samples){
	int i;

	if (!audio_boost_enabled)
		return;

	double gain = 1.0 + audio_boost_level * 1.125;   // level 20 -> ~23.5x

	// Smooth peak limiter. A static waveshaper (the old tanh) distorts every
	// peak the instant it crosses the knee, and at 20x gain that is most of
	// the loud speech. Instead the gain itself is pulled down smoothly when a
	// peak would pass the target (fast attack, slow release), so the waveform
	// is scaled rather than bent. A gentle tanh remains only as a backstop for
	// the odd peak that gets through before the gain has caught up. The target
	// sits at about 60% of full scale: loud, but leaving headroom so the
	// codec's output stage does not clip at high volume settings.
	static double lim_g = 1.0;
	const double target = 1.3e9;
	const double knee = 1.5e9, ceil_ = 2.0e9;
	const double span = ceil_ - knee;
	for (i = 0; i < n_samples; i++){
		double y = (double)samples[i] * gain;
		double a = fabs(y);
		double need = (a > target) ? target / a : 1.0;
		if (need < lim_g)
			lim_g += 0.05 * (need - lim_g);          // attack: ~0.2 ms
		else
			lim_g += 0.00008 * (1.0 - lim_g);        // release: ~130 ms
		y *= lim_g;
		a = fabs(y);
		if (a > knee)
			a = knee + span * tanh((a - knee) / span);
		samples[i] = (int32_t)(y < 0 ? -a : a);
	}
}


// --- VOX (Voice-Operated Transmit, new feature) ---
// Monitors live mic level continuously (even during RX, since input_mic
// is always passed into sound_process() regardless of in_tx state) and
// automatically keys TX when speech is detected, unkeying after a
// configurable hang time of silence -- standard VOX behavior, avoiding
// choppy keying during natural pauses in speech.
//
// Critically, VOX only ever unkeys a session IT ITSELF started
// (vox_keyed tracks this) -- it never interferes with a manual PTT
// session, even if the mic briefly goes quiet during one.
//
// The threshold is relative to the background level of the mic, not an
// absolute number, so it needs no calibration against the mic gain.
int vox_enabled = 0;
int vox_threshold = 10;   // 0-20, higher = requires louder speech to trigger
int vox_hangtime = 10;    // 0-20, mapped to 0-2000ms of silence before unkeying
static int vox_keyed = 0;            // true only if THIS session was started by VOX, not manual PTT
static int vox_silence_samples = 0;
// The radio is keyed and unkeyed by the user interface (so the screen, meters,
// waveform and frequency retune all follow, exactly as with the PTT). VOX only
// asks: 1 = key please, 2 = unkey please. The UI polls this about 1000x/sec.
volatile int vox_ui_request = 0;
static int vox_pending = 0, vox_pending_blocks = 0;
static double vox_floor_rx = 0.0, vox_floor_tx = 0.0, vox_smooth = 0.0;

// AI noise reduction is speech-trained, so it is only applied to voice modes.
// On CW, FT8 and the other digital modes it would mangle the signal, so it is
// forced off there regardless of the AINR setting.
static int ainr_level_for_mode(void){
	if (ai_nr_level <= 0)
		return 0;
	switch (rx_list->mode){
		case MODE_USB:
		case MODE_LSB:
		case MODE_AM:
			return ai_nr_level;
		default:
			return 0;
	}
}
  // running count of continuous below-threshold samples

extern int sweep_running;   // needed here since sweep_running is declared later in this file

static int32_t *vox_mic_src = NULL;       // what VOX listens to (the real microphone, even in FreeDV)

// FreeDV only applies on the voice sidebands.
#ifdef SBITX_FREEDV
static int fdv_voice_mode(void){
	return freedv_ext_active() && rx_list && (rx_list->mode == MODE_USB || rx_list->mode == MODE_LSB);
}
#else
static int fdv_voice_mode(void){ return 0; }
#endif

static void sound_process_main(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples);

// Optional USB microphone / USB speaker wrapped around the normal processing
// (see usbaudio.c). Both are no-ops unless switched on.
void sound_process(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	if (usb_mic_active())
		usb_mic_read(input_mic, n_samples);      // replaces the mic, for TX and VOX alike

	// FreeDV: on transmit the microphone audio is replaced by the modem signal
	// (the SSB transmitter then sends it as usual). VOX must keep listening to
	// the real microphone, so a copy is kept for it.
	vox_mic_src = input_mic;
#ifdef SBITX_FREEDV
	static int32_t raw_mic[2048];
	static int fdv_was_tx = 0;
	if (fdv_voice_mode() && in_tx){
		if (!fdv_was_tx)
			freedv_ext_tx_start();
		fdv_was_tx = 1;
		if (n_samples <= 2048){
			memcpy(raw_mic, input_mic, n_samples * sizeof(int32_t));
			vox_mic_src = raw_mic;
		}
		freedv_ext_tx(input_mic, n_samples);
	}
	else
		fdv_was_tx = 0;
#endif

	sound_process_main(input_rx, input_mic, output_speaker, output_tx, n_samples);

	if (usb_speaker_active() && !in_tx){
		// volume 0-100 -> -35..0 dB, silent at 0
		int g = (rx_vol <= 0) ? 0 : (int)(1000.0 * pow(10.0, (rx_vol - 100) * 0.35 / 20.0));
		usb_speaker_write(output_speaker, n_samples, g);
	}
}

static void sound_process_main(
	int32_t *input_rx, int32_t *input_mic, 
	int32_t *output_speaker, int32_t *output_tx, 
	int n_samples)
{
	// Voice Recorder: handled here, BEFORE the normal TX/RX branch, so it
	// works independent of whether PTT is pressed -- record and playback
	// are local operations, not tied to actually transmitting. Normal
	// TX/RX processing is fully suspended while either is active, so
	// only the recording itself is heard, matching "TX has to stop and
	// RX has to stop as well -- only my recording should be visible".
	if (rec_active){
		int rem = REC_MAX_SAMPLES - rec_write_pos;
		int to_copy = (rem < n_samples) ? rem : n_samples;
		if (to_copy > 0){
			memcpy(&rec_buffer[rec_channel_selected][rec_write_pos], input_mic,
				to_copy * sizeof(int32_t));
			rec_write_pos += to_copy;
			rec_length[rec_channel_selected] = rec_write_pos;
		}
		if (rec_write_pos >= REC_MAX_SAMPLES)
			rec_active = 0;   // buffer full -- auto-stop

		// Run the TX chain on a scratch copy only so the spectrum/waveform
		// trace keeps updating; the recording above stays raw.
		{
			static int32_t scr_mic[4096], scr_spk[4096], scr_tx[4096];
			if (n_samples <= 4096){
				memcpy(scr_mic, input_mic, n_samples * sizeof(int32_t));
				tx_process(input_rx, scr_mic, scr_spk, scr_tx, n_samples);
			}
		}
		memset(output_speaker, 0, n_samples * sizeof(int32_t));   // silence during capture
		memset(output_tx, 0, n_samples * sizeof(int32_t));        // never transmit while recording
		return;
	}

	if (play_active){
		if (!play_keyed){
			// Countdown before keying -- RX/TX runs completely normally
			// during this window, whether it's the first press (30ms) or
			// a repeat press (10ms, having already been unkeyed back to
			// normal operation by the command handler). Same handling
			// either way; only the countdown length differs.
			play_countdown -= n_samples;

			if (noise_blanker_enabled){
				apply_noise_blanker(input_rx, n_samples);
			}
			rx_linear(input_rx, input_mic, output_speaker, output_tx, n_samples);
			if (notch_enabled){
				apply_notch_filter(output_speaker, n_samples);
			}
			if (contour_enabled){
				apply_contour_filter(output_speaker, n_samples);
			}
			ainr_process(output_speaker, n_samples, ainr_level_for_mode());   // AI noise reduction (SSB/AM only), before the EQ
			apply_eq(&eq_rx, output_speaker, n_samples);
			apply_audio_boost(output_speaker, n_samples);

			if (play_countdown <= 0){
				tr_switch(1);   // programmatic key -- same mechanism proven in the SWR sweep
				play_keyed = 1;
			}
			if (pf_record)
				wav_record(output_speaker, n_samples);
			return;
		}

		// Actively transmitting: substitute the recording for input_mic
		// and run it through the FULL normal TX chain (MIC Compression,
		// filtering, etc.) exactly as if it were live speech.
		int rem = rec_length[rec_channel_selected] - play_read_pos;
		int to_copy = (rem < n_samples) ? rem : n_samples;
		if (to_copy > 0){
			double gain = rec_volume_level / 10.0;   // level 10 = unity, 0 = silent, 20 = 2x
			int k;
			for (k = 0; k < to_copy; k++){
				double sample = rec_buffer[rec_channel_selected][play_read_pos + k] * gain;
				if (sample > 2000000000.0) sample = 2000000000.0;
				if (sample < -2000000000.0) sample = -2000000000.0;
				input_mic[k] = (int32_t)sample;
				play_monitor_buf[k] = (int32_t)sample;
			}
			play_read_pos += to_copy;
		}
		if (to_copy < n_samples){
			memset(&input_mic[to_copy], 0, (n_samples - to_copy) * sizeof(int32_t));
			memset(&play_monitor_buf[to_copy], 0, (n_samples - to_copy) * sizeof(int32_t));
		}

		int finished = (play_read_pos >= rec_length[rec_channel_selected]);

		tx_process(input_rx, input_mic, output_speaker, output_tx, n_samples);

		// tx_process() deliberately zeroes output_speaker for voice modes
		// ("don't echo the voice modes") -- overwrite with our own copy
		// so the operator can monitor what's actually being transmitted.
		memcpy(output_speaker, play_monitor_buf, n_samples * sizeof(int32_t));

		if (pf_record)
			wav_record(output_speaker, n_samples);

		if (finished){
			// Reached the end with no repeat press in the meantime --
			// unkey and fully stop. A repeat press before this point would
			// already have reset play_read_pos via the command handler.
			tr_switch(0);
			play_active = 0;
			play_keyed = 0;
			play_read_pos = 0;
		}
		return;
	}

	// VOX: checked after rec/play/sweep (those take priority -- VOX never
	// engages while any of them are active), before the normal dispatch.
	//
	// The trigger is RELATIVE to the room: the mic level is compared with its
	// own background level (tracked as a slow-rising minimum), so it works
	// whatever the mic gain happens to be. VXTH 0-20 = speech must be 4 to
	// 24 dB above that background (10 = 14 dB). Higher = needs louder speech.
	if (vox_enabled && !rec_active && !play_active && !sweep_running){
		double mean = 0.0, lvl = 0.0;
		int k;
		const int32_t *vox_src = vox_mic_src ? vox_mic_src : input_mic;
		for (k = 0; k < n_samples; k++)
			mean += (double)vox_src[k];
		mean /= n_samples;
		for (k = 0; k < n_samples; k++)
			lvl += fabs((double)vox_src[k] - mean);
		lvl /= n_samples;
		vox_smooth = 0.5 * vox_smooth + 0.5 * lvl;
		lvl = vox_smooth;

		double ratio = pow(10.0, (4.0 + vox_threshold) / 20.0);
		const double abs_min = 2.0e5;     // about -80 dBFS: below this it is just digital silence

		if (!in_tx){
			if (vox_floor_rx <= 0.0)
				vox_floor_rx = lvl + 1.0;
			else if (lvl < vox_floor_rx)
				vox_floor_rx = 0.8 * vox_floor_rx + 0.2 * lvl;
			else
				vox_floor_rx += (lvl - vox_floor_rx) * 0.0005;    // slow rise, ~20 s

			if (vox_pending){
				// asked the UI to key; give it up to ~1 s
				if (++vox_pending_blocks > 100){
					vox_pending = 0;
					vox_keyed = 0;
				}
			}
			else if (lvl > abs_min && lvl > vox_floor_rx * ratio){
				vox_keyed = 1;
				vox_pending = 1;
				vox_pending_blocks = 0;
				vox_silence_samples = 0;
				vox_floor_tx = lvl / ratio;     // the speech that just keyed us; settles to the real TX background in the pauses
				vox_ui_request = 1;
			}
		}
		else if (vox_keyed){
			vox_pending = 0;
			if (lvl < vox_floor_tx)
				vox_floor_tx = 0.8 * vox_floor_tx + 0.2 * lvl;
			else
				vox_floor_tx += (lvl - vox_floor_tx) * 0.0005;

			if (lvl > vox_floor_tx * ratio){
				vox_silence_samples = 0;
			}
			else{
				vox_silence_samples += n_samples;
				int hangtime_samples = (vox_hangtime * 100 * 96000) / 1000;   // 0-20 -> 0-2000ms, at 96kHz
				if (vox_silence_samples >= hangtime_samples){
					vox_keyed = 0;
					vox_silence_samples = 0;
					vox_ui_request = 2;
				}
			}
		}
	}

	// Normal operation, unchanged.
	if (in_tx)
		tx_process(input_rx, input_mic, output_speaker, output_tx, n_samples);
	else{
		if (noise_blanker_enabled && !RX_DSP_BYPASS()){
			apply_noise_blanker(input_rx, n_samples);
		}
		rx_linear(input_rx, input_mic, output_speaker, output_tx, n_samples);
#ifdef SBITX_FREEDV
		if (fdv_voice_mode())
			freedv_ext_rx(output_speaker, n_samples);   // modem audio -> decoded speech
#endif
		if (notch_enabled){
			apply_notch_filter(output_speaker, n_samples);
		}
		if (contour_enabled){
			apply_contour_filter(output_speaker, n_samples);
		}
		ainr_process(output_speaker, n_samples, ainr_level_for_mode());   // AI noise reduction (SSB/AM only), before the EQ
		apply_eq(&eq_rx, output_speaker, n_samples);   // RX equalizer, after Contour, before Audio Boost
		apply_audio_boost(output_speaker, n_samples);
	}
	if (pf_record)
		wav_record(in_tx == 0 ? output_speaker : input_mic, n_samples);
}


void set_rx_filter(){
	//on AM filter at the IF level, instead of the baseband
	if (rx_list->mode == MODE_AM){
		printf("Setting AM filter\n");
   		filter_tune(rx_list->filter, 
      		(1.0 * (24000 - rx_list->high_hz))/96000.0 , 
      		(1.0 * (24000 + rx_list->high_hz))/96000.0 , 
      	5);
	}
	else if(rx_list->mode == MODE_LSB || rx_list->mode == MODE_CWR)
    		filter_tune(rx_list->filter, 
      		(1.0 * -rx_list->high_hz)/96000.0, 
      		(1.0 * -rx_list->low_hz)/96000.0 , 
      	5);
	else
		filter_tune(rx_list->filter, 
		(1.0 * rx_list->low_hz)/96000.0, 
		(1.0 * rx_list->high_hz)/96000.0 , 
     		 5);
}

/* 
Write code that mus repeatedly so things, it is called during the idle time 
of the event loop 
*/
void loop(){
	delay(10);
}

void signal_handler(int signum){
	digitalWrite(TX_LINE, LOW);
}

void setup_audio_codec(){
	strcpy(audio_card, "hw:0");

	//configure all the channels of the mixer
	sound_mixer(audio_card, "Input Mux", 0);
	sound_mixer(audio_card, "Line", 1);
	sound_mixer(audio_card, "Mic", 0);
	sound_mixer(audio_card, "Mic Boost", 0);
	sound_mixer(audio_card, "Playback Deemphasis", 0);
 
	sound_mixer(audio_card, "Master", 10);
	sound_mixer(audio_card, "Output Mixer HiFi", 1);
	sound_mixer(audio_card, "Output Mixer Mic Sidetone", 0);

}

void setup_oscillators(){
  //initialize the SI5351

  delay(200);
  si5351bx_init();
  delay(200);
  si5351bx_setfreq(1, bfo_freq);

  delay(200);
  si5351bx_setfreq(1, bfo_freq);

  si5351_reset();
}


static int hw_init_index = 0;
static int hw_settings_handler(void* user, const char* section, 
            const char* name, const char* value)
{
  char cmd[1000];
  char new_value[200];

	if (!strcmp(name, "f_start"))
		band_power[hw_init_index].f_start = atoi(value);
	if (!strcmp(name, "f_stop"))
		band_power[hw_init_index].f_stop = atoi(value);
	if (!strcmp(name, "scale"))
		band_power[hw_init_index++].scale = atof(value);

	if (!strcmp(name, "bfo_freq"))
		bfo_freq = atoi(value);
	if (!strcmp(name, "si570_xtal"))
		si570_xtal = atoi(value);
}

static void read_hw_ini(){
	hw_init_index = 0;
	char directory[PATH_MAX];
	char *path = getenv("HOME");
	strcpy(directory, path);
	strcat(directory, "/sbitx/data/hw_settings.ini");
  if (ini_parse(directory, hw_settings_handler, NULL)<0){
    printf("Unable to load ~/sbitx/data/hw_settings.ini\nLoading default_hw_settings.ini instead\n");
		strcpy(directory, path);
		strcat(directory, "/sbitx/data/default_hw_settings.ini");
  	ini_parse(directory, hw_settings_handler, NULL);
  }
}

/*
	 the PA gain varies across the band from 3.5 MHz to 30 MHz
 	here we adjust the drive levels to keep it up, almost level
*/

// ---------------------------------------------------------------------
// Fast mixer access for the TX/RX switch. The stock sound_mixer() opens
// the ALSA mixer, loads every control and closes it again on EVERY call,
// and a PTT change makes four such calls -- tens of milliseconds of the
// delay heard when the PTT is released. This keeps one mixer open and
// remembers the Master and Capture controls; the switching logic is the
// same one sound_mixer() uses. Any other control, or any failure, falls
// back to sound_mixer().
// ---------------------------------------------------------------------
static snd_mixer_t *fm_handle = NULL;
static snd_mixer_elem_t *fm_master = NULL, *fm_capture = NULL;
static int fm_tried = 0;
static pthread_mutex_t fm_lock = PTHREAD_MUTEX_INITIALIZER;

static void fm_open(char *card){
	fm_tried = 1;
	if (snd_mixer_open(&fm_handle, 0) < 0){ fm_handle = NULL; return; }
	if (snd_mixer_attach(fm_handle, card) < 0 || snd_mixer_selem_register(fm_handle, NULL, NULL) < 0 ||
		snd_mixer_load(fm_handle) < 0){
		snd_mixer_close(fm_handle); fm_handle = NULL; return;
	}
	snd_mixer_selem_id_t *sid;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");
	fm_master = snd_mixer_find_selem(fm_handle, sid);
	snd_mixer_selem_id_set_name(sid, "Capture");
	fm_capture = snd_mixer_find_selem(fm_handle, sid);
}

static void fast_mixer(char *card, char *element, int v){
	snd_mixer_elem_t *elem = NULL;
	long mn, mx;

	// The SWR sweep keys the radio through the original, proven mixer routine.
	if (sweep_running){
		sound_mixer(card, element, v);
		return;
	}

	pthread_mutex_lock(&fm_lock);
	if (!fm_tried)
		fm_open(card);
	if (fm_handle){
		if (!strcmp(element, "Master")) elem = fm_master;
		else if (!strcmp(element, "Capture")) elem = fm_capture;
	}
	if (!elem){
		pthread_mutex_unlock(&fm_lock);
		sound_mixer(card, element, v);
		return;
	}
	if (snd_mixer_selem_has_capture_switch(elem))
		snd_mixer_selem_set_capture_switch_all(elem, v);
	else if (snd_mixer_selem_has_playback_switch(elem))
		snd_mixer_selem_set_playback_switch_all(elem, v);
	else if (snd_mixer_selem_has_playback_volume(elem)){
		snd_mixer_selem_get_playback_volume_range(elem, &mn, &mx);
		snd_mixer_selem_set_playback_volume_all(elem, (long)v * mx / 100);
	}
	else if (snd_mixer_selem_has_capture_volume(elem)){
		snd_mixer_selem_get_capture_volume_range(elem, &mn, &mx);
		snd_mixer_selem_set_capture_volume_all(elem, (long)v * mx / 100);
	}
	pthread_mutex_unlock(&fm_lock);
}

void set_tx_power_levels(){
 // printf("Setting tx_power to %d, gain to %d\n", tx_power_watts, tx_gain);
	//int tx_power_gain = 0;

	//search for power in the approved bands
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		if (band_power[i].f_start <= freq_hdr && freq_hdr <= band_power[i].f_stop){
		
			//next we do a decimal coversion of the power reduction needed
			tx_amp = (1.0 * tx_drive * band_power[i].scale);  
		}	
	}
//	printf("tx_amp is set to %g for %d drive\n", tx_amp, tx_drive);
	//we keep the audio card output 'volume' constant'
	fast_mixer(audio_card, "Master", 95);
	// With the USB microphone the radio's own mic input is not used, so its
	// capture gain is left where RX needs it: dropping it to the MIC value and
	// raising it again on release made the receive audio return slowly.
	if (!usb_mic_active())
		fast_mixer(audio_card, "Capture", tx_gain);
	alc_level = 1.0;
}

/* calibrate power on all bands */

void calibrate_band_power(struct power_settings *b){
	
	set_rx1(b->f_start + 35000);
	printf("*calibrating for %d\n", freq_hdr);
	tx_list->mode = MODE_CALIBRATE;
	tx_drive = 100;

	int i, j;

//	double scale_delta = b->scale / 25;
	double scaling_factor = 0.0001;
	b->scale = scaling_factor;
 	set_tx_power_levels();
	delay(50);

	tr_switch(1);
	delay(100);

	for (i = 0; i < 200 && b->scale < 0.015; i++){
		scaling_factor *= 1.1;
		b->scale = scaling_factor;
 		set_tx_power_levels();
		delay(50); //let the new power levels take hold		

		int avg = 0;
		//take many readings to get a peak
		for (j = 0; j < 10; j++){
			delay(20);
			avg += fwdpower /10; //fwdpower in 1/10th of a watt
//			printf("  avg %d, fwd %d scale %g\n", avg, fwdpower, b->scale);
		}
		avg /= 10;
		printf("*%d, f %d : avg %d, max = %d\n", i, b->f_start, avg, b->max_watts);
		if (avg >= b->max_watts)
				break;
	}
	tr_switch(0);
	printf("*tx scale for %d is set to %g\n", b->f_start, b->scale);
	delay(100);	
}

// --- SWR Sweep (new feature) ---
// Repeated-reading statistical check at the current VFO frequency.
// Redesigned after examining drexjj's sbitx fork (github.com/drexjj/sbitx,
// src/swr_monitor.c), which has a mature, working SWR sweep -- two
// concrete things adopted directly from it:
//
// 1. It uses MODE_CALIBRATE to generate an actual test carrier (this
//    mode's sample source is an internal oscillator, tone_a, entirely
//    independent of mic input -- see the MODE_CALIBRATE branch in
//    tx_process() above). An earlier version of this sweep keyed TX but
//    fed silence into output_tx, meaning there was no real RF signal for
//    the power bridge to measure at all -- that's what was producing
//    constant/invalid readings. This version sets tx_list->mode to
//    MODE_CALIBRATE and lets the NORMAL tx_process() dispatch run
//    (rather than suspending it), so the carrier is generated exactly
//    the same way calibrate_band_power() already does above.
// 2. Rather than a full non-blocking redesign, it stays a single
//    function but pumps pending GTK events during every sleep interval
//    (pump_gtk_events(), a GTK-side helper) -- this keeps the UI fully
//    responsive (including an abort button press) without the added
//    complexity and risk of restructuring sound_process() into a state
//    machine. Same technique, called out explicitly in their source.
// Also per their own comment ("Power and VSWR update every 100ms, so
// each step must be >= 200ms"), reading spacing here is 250ms, not the
// 50ms an earlier version used -- too fast a spacing was likely reading
// stale values.
//
// Two independent safety layers, both still required:
// 1. sweep_drive_cap -- a hard ceiling on the tx_drive value used only
//    during the sweep (restored to the user's normal drive afterward).
//    PLACEHOLDER default below -- MUST be replaced with a user-verified
//    value once tested against a real wattmeter; this number alone is
//    not to be trusted blind.
// 2. A live cutoff on the ACTUAL measured fwdpower (already calibrated
//    against a real reference elsewhere in this file) -- if real forward
//    power ever exceeds 5W (50 in fwdpower's 1/10-watt units) plus a
//    small margin, the sweep aborts and unkeys immediately, regardless
//    of what the drive cap was set to.
int sweep_drive_cap = 30;        // PLACEHOLDER -- NOT yet verified against a wattmeter
int sweep_running = 0;           // true for the duration of the sweep, for UI display and re-entrancy guard
int sweep_iterations = 5;        // 1-10, how many readings to average
int sweep_avg_vswr_x10 = 0;      // fixed-point result, matches vswr's own scale
int sweep_stddev_vswr_x10 = 0;
static int sweep_cancel_requested = 0;   // set by the "sweep_abort" command, checked every reading

extern void write_console(int style, char *text);   // GTK-side console/log area (the empty left panel) -- style 5 = FONT_LOG
extern void pump_gtk_events(void);                   // GTK-side helper, keeps the UI responsive during the sleeps below

#define SWEEP_READING_SPACING_MS 250   // matches drexjj's own tested minimum (>=200ms; power/VSWR update every 100ms)

// Sleeps for total_ms while repeatedly pumping GTK events, so button
// presses (including an abort) are processed throughout. Returns 1 if
// sweep_cancel_requested became true during the sleep, 0 otherwise.
static int sweep_sleep_with_cancel(int total_ms){
	int elapsed = 0;
	while (elapsed < total_ms){
		pump_gtk_events();
		if (sweep_cancel_requested)
			return 1;
		usleep(20000);
		elapsed += 20;
	}
	return 0;
}

void run_swr_sweep(void){
	// Frequency is always whatever the VFO is currently tuned to -- this
	// is a repeated-reading statistical check, not a frequency scan. The
	// band-membership check stays as a safety/legality baseline: never
	// transmit outside a recognized amateur band regardless of what
	// measurement is being taken.
	if (!((freq_hdr >= 7000000 && freq_hdr <= 7200000) ||
	      (freq_hdr >= 14000000 && freq_hdr <= 14350000) ||
	      (freq_hdr >= 18068000 && freq_hdr <= 18168000) ||
	      (freq_hdr >= 21000000 && freq_hdr <= 21450000) ||
	      (freq_hdr >= 28000000 && freq_hdr <= 29700000))){
		write_console(5, "[SWR SWEEP] current frequency is not within a recognized amateur band -- aborting\r\n");
		return;
	}

	sweep_running = 1;
	sweep_cancel_requested = 0;
	sweep_avg_vswr_x10 = 0;
	sweep_stddev_vswr_x10 = 0;

	int original_drive = tx_drive;
	int original_mode = tx_list->mode;

	tx_drive = sweep_drive_cap;
	tx_list->mode = MODE_CALIBRATE;   // generates an actual test carrier, independent of mic input
	set_tx_power_levels();

	char msg[100];
	sprintf(msg, "\r\n[SWR SWEEP] %d iterations at %d Hz, power %d\r\n",
		sweep_iterations, freq_hdr, sweep_drive_cap);
	write_console(5, msg);

	tr_switch(1);
	if (sweep_sleep_with_cancel(100))   // let RF/relay stabilize, matches calibrate_band_power()'s own timing
		goto canceled;

	double sum_vswr = 0.0, sum_vswr_sq = 0.0;
	int n = 0, safety_tripped = 0, i;

	for (i = 0; i < sweep_iterations; i++){
		if (sweep_sleep_with_cancel(SWEEP_READING_SPACING_MS))
			goto canceled;

		double this_vswr = vswr / 10.0;

		// With very little forward power the bridge reading is mostly noise and
		// the SWR comes out as 10 whatever the antenna is. Such a reading is
		// shown but left out of the average.
		if (fwdpower < 10){
			sprintf(msg, "  reading %d/%d: fwd=%.1fW is too low for a valid SWR (shown %.1f) -- raise SWPW\r\n",
				i + 1, sweep_iterations, fwdpower / 10.0, this_vswr);
			write_console(5, msg);
			continue;
		}

		sum_vswr += this_vswr;
		sum_vswr_sq += this_vswr * this_vswr;
		n++;

		sprintf(msg, "  reading %d/%d: SWR=%.1f  fwd=%.1fW\r\n",
			i + 1, sweep_iterations, this_vswr, fwdpower / 10.0);
		write_console(5, msg);

		if (fwdpower > 55){   // 5.5W margin above the 5W ceiling
			sprintf(msg, "  SAFETY CUTOFF: fwd=%.1fW exceeds limit -- aborting\r\n", fwdpower / 10.0);
			write_console(5, msg);
			safety_tripped = 1;
			break;
		}
	}

	goto finish;

canceled:
	write_console(5, "[SWR SWEEP] canceled by user\r\n");

finish:
	tr_switch(0);
	tx_drive = original_drive;
	tx_list->mode = original_mode;
	set_tx_power_levels();
	// set_tx_power_levels() unconditionally sets Master volume to a fixed
	// TX-specific level (95), which is correct while actually
	// transmitting but wrong here -- we're back in RX by this point, so
	// explicitly restore the real RX volume the AUDIO slider is set to.
	fast_mixer(audio_card, "Master", RX_MASTER_VOL);

	if (n == 0 && !sweep_cancel_requested)
		write_console(5, "[SWR SWEEP] no valid reading: forward power stayed under 1 W. Raise SWPW and try again.\r\n");

	if (n > 0){
		double mean = sum_vswr / n;
		double variance = (sum_vswr_sq / n) - (mean * mean);
		if (variance < 0) variance = 0;   // guard against floating-point noise
		double stddev = sqrt(variance);

		sweep_avg_vswr_x10 = (int)(mean * 10.0 + 0.5);
		sweep_stddev_vswr_x10 = (int)(stddev * 10.0 + 0.5);

		sprintf(msg, "[SWR SWEEP] complete. AVG SWR = %.2f   STD DEV = %.2f (n=%d)%s\r\n",
			mean, stddev, n, safety_tripped ? "  [SAFETY CUTOFF]" : "");
		write_console(5, msg);
	}

	sweep_running = 0;
	sweep_cancel_requested = 0;
}


static void save_hw_settings(){
	static int last_save_at = 0;
	char file_path[200];	//dangerous, find the MAX_PATH and replace 200 with it

	char *path = getenv("HOME");
	strcpy(file_path, path);
	strcat(file_path, "/sbitx/data/hw_settings.ini");

	FILE *f = fopen(file_path, "w");
	if (!f){
		printf("Unable to save %s : %s\n", file_path, strerror(errno));
		return;
	}

	fprintf(f, "bfo_freq=%d\n\n", bfo_freq);
	//now save the band stack
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		fprintf(f, "[tx_band]\nf_start=%d\nf_stop=%d\nscale=%g\n\n", 
			band_power[i].f_start, band_power[i].f_stop, band_power[i].scale);
	}

	fclose(f);
}

pthread_t calibration_thread;

void *calibration_thread_function(void *server){

	int old_freq = freq_hdr;
	int old_mode = tx_list->mode;
	int	old_tx_drive = tx_drive;

	in_calibration = 1;
	for (int i = 0; i < sizeof(band_power)/sizeof(struct power_settings); i++){
		calibrate_band_power(band_power + i);
	}
	in_calibration = 0;

	set_rx1(old_freq);
	tx_list->mode = old_mode;
	tx_drive = old_tx_drive;
	save_hw_settings();
	printf("*Finished band power calibrations\n");
}

void tx_cal(){
	printf("*Starting tx calibration, with dummy load connected\n");
 	pthread_create( &calibration_thread, NULL, calibration_thread_function, 
		(void*)NULL);
}

// The receiver AGC gain from just before a transmission. The switch back to receive
// makes a loud click that would pull the AGC down, and the AGC climbs back so slowly
// (about 10 s) that it sounded like a long delay after the PTT. The gain is restored
// instead; the AGC's fast attack still lowers it at once if a strong signal is there.
static double agc_gain_before_tx = 0.0;

void tr_switch_de(int tx_on){
		if (tx_on){
			if (rx_list && rx_list->agc_gain > 0.0) agc_gain_before_tx = rx_list->agc_gain;
			//mute it all and hang on for a millisecond
			fast_mixer(audio_card, "Master", 0);
			fast_mixer(audio_card, "Capture", 0);
			delay(1);

			//now switch of the signal back
			//now ramp up after 5 msecs
			delay(2);
			digitalWrite(TX_LINE, HIGH);
			mute_count = 8;
			tx_process_restart = 1;
			//give time for the reed relay to switch
      delay(2);
			set_tx_power_levels();
			in_tx = 1;
			//finally ramp up the power 
			if (tr_relay){
				set_lpf_40mhz(freq_hdr);
				delay(10); //debounce the lpf relays
			}
			digitalWrite(TX_POWER, HIGH);
			spectrum_reset();
		}
		else {
			in_tx = 0;
			//mute it all and hang on
			fast_mixer(audio_card, "Master", 0);
			fast_mixer(audio_card, "Capture", 0);
			delay(1);
      fft_reset_m_bins();
			mute_count = MUTE_MAX;

			// Fix for a real bug in the base firmware: agc2() (which
			// decrements agc_loop, the AGC's hang-time countdown) only
			// runs during RX -- it's never called while transmitting. If
			// a loud signal set agc_loop to a large value (agc_speed can
			// be up to 300, at "slow" AGC) right before PTT was pressed,
			// that countdown is completely frozen for the entire TX
			// duration. On unkey, the AGC would otherwise have to wait
			// out that whole stale countdown -- up to several seconds --
			// before it's even allowed to start adjusting gain to
			// current conditions, which is exactly the "takes a few
			// seconds to come back to the same volume" symptom. Resetting
			// it here lets the AGC start adjusting immediately.
			rx_list->agc_loop = 0;
			agc_post_tx = 120;
			if (agc_gain_before_tx > 0.0) rx_list->agc_gain = agc_gain_before_tx;   // back to the pre-TX level at once

			//power down the PA chain to null any gain
			digitalWrite(TX_POWER, LOW);
			delay(2);

			if (tr_relay){
  			digitalWrite(LPF_A, LOW);
  			digitalWrite(LPF_B, LOW);
 	 		digitalWrite(LPF_C, LOW);
  			digitalWrite(LPF_D, LOW);
				delay(10);   // LPF relay settle time, only needed when there are LPF relays
			}

			//drive the tx line low, switching the signal path 
			digitalWrite(TX_LINE, LOW);
			delay(5); 
			//audio codec is back on
			fast_mixer(audio_card, "Master", RX_MASTER_VOL);
			fast_mixer(audio_card, "Capture", rx_gain);
			spectrum_reset();
			//rx_tx_ramp = 10;
		}
}

//v2 t/r switch uses the lpfs to cut the feedback during t/r transitions
void tr_switch_v2(int tx_on){
		if (tx_on){
			if (rx_list && rx_list->agc_gain > 0.0) agc_gain_before_tx = rx_list->agc_gain;

			//first turn off the LPFs, so PA doesnt connect 
  		digitalWrite(LPF_A, LOW);
  		digitalWrite(LPF_B, LOW);
 	 	digitalWrite(LPF_C, LOW);
  		digitalWrite(LPF_D, LOW);

			//mute it all and hang on for a millisecond
			fast_mixer(audio_card, "Master", 0);
			fast_mixer(audio_card, "Capture", 0);
			delay(1);

			//now switch of the signal back
			//now ramp up after 5 msecs
			delay(2);
			mute_count = 8;
			tx_process_restart = 1;
			digitalWrite(TX_LINE, HIGH);
      delay(20);
			set_tx_power_levels();
			in_tx = 1;
			prev_lpf = -1; //force this
			set_lpf_40mhz(freq_hdr);
			delay(10);
			spectrum_reset();
		}
		else {
			in_tx = 0;
			//mute it all and hang on
			fast_mixer(audio_card, "Master", 0);
			fast_mixer(audio_card, "Capture", 0);
			delay(1);
      fft_reset_m_bins();
			mute_count = MUTE_MAX;

			// Same AGC stale-hang-counter fix as tr_switch_de -- see its
			// own comment for the full explanation.
			rx_list->agc_loop = 0;
			agc_post_tx = 120;
			if (agc_gain_before_tx > 0.0) rx_list->agc_gain = agc_gain_before_tx;   // back to the pre-TX level at once

  		digitalWrite(LPF_A, LOW);
  		digitalWrite(LPF_B, LOW);
 	 	digitalWrite(LPF_C, LOW);
  		digitalWrite(LPF_D, LOW);
			prev_lpf = -1; //force the lpf to be re-energized
			delay(4);
			//power down the PA chain to null any gain
			digitalWrite(TX_LINE, LOW);
			delay(5); 
			//audio codec is back on
			fast_mixer(audio_card, "Master", RX_MASTER_VOL);
			fast_mixer(audio_card, "Capture", rx_gain);
			spectrum_reset();
			prev_lpf = -1;
			set_lpf_40mhz(freq_hdr);
			//rx_tx_ramp = 10;
		}
}

void tr_switch(int tx_on){
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (sbitx_version == SBITX_DE)
		tr_switch_de(tx_on);
	else
		tr_switch_v2(tx_on);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("[TXRX] switch to %s took %ld ms\n", tx_on ? "TX" : "RX",
		(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
}

/* 
This is the one-time initialization code 
*/
void setup(){

	read_hw_ini();

	//create_mcast_socket();

	//setup the LPF and the gpio pins
	pinMode(TX_LINE, OUTPUT);
	pinMode(TX_POWER, OUTPUT);
	pinMode(LPF_A, OUTPUT);
	pinMode(LPF_B, OUTPUT);
	pinMode(LPF_C, OUTPUT);
	pinMode(LPF_D, OUTPUT);
  digitalWrite(LPF_A, LOW);
  digitalWrite(LPF_B, LOW);
  digitalWrite(LPF_C, LOW);
  digitalWrite(LPF_D, LOW);
	digitalWrite(TX_LINE, LOW);
	digitalWrite(TX_POWER, LOW);

	fft_init();
	vfo_init_phase_table();
  setup_oscillators();
	q_init(&qremote, 8000);

	modem_init();

	add_rx(7000000, MODE_LSB, -3000, -300);
	add_tx(7000000, MODE_LSB, -3000, -300);
	rx_list->tuned_bin = 512;
  tx_list->tuned_bin = 512;
	tx_init(7000000, MODE_LSB, -3000, -150);

	//detect the version of sbitx
	uint8_t response[4];
	if(i2cbb_read_i2c_block_data(0x8, 0, 4, response) == -1)
		sbitx_version = SBITX_DE;
	else
		sbitx_version = SBITX_V2;

	setup_audio_codec();
	sound_thread_start("plughw:0,0");

	sleep(1); //why? to allow the aloop to initialize?

	vfo_start(&tone_a, 700, 0);
	vfo_start(&tone_b, 1900, 0);
	vfo_start(&am_carrier, 24000, 0);

	delay(2000);	
//	pf_debug = fopen("am_test.raw", "w");	
}

void sdr_request(char *request, char *response){
	char cmd[100], value[1000];

	char *p = strchr(request, '=');
	int n = p - request;
	if (!p)
		return;
	strncpy(cmd, request, n);
	cmd[n] = 0;
	strcpy(value, request+n+1);

	if (!strcmp(cmd, "stat:tx")){
		if (in_tx)
			strcpy(response, "ok on");
		else
			strcpy(response, "ok off");
	}
	else if (!strcmp(cmd, "r1:freq")){
		int d = atoi(value);
		set_rx1(d);
		//printf("Frequency set to %d\n", freq_hdr);
		strcpy(response, "ok");	
	} 
	else if (!strcmp(cmd, "r1:mode")){
		if (!strcmp(value, "LSB"))
			rx_list->mode = MODE_LSB;
		else if (!strcmp(value, "CW"))
			rx_list->mode = MODE_CW;
		else if (!strcmp(value, "CWR"))
			rx_list->mode = MODE_CWR;
		else if (!strcmp(value, "2TONE"))
			rx_list->mode = MODE_2TONE;
		else if (!strcmp(value, "FT8"))
			rx_list->mode = MODE_FT8;
		else if (!strcmp(value, "AM"))
			rx_list->mode = MODE_AM;
		else
			rx_list->mode = MODE_USB;
		
    		//set the tx mode to that of the rx1
    		tx_list->mode = rx_list->mode;

		// An interesting but non-essential note:
		// the sidebands inverted twice, to come out correctly after all
		// conisder that the second oscillator is set to 27.025 MHz and 
		// a 7 MHz signal is tuned in by a 34 Mhz oscillator.
		// The first IF will be 25 Mhz, converted to a second IF of 25 KHz
		// Now, imagine that the signal at 7 Mhz moves up by 1 Khz
		// the IF now is going to be 34 - 7.001 MHz = 26.999 MHz which 
		// converts to a second IF of 26.999 - 27.025 = 26 KHz
		// Effectively, if a signal moves up, so does the second IF

		if (rx_list->mode == MODE_AM){
			puts("\n\n\ntx am filter ");
			filter_tune(tx_list->filter, 
				(1.0 * 20000)/96000.0, 
				(1.0 * 30000)/96000.0 , 
				5);
			filter_tune(tx_filter, 
				(1.0 * 20000)/96000.0, 
				(1.0 * 30000)/96000.0 , 
				5);
		}
		else if (rx_list->mode == MODE_LSB || rx_list->mode == MODE_CWR){
			puts("\n\n\ntx LSB filter ");
			filter_tune(tx_list->filter, 
				(1.0 * -3000)/96000.0, 
				(1.0 * -300)/96000.0 , 
				5);
			filter_tune(tx_filter, 
				(1.0 * -3000)/96000.0, 
				(1.0 * -300)/96000.0 , 
				5);
		}
		else { 
			puts("\n\n\ntx USB filter ");
			filter_tune(tx_list->filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
			filter_tune(tx_filter, 
				(1.0 * 300)/96000.0, 
				(1.0 * 3000)/96000.0 , 
				5);
		}
	
		//we need to nudge the oscillator to adjust 
		//to cw offset. setting it to the already tuned freq
		//doesnt recalculte the offsets

		int f = freq_hdr;
		set_rx1(f-10);
		set_rx1(f);
	
		//printf("mode set to %d\n", rx_list->mode);
		strcpy(response, "ok");
	}
	else if (!strcmp(cmd, "txmode")){
		puts("\n\n\n\n###### tx filter #######");
		if (!strcmp(value, "LSB") || !strcmp(value, "CWR"))
			filter_tune(tx_filter, (1.0*-3000)/96000.0, (1.0 * -300)/96000.0, 5);
		else
			filter_tune(tx_filter, (1.0*300)/96000.0, (1.0*3000)/96000.0, 5);
	}
	else if(!strcmp(cmd, "record")){
		if (!strcmp(value, "off")){
			fclose(pf_record);
			pf_record = NULL;
		}
		else
			pf_record = wav_start_writing(value);
	}
	else if (!strcmp(cmd, "tx")){
		struct timespec tt0, tt1;
		clock_gettime(CLOCK_MONOTONIC, &tt0);
		if (!strcmp(value, "on"))
			tr_switch_v2(1);
		else
			tr_switch_v2(0);
		clock_gettime(CLOCK_MONOTONIC, &tt1);
		printf("[TXRX] switch to %s took %ld ms\n", !strcmp(value, "on") ? "TX" : "RX",
			(tt1.tv_sec - tt0.tv_sec) * 1000 + (tt1.tv_nsec - tt0.tv_nsec) / 1000000);
		strcpy(response, "ok");
	}
	else if (!strcmp(cmd, "rx_pitch")){
		rx_pitch = atoi(value);
	}
	else if (!strcmp(cmd, "tx_gain")){
		tx_gain = atoi(value);
		if(in_tx)
			set_tx_power_levels();
	}
	else if (!strcmp(cmd, "tx_power")){
    tx_drive = atoi(value);
		if(in_tx)
			set_tx_power_levels();	
	}
	else if (!strcmp(cmd, "bridge")){
    bridge_compensation = atoi(value);
	}
	else if(!strcmp(cmd, "r1:gain")){
		rx_gain = atoi(value);
		if(!in_tx)
			sound_mixer(audio_card, "Capture", rx_gain);
	}
	else if (!strcmp(cmd, "notch")){
		notch_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "notch_freq")){
		notch_freq_hz = (double)atoi(value);
	}
	else if (!strcmp(cmd, "notch_width")){
		// NARROW=50Hz, MEDIUM=100Hz, WIDE=150Hz, per request.
		if (!strcmp(value, "NARROW"))
			notch_width_hz = 50;
		else if (!strcmp(value, "WIDE"))
			notch_width_hz = 150;
		else
			notch_width_hz = 100;
	}
	else if (!strcmp(cmd, "contour")){
		contour_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "contour_freq")){
		contour_freq_hz = (double)atoi(value);
	}
	else if (!strcmp(cmd, "contour_width")){
		// NARROW=100Hz, MEDIUM=200Hz, WIDE=300Hz, per request.
		if (!strcmp(value, "NARROW"))
			contour_width_hz = 100;
		else if (!strcmp(value, "WIDE"))
			contour_width_hz = 300;
		else
			contour_width_hz = 200;
	}
	else if (!strcmp(cmd, "formant")){
		formant_protect_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "valley")){
		valley_atten = atof(value);
		if (valley_atten < 0.0)
			valley_atten = 0.0;
		if (valley_atten > 20.0)
			valley_atten = 20.0;
	}
	else if (!strcmp(cmd, "formant_valley_on")){
		formant_valley_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "formant_valley")){
		formant_valley_level = atoi(value);
		if (formant_valley_level < 1) formant_valley_level = 1;
		if (formant_valley_level > 20) formant_valley_level = 20;
	}
	else if (!strcmp(cmd, "nb")){
		noise_blanker_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "nb_threshold")){
		nb_threshold = atof(value);
		if (nb_threshold < 0.0)
			nb_threshold = 0.0;
		if (nb_threshold > 20.0)
			nb_threshold = 20.0;
	}
	else if (!strcmp(cmd, "nr")){
		baseline_nr_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "nr_threshold")){
		nr_threshold = atof(value);
		if (nr_threshold < 0.0)
			nr_threshold = 0.0;
		if (nr_threshold > 20.0)
			nr_threshold = 20.0;
	}
	else if (!strcmp(cmd, "ai_nr")){
		ai_nr_level = atoi(value);
		if (ai_nr_level < 0)
			ai_nr_level = 0;
		if (ai_nr_level > 20)
			ai_nr_level = 20;
	}
	else if (!strcmp(cmd, "mic_comp_on")){
		mic_comp_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "mic_comp")){
		mic_comp_level = atof(value);
		if (mic_comp_level < 0.0)
			mic_comp_level = 0.0;
		if (mic_comp_level > 20.0)
			mic_comp_level = 20.0;
	}
	else if (!strcmp(cmd, "audio_boost_on")){
		audio_boost_enabled = !strcmp(value, "ON") ? 1 : 0;
	}
	else if (!strcmp(cmd, "audio_boost")){
		audio_boost_level = atof(value);
		if (audio_boost_level < 0.0)
			audio_boost_level = 0.0;
		if (audio_boost_level > 20.0)
			audio_boost_level = 20.0;
	}
	else if (!strncmp(cmd, "eq_rx_", 6) || !strncmp(cmd, "eq_tx_", 6)){
		// "eq_rx_on" / "eq_rx_b0".."eq_rx_b5", and the eq_tx_ equivalents.
		// These are state values (re-sent from saved settings at every
		// startup), never one-shot triggers, so applying them is always safe.
		struct eq_chain *c = (cmd[3] == 'r') ? &eq_rx : &eq_tx;
		const char *what = cmd + 6;
		if (!strcmp(what, "on")){
			int on = (!strcmp(value, "1") || !strcmp(value, "ON")) ? 1 : 0;
			if (on && !c->enabled){
				c->reset = 1;   // start from clean filter state
				c->dirty = 1;
			}
			c->enabled = on;
		}
		else if (what[0] == 'b' && what[1] >= '0' && what[1] < '0' + EQ_BANDS && what[2] == 0){
			int g = atoi(value);
			if (g < -EQ_MAX_DB) g = -EQ_MAX_DB;
			if (g > EQ_MAX_DB) g = EQ_MAX_DB;
			c->gain_db[what[1] - '0'] = g;
			c->dirty = 1;
		}
	}
#ifdef SBITX_FREEDV
	else if (!strcmp(cmd, "freedv")){
		int ok = freedv_ext_set_mode(value);
		strcpy(response, ok ? "ok" : "failed");
	}
	else if (!strcmp(cmd, "freedv_status")){
		// "sync snr", read by the screen: e.g. "1 7.5"
		sprintf(response, "%d %.1f", freedv_ext_sync(), freedv_ext_snr());
	}
#endif
	else if (!strcmp(cmd, "usb_spk")){
		int on = !strcmp(value, "ON");
		usb_speaker_enable(on);
		fast_mixer(audio_card, "Master", RX_MASTER_VOL);   // mute / restore the radio's own earphone output
		strcpy(response, usb_speaker_active() == on ? "ok" : "failed");
	}
	else if (!strcmp(cmd, "usb_mic")){
		int on = !strcmp(value, "ON");
		usb_mic_enable(on);
		strcpy(response, usb_mic_active() == on ? "ok" : "failed");
	}
	else if (!strcmp(cmd, "vox_on")){
		vox_enabled = !strcmp(value, "ON") ? 1 : 0;
		if (!vox_enabled && vox_keyed){
			// Turning VOX off mid-session it started -- unkey immediately.
			vox_keyed = 0;
			vox_silence_samples = 0;
			vox_ui_request = 2;
		}
		vox_pending = 0;
	}
	else if (!strcmp(cmd, "vox_threshold")){
		vox_threshold = atoi(value);
		if (vox_threshold < 0) vox_threshold = 0;
		if (vox_threshold > 20) vox_threshold = 20;
	}
	else if (!strcmp(cmd, "vox_hangtime")){
		vox_hangtime = atoi(value);
		if (vox_hangtime < 0) vox_hangtime = 0;
		if (vox_hangtime > 20) vox_hangtime = 20;
	}
	else if (!strcmp(cmd, "rec_channel")){
		// Block changing the selected channel mid-record/playback, to
		// avoid confusing or corrupting state -- must stop first.
		if (!rec_active && !play_active){
			int ch = atoi(value);
			if (ch < 0) ch = 0;
			if (ch > REC_CHANNELS - 1) ch = REC_CHANNELS - 1;
			rec_channel_selected = ch;
		}
	}
	else if (!strcmp(cmd, "rec_volume")){
		rec_volume_level = atoi(value);
		if (rec_volume_level < 0) rec_volume_level = 0;
		if (rec_volume_level > 20) rec_volume_level = 20;
	}
	else if (!strcmp(cmd, "rec")){
		if (!strcmp(value, "ON")){
			if (!play_active){  // can't record while playing back
				rec_active = 1;
				rec_write_pos = 0;
			}
		}
		else{
			if (rec_active){
				rec_active = 0;
				rec_export_wav(rec_channel_selected);   // keep a copy as a WAV file
			}
		}
	}
	else if (!strcmp(cmd, "play")){
		if (!strcmp(value, "ON")){
			if (!rec_active && rec_length[rec_channel_selected] > 0){  // can't play while recording, and nothing to play if empty
				if (play_keyed){
					// Currently transmitting -- unkey immediately, returning
					// to normal RX/TX operation, before the countdown below
					// re-keys for the repeat CQ call.
					tr_switch(0);
					play_keyed = 0;
				}
				play_active = 1;
				play_read_pos = 0;
				play_countdown = play_has_fired_once ?
					PLAY_REPEAT_PRESS_DELAY_SAMPLES : PLAY_FIRST_PRESS_DELAY_SAMPLES;
				play_has_fired_once = 1;
			}
		}
		else{
			// Explicit OFF (double-tap) -- fully stop and unkey regardless
			// of phase, and reset so the next press is a fresh first press.
			if (play_keyed)
				tr_switch(0);
			play_active = 0;
			play_keyed = 0;
			play_has_fired_once = 0;
			play_countdown = 0;
			play_read_pos = 0;
		}
	}
	else if (!strcmp(cmd, "rec_export")){
		if (!strcmp(value, "1") && !rec_active && !play_active && rec_length[rec_channel_selected] > 0)
			rec_export_wav(rec_channel_selected);
	}
	else if (!strcmp(cmd, "erase")){
		// Only act on the specific trigger value (always "1" from the UI's
		// own tap handler) -- NOT unconditionally on any "erase=X" command.
		// This field's value gets saved/restored across restarts along
		// with every other field (user_settings.ini saves the entire
		// active_layout[] indiscriminately), so an unconditional trigger
		// here would silently erase a recording on every single startup.
		if (!strcmp(value, "1") && !rec_active && !play_active){  // can't erase the channel currently in use
			rec_length[rec_channel_selected] = 0;
		}
	}
	else if (!strcmp(cmd, "sweep_drive")){
		// Range now 30-50 per spec. The REAL enforcement is still the
		// live fwdpower cutoff inside run_swr_sweep() -- if actual
		// measured power at these drive levels exceeds the 5.5W cutoff,
		// the sweep will abort early. That cutoff hasn't been changed
		// alongside this range, so it's worth confirming it still fits
		// your intended test power.
		sweep_drive_cap = atoi(value);
		if (sweep_drive_cap < 30) sweep_drive_cap = 30;
		if (sweep_drive_cap > 50) sweep_drive_cap = 50;
	}
	else if (!strcmp(cmd, "sweep_iterations")){
		if (!sweep_running){   // don't allow changing this mid-run
			sweep_iterations = atoi(value);
			if (sweep_iterations < 1) sweep_iterations = 1;
			if (sweep_iterations > 10) sweep_iterations = 10;
		}
	}
	else if (!strcmp(cmd, "sweep")){
		// Same fix as "erase" above, and for the exact same reason: this
		// is the actual root cause of the sweep auto-running (and
		// immediately self-aborting, since band/state checks fail) on
		// every startup -- user_settings.ini restoration resends every
		// field's saved value, including this one's harmless default
		// ("0"), through this identical command pathway. Without checking
		// for the specific "1" trigger value, ANY "sweep=X" command was
		// unconditionally starting a real transmission during startup.
		if (!strcmp(value, "1") && !in_tx && !sweep_running && !rec_active && !play_active)
			run_swr_sweep();
	}
	else if (!strcmp(cmd, "sweep_abort")){
		if (sweep_running)
			sweep_cancel_requested = 1;   // checked during the sweep's own GTK-pumped sleep, on its next wake
	}
	else if (!strcmp(cmd, "r1:volume")){
		rx_vol = atoi(value);
		if(!in_tx)	
			sound_mixer(audio_card, "Master", RX_MASTER_VOL);
	}
	else if(!strcmp(cmd, "r1:high")){
    rx_list->high_hz = atoi(value);
    set_rx_filter();
  }
	else if(!strcmp(cmd, "r1:low")){
    rx_list->low_hz = atoi(value);
    set_rx_filter();
  }
  else if (!strcmp(cmd, "r1:agc")){
    if (!strcmp(value, "OFF"))
      rx_list->agc_speed = -1;
    else if (!strcmp(value, "SLOW"))
      rx_list->agc_speed = 100;
		else if (!strcmp(value, "MED"))
			rx_list->agc_speed = 33; 
    else if (!strcmp(value, "FAST"))
      rx_list->agc_speed = 10;
  }
	else if (!strcmp(cmd, "sidetone")){ //between 100 and 0
		float t_sidetone = atof(value);
		if (0 <= t_sidetone && t_sidetone <= 100)
			sidetone = atof(value) * 20000000;
	}
  else if (!strcmp(cmd, "mod")){
    if (!strcmp(value, "MIC"))
      tx_use_line = 0;
    else if (!strcmp(value, "LINE"))
      tx_use_line = 1;
  }
	else if (!strcmp(cmd, "txcal"))
		tx_cal();
	else if (!strcmp(cmd, "tx_compress"))
		tx_compress = atoi(value); 
  /* else
		printf("*Error request[%s] not accepted\n", request); */
}
