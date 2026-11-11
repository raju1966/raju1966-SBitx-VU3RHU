/*
The initial sync between the gui values, the core radio values, settings, et al are manually set.
*/

#ifdef SBITX_FREEDV
#include "freedv_ext.h"
#endif
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <math.h>
#include <fcntl.h>
#include <complex.h>
#include <fftw3.h>
#include <linux/fb.h>
#include <sys/types.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <ncurses.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <cairo.h>
#include <sys/file.h>
#include <errno.h>
#include <sys/file.h>
#include <errno.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include "sdr.h"
#include "sound.h"
#include "sdr_ui.h"
#include "ini.h"
#include "hamlib.h"
#include "remote.h"
#include "modem_ft8.h"
#include "i2cbb.h"
#include "webserver.h"
#include "logbook.h"
#include "oled.h"

#define FT8_START_QSO 1
#define FT8_CONTINUE_QSO 0
void ft8_process(char *received, int operation);
void change_band(char *request);

/* command  buffer for commands received from the remote */
struct Queue q_remote_commands;
struct Queue q_tx_text;

/* Front Panel controls */
char pins[15] = {0, 2, 3, 6, 7, 
								10, 11, 12, 13, 14, 
								21, 22, 23, 25, 27};

#define ENC1_A (13)
#define ENC1_B (12)
#define ENC1_SW (14)

#define ENC2_A (0)
#define ENC2_B (2)
#define ENC2_SW (3)

#define SW5 (22)
#define PTT (7)
#define DASH (21)

#define ENC_FAST 1
#define ENC_SLOW 5

#define DS3231_I2C_ADD 0x68
//time sync, when the NTP time is not synced, this tracks the number of seconds 
//between the system cloc and the actual time set by \utc command
static long time_delta = 0;

//mouse/touch screen state
static int mouse_down = 0;
static int last_mouse_x = -1;
static int last_mouse_y = -1;

//encoder state
struct encoder {
	int pin_a,  pin_b;
	int speed;
	int prev_state;
	int history;
};
void tuning_isr(void);

#define COLOR_SELECTED_TEXT 0
#define COLOR_TEXT 1
#define COLOR_TEXT_MUTED 2
#define COLOR_SELECTED_BOX 3 
#define COLOR_BACKGROUND 4
#define COLOR_FREQ 5
#define COLOR_LABEL 6
#define SPECTRUM_BACKGROUND 7
#define SPECTRUM_GRID 8
#define SPECTRUM_PLOT 9
#define SPECTRUM_NEEDLE 10
#define COLOR_CONTROL_BOX 11
#define SPECTRUM_BANDWIDTH 12
#define COLOR_RX_PITCH 13
#define SELECTED_LINE 14
#define COLOR_FIELD_SELECTED 15 
#define COLOR_TX_PITCH 16

float palette[][3] = {
	{1,1,1}, 		// COLOR_SELECTED_TEXT
	{0,1,1},		// COLOR_TEXT
	{0.5,0.5,0.5}, //COLOR_TEXT_MUTED
	{1,1,0},		// COLOR_SELECTED_BOX
	{0,0,0},		// COLOR_BACKGROUND
	{1,1,0},		//COLOR_FREQ
	{1,0,1},		//COLOR_LABEL
	//spectrum
	{0,0,0},	//SPECTRUM_BACKGROUND
	{0.1, 0.1, 0.1}, //SPECTRUM_GRID
	{1,1,0},	//SPECTRUM_PLOT
	{0.2,0.2,0.2}, 	//SPECTRUM_NEEDLE
	{0.5,0.5,0.5}, //COLOR_CONTROL_BOX
	{0.2, 0.2, 0.2}, //SPECTRUM_BANDWIDTH
	{0,1,0},	//COLOR_RX__PITCH
	{0.1, 0.1, 0.2}, //SELECTED_LINE
	{0.1, 0.1, 0.2}, // COLOR_FIELD_SELECTED
	{1,0,0},	//COLOR_TX_PITCH
};

char *ui_font = "Sans";
int field_font_size = 12;
int screen_width=800, screen_height=480;

// we just use a look-up table to define the fonts used
// the struct field indexes into this table
struct font_style {
	int index;
	double r, g, b;
	char name[32];
	int height;
	int weight;
	int type;
	
};

guint key_modifier = 0;

struct font_style font_table[] = {
	{FONT_FIELD_LABEL, 0, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FIELD_VALUE, 1, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_FIELD, 0, 1, 1, "Mono", 14, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LARGE_VALUE, 1, 1, 1, "Arial", 24, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_SMALL, 0, 1, 1, "Mono", 10, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_LOG, 1, 1, 1, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_RX, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_TX, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_SMALL_FIELD_VALUE, 1, 1, 1, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_CW_RX, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_CW_TX, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FLDIGI_RX, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FLDIGI_TX, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_TELNET, 0, 1, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_QUEUED, 0.5, 0.5, 0.5, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
	{FONT_FT8_REPLY, 1, 0.6, 0, "Mono", 11, CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_SLANT_NORMAL},
};

struct encoder enc_a, enc_b;

#define MAX_FIELD_LENGTH 128

#define FIELD_NUMBER 0
#define FIELD_BUTTON 1
#define FIELD_TOGGLE 2
#define FIELD_SELECTION 3
#define FIELD_TEXT 4
#define FIELD_STATIC 5
#define FIELD_CONSOLE 6

// The console is a series of lines
#define MAX_CONSOLE_BUFFER 10000
#define MAX_LINE_LENGTH 128
#define MAX_CONSOLE_LINES 500
static int 	console_cols = 50;

//we use just one text list in our user interface

struct console_line {
	char text[MAX_LINE_LENGTH];
	int style;
};
static int console_style = FONT_LOG;
static struct console_line console_stream[MAX_CONSOLE_LINES];
int console_current_line = 0;
int	console_selected_line = -1;
struct Queue q_web;

//oled stuf
static uint8_t oled_available = 0;

// event ids, some of them are mapped from gtk itself
#define FIELD_DRAW 0
#define FIELD_UPDATE 1 
#define FIELD_EDIT 2
#define MIN_KEY_UP 0xFF52
#define MIN_KEY_DOWN	0xFF54
#define MIN_KEY_LEFT 0xFF51
#define MIN_KEY_RIGHT 0xFF53
#define MIN_KEY_ENTER 0xFF0D
#define MIN_KEY_ESC	0xFF1B
#define MIN_KEY_BACKSPACE 0xFF08
#define MIN_KEY_TAB 0xFF09
#define MIN_KEY_CONTROL 0xFFE3
#define MIN_KEY_F1 0xFFBE
#define MIN_KEY_F2 0xFFBF
#define MIN_KEY_F3 0xFFC0
#define MIN_KEY_F4 0xFFC1
#define MIN_KEY_F5 0xFFC2
#define MIN_KEY_F6 0xFFC3
#define MIN_KEY_F7 0xFFC4
#define MIN_KEY_F8 0xFFC5
#define MIN_KEY_F9 0xFFC6
#define MIN_KEY_F9 0xFFC6
#define MIN_KEY_F10 0xFFC7
#define MIN_KEY_F11 0xFFC8
#define MIN_KEY_F12 0xFFC9
#define COMMAND_ESCAPE '\\'

void set_ui(int id);
void set_bandwidth(int hz);

/* 	the field in focus will be exited when you hit an escape
		the field in focus will be changeable until it loses focus
		hover will always be on the field in focus.
		if the focus is -1,then hover works
*/

/*
	Warning: The field selection is used for TOGGLE and SELECTION fields
	each selection by the '/' should be unique. otherwise, the simple logic will
	get confused 
*/


//the main app window
GtkWidget *window;
GtkWidget *display_area = NULL;
GtkWidget *text_area = NULL;
extern void settings_ui(GtkWidget*p);
extern int logbook_open();

// these are callbacks called by the operating system
static gboolean on_draw_event( GtkWidget* widget, cairo_t *cr, 
	gpointer user_data); 
static gboolean on_key_release (GtkWidget *widget, GdkEventKey *event, 
	gpointer user_data);
static gboolean on_key_press (GtkWidget *widget, GdkEventKey *event, 
	gpointer user_data);
static gboolean on_mouse_press (GtkWidget *widget, GdkEventButton *event, 
	gpointer data); 
static gboolean on_mouse_move (GtkWidget *widget, GdkEventButton *event, 
	gpointer data); 
static gboolean on_mouse_release (GtkWidget *widget, GdkEventButton *event, 
	gpointer data); 
static gboolean on_scroll (GtkWidget *widget, GdkEventScroll *event, 
	gpointer data); 
static gboolean on_window_state (GtkWidget *widget, GdkEventKey *event, 
	gpointer user_data);
static gboolean on_resize(GtkWidget *widget, GdkEventConfigure *event, 
	gpointer user_data);
gboolean ui_tick(gpointer gook);

static int measure_text(cairo_t *gfx, char *text, int font_entry){
	cairo_text_extents_t ext;
	struct font_style *s = font_table + font_entry;
	
	cairo_select_font_face(gfx, s->name, s->type, s->weight);
	cairo_set_font_size(gfx, s->height);
	cairo_move_to(gfx, 0, 0);
	cairo_text_extents(gfx, text, &ext);
	return (int) ext.x_advance;
}

static void draw_text(cairo_t *gfx, int x, int y, char *text, int font_entry){
	struct font_style *s  = font_table + font_entry;
  cairo_set_source_rgb( gfx, s->r, s->g, s->b);
	cairo_select_font_face(gfx, s->name, s->type, s->weight);
	cairo_set_font_size(gfx, s->height);
	cairo_move_to(gfx, x, y + s->height);
	cairo_show_text(gfx, text);
}

static void fill_rect(cairo_t *gfx, int x, int y, int w, int h, int color){
  cairo_set_source_rgb( gfx, palette[color][0], palette[color][1], palette[color][2]);
	cairo_rectangle(gfx, x, y, w, h);
  cairo_fill(gfx);
}

static void rect(cairo_t *gfx, int x, int y, int w, int h, 
	int color, int thickness){

  cairo_set_source_rgb( gfx, 
		palette[color][0], 
		palette[color][1], 
		palette[color][2]);

	cairo_set_line_width(gfx, thickness);
	cairo_rectangle(gfx, x, y, w, h);
  cairo_stroke(gfx);
}


/****************************************************************************
	Using the above hooks and primitives, we build user interface controls,
	All of them are defined by the struct field
****************************************************************************/


struct field {
	char	*cmd;
	int		(*fn)(struct field *f, cairo_t *gfx, int event, int param_a, int param_b, int param_c);
	int		x, y, width, height;
	char	label[30];
	int 	label_width;
	char	value[MAX_FIELD_LENGTH];
	char	value_type; //NUMBER, SELECTION, TEXT, TOGGLE, BUTTON
	int 	font_index; //refers to font_style table
	char  selection[1000];
	long int	 	min, max;
  int step;
	int 	section;
	char is_dirty;
	char update_remote;
	void *data;
};

#define STACK_DEPTH 4

struct band {
	char name[10];
	int	start;
	int	stop;
	//int	power;
	//int	max;
	int index;
	int	freq[STACK_DEPTH];
	int mode[STACK_DEPTH];
};

struct cmd {
	char *cmd;
	int (*fn)(char *args[]);
};


static unsigned long focus_since = 0;
static struct field *f_focus = NULL;
static struct field *f_hover = NULL;
static struct field *f_last_text = NULL;

//variables to power up and down the tx

static int in_tx = TX_OFF;
static int key_down = 0;
static int tx_start_time = 0;

static int *tx_mod_buff = NULL;
static int tx_mod_index = 0;
static int tx_mod_max = 0;

char*mode_name[MAX_MODES] = {
	"USB", "LSB", "CW", "CWR", "NBFM", "AM", "FT8", "PSK31", "RTTY", 
	"DIGITAL", "2TONE" 
};

static int serial_fd = -1;
static int xit = 512; 
static long int tuning_step = 1000;
static int tx_mode = MODE_USB;


#define BAND80M	0
#define BAND40M	1
#define BAND30M 2	
#define BAND20M 3	
#define BAND17M 4	
#define BAND15M 5
#define BAND12M 6 
#define BAND10M 7 

struct band band_stack[] = {
	{"80M", 3500000, 4000000, 0, 
		{3500000,3574000,3600000,3700000},{MODE_CW, MODE_USB, MODE_CW,MODE_LSB}},
	{"40M", 7000000,7300000, 0,
		{7000000,7040000,7074000,7150000},{MODE_CW, MODE_CW, MODE_USB, MODE_LSB}},
	{"30M", 10100000, 10150000, 0,
		{10100000, 10100000, 10136000, 10150000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"20M", 14000000, 14400000, 0,
		{14010000, 14040000, 14074000, 14200000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"17M", 18068000, 18168000, 0,
		{18068000, 18100000, 18110000, 18160000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"15M", 21000000, 21500000, 0,
		{21010000, 21040000, 21074000, 21250000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"12M", 24890000, 24990000, 0,
		{24890000, 24910000, 24950000, 24990000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
	{"10M", 28000000, 29700000, 0,
		{28000000, 28040000, 28074000, 28250000}, {MODE_CW, MODE_CW, MODE_USB, MODE_USB}},
};


#define VFO_A 0 
#define VFO_B 1 
//int	vfo_a_freq = 7000000;
//int	vfo_b_freq = 14000000;
char vfo_a_mode[10];
char vfo_b_mode[10];

//usefull data for macros, logging, etc
int	tx_id = 0;

//recording duration in seconds
time_t record_start = 0;
int	data_delay = 700;

#define MAX_RIT 25000

int spectrum_span = 48000;

// --- Menu system ---
// Screen real estate is limited, so feature controls beyond the main
// always-visible row now live behind a paged MENU button (next to KBD).
// current_menu_page cycles 1-9; menu_row_y is set once during layout and
// reused here so update_menu_page() (called both from layout_ui() and
// from the MENU button's own tap callback) always positions things on
// the same row without needing direct access to layout_ui()'s locals.
int current_menu_page = 1;
int menu_row_y = 0;
int menu_system_visible = 1;  // our menu system only has room in USB/LSB/AM/NBFM
                               // layouts; FT8/CW/digital modes use that same
                               // screen row for their own controls (F1-F8,
                               // FT8_TX1ST, SIDETONE, etc.), so our fields must
                               // be forced hidden there instead of staying
                               // stuck at their last (USB/LSB) position.
#ifdef SBITX_FREEDV
#define MENU_PAGE_COUNT 9
#else
#define MENU_PAGE_COUNT 8
#endif
extern int spectrum_plot[];
extern int fwdpower, vswr;
extern double alc_level;
extern int notch_enabled;
extern double notch_freq_hz;
extern int notch_width_hz;
extern int contour_enabled;
extern double contour_freq_hz;
extern int contour_width_hz;
extern int formant_protect_enabled;
extern int formant_valley_enabled;
extern int formant_valley_level;
extern double valley_atten;
extern int noise_blanker_enabled;
extern double nb_threshold;
extern int baseline_nr_enabled;
extern double nr_threshold;
extern int mic_comp_enabled;
extern int audio_boost_enabled;
extern volatile int vox_ui_request;
extern int vox_enabled;
extern int vox_threshold;
extern int vox_hangtime;
extern int rec_channel_selected;
extern int rec_active;
extern int play_active;
extern int rec_length[5];   // matches REC_CHANNELS in sbitx.c
extern int sweep_drive_cap;
extern int sweep_running;
extern int sweep_iterations;
extern int sweep_avg_vswr_x10;
extern int sweep_stddev_vswr_x10;
extern int freq_hdr;

void do_control_action(char *cmd);
void cmd_exec(char *cmd);


int do_spectrum(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_waterfall(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_tuning(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_text(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_console(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_pitch(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_toggle_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_mouse_move(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_macro(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_record(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_bandwidth(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_notch_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
// Double-tap detection for the combined toggle+scale boxes (NBVL, NRVL,
// FMNT, MCMP, ABST). No time window -- purely consecutive-tap based.
// Exact intended behavior:
//   - box OFF, single tap  -> turns ON, knob then adjusts the level
//   - box ON,  single tap  -> does nothing except select it (so revisiting
//                             an already-on box after using a different
//                             function to adjust its level via knob never
//                             accidentally toggles it off -- focus_field()
//                             clears this tracker whenever a different
//                             field is focused, so "revisiting" is
//                             correctly distinguished from "tapping the
//                             same box again right now")
//   - box ON,  a SECOND tap on the SAME box, with nothing else focused in
//              between -> turns OFF
static struct field *last_toggle_tap = NULL;

// PLAY's own double-tap window (voice-keyer semantics differ from every
// other combined box: a normal-paced tap should always retrigger, only a
// genuinely rapid second tap means "stop") -- deliberately separate from
// the shared last_toggle_tap logic above.
#define PLAY_DOUBLE_TAP_MS 400

void draw_menu_field(struct field *f, cairo_t *gfx, const char *display);
int do_notch_freq(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_notch_width(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_contour_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_contour_freq(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_contour_width(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_valley_atten(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_formant_valley(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_nb_threshold(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_nr_threshold(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_ai_nr(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_usb_spk(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_usb_mic(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_usb_ptt(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
#ifdef SBITX_FREEDV
int do_fdv_mode(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_fdv_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_fdv_window(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
#endif
int do_mic_comp(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_audio_boost(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_vox_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_vox_threshold(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_vox_hangtime(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_eq_rx_open(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_eq_tx_open(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_rec_channel(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_rec_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_play_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_rec_volume(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_rec_erase(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_rec_export(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_sweep_drive(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_sweep_iterations(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_sweep_trigger(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_sweep_freq_display(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
int do_menu_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c);
void update_menu_page();

struct field *active_layout = NULL;
char settings_updated = 0;
#define LAYOUT_KBD 0
#define LAYOUT_MACROS 1
int current_layout = LAYOUT_KBD;

#define COMMON_CONTROL 1
#define FT8_CONTROL 2
#define CW_CONTROL 4
#define VOICE_CONTROL 8
#define DIGITAL_CONTROL 16


// the cmd fields that have '#' are not to be sent to the sdr
struct field main_controls[] = {
	/* band stack registers */
	{"#10m", NULL, 50, 5, 40, 40, "10M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#12m", NULL, 90, 5, 40, 40, "12M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#15m", NULL, 130, 5, 40, 40, "15M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0i,COMMON_CONTROL},
	{"#17m", NULL, 170, 5, 40, 40, "17M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#20m", NULL, 210, 5, 40, 40, "20M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#30m", NULL, 250, 5, 40, 40, "30M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#40m", NULL, 290, 5, 40, 40, "40M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#80m", NULL, 330, 5, 40, 40, "80M", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{ "#record", do_record, 380, 5, 40, 40, "REC", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0, 0,COMMON_CONTROL},
	{ "#web", NULL, 420,5,  40, 40, "WEB", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0, 0,COMMON_CONTROL},
	{"#set", NULL, 460, 5, 40, 40, "SET", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,COMMON_CONTROL}, 
	{ "r1:gain", NULL, 375, 5, 40, 40, "IF", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1,COMMON_CONTROL},
	{ "r1:agc", NULL, 415, 5, 40, 40, "AGC", 40, "SLOW", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"OFF/SLOW/MED/FAST", 0, 1024, 1,COMMON_CONTROL},
	{ "tx_power", NULL, 455, 5, 40, 40, "DRIVE", 40, "40", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 1, 100, 1,COMMON_CONTROL},


	{ "r1:freq", do_tuning, 600, 0, 150, 49, "FREQ", 5, "14000000", FIELD_NUMBER, FONT_LARGE_VALUE, 
		"", 500000, 30000000, 100,COMMON_CONTROL},

	{ "r1:volume", NULL, 755, 5, 40, 40, "AUDIO", 40, "60", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1,COMMON_CONTROL},

	{"#step", NULL, 560, 5 ,40, 40, "STEP", 1, "10Hz", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"10K/1K/500H/100H/10H", 0,0,0,COMMON_CONTROL},
	{"#span", NULL, 560, 50 , 40, 40, "SPAN", 1, "A", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"25K/10K/6K/2.5K", 0,0,0,COMMON_CONTROL},

	{"#rit", NULL, 600, 50, 40, 40, "RIT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0,COMMON_CONTROL},
	{"#vfo", NULL, 640, 50 , 40, 40, "VFO", 1, "A", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"A/B", 0,0,0,COMMON_CONTROL},
	{"#split", NULL, 680, 50, 40, 40, "SPLIT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0,COMMON_CONTROL},

	{ "#bw", do_bandwidth, 495, 5, 40, 40, "BW", 40, "", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 50, 5000, 50,COMMON_CONTROL},

	{ "r1:mode", NULL, 5, 5, 40, 40, "MODE", 40, "USB", FIELD_SELECTION, FONT_FIELD_VALUE, 
		"USB/LSB/CW/CWR/FT8/AM/DIGITAL/2TONE", 0,0,0, COMMON_CONTROL},

	/* logger controls */

	{"#contact_callsign", do_text, 5, 50, 85, 20, "CALL", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0,11,0,COMMON_CONTROL},
	{"#rst_sent", do_text, 90, 50, 50, 20, "SENT", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#rst_received", do_text, 140, 50, 50, 20, "RECV", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#exchange_received", do_text, 190, 50, 50, 20, "EXCH", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#exchange_sent", do_text, 240, 50, 50, 20, "NR", 70, "", FIELD_TEXT, FONT_LOG, 
		"", 0, 7,0,COMMON_CONTROL},
	{"#enter_qso", NULL, 290, 50, 40, 40, "SAVE", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"", 0,0,0,COMMON_CONTROL},
	{"#wipe", NULL, 330, 50, 40, 40, "WIPE", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,COMMON_CONTROL}, 
	{"#mfqrz", NULL, 370, 50, 40, 40, "QRZ", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,COMMON_CONTROL}, 
	{"#logbook", NULL, 410, 50, 40, 40, "LOG", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,COMMON_CONTROL}, 
	{"#text_in", do_text, 5, 70, 285, 20, "TEXT", 70, "text box", FIELD_TEXT, FONT_LOG, 
		"nothing valuable", 0,128,0,COMMON_CONTROL},

	{ "#toggle_kbd", do_toggle_kbd, 495, 50, 40, 40, "KBD", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0, 0,COMMON_CONTROL},


/* end of common controls */

	//tx 
	{ "tx_gain", NULL, 550, -350, 50, 50, "MIC", 40, "50", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 0, 100, 1, VOICE_CONTROL},

	{ "tx_compress", NULL, 600, -350, 50, 50, "COMP", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"ON/OFF", 0,100,10, VOICE_CONTROL},
	{ "#tx_wpm", NULL, 650, -350, 50, 50, "WPM", 40, "12", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 1, 50, 1, CW_CONTROL},
	{ "rx_pitch", do_pitch, 700, -350, 50, 50, "PITCH", 40, "600", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 100, 3000, 10, FT8_CONTROL | DIGITAL_CONTROL},
	

	{ "#tx", NULL, 1000, -1000, 50, 50, "TX", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"RX/TX", 0,0, 0, VOICE_CONTROL},

	{ "#rx", NULL, 650, -400, 50, 50, "RX", 40, "", FIELD_BUTTON, FONT_FIELD_VALUE, 
		"RX/TX", 0,0, 0, VOICE_CONTROL | DIGITAL_CONTROL},
	

	{"r1:low", NULL, 660, -350, 50, 50, "LOW", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 100,4000, 50, 0, DIGITAL_CONTROL},
	{"r1:high", NULL, 580, -350, 50, 50, "HIGH", 40, "3000", FIELD_NUMBER, FONT_FIELD_VALUE, 
		"", 100, 10000, 50, 0, DIGITAL_CONTROL},

	{"spectrum", do_spectrum, 400, 101, 400, 100, "SPECTRUM", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"", 0,0,0, COMMON_CONTROL},  
	{"#status", do_status, -1000, -1000, 400, 29, "STATUS", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"status", 0,0,0, 0},  

	{"waterfall", do_waterfall, 400, 201 , 400, 99, "WATERFALL", 70, "7000 KHz", FIELD_STATIC, FONT_SMALL, 
		"", 0,0,0, COMMON_CONTROL},
	{"#console", do_console, 0, 100, 400, 200, "CONSOLE", 70, "console box", FIELD_CONSOLE, FONT_LOG, 
		"nothing valuable", 0,0,0, COMMON_CONTROL},

	{"#log_ed", NULL, 0, 480, 480, 20, "", 70, "", FIELD_STATIC, FONT_LOG, 
		"nothing valuable", 0,128,0, 0},
  // other settings - currently off screen
  { "reverse_scrolling", NULL, 1000, -1000, 50, 50, "RS", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0,0,0, 0},
  { "tuning_acceleration", NULL, 1000, -1000, 50, 50, "TA", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0,0,0, 0},
  { "tuning_accel_thresh1", NULL, 1000, -1000, 50, 50, "TAT1", 40, "10000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100,99999,100, 0},
  { "tuning_accel_thresh2", NULL, 1000, -1000, 50, 50, "TAT2", 40, "500", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100,99999,100, 0},
  { "mouse_pointer", NULL, 1000, -1000, 50, 50, "MP", 40, "LEFT", FIELD_SELECTION, FONT_FIELD_VALUE,
    "BLANK/LEFT/RIGHT/CROSSHAIR", 0,0,0,0},

	// Settings Panel
	{"#mycallsign", NULL, 1000, -1000, 400, 149, "MYCALLSIGN", 70, "CALL", FIELD_TEXT, FONT_SMALL, 
		"", 3,10,1,0},
	{"#mygrid", NULL, 1000, -1000, 400, 149, "MYGRID", 70, "NOWHERE", FIELD_TEXT, FONT_SMALL, 
		"", 4,6,1,0},
	{"#passkey", NULL, 1000, -1000, 400, 149, "PASSKEY", 70, "123", FIELD_TEXT, FONT_SMALL, 
		"", 0,32,1,0},

	//moving global variables into fields 	
  { "#vfo_a_freq", NULL, 1000, -1000, 50, 50, "VFOA", 40, "14000000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 500000,30000000,1,0},
  {"#vfo_b_freq", NULL, 1000, -1000, 50, 50, "VFOB", 40, "7000000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 500000,30000000,1,0},
  {"#rit_delta", NULL, 1000, -1000, 50, 50, "RIT_DELTA", 40, "000000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -25000,25000,1,0},

  { "#cwinput", NULL, 1000, -1000, 50, 50, "CW_INPUT", 40, "KEYBOARD", FIELD_SELECTION, FONT_FIELD_VALUE,
		"IAMBIC/IAMBICB/STRAIGHT", 0,0,0, CW_CONTROL},
  { "#cwdelay", NULL, 1000, -1000, 50, 50, "CW_DELAY", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 50, 1000, 50, CW_CONTROL},
	{ "#tx_pitch", NULL, 400, -1000, 50, 50, "TX_PITCH", 40, "600", FIELD_NUMBER, FONT_FIELD_VALUE, 
    "", 300, 3000, 10, FT8_CONTROL},
	{ "sidetone", NULL, 1000, -1000, 50, 50, "SIDETONE", 40, "25", FIELD_NUMBER, FONT_FIELD_VALUE, 
    "", 0, 100, 5, CW_CONTROL},
	{"#sent_exchange", NULL, 1000, -1000, 400, 149, "SENT_EXCHANGE", 70, "", FIELD_TEXT, FONT_SMALL, 
		"", 0,10,1, COMMON_CONTROL},
  { "#contest_serial", NULL, 1000, -1000, 50, 50, "CONTEST_SERIAL", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0,1000000,1, COMMON_CONTROL},
	{"#current_macro", NULL, 1000, -1000, 400, 149, "MACRO", 70, "", FIELD_TEXT, FONT_SMALL, 
		"", 0,32,1, COMMON_CONTROL},
  { "#fwdpower", NULL, 1000, -1000, 50, 50, "POWER", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0,10000,1, COMMON_CONTROL},
  { "#vswr", NULL, 1000, -1000, 50, 50, "REF", 40, "300", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0,10000, 1, COMMON_CONTROL},
  { "#alc", NULL, 1000, -1000, 50, 50, "ALC", 40, "100", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0,100, 1, COMMON_CONTROL},
  { "notch", do_notch_toggle, 1000, -1000, 40, 40, "NOTC", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0, 1, 1, COMMON_CONTROL},
  { "notch_freq", do_notch_freq, 1000, -1000, 50, 50, "NFRQ", 40, "700", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100, 3000, 10, COMMON_CONTROL},
  { "notch_width", do_notch_width, 1000, -1000, 90, 45, "NWTH", 40, "MEDIUM", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "NARROW/MEDIUM/WIDE", 0, 2, 1, COMMON_CONTROL},
  { "contour", do_contour_toggle, 1000, -1000, 40, 40, "CONT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0, 1, 1, COMMON_CONTROL},
  { "contour_freq", do_contour_freq, 1000, -1000, 50, 50, "CFRQ", 40, "1200", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 100, 3000, 10, COMMON_CONTROL},
  { "contour_width", do_contour_width, 1000, -1000, 90, 45, "CONW", 40, "MEDIUM", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "NARROW/MEDIUM/WIDE", 0, 2, 1, COMMON_CONTROL},
  { "valley", do_valley_atten, 1000, -1000, 50, 45, "FMNT", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "formant_valley", do_formant_valley, 1000, -1000, 50, 45, "FVLY", 40, "1", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 1, 20, 1, COMMON_CONTROL},
  { "nb_threshold", do_nb_threshold, 1000, -1000, 50, 45, "NBVL", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "nr_threshold", do_nr_threshold, 1000, -1000, 50, 45, "NRVL", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "ai_nr", do_ai_nr, 1000, -1000, 50, 45, "AINR", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "usb_spk", do_usb_spk, 1000, -1000, 48, 36, "USBS", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0, 1, 1, COMMON_CONTROL},
  { "usb_mic", do_usb_mic, 1000, -1000, 48, 36, "USBM", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0, 1, 1, COMMON_CONTROL},
  { "usb_ptt", do_usb_ptt, 1000, -1000, 48, 36, "UPTT", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 4, 1, COMMON_CONTROL},
#ifdef SBITX_FREEDV
  { "freedv", do_fdv_mode, 1000, -1000, 48, 36, "FDVM", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 5, 1, COMMON_CONTROL},
  { "#freedv_status", do_fdv_status, 1000, -1000, 48, 36, "FDVS", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "#freedv_window", do_fdv_window, 1000, -1000, 48, 36, "FDVW", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
#endif
  { "mic_comp", do_mic_comp, 1000, -1000, 90, 45, "MCMP", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "audio_boost", do_audio_boost, 1000, -1000, 90, 45, "ABST", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "vox_on", do_vox_toggle, 1000, -1000, 48, 36, "VOXT", 40, "OFF", FIELD_TOGGLE, FONT_FIELD_VALUE,
    "ON/OFF", 0, 1, 1, COMMON_CONTROL},
  { "vox_threshold", do_vox_threshold, 1000, -1000, 48, 36, "VXTH", 40, "10", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "vox_hangtime", do_vox_hangtime, 1000, -1000, 48, 36, "VXHT", 40, "10", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  // Equalizer: the two visible menu buttons just open their popup. The
  // actual settings live in the hidden NULL-callback fields below, which
  // is what makes them persist across restarts and get re-sent to the DSP
  // at startup by the normal settings-restore path (set_field ->
  // do_control_action -> sdr_request). The popups are only a view of these.
  { "#eq_rx_open", do_eq_rx_open, 1000, -1000, 48, 36, "EQRX", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "#eq_tx_open", do_eq_tx_open, 1000, -1000, 48, 36, "EQTX", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "eq_rx_on", NULL, 1000, -1000, 48, 36, "ERON", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 1, 1, COMMON_CONTROL},
  { "eq_rx_b0", NULL, 1000, -1000, 48, 36, "ERB0", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_rx_b1", NULL, 1000, -1000, 48, 36, "ERB1", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_rx_b2", NULL, 1000, -1000, 48, 36, "ERB2", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_rx_b3", NULL, 1000, -1000, 48, 36, "ERB3", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_rx_b4", NULL, 1000, -1000, 48, 36, "ERB4", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_rx_b5", NULL, 1000, -1000, 48, 36, "ERB5", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_tx_on", NULL, 1000, -1000, 48, 36, "ETON", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 1, 1, COMMON_CONTROL},
  { "eq_tx_b0", NULL, 1000, -1000, 48, 36, "ETB0", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_tx_b1", NULL, 1000, -1000, 48, 36, "ETB1", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_tx_b2", NULL, 1000, -1000, 48, 36, "ETB2", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_tx_b3", NULL, 1000, -1000, 48, 36, "ETB3", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_tx_b4", NULL, 1000, -1000, 48, 36, "ETB4", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "eq_tx_b5", NULL, 1000, -1000, 48, 36, "ETB5", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", -20, 20, 1, COMMON_CONTROL},
  { "rec_channel", do_rec_channel, 1000, -1000, 48, 36, "CHAN", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 4, 1, COMMON_CONTROL},
  { "rec", do_rec_toggle, 1000, -1000, 48, 36, "RECD", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "play", do_play_toggle, 1000, -1000, 48, 36, "PLAY", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "rec_volume", do_rec_volume, 1000, -1000, 48, 36, "VOLM", 40, "10", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 20, 1, COMMON_CONTROL},
  { "rec_erase", do_rec_erase, 1000, -1000, 48, 36, "ERAS", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "rec_export", do_rec_export, 1000, -1000, 48, 36, "EXPT", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "#sweep_freq_display", do_sweep_freq_display, 1000, -1000, 48, 36, "SWFR", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "sweep_iterations", do_sweep_iterations, 1000, -1000, 48, 36, "SWIT", 40, "5", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 1, 10, 1, COMMON_CONTROL},
  { "sweep_drive", do_sweep_drive, 1000, -1000, 48, 36, "SWPW", 40, "30", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 30, 50, 1, COMMON_CONTROL},
  { "sweep", do_sweep_trigger, 1000, -1000, 48, 36, "SWPT", 40, "0", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "menu", do_menu_toggle, 1000, -1000, 45, 45, "MENU", 40, "M1", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 0, 0, 0, COMMON_CONTROL},
  { "bridge", NULL, 1000, -1000, 50, 50, "BRIDGE", 40, "100", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 10,100, 1, COMMON_CONTROL},
	//cw, ft8 and many digital modes need abort
	{"#abort", NULL, 370, 50, 40, 40, "ESC", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,CW_CONTROL}, 

	//FT8 should be 4000 Hz
  {"#bw_voice", NULL, 1000, -1000, 50, 50, "BW_VOICE", 40, "2200", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 300, 3000, 50, 0},
  {"#bw_cw", NULL, 1000, -1000, 50, 50, "BW_CW", 40, "400", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 300, 3000, 50, 0},
  {"#bw_digital", NULL, 1000, -1000, 50, 50, "BW_DIGITAL", 40, "3000", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 300, 3000, 50, 0},

	//FT8 controls
	{"#ft8_auto", NULL, 1000, -1000, 50, 50, "FT8_AUTO", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0, FT8_CONTROL},
	{"#ft8_tx1st", NULL, 1000, -1000, 50, 50, "FT8_TX1ST", 40, "ON", FIELD_TOGGLE, FONT_FIELD_VALUE, 
		"ON/OFF", 0,0,0, FT8_CONTROL},
  { "#ft8_repeat", NULL, 1000, -1000, 50, 50, "FT8_REPEAT", 40, "5", FIELD_NUMBER, FONT_FIELD_VALUE,
    "", 1, 10, 1, FT8_CONTROL},

	{"#telneturl", NULL, 1000, -1000, 400, 149, "TELNETURL", 70, "dxc.nc7j.com:7373", FIELD_TEXT, FONT_SMALL, 
		"", 0,32,1, 0},

	//soft keyboard
	{"#kbd_q", do_kbd, 0, 300 ,50, 50, "", 1, "Q", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_w", do_kbd, 50, 300, 50, 50, "", 1, "W", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_e", do_kbd, 100, 300, 50, 50, "", 1, "E", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_r", do_kbd, 150, 300, 50, 50, "", 1, "R", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_t", do_kbd, 200, 300, 50, 50, "", 1, "T", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_y", do_kbd, 250, 300, 50, 50, "", 1, "Y", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_u", do_kbd, 300, 300, 50, 50, "", 1, "U", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_i", do_kbd, 350, 300, 50, 50, "", 1, "I", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_o", do_kbd, 400, 300, 50, 50, "", 1, "O", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_p", do_kbd, 450, 300, 50, 50, "", 1, "P", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_@", do_kbd, 500, 300, 50, 50, "", 1, "@", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#kbd_1", do_kbd, 550, 300, 50, 50, "", 1, "1", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_2", do_kbd, 600, 300, 50, 50, "", 1, "2", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_3", do_kbd, 650, 300, 50, 50, "", 1, "3", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_bs", do_kbd, 700, 300, 100, 50, "", 1, "DEL", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0},

	{"#kbd_alt", do_kbd, 0, 350 ,50, 50, "", 1, "CMD", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_a", do_kbd, 50, 350 ,50, 50, "*", 1, "A", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_s", do_kbd, 100, 350, 50, 50, "", 1, "S", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_d", do_kbd, 150, 350, 50, 50, "", 1, "D", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_f", do_kbd, 200, 350, 50, 50, "", 1, "F", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_g", do_kbd, 250, 350, 50, 50, "", 1, "G", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_h", do_kbd, 300, 350, 50, 50, "", 1, "H", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_j", do_kbd, 350, 350, 50, 50, "", 1, "J", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_k", do_kbd, 400, 350, 50, 50, "'", 1, "K", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_l", do_kbd, 450, 350, 50, 50, "", 1, "L", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_/", do_kbd, 500, 350, 50, 50, "", 1, "/", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#kbd_4", do_kbd, 550, 350, 50, 50, "", 1, "4", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_5", do_kbd, 600, 350, 50, 50, "", 1, "5", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_6", do_kbd, 650, 350, 50, 50, "", 1, "6", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_enter", do_kbd, 700, 400, 100, 50, "", 1, "Enter", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
 
	{"#kbd_ ", do_kbd, 0, 400, 50, 50, "", 1, "SPACE", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_z", do_kbd, 50, 400, 50, 50, "", 1, "Z", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_x", do_kbd, 100, 400, 50, 50, "", 1, "X", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_c", do_kbd, 150,	400, 50, 50, "", 1, "C", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_v", do_kbd, 200, 400, 50, 50, "", 1, "V", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_b", do_kbd, 250, 400, 50, 50, "", 1, "B", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_n", do_kbd, 300, 400, 50, 50, "", 1, "N", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_m", do_kbd, 350, 400, 50, 50, "", 1, "M", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_,", do_kbd, 400, 400, 50, 50, "", 1, ",", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_.", do_kbd, 450, 400, 50, 50, "", 1, ".", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_?", do_kbd, 500, 400, 50, 50, "", 1, "?", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#kbd_7", do_kbd, 550, 400, 50, 50, "", 1, "7", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_8", do_kbd, 600, 400, 50, 50, "", 1, "8", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_9", do_kbd, 650, 400, 50, 50, "", 1, "9", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 
	{"#kbd_0", do_kbd, 700, 350, 50, 50, "", 1, "0", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//macros keyboard

	//row 1
	{"#mf1", do_macro, 0, 1360, 65, 40, "F1", 1, "CQ", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf2", do_macro, 65, 1360, 65, 40, "F2", 1, "Call", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf3", do_macro, 130, 1360, 65, 40, "F3", 1, "Reply", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf4", do_macro, 195, 1360, 65, 40, "F4", 1, "RRR", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf5", do_macro, 260, 1360, 70, 40, "F5", 1, "73", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf6", do_macro, 330, 1360, 70, 40, "F6", 1, "Call", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//row 2

	{"#mf7", do_macro, 0, 1400, 65, 40, "F7", 1, "Exch", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf8", do_macro, 65, 1400, 65, 40, "F8", 1, "Tu", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf9", do_macro, 130, 1400, 65, 40, "F9", 1, "Rpt", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf10", do_macro, 195, 1400, 65, 40, "F10", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf11", do_macro, 260, 1400, 70, 40, "F11", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mf12", do_macro, 330, 1400, 70, 40, "F12", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//row 3



	{"#mfedit", do_macro, 195, 1440, 65, 40, "Edit", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mfspot"	, do_macro, 260, 1440, 70, 40, "Spot", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	{"#mfkbd", do_macro, 330, 1440, 70, 40, "Kbd", 1, "", FIELD_BUTTON, FONT_FIELD_VALUE,"", 0,0,0,0}, 

	//the last control has empty cmd field 
	{"", NULL, 0, 0 ,0, 0, "#", 1, "Q", FIELD_BUTTON, FONT_FIELD_VALUE, "", 0,0,0,0},
};


struct field *get_field(const char *cmd);
void update_field(struct field *f);
void tx_on();
void tx_off();

//#define MAX_CONSOLE_LINES 1000
//char *console_lines[MAX_CONSOLE_LINES];
int last_log = 0;

struct field *get_field(const char *cmd){
	for (int i = 0; active_layout[i].cmd[0] > 0; i++)
		if (!strcmp(active_layout[i].cmd, cmd))
			return active_layout + i;
	return NULL;
}

//set the field directly to a particuarl value, programmatically
int set_field(const char *id, const char *value){
	struct field *f = get_field(id);
	int v;
	int debug = 0;

	if (!f){
		printf("*Error: field[%s] not found. Check for typo?\n", id);
		return 1;
	}
	
	if (f->value_type == FIELD_NUMBER){
		int	v = atoi(value);
		if (v < f->min)
			v = f->min;
		if (v > f->max)
			v = f->max;
		sprintf(f->value, "%d",  v);
	}
	else if (f->value_type == FIELD_SELECTION || f->value_type == FIELD_TOGGLE){
		// toggle and selection are the same type: toggle has just two values instead of many more
		char *p, *prev, *next, b[100];
		//search the current text in the selection
		prev = NULL;
		if (debug)
			printf("field selection [%s]\n");
		strcpy(b, f->selection);
		p = strtok(b, "/");
		if (debug)
			printf("first token [%s]\n", p);
		while(p){
			if (!strcmp(value, p))
				break;
			else
				prev = p;
			p = strtok(NULL, "/");
			if (debug)
				printf("second token [%s]\n", p);
		}	
		//set to the first option
		if (p == NULL){
			if (prev)
				strcpy(f->value, prev);
			printf("*Error: setting field[%s] to [%s] not permitted\n", f->cmd, value);
			return 1;
		}
		else{
			if (debug)
				printf("Setting field to %s\n", value);
			strcpy(f->value, value);
		}
	}
	else if (f->value_type == FIELD_BUTTON){
		strcpy(f->value, value);
		do_control_action(f->label);
	}
	else if (f->value_type == FIELD_TEXT){
		if (strlen(value) > f->max || strlen(value) < f->min){
			printf("*Error: field[%s] can't be set to [%s], improper size.\n", f->cmd, value);
			return 1;
		}
		else
			strcpy(f->value, value);
	}

	if (!strcmp(id, "#rit") || !strcmp(id, "#ft8_auto"))
		debug = 1; 

	//send a command to the radio 
	char buff[200];
	sprintf(buff, "%s %s", f->label, f->value);
	do_control_action(buff);

	// AINR keeps its own state in the DSP side, which only learns about it
	// through the "ai_nr=" command. Without this, the level restored from
	// user_settings.ini at start-up would show in the box while the radio
	// itself still had AI noise reduction off.
	if (!strcmp(f->cmd, "ai_nr")){
		char ai_req[40], ai_resp[40];
		sprintf(ai_req, "ai_nr=%s", f->value);
		sdr_request(ai_req, ai_resp);
	}

	update_field(f);
	return 0;
}

struct field *get_field_by_label(const char *label){
	for (int i = 0; active_layout[i].cmd[0] > 0; i++)
		if (!strcasecmp(active_layout[i].label, label))
			return active_layout + i;
	return NULL;
}

const char *field_str(char *label){
	struct field *f = get_field_by_label(label);
	if (f)
		return f->value;
	else
		return NULL; 
}

int field_int(char *label){
	struct field *f = get_field_by_label(label);
	if (f){
		return atoi(f->value);
	}
	else {
		printf("field_int: %s not found\n", label);
		return -1;
	}
}

int field_set(const char *label, const char *new_value){
	struct field *f = get_field_by_label(label);
	if (!f)
		return -1;
	int r = set_field(f->cmd, new_value); 
	update_field(f);
}

int get_field_value(char *cmd, char *value){
	struct field *f = get_field(cmd);
	if (!f)
		return -1;
	strcpy(value, f->value);
	return 0;
}

int get_field_value_by_label(char *label, char *value){
	struct field *f = get_field_by_label(label);
	if (!f)
		return -1;
	strcpy(value, f->value);
	return 0;
}

int remote_update_field(int i, char *text){
	struct field * f = active_layout + i;

	if (f->cmd[0] == 0)
		return -1;
	
	//always send status afresh
	if (!strcmp(f->label, "STATUS")){
		//send time
		time_t now = time_sbitx();
		struct tm *tmp = gmtime(&now);
		sprintf(text, "STATUS %04d/%02d/%02d %02d:%02d:%02dZ",  
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
		return 1;
	}

	strcpy(text, f->label);
	strcat(text, " ");
	strcat(text, f->value);
	int update = f->update_remote;
	f->update_remote = 0;

	//debug on
//	if (!strcmp(f->cmd, "#text_in") && strlen(f->value))
//		printf("#text_in [%s] %d\n", f->value, update);
	//debug off
	return update;
}


// log is a special field that essentially is a like text
// on a terminal

void console_init(){
	for (int i =0;  i < MAX_CONSOLE_LINES; i++){
		console_stream[i].text[0] = 0;
		console_stream[i].style = console_style;
	}
	struct field *f = get_field("#console");
	console_current_line = 0;
	f->is_dirty = 1;
}

void web_add_string(char *string){
	while (*string){
		q_write(&q_web, *string++);
	}
}

void  web_write(int style, char *data){
	char tag[20];

	switch(style){
		case FONT_FT8_REPLY:
		case FONT_FT8_RX:
			strcpy(tag, "WSJTX-RX");
			break;
		case FONT_FLDIGI_RX:
			strcpy(tag, "FLDIGI-RX");
			break;
		case FONT_CW_RX:
			strcpy(tag, "CW-RX");
			break;
		case FONT_FT8_TX:
			strcpy(tag, "WSJTX-TX");
			break;
		case FONT_FT8_QUEUED:
			strcpy(tag, "WSJTX-Q");
			break;
		case FONT_FLDIGI_TX:
			strcpy(tag, "FLDIGI-TX");
			break;
		case FONT_CW_TX:
			strcpy(tag, "CW-TX");
			break;
		case FONT_TELNET:
			strcpy(tag, "TELNET");
			break;
		default:
			strcpy(tag, "LOG");
	}

	web_add_string("<");
	web_add_string(tag);		
	web_add_string(">");
	while (*data){
				switch(*data){
				case '<':
					q_write(&q_web, '&');
					q_write(&q_web, 'l');
					q_write(&q_web, 't');
					q_write(&q_web, ';');
					break;
				case '>':
					q_write(&q_web, '&');
					q_write(&q_web, 'g');
					q_write(&q_web, 't');
					q_write(&q_web, ';');
					break;
			 	case '"':
					q_write(&q_web, '&');
					q_write(&q_web, 'q');
					q_write(&q_web, 'u');
					q_write(&q_web, 't');
					q_write(&q_web, 'e');
					q_write(&q_web, ';');
					break;
				case '\'':
					q_write(&q_web, '&');
					q_write(&q_web, 'a');
					q_write(&q_web, 'p');
					q_write(&q_web, 'o');
					q_write(&q_web, 's');
					q_write(&q_web, ';');
					break;
				case '\n':
					q_write(&q_web, '&');
					q_write(&q_web, '#');
					q_write(&q_web, 'x');
					q_write(&q_web, 'A');
					q_write(&q_web, ';');
					break;	
				default:
					q_write(&q_web, *data);
			}
			data++;
	}			
	web_add_string("</");
	web_add_string(tag);
	web_add_string(">");
}

int console_init_next_line(){
	console_current_line++;
	if (console_current_line == MAX_CONSOLE_LINES)
		console_current_line = 0;
	console_stream[console_current_line].text[0] = 0;	
	console_stream[console_current_line].style = console_style;
	return console_current_line;
}

void write_to_remote_app(int style, char *text){
	remote_write("{");
	remote_write(text);
	remote_write("}");
}

// Pumps pending GTK events without blocking -- lets a long-running,
// otherwise-blocking operation (like the SWR sweep) keep the UI
// responsive (redraws, and critically, button presses like an abort)
// during its own sleep/wait periods, without needing a full non-blocking
// redesign. Same technique used in drexjj's sbitx fork for the same
// purpose.
void pump_gtk_events(void){
	while (gtk_events_pending())
		gtk_main_iteration_do(FALSE);
}

void write_console(int style, char *text){
	char directory[200];	//dangerous, find the MAX_PATH and replace 200 with it
	char *path = getenv("HOME");
	strcpy(directory, path);
	strcat(directory, "/sbitx/data/display_log.txt");

	web_write(style, text);
	//move to a new line if the style has changed
	if (style != console_style){
		q_write(&q_web, '{');
		q_write(&q_web, style + 'A');
		console_style = style;
		if (strlen(console_stream[console_current_line].text)> 0)
			console_init_next_line();	
		console_stream[console_current_line].style = style;
		switch(style){
			case FONT_FT8_RX:
			case FONT_FLDIGI_RX:
			case FONT_CW_RX:
				break;
			case FONT_FT8_TX:
			case FONT_FLDIGI_TX:
			case FONT_CW_TX:
			case FONT_FT8_REPLY:
				break;
			default:
				break;
		}
	}

	if (strlen(text) == 0)
		return;

	write_to_remote_app(style, text);
	if (oled_available)
		oled_console(style, text);
	while(*text){
		char c = *text;
		if (c == '\n')
			console_init_next_line();
		else if (c < 128 && c >= ' '){
			char *p = console_stream[console_current_line].text;
			int len = strlen(p);
			if(len >= console_cols - 1){
				//start a fresh line
				console_init_next_line();
				p = console_stream[console_current_line].text;
				len = 0;
			}
		
			//printf("Adding %c to %d\n", (int)c, console_current_line);	
			p[len++] = c & 0x7f;
			p[len] = 0;
		}
		text++;	
	}

	struct field *f = get_field("#console");
	if (f)
		f->is_dirty = 1;
}

void draw_console(cairo_t *gfx, struct field *f){
	char this_line[1000];
	int line_height = font_table[f->font_index].height; 	
	int n_lines = (f->height / line_height) - 1;

	rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

	//estimate!
	int char_width = measure_text(gfx, "01234567890123456789", f->font_index)/20;
	console_cols = f->width / char_width;
	int y = f->y; 
	int j = 0;

	int start_line = console_current_line - n_lines;
	if (start_line < 0)
		start_line += MAX_CONSOLE_LINES;

 	for (int i = 0; i <= n_lines; i++){
		struct console_line *l = console_stream + start_line;
		if (start_line == console_selected_line)
			fill_rect(gfx, f->x, y+1, f->width, font_table[l->style].height+1, SELECTED_LINE);
		draw_text(gfx, f->x+1, y, l->text, l->style);
		start_line++;
		y += line_height;
		if(start_line >= MAX_CONSOLE_LINES)
			start_line = 0;
	}
}

int do_console(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[100], *p, *q;

	int line_height = font_table[f->font_index].height; 	
	int n_lines = (f->height / line_height) - 1;
	int	l = 0;
	int start_line = console_current_line - n_lines;

	switch(event){
		case FIELD_DRAW:
			draw_console(gfx, f);
			return 1;
		break;
		case GDK_BUTTON_PRESS:
		case GDK_MOTION_NOTIFY:
			l = start_line + ((b - f->y)/line_height);
			if (l < 0)
				l += MAX_CONSOLE_LINES;
			console_selected_line = l;	
			f->is_dirty = 1;
			return 1;
		break;
		case GDK_BUTTON_RELEASE:
			if (!strcmp(get_field("r1:mode")->value, "FT8")){
				char ft8_message[100], ft8_response[100];
				strcpy(ft8_message, console_stream[console_selected_line].text);
				ft8_process(ft8_message, FT8_START_QSO);
			}
			f->is_dirty = 1;
			return 1;
		break;
		case FIELD_EDIT:
			if (a == MIN_KEY_UP && console_selected_line > start_line)
				console_selected_line--;
			else if (a == MIN_KEY_DOWN && console_selected_line < start_line + n_lines - 1)
				console_selected_line++;
		break;
	}
	return 0;	
}

void draw_field(GtkWidget *widget, cairo_t *gfx, struct field *f){
	struct font_style *s = font_table + 0;

	//push this to the web as well
	
	f->is_dirty = 0;
	if (f->x <= -1000)
		return;

	//if there is a handling function, use that else
	//skip down to the default behaviour of the controls
	if (f->fn){
		if(f->fn(f, gfx, FIELD_DRAW, -1, -1, 0)){
			f->is_dirty = 0;
			return;
		}
	}

	if (f_focus == f)
		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_FIELD_SELECTED);
	else 
		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
	if (f_focus == f)
		rect(gfx, f->x, f->y, f->width-1,f->height, SELECTED_LINE, 2);
	else if (f_hover == f)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 1);
	else if (f->value_type != FIELD_STATIC)
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

	int width, offset_x, text_length, line_start, y, label_height, 
		value_height, value_font, label_font;	
	char this_line[MAX_FIELD_LENGTH];
	int text_line_width = 0;

	int label_y;
	int use_reduced_font = 0;
	char *label = f->label;

	switch(f->value_type){
		case FIELD_TEXT:
			text_length = strlen(f->value);
			line_start = 0;
			y = f->y + 2;
			text_line_width = 0;
			while(text_length > 0){
				if (text_length > console_cols){
					strncpy(this_line, f->value + line_start, console_cols);
					this_line[console_cols] = 0;
				}
				else
					strcpy(this_line, f->value + line_start);		
				draw_text(gfx, f->x + 2, y, this_line, f->font_index);
				text_line_width= measure_text(gfx, this_line, f->font_index);
				y += 14;
				line_start += console_cols;
				text_length -= console_cols;
			}
			//draw the text cursor, if there is no text, the text baseline is zero
			if (strlen(f->value))
				y -= 14;
			fill_rect(gfx, f->x + text_line_width + 5, y+3, 9, 10, f->font_index);
		break;
		case FIELD_SELECTION:
		case FIELD_NUMBER:
		case FIELD_TOGGLE:
		case FIELD_BUTTON:
			label_height = font_table[FONT_FIELD_LABEL].height;
			width = measure_text(gfx, label, FONT_FIELD_LABEL);
			//skip the underscore in the label if it is too wide
			if (width > f->width && strchr(label, '_')){
				label = strchr(label, '_') + 1;
				width = measure_text(gfx, label, FONT_FIELD_LABEL);
			}

			offset_x = f->x + f->width/2 - width/2;
			//is it a two line display or a single line?
			if (f->value_type == FIELD_BUTTON && !f->value[0]){
				label_y = f->y + (f->height - label_height)/2;
				draw_text(gfx, offset_x,label_y, f->label, FONT_FIELD_LABEL);
			} 
			else {
				value_height = font_table[FONT_FIELD_VALUE].height;
				label_y = f->y + ((f->height  - label_height  - value_height)/2);
				draw_text(gfx, offset_x, label_y, label, FONT_FIELD_LABEL);
				width = measure_text(gfx, f->value, FONT_FIELD_VALUE);
				label_y += font_table[FONT_FIELD_LABEL].height;
				draw_text(gfx, f->x + f->width/2 - width/2, label_y, f->value, 
					FONT_FIELD_VALUE);
			}
      break;
		case FIELD_STATIC:
			draw_text(gfx, f->x, f->y, f->label, FONT_FIELD_LABEL);
			break;
		case FIELD_CONSOLE:
			//draw_console(gfx, f);
			break;
	}
}

static int mode_id(const char *mode_str){
	if (!strcmp(mode_str, "CW"))
		return MODE_CW;
	else if (!strcmp(mode_str, "CWR"))
		return MODE_CWR;
	else if (!strcmp(mode_str, "USB"))
		return MODE_USB;
	else if (!strcmp(mode_str,  "LSB"))
		return MODE_LSB;
	else if (!strcmp(mode_str,  "FT8"))
		return MODE_FT8;
	else if (!strcmp(mode_str,  "PSK31"))
		return MODE_PSK31;
	else if (!strcmp(mode_str,  "RTTY"))
		return MODE_RTTY;
	else if (!strcmp(mode_str, "NBFM"))
		return MODE_NBFM;
	else if (!strcmp(mode_str, "AM"))
		return MODE_AM;
	else if (!strcmp(mode_str, "2TONE"))
		return MODE_2TONE;
	else if (!strcmp(mode_str, "DIGITAL"))
		return MODE_DIGITAL;
	return -1;
}


static void save_user_settings(int forced){
	static int last_save_at = 0;
	char file_path[200];	//dangerous, find the MAX_PATH and replace 200 with it

	//attempt to save settings only if it has been 30 seconds since the 
	//last time the settings were saved
	int now = millis();
	if ((now < last_save_at + 30000 || !settings_updated) && forced == 0)
		return;

	char *path = getenv("HOME");
	strcpy(file_path, path);
	strcat(file_path, "/sbitx/data/user_settings.ini");

	//copy the current freq settings to the currently selected vfo
	struct field *f_freq = get_field("r1:freq");
	struct field *f_vfo  = get_field("#vfo");

	FILE *f = fopen(file_path, "w");
	if (!f){
		printf("Unable to save %s : %s\n", file_path, strerror(errno));
		settings_updated = 0;  // stop repeated attempts to write if file cannot be opened.		
		return;
	}

  // save the field values
	int i;
	for (i= 0; active_layout[i].cmd[0] > 0; i++){
		fprintf(f, "%s=%s\n", active_layout[i].cmd, active_layout[i].value);
	}

	//now save the band stack
	for (int i = 0; i < sizeof(band_stack)/sizeof(struct band); i++){
		fprintf(f, "\n[%s]\n", band_stack[i].name);
		//fprintf(f, "power=%d\n", band_stack[i].power);
		for (int j = 0; j < STACK_DEPTH; j++)
			fprintf(f, "freq%d=%d\nmode%d=%d\n", j, band_stack[i].freq[j], j, band_stack[i].mode[j]);
	}


	fclose(f);
	last_save_at = now;	// As proposed by Dave N1AI
	settings_updated = 0;
}


void enter_qso(){
	const char *callsign = field_str("CALL");
	const char *rst_sent = field_str("SENT");
	const char *rst_received = field_str("RECV");

	// skip empty or half filled log entry
	if (strlen(callsign) < 3 || strlen(rst_sent) < 1 || strlen(rst_received) < 1){
		printf("log entry is empty [%s], [%s], [%s], no log created\n", callsign, rst_sent, rst_received);
		return;
	}
 
	if (logbook_count_dup(field_str("CALL"), 60)){
		printf("Duplicate log entry not accepted for %s within two minutes of last entry of %s.\n", callsign, callsign);
		return;
	}	
	logbook_add(get_field("#contact_callsign")->value, 
		get_field("#rst_sent")->value, 
		get_field("#exchange_sent")->value, 
		get_field("#rst_received")->value, 
		get_field("#exchange_received")->value);
	char buff[100];
	sprintf(buff, "Logged: %s %s-%s %s-%s\n", 
		field_str("CALL"), field_str("SENT"), field_str("NR"), 
		field_str("RECV"), field_str("EXCH"));
	write_console(FONT_LOG, buff);
}

// The DSP side learns the level of each custom control only when the knob is
// turned. After a restart the boxes show the saved levels while the DSP still
// has its defaults, so, for example, NRVL showing 10 would really run at 0
// until the knob was touched. This pushes every saved level (and the saved
// ON/OFF of the toggle-type boxes) across once, right after the settings load.
// The combined level boxes (NBVL, NRVL, FMNT, FVLY, MCMP, ABST) still start
// switched off, as before; only their level is restored.
static void sync_dsp_levels(void){
#ifdef SBITX_FREEDV
	struct field *fdv_f = get_field("freedv");
	if (fdv_f)
		strcpy(fdv_f->value, "0");     // FreeDV always starts switched off
#endif
	static const char *cmds[] = {
		"notch", "notch_freq", "notch_width",
		"contour", "contour_freq", "contour_width",
		"valley", "formant_valley", "nb_threshold", "nr_threshold",
		"mic_comp", "audio_boost",
		"vox_threshold", "vox_hangtime", "vox_on",
		"usb_spk", "usb_mic", NULL
	};
	for (int i = 0; cmds[i]; i++){
		struct field *f = get_field((char *)cmds[i]);
		if (!f || !f->value[0])
			continue;
		char req[100], resp[100];
		snprintf(req, sizeof(req), "%s=%s", cmds[i], f->value);
		resp[0] = 0;
		sdr_request(req, resp);
		if (!strncmp(cmds[i], "usb_", 4) && strcmp(resp, "ok"))
			strcpy(f->value, "OFF");     // saved as ON but the USB device is not there now
	}
}

static int user_settings_handler(void* user, const char* section, 
            const char* name, const char* value)
{
    char cmd[1000];
    char new_value[200];

    strcpy(new_value, value);
    if (!strcmp(section, "r1")){
      sprintf(cmd, "%s:%s", section, name);
      set_field(cmd, new_value);
    }
    else if (!strcmp(section, "tx")){
      strcpy(cmd, name);
      set_field(cmd, new_value);
    }
	else if (!strncmp(section, "#kbd", 4)){
		return 1; //skip the keyboard values
	}
    // if it is an empty section
    else if (strlen(section) == 0){
      sprintf(cmd, "%s", name);
			//skip the button actions 
			struct field *f = get_field(cmd);
			if (f){
				if (f->value_type != FIELD_BUTTON)
      		set_field(cmd, new_value); 
			}
    }

		//band stacks
		int band = -1;
		if (!strcmp(section, "80M"))
			band = 0;
		else if (!strcmp(section, "40M"))
			band = 1;
		else if (!strcmp(section, "30M"))
			band = 2;
		else if (!strcmp(section, "20M"))
			band = 3;
		else if (!strcmp(section, "17M"))
			band = 4;
		else if (!strcmp(section, "15M"))
			band = 5;
		else if (!strcmp(section, "12M"))	
			band = 6;
		else if (!strcmp(section, "10M"))
			band = 7;	

		if (band >= 0  && !strcmp(name, "freq0"))
			band_stack[band].freq[0] = atoi(value);
		else if (band >= 0  && !strcmp(name, "freq1"))
			band_stack[band].freq[1] = atoi(value);
		else if (band >= 0  && !strcmp(name, "freq2"))
			band_stack[band].freq[2] = atoi(value);
		else if (band >= 0  && !strcmp(name, "freq3"))
			band_stack[band].freq[3] = atoi(value);
		else if (band >= 0 && !strcmp(name, "mode0"))
			band_stack[band].mode[0] = atoi(value);	
		else if (band >= 0 && !strcmp(name, "mode1"))
			band_stack[band].mode[1] = atoi(value);	
		else if (band >= 0 && !strcmp(name, "mode2"))
			band_stack[band].mode[2] = atoi(value);	
		else if (band >= 0 && !strcmp(name, "mode3"))
			band_stack[band].mode[3] = atoi(value);	

    return 1;
}
/* rendering of the fields */

// mod disiplay holds the tx modulation time domain envelope
// even values are the maximum and the even values are minimum

#define MOD_MAX 800
int mod_display[MOD_MAX];
int mod_display_index = 0;

void sdr_modulation_update(int32_t *samples, int count, double scale_up){
	double min=0, max=0;

	for (int i = 0; i < count; i++){
		if (i % 48 == 0 && i > 0){
			if (mod_display_index >= MOD_MAX)
				mod_display_index = 0;
			mod_display[mod_display_index++] = (min / 40000000.0) / scale_up;
			mod_display[mod_display_index++] = (max / 40000000.0) / scale_up;
			min = 0x7fffffff;
			max = -0x7fffffff;
		}
		if (*samples < min)
			min = *samples;
		if (*samples > max)
			max = *samples;
		samples++;
	}
}

void draw_modulation(struct field *f, cairo_t *gfx){

	int y, sub_division, i, grid_height;
	long	freq, freq_div;
	char	freq_text[20];

//	f = get_field("spectrum");
	sub_division = f->width / 10;
	grid_height = f->height - 10;

	// clear the spectrum	
	fill_rect(gfx, f->x,f->y, f->width, f->height, SPECTRUM_BACKGROUND);
	cairo_stroke(gfx);
	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);

	//draw the horizontal grid
	for (i =  0; i <= grid_height; i += grid_height/4){
		cairo_move_to(gfx, f->x, f->y + i);
		cairo_line_to(gfx, f->x + f->width, f->y + i); 
	}

	//draw the vertical grid
	for (i = 0; i <= f->width; i += f->width/10){
		cairo_move_to(gfx, f->x + i, f->y);
		cairo_line_to(gfx, f->x + i, f->y + grid_height); 
	}
	cairo_stroke(gfx);

	//start the plot
	cairo_set_source_rgb(gfx, palette[SPECTRUM_PLOT][0], 
		palette[SPECTRUM_PLOT][1], palette[SPECTRUM_PLOT][2]);
	cairo_move_to(gfx, f->x + f->width, f->y + grid_height);


	// Voice level, one-sided like a transceiver's modulation display: bars grow
	// up from the bottom, height = the larger of the positive and negative
	// peaks in that slice of time. Green for normal, amber when strong, red
	// near the top (the drive is about to clip).
	int n_env_samples = sizeof(mod_display)/sizeof(int32_t);
	int base_y = f->y + grid_height;
	for (i = 0; i < f->width; i++){
		int index = ((i * n_env_samples)/f->width) & ~1;
		int mn = mod_display[index];
		int mx = mod_display[index + 1];
		int h = (abs(mn) > abs(mx) ? abs(mn) : abs(mx)) * 2;
		if (h > grid_height) h = grid_height;
		if (h < 1) continue;
		int h_g = (int)(grid_height * 0.60), h_a = (int)(grid_height * 0.85);
		int seg = h < h_g ? h : h_g;
		cairo_set_source_rgb(gfx, 0.1, 0.85, 0.2);
		cairo_rectangle(gfx, f->x + i, base_y - seg, 1, seg);
		cairo_fill(gfx);
		if (h > h_g){
			seg = h < h_a ? h : h_a;
			cairo_set_source_rgb(gfx, 1.0, 0.75, 0.0);
			cairo_rectangle(gfx, f->x + i, base_y - seg, 1, seg - h_g);
			cairo_fill(gfx);
		}
		if (h > h_a){
			cairo_set_source_rgb(gfx, 1.0, 0.15, 0.1);
			cairo_rectangle(gfx, f->x + i, base_y - h, 1, h - h_a);
			cairo_fill(gfx);
		}
	}
}

static int waterfall_offset = 30;
static int  *wf = NULL;
GdkPixbuf *waterfall_pixbuf = NULL;
guint8 *waterfall_map = NULL;

// ---------------------------------------------------------------------
// Two-colour waterfall: blue = baseband noise, red = signal above it.
// Set WF_TWO_COLOUR to 0 to get the original blue-cyan-green-yellow-red
// ramp back.
//
// "Noise" is judged against a running noise-floor estimate taken from the
// spectrum itself, in dB (the radio's own spectrum scale: 10*log10 of
// the squared FFT bin, i.e. relative dB, not calibrated dBm). Every
// number below was chosen by simulating the radio's own smoothed
// spectrum (bins are magnitude-averaged at spectrum_speed = 0.3):
//
//  - The floor is the 20TH PERCENTILE of the row plus 1.7 dB, not the
//    average or median. The median jumps onto the signal as soon as one
//    fills more than half the span (+28 dB error at 60% occupancy); the
//    20th percentile stays within ~1.7 dB even then. The +1.7 dB makes a
//    noise-only row read as its true median.
//  - A pixel is "signal" (red) when it is more than 7 dB over that floor.
//    Smoothed noise only fluctuates ~2 dB (std), so at 7 dB pure noise
//    produced zero false red in simulation; signals ~10 dB over the noise
//    show ~90% red and 15 dB and up show fully red. Lower it (5-6) to
//    catch weaker signals at the cost of occasional red speckle in the
//    noise; raise it for an even cleaner blue.
//  - The floor follows with alpha 0.25 per row and SNAPS when it moves
//    more than 8 dB. Slow tracking (alpha 0.10) left the whole waterfall
//    red for ~10 rows after a +20 dB noise step (band change, gain
//    change); with the snap there are no red rows after a step.
//
// Known limitation: at the 2.5K span a full-width SSB signal can fill the
// entire window, leaving no noise to measure, so it won't show red there.
// ---------------------------------------------------------------------
#define WF_TWO_COLOUR 1
#define WF_FLOOR_PERCENTILE 20
#define WF_FLOOR_PERCENTILE_OFFSET_DB 1.7
#define WF_SIGNAL_MARGIN_DB 7.0
#define WF_FLOOR_TRACK 0.25
#define WF_FLOOR_SNAP_DB 8.0
#define WF_DB_MIN -120
#define WF_DB_MAX 120

extern volatile int dsp_nr_reduction_x10;   // from sbitx.c: how much quieter NRVL/FVLY/FMNT made the passband noise, in tenths of a dB

static int *wf_db = NULL;           // raw dB per waterfall column, same indexing as wf[]
static double wf_floor_db = 0.0;    // current noise-floor estimate, in the spectrum's dB
static int wf_floor_valid = 0;

// Called once per spectrum draw, after wf_db has been filled for the row.
static void wf_update_noise_floor(int width){
	int hist[WF_DB_MAX - WF_DB_MIN + 1];
	int i, b, cum = 0;

	if (!wf_db || width < 4)
		return;

	// The plot loop never writes column 0 (its indices run 1..width+n), so
	// it would stay at its initial value forever -- copy its neighbour in,
	// otherwise it would be drawn as a permanent stripe down the left edge.
	wf_db[0] = wf_db[1];

	memset(hist, 0, sizeof(hist));
	for (i = 0; i < width; i++){
		int d = wf_db[i];
		if (d < WF_DB_MIN) d = WF_DB_MIN;
		if (d > WF_DB_MAX) d = WF_DB_MAX;
		hist[d - WF_DB_MIN]++;
	}

	int target = (width * WF_FLOOR_PERCENTILE) / 100;
	for (b = 0; b < WF_DB_MAX - WF_DB_MIN + 1; b++){
		cum += hist[b];
		if (cum > target)
			break;
	}
	double est = (b + WF_DB_MIN) + WF_FLOOR_PERCENTILE_OFFSET_DB;

	if (!wf_floor_valid || fabs(est - wf_floor_db) > WF_FLOOR_SNAP_DB){
		wf_floor_db = est;      // first row, or the level genuinely stepped: re-lock at once
		wf_floor_valid = 1;
	}
	else
		wf_floor_db += WF_FLOOR_TRACK * (est - wf_floor_db);
}

void init_waterfall(){
	struct field *f = get_field("waterfall");

	if (wf)
		free(wf);
	//this will store the db values of waterfall
	wf = malloc((MAX_BINS/2) * f->height * sizeof(int));
	if (!wf){
		puts("*Error: malloc failed on waterfall buffer");
		exit(0);
	}
	memset(wf, 0, (MAX_BINS/2) * f->height * sizeof(int));

	// raw per-column dB for the two-colour waterfall; only one row's worth
	// is ever needed. Starts at the bottom of the range, not 0, so an
	// unwritten column can never look like a strong signal.
	if (wf_db)
		free(wf_db);
	wf_db = malloc((MAX_BINS/2) * sizeof(int));
	if (!wf_db){
		puts("*Error: malloc failed on waterfall dB buffer");
		exit(0);
	}
	for (int i = 0; i < MAX_BINS/2; i++)
		wf_db[i] = WF_DB_MIN;
	wf_floor_valid = 0;

	if (waterfall_map)
		free(waterfall_map);
	//this will store the bitmap pixles, 3 bytes per pixel
	waterfall_map = malloc(f->width * f->height * 3);
	for (int i = 0; i < f->width; i++)
		for (int j = 0; j < f->height; j++){
			int row = j * f->width * 3;
			int	index = row + i * 3;
			waterfall_map[index++] = 0;
			waterfall_map[index++] = 0;//i % 256;
			waterfall_map[index++] =0;// j % 256; 
	}
	if (waterfall_pixbuf)
		g_object_unref(waterfall_pixbuf);
	waterfall_pixbuf = gdk_pixbuf_new_from_data(waterfall_map,
		GDK_COLORSPACE_RGB, FALSE, 8, f->width, f->height, f->width*3, NULL,NULL);
		// format,         alpha?, bit,  widht,    height, rowstride, destryfn, data

//	printf("%ld return from pixbuff", (int)waterfall_pixbuf);	
}

// One horizontal colour bar: label on the left, bar with tick marks in the
// middle (green -> amber -> red zones, the filled part drawn bright, the
// rest of each zone dim), value on the right.
//   frac        0..1   how far the bar is filled
//   amber, red  0..1   where those zones start
static void draw_colour_bar(cairo_t *gfx, int x, int y, int w, int h,
	char *name, char *value, double frac, double amber, double red,
	char *t0, char *t1, char *t2){
	int label_w = 44, value_w = 74;
	int bx = x + label_w, bw = w - label_w - value_w;
	double zone[3][2] = {{0, amber}, {amber, red}, {red, 1.0}};
	double col[3][3] = {{0.10, 0.80, 0.20}, {1.0, 0.75, 0.0}, {1.0, 0.15, 0.10}};

	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;

	draw_text(gfx, x + 2, y + (h - 14)/2, name, FONT_FIELD_LABEL);

	for (int z = 0; z < 3; z++){
		double z0 = zone[z][0], z1 = zone[z][1];
		int px0 = bx + (int)(z0 * bw), px1 = bx + (int)(z1 * bw);
		// dim background of the whole zone
		cairo_set_source_rgb(gfx, col[z][0]*0.22, col[z][1]*0.22, col[z][2]*0.22);
		cairo_rectangle(gfx, px0, y, px1 - px0, h);
		cairo_fill(gfx);
		// bright part up to the current value
		if (frac > z0){
			double top = frac < z1 ? frac : z1;
			int pf = bx + (int)(top * bw);
			cairo_set_source_rgb(gfx, col[z][0], col[z][1], col[z][2]);
			cairo_rectangle(gfx, px0, y, pf - px0, h);
			cairo_fill(gfx);
		}
	}
	// outline
	cairo_set_source_rgb(gfx, 0.7, 0.7, 0.7);
	cairo_set_line_width(gfx, 1);
	cairo_rectangle(gfx, bx + 0.5, y + 0.5, bw, h);
	cairo_stroke(gfx);

	// scale ticks (every 10%) and three numbers underneath
	for (int i = 1; i < 10; i++){
		int tx = bx + (i * bw) / 10;
		cairo_set_source_rgb(gfx, 0.0, 0.0, 0.0);
		cairo_move_to(gfx, tx + 0.5, y + h - (i == 5 ? 6 : 3));
		cairo_line_to(gfx, tx + 0.5, y + h);
		cairo_stroke(gfx);
	}
	if (t0[0]){      // an empty first label means: compact bar, no numbers underneath
		int sy = y + h + 1;
		draw_text(gfx, bx, sy, t0, FONT_SMALL);
		draw_text(gfx, bx + bw/2 - measure_text(gfx, t1, FONT_SMALL)/2, sy, t1, FONT_SMALL);
		draw_text(gfx, bx + bw - measure_text(gfx, t2, FONT_SMALL), sy, t2, FONT_SMALL);
	}

	// value, in the colour of the zone it is in
	int zi = frac >= red ? 2 : (frac >= amber ? 1 : 0);
	cairo_set_source_rgb(gfx, col[zi][0], col[zi][1], col[zi][2]);
	{
		struct font_style *fs = font_table + FONT_FIELD_VALUE;
		cairo_select_font_face(gfx, fs->name, fs->type, fs->weight);
		cairo_set_font_size(gfx, fs->height);
		cairo_move_to(gfx, bx + bw + 8, y + (h - fs->height)/2 + fs->height);
		cairo_show_text(gfx, value);
	}
}

// Transmit meters: forward power, VSWR and ALC as three colour bars, shown
// together in the lower panel (where the waterfall is, while receiving).
void draw_tx_meters(struct field *f, cairo_t *gfx){
	char val[32];
	int vswr = field_int("REF");
	int power = field_int("POWER");     // 1/10 watt

	if (!in_tx)
		power = 0;                       // receiving: show idle meters, not the last transmit values

	//power is in 1/10th of watts and vswr is also 1/10th
	if (power < 30)
		vswr = 10;

	// ALC: alc_level 1.0 = no reduction, below 1.0 = limiter pulling drive back.
	// The bar shows how much is being taken off, so empty = ALC not acting.
	char alc_buff[10];
	double alc_cut = 0;
	if (in_tx){
		sprintf(alc_buff, "%d", (int)(alc_level * 100));
		set_field("#alc", alc_buff);
		alc_cut = 1.0 - alc_level;
		if (alc_cut < 0) alc_cut = 0;
	}

	// four compact bars: PWR, SWR, ALC and DRV (the drive setting), no numbers underneath
	int bh = 10, pitch = 16;
	int strip_h = 3 * pitch + bh + 8;
	if (strip_h > f->height * 2 / 3)
		strip_h = f->height * 2 / 3;              // never take more than two thirds of the waterfall
	if (strip_h < 3 * pitch + bh + 8){            // small waterfall: tighten the bars
		pitch = (strip_h - 8 - bh) / 3;
		if (pitch < bh + 1) pitch = bh + 1;
	}
	int x = f->x + 6, w = f->width - 12, y = f->y + f->height - strip_h + 4;
	fill_rect(gfx, f->x, f->y + f->height - strip_h, f->width, strip_h, SPECTRUM_BACKGROUND);
	cairo_set_source_rgb(gfx, 0.5, 0.5, 0.5);
	cairo_move_to(gfx, f->x, f->y + f->height - strip_h + 0.5);
	cairo_line_to(gfx, f->x + f->width, f->y + f->height - strip_h + 0.5);
	cairo_stroke(gfx);

	if (power >= 1000)
		sprintf(val, "%d W", power/10);
	else
		sprintf(val, "%d.%d W", power/10, power%10);
	draw_colour_bar(gfx, x, y, w, bh, "PWR", val, power / 500.0, 0.70, 0.92, "", "", "");

	if (vswr >= 100)
		strcpy(val, "9.9+");
	else
		sprintf(val, "%d.%d", vswr/10, vswr%10);
	// scale 1.0 .. 3.0 (3.0 and above pegs the bar)
	draw_colour_bar(gfx, x, y + pitch, w, bh, "SWR", val, (vswr - 10) / 20.0, 0.25, 0.50, "", "", "");

	sprintf(val, "-%d %%", (int)(alc_cut * 100));
	draw_colour_bar(gfx, x, y + 2*pitch, w, bh, "ALC", val, alc_cut, 0.20, 0.50, "", "", "");

	// DRV: the DRIVE setting (0-100), shown on receive as well as transmit
	int drive = field_int("DRIVE");
	if (drive < 0) drive = 0;
	if (drive > 100) drive = 100;
	sprintf(val, "%d %%", drive);
	draw_colour_bar(gfx, x, y + 3*pitch, w, bh, "DRV", val, drive / 100.0, 0.70, 0.90, "", "", "");
}

void draw_waterfall(struct field *f, cairo_t *gfx){

	memmove(waterfall_map + f->width * 3, waterfall_map, 
		f->width * (f->height - 1) * 3);

	int index = 0;

#if WF_TWO_COLOUR
	// Blue = noise, red = signal above it. Brightness still follows level
	// within each colour so the texture of the noise and the strength of a
	// signal stay readable; only the hue changes at the noise/signal line.
	{
		double floor_db = wf_floor_db;
		for (int i = 0; i < f->width; i++){
			int d = wf_db ? wf_db[i] : WF_DB_MIN;
			if (d < WF_DB_MIN) d = WF_DB_MIN;
			if (d > WF_DB_MAX) d = WF_DB_MAX;
			double rel = d - floor_db;
			double t;

			if (rel < WF_SIGNAL_MARGIN_DB){
				// noise: dark blue well below the floor, bright blue right at the line
				t = (rel + 8.0) / (WF_SIGNAL_MARGIN_DB + 8.0);
				if (t < 0.0) t = 0.0;
				if (t > 1.0) t = 1.0;
				waterfall_map[index++] = 0;
				waterfall_map[index++] = 0;
				waterfall_map[index++] = (guint8)(70.0 + 185.0 * t);
			}
			else{
				// signal: dull red just over the line, full red 25 dB above it
				t = (rel - WF_SIGNAL_MARGIN_DB) / 25.0;
				if (t > 1.0) t = 1.0;
				waterfall_map[index++] = (guint8)(150.0 + 105.0 * t);
				waterfall_map[index++] = 0;
				waterfall_map[index++] = 0;
			}
		}
	}
#else
	for (int i = 0; i < f->width; i++){
			int v = wf[i] * 2;
			if (v > 100)		//we limit ourselves to 100 db range
				v = 100;

			if (v < 20){									// r = 0, g= 0, increase blue
				waterfall_map[index++] = 0;
				waterfall_map[index++] = 0;
				waterfall_map[index++] = v * 12; 
			}
			else if (v < 40){							// r = 0, increase g, blue is max
				waterfall_map[index++] = 0;
				waterfall_map[index++] = (v - 20) * 12;
				waterfall_map[index++] = 255; 
			}
			else if (v < 60){							// r = 0, g= max, decrease b
				waterfall_map[index++] = 0;
				waterfall_map[index++] = 255; 
				waterfall_map[index++] = (60-v)*12; 
			}
			else if (v < 80){						 	// increase r, g = max, b = 0
				waterfall_map[index++] = (v-60) * 12;
				waterfall_map[index++] = 255;
				waterfall_map[index++] = 0; 
			}else {												// r = max, decrease g, b = 0
				waterfall_map[index++] = 255;
				waterfall_map[index++] = (100-v) * 12;
				waterfall_map[index++] = 0; 
			}
	}
#endif

	gdk_cairo_set_source_pixbuf(gfx, waterfall_pixbuf, f->x, f->y);		
	cairo_paint(gfx);
	cairo_fill(gfx);

	// transmitting: the waterfall keeps scrolling above, the meters sit in a
	// compact strip along the bottom of the same area
	draw_tx_meters(f, gfx);     // always shown; reads zero while receiving
}

void draw_spectrum_grid(struct field *f_spectrum, cairo_t *gfx){
	int sub_division, grid_height;
	struct field *f = f_spectrum;

	sub_division = f->width / 10;
	grid_height = f->height - (font_table[FONT_SMALL].height * 4 / 3); 

	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], 
		palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);


	cairo_set_line_width(gfx, 1);
	cairo_set_source_rgb(gfx, palette[SPECTRUM_GRID][0], 
		palette[SPECTRUM_GRID][1], palette[SPECTRUM_GRID][2]);

	//draw the horizontal grid
	int i;
	for (i =  0; i <= grid_height; i += grid_height/4){
		cairo_move_to(gfx, f->x, f->y + i);
		cairo_line_to(gfx, f->x + f->width, f->y + i); 
	}

	//draw the vertical grid
	for (i = 0; i <= f->width; i += f->width/10){
		cairo_move_to(gfx, f->x + i, f->y);
		cairo_line_to(gfx, f->x + i, f->y + grid_height); 
	}
	cairo_stroke(gfx);
}

// dB -> screen y on the spectrum graph, using EXACTLY the mapping the trace
// itself uses (y = ((db + waterfall_offset) * f->height) / 80, measured up
// from the bottom of the grid), so the axis can never disagree with it.
static int spectrum_db_to_y(struct field *f, int grid_height, double db){
	return f->y + grid_height - (int)(((db + waterfall_offset) * f->height) / 80.0);
}

// One axis label: opaque dark backing so it stays readable where the
// trace passes behind it (a translucent one let the trace show through the
// letters, so "NF -2" read like "NF~-2"), then the text, vertically centred on y_center.
static void draw_axis_label(cairo_t *gfx, int x, int y_center, const char *text,
	double r, double g, double b){
	struct font_style *s = font_table + FONT_SMALL;
	cairo_text_extents_t ext;

	cairo_select_font_face(gfx, s->name, s->type, s->weight);
	cairo_set_font_size(gfx, s->height);
	cairo_text_extents(gfx, text, &ext);
	cairo_set_source_rgb(gfx, 0, 0, 0);
	cairo_rectangle(gfx, x - 2, y_center - s->height/2 - 1, ext.x_advance + 4, s->height + 2);
	cairo_fill(gfx);
	cairo_set_source_rgb(gfx, r, g, b);
	cairo_move_to(gfx, x, y_center - s->height/2 + s->height);
	cairo_show_text(gfx, text);
}

// Y axis for the spectrum graph, in dB. These are the radio's own relative
// spectrum dB (10*log10 of the squared FFT bin), the same scale the trace
// is already drawn in -- NOT calibrated dBm. Short ticks every 10 dB with
// labels every 20 dB down the left edge, a dashed blue line at the current
// noise floor, and that floor's value as a flag on the axis. The floor is
// the very level the waterfall's blue/red split is measured from, so the
// axis and the waterfall tell the same story.
//
// Drawn LAST (over the trace), not with the grid: the trace is the noise,
// so it always runs along the floor line -- anything drawn underneath it
// would be struck through by it. If the floor flag lands on a tick label,
// the tick label gives way (the flag carries the number instead).
static void draw_spectrum_yaxis(struct field *f, cairo_t *gfx, int grid_height, int pb_x, int pb_w){
	struct font_style *s = font_table + FONT_SMALL;
	char label[24];
	int d, fh = s->height;
	int nf_y = -1;

	if (wf_floor_valid){
		nf_y = spectrum_db_to_y(f, grid_height, wf_floor_db);
		if (nf_y < f->y + 1)
			nf_y = f->y + 1;
		if (nf_y > f->y + grid_height - 1)
			nf_y = f->y + grid_height - 1;
	}

	// noise floor: one dashed line across the graph
	if (nf_y >= 0){
		double dash[2] = {3.0, 3.0};
		cairo_set_line_width(gfx, 1);
		cairo_set_source_rgb(gfx, 0.6, 0.8, 1.0);
		cairo_set_dash(gfx, dash, 2, 0);
		cairo_move_to(gfx, f->x + 5, nf_y + 0.5);
		cairo_line_to(gfx, f->x + f->width, nf_y + 0.5);
		cairo_stroke(gfx);
		cairo_set_dash(gfx, NULL, 0, 0);
	}

	// Scale: just three numbers (-20, 0, +20 dB), no tick marks -- the
	// faint grid lines already mark the levels. A number is left out if
	// it would sit on the noise-floor flag.
	for (d = -20; d <= 20; d += 20){
		int y = spectrum_db_to_y(f, grid_height, d);
		if (y < f->y + fh/2 || y > f->y + grid_height - fh/2)
			continue;
		if (nf_y >= 0 && abs(y - nf_y) < fh + 2)
			continue;
		if (d == 0)
			strcpy(label, "0");
		else
			sprintf(label, "%+d", d);
		draw_axis_label(gfx, f->x + 2, y, label, s->r, s->g, s->b);
	}

	// noise floor flag, e.g. "NF -14 dB", at the left edge
	if (nf_y >= 0){
		int ny = nf_y;
		if (ny - fh/2 < f->y)
			ny = f->y + fh/2;
		if (ny + fh/2 > f->y + grid_height)
			ny = f->y + grid_height - fh/2;
		sprintf(label, "NF %d dB", (int)floor(wf_floor_db + 0.5));
		draw_axis_label(gfx, f->x + 2, ny, label, 0.6, 0.8, 1.0);
	}

	// DSP noise reduction: a small number at the top right, only when the
	// DSP filters are actually taking noise out.
	int red_x10 = dsp_nr_reduction_x10;
	if (red_x10 >= 5){
		char rl[24];
		sprintf(rl, "NR -%d.%d dB", red_x10 / 10, red_x10 % 10);
		int rw = measure_text(gfx, rl, FONT_SMALL);
		draw_axis_label(gfx, f->x + f->width - rw - 4, f->y + fh/2 + 1, rl, 0.4, 1.0, 0.4);
	}
}

void draw_spectrum(struct field *f_spectrum, cairo_t *gfx){
	int y, sub_division, i, grid_height, bw_high, bw_low, pitch, tx_pitch;
	float span;
	struct field *f;
	long	freq, freq_div;
	char	freq_text[20];

	// no separate transmit display: the spectrum keeps running while sending
	// (the radio feeds it the transmitted audio), so VOX/PTT never blanks it

	pitch = field_int("PITCH");
	tx_pitch = field_int("TX_PITCH");
	struct field *mode_f = get_field("r1:mode");
	freq = atol(get_field("r1:freq")->value);

	span = atof(get_field("#span")->value);
	bw_high = atoi(get_field("r1:high")->value);
	bw_low = atoi(get_field("r1:low")->value);
	grid_height = f_spectrum->height - ((font_table[FONT_SMALL].height * 4) /3);
	sub_division = f_spectrum->width / 10;

	// the step is in khz, we multiply by 1000 and div 10(divisions) = 100 
	freq_div = span * 100;  

	//calculate the position of bandwidth strip
	int filter_start, filter_width;

	if(!strcmp(mode_f->value, "CWR") || !strcmp(mode_f->value, "LSB")){
	 	filter_start = f_spectrum->x + (f_spectrum->width/2) - 
			((f_spectrum->width * bw_high)/(span * 1000)); 
		if (filter_start < f_spectrum->x){
	 	  filter_width = ((f_spectrum->width * (bw_high -bw_low))/(span * 1000)) - (f_spectrum->x - filter_start); 
			filter_start = f_spectrum->x;
    } else {
	 	  filter_width = (f_spectrum->width * (bw_high -bw_low))/(span * 1000); 
    }
		if (filter_width + filter_start > f_spectrum->x + f_spectrum->width)
			filter_width = f_spectrum->x + f_spectrum->width - filter_start;
		pitch = f_spectrum->x + (f_spectrum->width/2) -
			((f_spectrum->width * pitch)/(span * 1000));
	}
	else {
		filter_start = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * bw_low)/(span * 1000)); 
		if (filter_start < f_spectrum->x)
			filter_start = f_spectrum->x;
		filter_width = (f_spectrum->width * (bw_high-bw_low))/(span * 1000); 
		if (filter_width + filter_start > f_spectrum->x + f_spectrum->width)
			filter_width = f_spectrum->x + f_spectrum->width - filter_start;
		pitch = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * pitch)/(span * 1000));
		tx_pitch = f_spectrum->x + (f_spectrum->width/2) + 
			((f_spectrum->width * tx_pitch)/(span * 1000));
	}
	// clear the spectrum	
	f = f_spectrum;
	fill_rect(gfx, f->x,f->y, f->width, f->height, SPECTRUM_BACKGROUND);
	cairo_stroke(gfx);
	// Passband envelope: a rectangle, centered on the tuning frequency
	// (spectrum center) and widened (1.8x) for visual clarity/visibility.
	// trap_start/trap_width are hoisted out of their own block (unlike
	// before) because the notch and contour markers below now use this
	// SAME box as their positioning reference -- 0% along its width maps
	// to the passband's LOW edge, 100% maps to HIGH, so NFRQ/CFRQ travel
	// the box's full width and reach its edges exactly, rather than being
	// positioned against the much larger overall SPAN (which is why they
	// barely appeared to move before).
	int bw_field = field_int("BW");
	int total_width_hz;
	if ((!strcmp(mode_f->value, "USB") || !strcmp(mode_f->value, "LSB")) && bw_field > 0)
		total_width_hz = bw_field;
	else
		total_width_hz = bw_high - bw_low;
	if (total_width_hz < 1)
		total_width_hz = 1;

	int widened_width_px = (int)((f_spectrum->width * total_width_hz * 1.8) / (span * 1000));
	int trap_start = f_spectrum->x + (f_spectrum->width/2) - (widened_width_px/2);
	int trap_width = widened_width_px;

	if (trap_start < f_spectrum->x){
		trap_width -= (f_spectrum->x - trap_start);
		trap_start = f_spectrum->x;
	}
	if (trap_start + trap_width > f_spectrum->x + f_spectrum->width)
		trap_width = f_spectrum->x + f_spectrum->width - trap_start;

	// Passband envelope: a plain rectangle, per explicit preference.
	fill_rect(gfx, trap_start, f->y, trap_width, grid_height, SPECTRUM_BANDWIDTH);
	cairo_stroke(gfx);

	// Band-edge indicator (separate from the audio passband above): shows
	// the legal India WPC amateur allocation limits for whichever HF band
	// the current tuned frequency falls in. A different scale entirely
	// from the audio passband (100-1700kHz wide vs a few kHz) -- drawn as
	// thin vertical lines rather than a filled region, and only visible
	// when the current SPAN is narrow enough / tuning is close enough to
	// an edge for it to actually fall within the displayed window.
	{
		long band_low_hz = 0, band_high_hz = 0;
		if (freq >= 7000000 && freq <= 7200000){ band_low_hz = 7000000; band_high_hz = 7200000; }
		else if (freq >= 14000000 && freq <= 14350000){ band_low_hz = 14000000; band_high_hz = 14350000; }
		else if (freq >= 18068000 && freq <= 18168000){ band_low_hz = 18068000; band_high_hz = 18168000; }
		else if (freq >= 21000000 && freq <= 21450000){ band_low_hz = 21000000; band_high_hz = 21450000; }
		else if (freq >= 28000000 && freq <= 29700000){ band_low_hz = 28000000; band_high_hz = 29700000; }

		if (band_low_hz > 0){
			long span_hz = (long)(span * 1000);
			int edge_x;

			if (labs(freq - band_low_hz) <= span_hz/2){
				edge_x = f_spectrum->x + (f_spectrum->width/2) +
					(int)((f_spectrum->width * (band_low_hz - freq)) / span_hz);
				cairo_set_source_rgb(gfx, 1.0, 0.7, 0.0);  // amber: regulatory limit, distinct from notch/contour colors
				cairo_set_line_width(gfx, 2);
				cairo_move_to(gfx, edge_x, f->y);
				cairo_line_to(gfx, edge_x, f->y + grid_height);
				cairo_stroke(gfx);
			}
			if (labs(freq - band_high_hz) <= span_hz/2){
				edge_x = f_spectrum->x + (f_spectrum->width/2) +
					(int)((f_spectrum->width * (band_high_hz - freq)) / span_hz);
				cairo_set_source_rgb(gfx, 1.0, 0.7, 0.0);
				cairo_set_line_width(gfx, 2);
				cairo_move_to(gfx, edge_x, f->y);
				cairo_line_to(gfx, edge_x, f->y + grid_height);
				cairo_stroke(gfx);
			}
		}
	}

	// Notch marker: a V-shape nested inside the passband box, tip pointing
	// down at the exact notch frequency, sides spreading up to the width
	// on either side. Position is now a proportion of the passband BOX
	// itself, using the ACTUAL REACHABLE range (accounting for the
	// passband lock, which keeps NFRQ within [LOW+half_width,
	// HIGH-half_width] so the whole notch stays inside the passband) --
	// not the raw LOW-HIGH span. Using the raw span meant NFRQ could
	// never actually reach frac=0 (it's locked to stop half a width short
	// of LOW), leaving a visible gap at the left edge. Mapping against the
	// true reachable range makes the marker genuinely span edge-to-edge.
	if (notch_enabled && total_width_hz > 0){
		// Determine current width preset's Hz value first, since the
		// achievable range depends on it.
		const char *nwidth_str = field_str("NWTH");
		double notch_pct;
		int notch_min_px;
		int notch_width_val;
		if (nwidth_str && !strcmp(nwidth_str, "NARROW")){
			notch_pct = 0.03; notch_min_px = 4; notch_width_val = 50;
		}
		else if (nwidth_str && !strcmp(nwidth_str, "WIDE")){
			notch_pct = 0.08; notch_min_px = 10; notch_width_val = 150;
		}
		else{
			notch_pct = 0.05; notch_min_px = 7; notch_width_val = 100;
		}
		int notch_half_hz = notch_width_val / 2;

		double reach_low = bw_low + notch_half_hz;
		double reach_high = bw_low + total_width_hz - notch_half_hz;
		double reach_span = reach_high - reach_low;
		double frac = (reach_span > 0) ? (notch_freq_hz - reach_low) / reach_span : 0.5;
		if (frac < 0) frac = 0;
		if (frac > 1) frac = 1;

		int notch_x;
		if (!strcmp(mode_f->value, "CWR") || !strcmp(mode_f->value, "LSB"))
			notch_x = trap_start + trap_width - (int)(frac * trap_width);
		else
			notch_x = trap_start + (int)(frac * trap_width);

		int notch_half_px = (int)(notch_pct * trap_width / 2.0);
		if (notch_half_px < notch_min_px)
			notch_half_px = notch_min_px;

		cairo_set_source_rgb(gfx, 1.0, 0.15, 0.15);
		cairo_set_line_width(gfx, 2);
		cairo_move_to(gfx, notch_x - notch_half_px, f->y);
		cairo_line_to(gfx, notch_x, f->y + grid_height);
		cairo_line_to(gfx, notch_x + notch_half_px, f->y);
		cairo_stroke(gfx);

		// Frequency label directly beneath the marker on the spectrum
		// itself, per request.
		char notch_label[16];
		sprintf(notch_label, "%dHz", notch_width_val);
		int label_w = measure_text(gfx, notch_label, FONT_FIELD_LABEL);
		draw_text(gfx, notch_x - label_w/2, f->y + grid_height + 2, notch_label, FONT_FIELD_LABEL);
	}

	// Contour marker: an inverted-U (dome) nested in the upper half of the
	// passband box, base not touching the floor. Same achievable-range
	// positioning and sizing fix as the notch above.
	if (contour_enabled && total_width_hz > 0){
		const char *cwidth_str = field_str("CONW");
		double contour_pct;
		int contour_min_px;
		int contour_width_val;
		if (cwidth_str && !strcmp(cwidth_str, "NARROW")){
			contour_pct = 0.05; contour_min_px = 6; contour_width_val = 100;
		}
		else if (cwidth_str && !strcmp(cwidth_str, "WIDE")){
			contour_pct = 0.14; contour_min_px = 16; contour_width_val = 300;
		}
		else{
			contour_pct = 0.09; contour_min_px = 11; contour_width_val = 200;
		}
		int contour_half_hz = contour_width_val / 2;

		double reach_low = bw_low + contour_half_hz;
		double reach_high = bw_low + total_width_hz - contour_half_hz;
		double reach_span = reach_high - reach_low;
		double frac = (reach_span > 0) ? (contour_freq_hz - reach_low) / reach_span : 0.5;
		if (frac < 0) frac = 0;
		if (frac > 1) frac = 1;

		int contour_x;
		if (!strcmp(mode_f->value, "CWR") || !strcmp(mode_f->value, "LSB"))
			contour_x = trap_start + trap_width - (int)(frac * trap_width);
		else
			contour_x = trap_start + (int)(frac * trap_width);

		int contour_half_px = (int)(contour_pct * trap_width / 2.0);
		if (contour_half_px < contour_min_px)
			contour_half_px = contour_min_px;

		int dome_base_y = f->y + (grid_height / 2);   // base sits at the midline -- upper half only
		int dome_peak_y = f->y;                        // peak reaches the top of the passband

		cairo_set_source_rgb(gfx, 0.2, 0.85, 0.3);   // green: boost, distinct from the notch's red
		cairo_set_line_width(gfx, 3);   // thicker than notch's 2px, for more visual prominence
		cairo_move_to(gfx, contour_x - contour_half_px, dome_base_y);
		cairo_curve_to(gfx,
			contour_x - contour_half_px, dome_peak_y,   // control point 1: pulls left side up
			contour_x + contour_half_px, dome_peak_y,   // control point 2: pulls right side up
			contour_x + contour_half_px, dome_base_y);  // end point: right base
		cairo_stroke(gfx);

		char contour_label[16];
		sprintf(contour_label, "%dHz", contour_width_val);
		int clabel_w = measure_text(gfx, contour_label, FONT_FIELD_LABEL);
		draw_text(gfx, contour_x - clabel_w/2, f->y + grid_height + 2, contour_label, FONT_FIELD_LABEL);
	}

	draw_spectrum_grid(f_spectrum, gfx);
	f = f_spectrum;

	//draw the frequency readout at the bottom
	cairo_set_source_rgb(gfx, palette[COLOR_TEXT_MUTED][0], 
			palette[COLOR_TEXT_MUTED][1], palette[COLOR_TEXT_MUTED][2]);
	long f_start = freq - (4 * freq_div); 
	for (i = f->width/10; i < f->width; i += f->width/10){
    if ((span == 25) || (span == 10)){
		  sprintf(freq_text, "%ld", f_start/1000);
    } else {
      float f_start_temp = (((float)f_start/1000000.0) - ((int)(f_start/1000000))) *1000;
		  sprintf(freq_text, "%5.1f", f_start_temp);
    }
		int off = measure_text(gfx, freq_text, FONT_SMALL)/2;
		draw_text(gfx, f->x + i - off , f->y+grid_height , freq_text, FONT_SMALL);
		f_start += freq_div;
	}

	//we only plot the second half of the bins (on the lower sideband
	int last_y = 100;

	int n_bins = (int)((1.0 * spectrum_span) / 46.875);
	//the center frequency is at the center of the lower sideband,
	//i.e, three-fourth way up the bins.
	int starting_bin = (3 *MAX_BINS)/4 - n_bins/2;
	int ending_bin = starting_bin + n_bins; 

	float x_step = (1.0 * f->width )/n_bins;

	//start the plot
	static float tr_x[MAX_BINS + 8], tr_y[MAX_BINS + 8];
	int tr_n = 0;

//	float x = fmod((1.0 * spectrum_span), 46.875);
	float x = 0;
	int j = 0;

	for (i = starting_bin; i <= ending_bin; i++){
		int y;

		// the center fft bin is at zero, from MAX_BINS/2 onwards,
		// the bins are at lowest frequency (-ve frequency)
		//y axis is the power  in db of each bin, scaled to 80 db
		y = ((spectrum_plot[i] + waterfall_offset) * f->height)/80; 
		// limit y inside the spectrum display box
		if ( y <  0)
			y = 0;
		if (y > f->height)
			y = f->height - 1;
		//the plot should be increase upwards
		if (tr_n < MAX_BINS + 8){
			tr_x[tr_n] = f->x + f->width - (int)x;
			tr_y[tr_n] = f->y + grid_height - y;
			tr_n++;
		}

		//fill the waterfall
		for (int k = 0; k <= 1 + (int)x_step; k++){
			wf[k + f->width - (int)x] = (y * 100)/grid_height;
			// the raw dB too (unclamped): the colour split and the noise
			// floor need the real level, which the clamped trace height loses
			if (wf_db)
				wf_db[k + f->width - (int)x] = spectrum_plot[i];
		}
		x += x_step;
		if (f->width <= x)
			x = f->width - 1;
	}
	wf_update_noise_floor(f->width);

	// Waveform in two colours: blue = noise, red = signal standing clear of
	// the noise floor (the same floor and 7 dB margin the dB axis uses).
	// The whole trace is filled blue; the part of it rising above the
	// signal line is then filled red by clipping to the area above that line.
	if (tr_n > 1){
		double sig_y = -1;
		if (wf_floor_valid)
			sig_y = spectrum_db_to_y(f, grid_height, wf_floor_db + WF_SIGNAL_MARGIN_DB);

		#define TRACE_PATH() do { \
			cairo_move_to(gfx, tr_x[0], f->y + grid_height); \
			for (int q = 0; q < tr_n; q++) cairo_line_to(gfx, tr_x[q], tr_y[q]); \
			cairo_line_to(gfx, tr_x[tr_n-1], f->y + grid_height); \
			cairo_close_path(gfx); } while (0)
		#define TRACE_LINE() do { \
			cairo_move_to(gfx, tr_x[0], tr_y[0]); \
			for (int q = 1; q < tr_n; q++) cairo_line_to(gfx, tr_x[q], tr_y[q]); } while (0)

		cairo_save(gfx);
		cairo_rectangle(gfx, f->x, f->y, f->width, grid_height);
		cairo_clip(gfx);

		TRACE_PATH();
		cairo_set_source_rgba(gfx, 0.10, 0.35, 1.0, 0.55);
		cairo_fill(gfx);
		cairo_set_line_width(gfx, 1.5);
		TRACE_LINE();
		cairo_set_source_rgb(gfx, 0.45, 0.7, 1.0);
		cairo_stroke(gfx);

		if (sig_y > f->y){
			cairo_save(gfx);
			cairo_rectangle(gfx, f->x, f->y, f->width, sig_y - f->y);
			cairo_clip(gfx);
			TRACE_PATH();
			cairo_set_source_rgba(gfx, 1.0, 0.12, 0.10, 0.85);
			cairo_fill(gfx);
			TRACE_LINE();
			cairo_set_source_rgb(gfx, 1.0, 0.55, 0.5);
			cairo_stroke(gfx);
			cairo_restore(gfx);
		}
		cairo_restore(gfx);
		cairo_set_line_width(gfx, 1);
		#undef TRACE_PATH
		#undef TRACE_LINE
	}
 
  if (pitch >= f_spectrum->x){
    cairo_set_source_rgb(gfx, palette[COLOR_RX_PITCH][0],
			palette[COLOR_RX_PITCH][1], palette[COLOR_RX_PITCH][2]);
    if(!strcmp(mode_f->value, "USB") || !strcmp(mode_f->value, "LSB")){ // for LSB and USB draw pitch line at center
	    cairo_move_to(gfx, f->x + (f->width/2), f->y);
	    cairo_line_to(gfx, f->x + (f->width/2), f->y + grid_height); 
    } else {
	    cairo_move_to(gfx, pitch, f->y);
	    cairo_line_to(gfx, pitch, f->y + grid_height); 
    }
   	cairo_stroke(gfx);
  }

  if (tx_pitch >= f_spectrum->x && !strcmp(mode_f->value, "FT8")){
    cairo_set_source_rgb(gfx, palette[COLOR_TX_PITCH][0],
			palette[COLOR_TX_PITCH][1], palette[COLOR_TX_PITCH][2]);
	  cairo_move_to(gfx, tx_pitch, f->y);
	  cairo_line_to(gfx, tx_pitch, f->y + grid_height); 
   	cairo_stroke(gfx);
  }
	//draw the needle
	for (struct rx *r = rx_list; r; r = r->next){
		int needle_x  = (f->width*(MAX_BINS/2 - r->tuned_bin))/(MAX_BINS/2);
		fill_rect(gfx, f->x + needle_x, f->y, 1, grid_height,  SPECTRUM_NEEDLE);
	}

	// dB axis and noise-floor marker, last so the trace can't strike through them
	draw_spectrum_yaxis(f_spectrum, gfx, grid_height, trap_start, trap_width);
}

int waterfall_fn(struct field *f, cairo_t *gfx, int event, int a, int b){
		if(f->fn(f, gfx, FIELD_DRAW, -1, -1, 0))
	switch(FIELD_DRAW){
		case FIELD_DRAW:
			draw_waterfall(f, gfx);
			break;
	}
}

char* freq_with_separators(const char* freq_str){

  int freq = atoi(freq_str);
  int f_mhz, f_khz, f_hz;
  char temp_string[11];
  static char return_string[11];

  f_mhz = freq / 1000000;
  f_khz = (freq - (f_mhz*1000000)) / 1000;
  f_hz = freq - (f_mhz*1000000) - (f_khz*1000);

  sprintf(temp_string,"%d",f_mhz);
  strcpy(return_string,temp_string);
  strcat(return_string,".");
  if (f_khz < 100){
    strcat(return_string,"0");
  }
  if (f_khz < 10){
    strcat(return_string,"0");
  }
  sprintf(temp_string,"%d",f_khz);
  strcat(return_string,temp_string);
  strcat(return_string,".");
  if (f_hz < 100){
    strcat(return_string,"0");
  }
  if (f_hz < 10){
    strcat(return_string,"0");
  }
  sprintf(temp_string,"%d",f_hz);
  strcat(return_string,temp_string);
  return return_string;
}

void draw_dial(struct field *f, cairo_t *gfx){
	struct font_style *s = font_table + 0;
	struct field *rit = get_field("#rit");
	struct field *split = get_field("#split");
	struct field *vfo = get_field("#vfo");
	struct field *vfo_a = get_field("#vfo_a_freq");
	struct field *vfo_b = get_field("#vfo_b_freq");
	struct field *rit_delta = get_field("#rit_delta");
	char buff[20];

  char temp_str[20];

	fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);

	//update the vfos
	if (vfo->value[0] == 'A')
			strcpy(vfo_a->value, f->value);
	else
			strcpy(vfo_b->value, f->value);

  if (!strcmp(rit->value, "ON")){
    if (!in_tx){
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(temp_str, "%d", (atoi(f->value) + atoi(rit_delta->value)));
      sprintf(buff, "RX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
    else {
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
      sprintf(temp_str, "%d", (atoi(f->value) + atoi(rit_delta->value)));
      sprintf(buff, "RX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
    }
  }
  else if (!strcmp(split->value, "ON")){
    if (!in_tx){
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "TX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "RX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
    else {
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "TX:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
      sprintf(buff, "RX:%d", atoi(f->value) + atoi(rit_delta->value));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
    }
  }
  else if (!strcmp(vfo->value, "A")){
    if (!in_tx){
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "B:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "A:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    } else {
			strcpy(temp_str, vfo_b->value);
      sprintf(buff, "B:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
  }
  else{ /// VFO B is active
    if (!in_tx){
			strcpy(temp_str, vfo_a->value);
      //sprintf(temp_str, "%d", vfo_a_freq);
      sprintf(buff, "A:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "B:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }else {
			strcpy(temp_str, vfo_a->value);
      //sprintf(temp_str, "%d", vfo_a_freq);
      sprintf(buff, "A:%s", freq_with_separators(temp_str));
      draw_text(gfx, f->x+5 , f->y+1 , buff , FONT_LARGE_FIELD);
      sprintf(buff, "TX:%s", freq_with_separators(f->value));
      draw_text(gfx, f->x+5 , f->y+15 , buff , FONT_LARGE_VALUE);
    }
  }
}

void invalidate_rect(int x, int y, int width, int height){
	if (display_area){
		gtk_widget_queue_draw_area(display_area, x, y, width, height);
	}
}

//the keyboard appears at the bottom 150 pixels of the window
void keyboard_display(int show){
	struct field *f;

	//we start the height at -200 because the first key
	//will bump it down by a row
	int height = screen_height - 200; 
	for (f = active_layout; f->cmd[0]; f++){
		if (!strncmp(f->cmd,"#kbd", 4)){
			//start of a new line? move down
			if (f->x == 0)
				height += 50;
			update_field(f);
			if (show && f->y < 0)
				f->y = height;
			else if (!show)
				f->y = -1000;
			update_field(f);
		}
	}
}

void field_move(char *field_label, int x, int y, int width, int height){
	struct field *f = get_field_by_label(field_label);
	if (!f)
		return;

	// Capture the OLD on-screen rectangle before overwriting it. This is
	// the actual root cause of fields from a previous menu page appearing
	// as "ghosts" after switching pages: update_field() only marks a
	// field dirty when ITS OWN position is on-screen (y >= 0), so hiding
	// a field (moving it to y=-1000) never told GTK to redraw the OLD
	// spot it used to occupy -- leaving stale pixels of whatever was
	// there before. Explicitly invalidating the old rectangle here forces
	// that region to be properly redrawn/cleared regardless of where the
	// field is moving to.
	int old_x = f->x, old_y = f->y, old_w = f->width, old_h = f->height;

	f->x = x;
	f->y = y;

	f->width = width;
	f->height = height;
	update_field(f);
	if (old_y >= 0)
		invalidate_rect(old_x, old_y, old_w, old_h);
	if (!strcmp(field_label, "WATERFALL"))
		init_waterfall();
}

//scales the ui as per current screen width from
//the nominal 800x480 size of the original layout
static void layout_ui(){
	int x1, y1, x2, y2;
	struct field *f;
	
	x1 =0 ;
	x2 = screen_width;
	y1 = 100;
	y2 = screen_height;

	//first move all the controls that are not common out of sight
	for (f = active_layout; f->cmd[0]; f++)
		if (!(f->section & COMMON_CONTROL)){
			update_field(f);
			f->y = -1000;
			update_field(f);
		}

	//locate the kbd to the right corner
	field_move("KBD", screen_width - 47, screen_height-47, 45, 45);
	field_move("MENU", screen_width - 47, screen_height-97, 45, 45);

	//now, move the main radio controls to the right
	field_move("FREQ", x2-205, 0, 180, 40);
	field_move("AUDIO", x2-45, 5, 40, 40);
	field_move("IF", x2-45, 50, 40, 40);
	field_move("DRIVE", x2-85, 50, 40, 40);
	field_move("BW", x2-125, 50, 40, 40);
	field_move("AGC", x2-165, 50, 40, 40);

	field_move("STEP", x2-245, 5, 40, 40);
	field_move("RIT", x2-285, 5, 40, 40);
	field_move("SPLIT", x2-285, 50, 40, 40);
	field_move("VFO", x2-245, 50, 40, 40);
	field_move("SPAN", x2-205, 50, 40, 40);

	//new notch/noise-blanker/noise-reduction controls now live behind the
	//MENU button (next to KBD) instead of all being crammed on screen at
	//once -- see update_menu_page(). Positioned right after RX below,
	//using y1 directly rather than recomputing y2-50 here -- y2 gets
	//adjusted further down (KBD on/off changes it), so grabbing it this
	//early caused the menu row to drift out of sync with the real RX row
	//whenever KBD was toggled.
	

	if (!strcmp(field_str("KBD"), "ON")){
		//take out 3 button widths from the bottom
		y2 = screen_height - 150;
		keyboard_display(1);
	}
	else
		keyboard_display(0);
	
	int m_id = mode_id(field_str("MODE"));
	int button_width = 100;
	switch(m_id){
		case MODE_FT8:
			field_move("CONSOLE", 5, y1, 350, y2-y1-55);
			field_move("SPECTRUM", 360, y1, x2-365, 100);
			field_move("WATERFALL", 360, y1+100, x2-365, y2-y1-155);
			field_move("ESC", 5, y2-47, 40, 45);
			field_move("F1", 50, y2-47, 50, 45);
			field_move("F2", 100, y2-47, 50, 45);
			field_move("F3", 150, y2-47, 50, 45);
			field_move("F4", 200, y2-47, 50, 45);
			field_move("F5", 250, y2-47, 50, 45);
			field_move("F6", 300, y2-47, 50, 45);
			field_move("F7", 350, y2-47, 50, 45);
			field_move("F8", 400, y2-47, 45, 45);
			field_move("FT8_REPEAT", 450, y2-47, 50, 45);
			field_move("FT8_TX1ST", 500, y2-47, 50, 45);
			field_move("FT8_AUTO", 550, y2-47, 50, 45);
			field_move("TX_PITCH", 600, y2-47, 73, 45);
			field_move("SIDETONE", 675, y2-47, 73, 45);
			menu_system_visible = 0;
			update_menu_page();
		break;
		case MODE_CW:
		case MODE_CWR:
			field_move("CONSOLE", 5, y1, 350, y2-y1-110);
			field_move("SPECTRUM", 360, y1, x2-365, 70);
			field_move("WATERFALL", 360, y1+60, x2-365, y2-y1-110);
			// first line below the decoder/waterfall
			field_move("ESC", 5, y2-47, 40, 45);
			field_move("F1", 50, y2-47, 40, 45);
			field_move("F2", 90, y2-47, 40, 45);
			field_move("F3", 130, y2-47, 40, 45);
			field_move("F4", 170, y2-47, 40, 45);
			field_move("F5", 210, y2-47, 40, 45);
			field_move("F6", 250, y2-47, 40, 45);
			field_move("F7", 290, y2-47, 40, 45);
			field_move("F8", 330, y2-47, 40, 45);
			field_move("F9", 370, y2-47, 40, 45);
			field_move("F10", 410, y2-47, 45, 45);
			field_move("WPM",455, y2-47, 45, 45);
			field_move("PITCH", 500, y2-47, 45, 45);
			field_move("CW_DELAY", 545, y2-47,50, 45);
			field_move("CW_INPUT", 595, y2-47, 70 , 45);
			field_move("SIDETONE", 665, y2-47, 70, 45);
			menu_system_visible = 0;
			update_menu_page();
			break;
		case MODE_USB:
		case MODE_LSB:
		case MODE_AM:
		case MODE_NBFM:
			field_move("CONSOLE", 5, y1, 350, y2-y1-55);
			field_move("SPECTRUM", 360, y1, x2-365, 70);
			field_move("WATERFALL", 360, y1+70, x2-365, y2-y1-125);
			y1 = y2 -50;
			field_move("MIC", 5, y1, 45, 45);
			field_move("LOW", 60, y1, 95, 45);
			field_move("HIGH", 160, y1, 95, 45);
			field_move("TX", 260, y1, 95, 45);
			field_move("RX", 360, y1, 95, 45);
			menu_row_y = y1;
			menu_system_visible = 1;
			update_menu_page();
		break;
		default:
			field_move("CONSOLE", 5, y1, 350, y2-y1-110);
			field_move("SPECTRUM", 360, y1, x2-365, 70);
			field_move("WATERFALL", 360, y1+70, x2-365, y2-y1-180);
			y1 = y2 - 105;
			field_move("F1", 5, y1, 90, 45);
			field_move("F2", 100, y1, 95, 45);
			field_move("F3", 200, y1, 100, 45);
			field_move("F4", 300, y1, 100, 45);
			field_move("F5", 400, y1, 100, 45);
			field_move("F6", 500, y1, 100, 45);
			field_move("F7", 600, y1, 100, 45);
			field_move("F8", 700, y1, 95, 45);
			y1 += 50;
			field_move("F9", 5, y1, 95, 45);
			field_move("F10", 100, y1, 100, 45);
			field_move("F11",200, y1, 100, 45);
			field_move("F12",300, y1, 95, 45);
			field_move("LOW", 400, y1, 50, 45);
			field_move("HIGH", 475, y1, 50, 45);
			field_move("PITCH", 550, y1, 50, 45);
			field_move("SIDETONE", 600, y1, 95, 45);
			menu_system_visible = 0;
			update_menu_page();
		break;	
	}
	invalidate_rect(0,0,screen_width, screen_height);
}
void dump_ui(){
	FILE *pf = fopen("main_ui.ini", "w");
	for (int i = 0; active_layout[i].cmd[0] > 0; i++){
		struct field *f = active_layout + i;
		fprintf(pf, "\n[%s]\n", f->cmd);
		fprintf(pf, "label: %s\n", f->label);
		fprintf(pf, "x:%d\n", f->x);
		fprintf(pf, "y:%d\n", f->y);
		fprintf(pf, "width:%d\n", f->width);
		fprintf(pf, "height:%d\n",f->height);
		fprintf(pf, "value:%s\n", f->value);
		fprintf(pf, "type:%d\n", f->value_type);
		fprintf(pf, "font:%d\n", f->font_index);
		fprintf(pf, "selection:%s\n", f->selection);
		fprintf(pf, "min:%d\n", f->min);
		fprintf(pf, "max:%d\n", f->max);
		fprintf(pf, "step:%d\n", f->step);
	}
	fclose(pf);
}

void redraw_main_screen(GtkWidget *widget, cairo_t *gfx){
	double dx1, dy1, dx2, dy2;
	int x1, y1, x2, y2;

	cairo_clip_extents(gfx, &dx1, &dy1, &dx2, &dy2);
	x1 = (int)dx1;
	y1 = (int)dy1;
	x2 = (int)dx2;
	y2 = (int)dy2;

	fill_rect(gfx, x1, y1, x2-x1, y2-y1, COLOR_BACKGROUND);
	for (int i = 0; active_layout[i].cmd[0] > 0; i++){
		double cx1, cx2, cy1, cy2;
		struct field *f = active_layout + i;
		cx1 = f->x;
		cx2 = cx1 + f->width;
		cy1 = f->y;
		cy2 = cy1 + f->height;
		if (cairo_in_clip(gfx, cx1, cy1) || cairo_in_clip(gfx, cx2, cy2))
			draw_field(widget, gfx, active_layout + i);
		//else if (f->label[0] == 'F')
		//	printf("skipping %s\n", active_layout[i].label);
	}
}

/* gtk specific routines */
static gboolean on_draw_event( GtkWidget* widget, cairo_t *cr, gpointer user_data ) {
	redraw_main_screen(widget, cr);	
  return FALSE;
}

static gboolean on_resize(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
	screen_width = event->width;
	screen_height = event->height;
//	gtk_container_resize_children(GTK_CONTAINER(window));
//	gtk_widget_set_default_size(display_area, screen_width, screen_height);
	layout_ui();	
	return FALSE;
}

void update_field(struct field *f){
	if (f->y >= 0)
		f->is_dirty = 1;
	f->update_remote = 1;
} 

static void hover_field(struct field *f){
	struct field *prev_hover = f_hover;
	if (f){
		//set f_hover to none to remove the outline
		f_hover = NULL;
		update_field(prev_hover);
	}
	f_hover = f;
	update_field(f);
}


// respond to a UI request to change the field value
static void edit_field(struct field *f, int action){
	int v;
	if (f == f_focus)
		focus_since = millis();

	if (f->fn){
		f->is_dirty = 1;
	 	f->update_remote = 1;
		if (f->fn(f, NULL, FIELD_EDIT, action, 0, 0))
			return;
	}
	
	if (f->value_type == FIELD_NUMBER){
		int	v = atoi(f->value);
		if (action == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (action == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d",  v);
	}
	else if (f->value_type == FIELD_SELECTION){
		char *p, *prev, *next, b[100], *first, *last;
    // get the first and last selections
    strcpy(b, f->selection);
    p = strtok(b, "/");
    first = p;
    while(p){
      last = p;
      p = strtok(NULL, "/");
    }
		//search the current text in the selection
		prev = NULL;
		strcpy(b, f->selection);
		p = strtok(b, "/");
		while(p){
			if (!strcmp(p, f->value))
				break;
			else
				prev = p;
			p = strtok(NULL, "/");
		}	
		//set to the first option
		if (p == NULL){
			if (prev)
				strcpy(f->value, prev);
		}
		else if (action == MIN_KEY_DOWN){
			prev = p;
			p = strtok(NULL,"/");
			if (p)
				strcpy(f->value, p);
			else
        strcpy(f->value, first); // roll over
				//return;
				//strcpy(f->value, prev); 
		}
		else if (action == MIN_KEY_UP){
			if (prev)
				strcpy(f->value, prev);
			else
        strcpy(f->value, last); // roll over
				//return;
		}
	}
	else if (f->value_type == FIELD_TOGGLE){
		char *p, *prev, *next, b[100];
		//search the current text in the selection
		prev = NULL;
		strcpy(b, f->selection);
		p = strtok(b, "/");
		while(p){
			if (strcmp(p, f->value))
				break;
			p = strtok(NULL, "/");
		}	
		strcpy(f->value, p);
	}
	else if (f->value_type == FIELD_BUTTON){
		NULL; // ah, do nothing!
	}

	//send a command to the radio
	char buff[200];

//	sprintf(buff, "%s=%s", f->cmd, f->value);

	sprintf(buff, "%s %s", f->label, f->value);
	do_control_action(buff);
	f->is_dirty = 1;
	f->update_remote = 1;
//	update_field(f);
	settings_updated++;
}

static void focus_field(struct field *f){
	struct field *prev_hover = f_hover;
	struct field *prev_focus = f_focus;

	// If focus is moving to a genuinely different field, clear the
	// toggle-tap tracker -- otherwise coming back to a combined
	// toggle+scale box after tapping elsewhere would be wrongly treated
	// as a repeat tap on the same box and skip toggling.
	if (prev_focus != f)
		last_toggle_tap = NULL;
	
	f_focus = NULL;
	if (prev_hover)
		update_field(prev_hover);
	if (prev_focus)
		update_field(prev_focus);
	if (f){
		f_focus = f_hover = f;
		focus_since = millis();
	}
	update_field(f_hover);

	//is it a toggle field?
	if (f_focus->value_type == FIELD_TOGGLE)
		edit_field(f_focus, MIN_KEY_DOWN);	

	if (f_focus->value_type == FIELD_TEXT)
		f_last_text = f_focus;
  //is it a selection field?
  if (f_focus->value_type == FIELD_SELECTION) 
    edit_field(f_focus, MIN_KEY_UP);

	//if the button has been pressed, do the needful
	if (f_focus->value_type == FIELD_TOGGLE || 
			f_focus->value_type == FIELD_BUTTON)
				do_control_action(f->label);
}

time_t time_sbitx(){
	if (time_delta)
		return time(NULL);
}


// setting the frequency is complicated by having to take care of the
// rit/split and power levels associated with each frequency
void set_operating_freq(int dial_freq, char *response){
	struct field *rit = get_field("#rit");
	struct field *split = get_field("#split");
	struct field *vfo_a = get_field("#vfo_a_freq");
	struct field *vfo_b = get_field("#vfo_b_freq");
	struct field *rit_delta = get_field("#rit_delta");

	char freq_request[30];
 
	if (!strcmp(rit->value, "ON")){
		if (!in_tx)
			sprintf(freq_request, "r1:freq=%d", dial_freq + atoi(rit_delta->value)); 		
		else
			sprintf(freq_request, "r1:freq=%d", dial_freq); 		
	}
	else if (!strcmp(split->value, "ON")){
		if (!in_tx)
			sprintf(freq_request, "r1:freq=%s", vfo_b->value);
		else
			sprintf(freq_request, "r1:freq=%d", dial_freq);
	}
	else
			sprintf(freq_request, "r1:freq=%d", dial_freq);

	//get back to setting the frequency
	sdr_request(freq_request, response);
}

void abort_tx(){
	set_field("#text_in", "");
	modem_abort();
	tx_off();
}

int do_spectrum(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	struct field *f_freq, *f_span, *f_pitch;
	int span, pitch;
  long freq;
	char buff[100];
  int mode = mode_id(get_field("r1:mode")->value);

	switch(event){
		case FIELD_DRAW:
			draw_spectrum(f, gfx);
			return 1;
		break;
		case GDK_MOTION_NOTIFY:
	    f_freq = get_field("r1:freq");
		  freq = atoi(f_freq->value);
		  f_span = get_field("#span");
		  span = atof(f_span->value) * 1000;
		  //a has the x position of the mouse
		  freq -= ((a - last_mouse_x) * (span/f->width));
		  sprintf(buff, "%ld", freq);
		  set_field("r1:freq", buff);
		  return 1;
		break;
    case GDK_BUTTON_PRESS: 
      if (c == GDK_BUTTON_SECONDARY){ // right click QSY
        f_freq = get_field("r1:freq");
        freq = atoi(f_freq->value);
        f_span = get_field("#span");
        span = atof(f_span->value) * 1000;
        f_pitch = get_field("rx_pitch");
        pitch = atoi(f_pitch->value);
        if (mode == MODE_CW){
          freq += ((((float)(a - f->x) / (float)f->width) - 0.5) * (float)span) - pitch;
        } else if (mode == MODE_CWR){
          freq += ((((float)(a - f->x) / (float)f->width) - 0.5) * (float)span) + pitch;
        } else { // other modes may need to be optimized - k3ng 2022-09-02
          freq += (((float)(a - f->x) / (float)f->width) - 0.5) * (float)span;
        }
        sprintf(buff, "%ld", freq);
        set_field("r1:freq", buff);
        return 1;
      }
    break;
	}
	return 0;	
}

int do_waterfall(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	switch(event){
		case FIELD_DRAW:
			draw_waterfall(f, gfx);
			return 1;
/*
		case GDK_MOUSE_MOVE:{
			struct field *f_freq = get_field("r1:freq");
			long freq = atoi(f_freq->value);
			struct field *f_span = get_field("#span");
			int span = atoi(f_focus->value);
			freq -= ((x - last_mouse_x) *tuning_step)/4;	//slow this down a bit
			sprintf(buff, "%ld", freq);
			set_field("r1:freq", buff);
			}
			return 1;
		break;
*/
	}
	return 0;	
}

void remote_execute(char *cmd){

	if (q_remote_commands.overflow)
		q_empty(&q_remote_commands);
	while (*cmd)
		q_write(&q_remote_commands, *cmd++);
	q_write(&q_remote_commands, 0);
}


void call_wipe(){
	field_set("CALL", "");
	field_set("SENT", "");
	field_set("RECV", "");
	field_set("EXCH", "");
	field_set("NR", "");
}

void update_titlebar(){
	char buff[100];

	time_t now = time_sbitx();
	struct tm *tmp = gmtime(&now);
	sprintf(buff, "sBitx %s %s %04d/%02d/%02d %02d:%02d:%02dZ",  
		get_field("#mycallsign")->value, get_field("#mygrid")->value,
		tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
 	gtk_window_set_title( GTK_WINDOW(window), buff);

}

// calcualtes the LOW and HIGH settings from bw
// and sets them up, called from UI
void save_bandwidth(int hz){
	char bw[10];

 	int mode = mode_id(get_field("r1:mode")->value);
	sprintf(bw, "%d", hz);
	switch(mode){
		case MODE_CW:
		case MODE_CWR:
			field_set("BW_CW",bw); 
			break;
		case MODE_USB:
		case MODE_LSB:
		case MODE_NBFM:
		case MODE_AM:
			field_set("BW_VOICE",bw); 
			break;
		default:
			field_set("BW_DIGITAL",bw); 
	}
}

void set_filter_high_low(int hz){
	char buff[10], bw_str[10];
	int low, high;

	if (hz < 50)
		return;

	struct field *f_mode = get_field("r1:mode");
	struct field *f_pitch = get_field("rx_pitch");

	switch(mode_id(f_mode->value)){
		case MODE_CW:
		case MODE_CWR:
			low = atoi(f_pitch->value) - hz/2;
			high = atoi(f_pitch->value) + hz/2;
			break;
		case MODE_LSB:
		case MODE_USB:
			low = 300;
			high = low + hz;
			break;
		case MODE_DIGITAL:
			low = atoi(f_pitch->value) - (hz/2);
			high = atoi(f_pitch->value) + (hz/2);
			break;
		case MODE_AM:
			low = 300;
			high = hz;
			break;
		case MODE_FT8:
			low = 50;
			high = 4000;
			break;
		default:
			low = 50;
			high = 3000;
	}

	if (low < 50)
		low = 50;
	if (high > 5000)
		high = 5000;

	//now set the bandwidth
	sprintf(buff, "%d", low);
	set_field("r1:low", buff);
	sprintf(buff, "%d", high);
	set_field("r1:high", buff);
}
int do_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[100];

	if (event == FIELD_DRAW){
		time_t now = time_sbitx();
		struct tm *tmp = gmtime(&now);
		sprintf(buff, "%04d/%02d/%02d %02d:%02d:%02dZ",  
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
		int width = measure_text(gfx, buff, FONT_FIELD_LABEL);
		int line_height = font_table[f->font_index].height; 	
		strcpy(f->value, buff);
		f->is_dirty = 1;
		f->update_remote = 1;
		sprintf(buff, "sBitx %s %s %04d/%02d/%02d %02d:%02d:%02dZ",  
			get_field("#mycallsign")->value, get_field("#mygrid")->value,
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 
  	gtk_window_set_title( GTK_WINDOW(window), buff);

		return 1;
	}
	return 0;
}

void execute_app(char *app){
	char buff[1000];

	sprintf(buff, "%s 0> /dev/null", app); 
	int pid = fork();
	if (!pid){
		system(buff);
		exit(0);	
	}
}

int do_text(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int width, offset, text_length, line_start, y;	
	char this_line[MAX_FIELD_LENGTH];
	int text_line_width = 0;

	if (event == FIELD_EDIT){
		//if it is a command, then execute it and clear the field
		if (f->value[0] == COMMAND_ESCAPE &&  strlen(f->value) > 1 && (a == '\n' || a == MIN_KEY_ENTER)){
			cmd_exec(f->value + 1);
			f->value[0] = 0;
			update_field(f);
		}
		else if ((a =='\n' || a == MIN_KEY_ENTER) && !strcmp(get_field("r1:mode")->value, "FT8") 
			&& f->value[0] != COMMAND_ESCAPE){
			ft8_tx(f->value, field_int("TX_PITCH"));
			f->value[0] = 0;		
		}
		else if (a >= ' ' && a <= 127 && strlen(f->value) < f->max-1){
			int l = strlen(f->value);
			f->value[l++] = a;
			f->value[l] = 0;
		}
		//handle ascii delete 8 or gtk 
		else if ((a == MIN_KEY_BACKSPACE || a == 8) && strlen(f->value) > 0){
			int l = strlen(f->value) - 1;
			f->value[l] = 0;
		}
		f->is_dirty = 1;
		f->update_remote = 1;
		f_last_text = f; 
		return 1;
	}
	else if (event == FIELD_DRAW){
		if (f_focus == f)
			fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_FIELD_SELECTED);
		else
			fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);

		rect(gfx, f->x, f->y, f->width-1,f->height, COLOR_CONTROL_BOX, 1);
		text_length = strlen(f->value);
		line_start = 0;
		y = f->y + 1;
		text_line_width= measure_text(gfx, f->value, f->font_index);
		if (!strlen(f->value))
			draw_text(gfx, f->x + 1, y+1, f->label, FONT_FIELD_LABEL);
		else 
			draw_text(gfx, f->x + 1, y+1, f->value, f->font_index);
		//draw the text cursor, if there is no text, the text baseline is zero
		if (f_focus == f){
			fill_rect(gfx, f->x + text_line_width+3, y+16, 9, 2, COLOR_SELECTED_BOX);
		}
		
		return 1;
	}
	return 0;
}

int do_pitch(struct field *f, cairo_t *gfx, int event, int a, int b, int c){

	int	v = atoi(f->value);

	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max){
			v += f->step;
		}
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min){
			v -= f->step;
		}
		sprintf(f->value, "%d", v);
		update_field(f);
		modem_set_pitch(v);
		char buff[20], response[20];
		sprintf(buff, "rx_pitch=%d", v);
		sdr_request(buff, response);

		//move the bandwidth accordingly
  	int mode = mode_id(get_field("r1:mode")->value);
		int bw = 4000;
		switch(mode){
			case MODE_CW:
			case MODE_CWR:
				bw = field_int("BW_CW");
				break;
			case MODE_USB:
			case MODE_LSB:
				bw = field_int("BW_VOICE");
				break;
			case MODE_FT8:
				bw = 4000;
				break;	
			default:
				bw = field_int("BW_DIGITAL");
		}
		set_filter_high_low(bw);
		return 1;
	}
		
	return 0;
}

int do_bandwidth(struct field *f, cairo_t *gfx, int event, int a, int b, int c){

	int	v = atoi(f->value);

	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max){
			v += f->step;
		}
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min){
			v -= f->step;
		}
		sprintf(f->value, "%d", v);
		update_field(f);
		modem_set_pitch(v);
		char buff[20], response[20];
		sprintf(buff, "rx_pitch=%d", v);
		sdr_request(buff, response);
		set_filter_high_low(v);
		save_bandwidth(v);
		return 1;
	}
		
	return 0;
}
//called for RIT as well as the main tuning
int do_tuning(struct field *f, cairo_t *gfx, int event, int a, int b, int c){

	static struct timespec last_change_time, this_change_time;

	int	v = atoi(f->value);
  int temp_tuning_step = tuning_step;

	if (event == FIELD_EDIT){

  if (!strcmp(get_field("tuning_acceleration")->value, "ON")){
    clock_gettime(CLOCK_MONOTONIC_RAW, &this_change_time);
    uint64_t delta_us = (this_change_time.tv_sec - last_change_time.tv_sec) * 1000000 + (this_change_time.tv_nsec - last_change_time.tv_nsec) / 1000;
    char temp_char[100];
    //sprintf(temp_char, "delta: %d", delta_us);
    //strcat(temp_char,"\r\n");
    //write_console(FONT_LOG, temp_char);
    clock_gettime(CLOCK_MONOTONIC_RAW, &last_change_time);
    if (delta_us < atof(get_field("tuning_accel_thresh2")->value)){
      if (tuning_step < 10000){
        tuning_step = tuning_step * 100;
        //sprintf(temp_char, "x100 activated\r\n");
        //write_console(FONT_LOG, temp_char);
      }
    } else if (delta_us < atof(get_field("tuning_accel_thresh1")->value)){
      if (tuning_step < 1000){
        tuning_step = tuning_step * 10;
        //printf(temp_char, "x10 activated\r\n");
        //write_console(FONT_LOG, temp_char);
      }
    }
  }

		if (a == MIN_KEY_UP && v + f->step <= f->max){
			//this is tuning the radio
			if (!strcmp(get_field("#rit")->value, "ON")){
				struct field *f = get_field("#rit_delta");
				int rit_delta = atoi(f->value);
				if(rit_delta < MAX_RIT){
					rit_delta += tuning_step;
					char tempstr[100];
					sprintf(tempstr, "%d", rit_delta);
					set_field("#rit_delta", tempstr);
				}
				else
					return 1;
			}
			else
				v = (v / tuning_step + 1)*tuning_step;
		}
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min){
			if (!strcmp(get_field("#rit")->value, "ON")){
				struct field *f = get_field("#rit_delta");
				int rit_delta = atoi(f->value);
				if (rit_delta > -MAX_RIT){
					rit_delta -= tuning_step;
					char tempstr[100];
					sprintf(tempstr, "%d", rit_delta);
					set_field("#rit_delta", tempstr);
					printf("moved rit to %s\n", f->value);
				}
				else
					return 1;
			}
			else
				v = (v / tuning_step - 1)*tuning_step;
			abort_tx();
		}
		
		sprintf(f->value, "%d",  v);
		tuning_step = temp_tuning_step;
		//send the new frequency to the sbitx core
		char buff[100];
		//sprintf(buff, "%s=%s", f->cmd, f->value);
		sprintf(buff, "%s %s", f->label, f->value);
		do_control_action(buff);
		//update the GUI
		update_field(f);
		settings_updated++;
		//leave it to us, we have handled it

		return 1;
	}
	else if (event == FIELD_DRAW){
			draw_dial(f, gfx);

			return 1; 
	}
	return 0;	
}

int do_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if(event == GDK_BUTTON_PRESS){
		//the default focus is on text input
		struct field *f_text = get_field("#text_in");
		if (f_focus && f_focus->value_type == FIELD_TEXT)
			f_text = f_focus;
		
		if (!strcmp(f->cmd, "#kbd_bs"))
			edit_field(f_text, MIN_KEY_BACKSPACE);
		else if (!strcmp(f->value, "CMD"))
			edit_field(f_text, COMMAND_ESCAPE);
		else if (!strcmp(f->value, "SPACE"))
			edit_field(f_text, ' ');
		else if (!strcmp(f->cmd, "#kbd_enter"))
			edit_field(f_text, '\n');
		else
			edit_field(f_text, f->value[0]);
		focus_since = millis();
		return 1;
	}
	else if (event == FIELD_DRAW){
		int label_height = font_table[FONT_FIELD_LABEL].height;
		int width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
		int offset_x = f->x + f->width/2 - width/2;
		int label_y;
		int value_font;

		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);
		//is it a two line display or a single line?
		if (!f->value[0]){
			label_y = f->y + (f->height - label_height)/2;
			draw_text(gfx, offset_x,label_y, f->label, FONT_FIELD_LABEL);
		} 
		else {
			if(width >= f->width+2)
				value_font = FONT_SMALL_FIELD_VALUE;
			else
				value_font = FONT_FIELD_VALUE;
			int value_height = font_table[value_font].height;
			label_y = f->y +3;
			draw_text(gfx, f->x + 3, label_y, f->label, FONT_FIELD_LABEL);
			width = measure_text(gfx, f->value, value_font);
			label_y = f->y + (f->height - label_height)/2;
			draw_text(gfx, f->x + f->width/2 - width/2, label_y, f->value, value_font);
		}
		return 1;
	}	
	return 0;
}


int do_toggle_kbd(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if(event == GDK_BUTTON_PRESS){
		focus_field(f_last_text);
		return 1;
	}
	return 0;
}

// Shared drawing helper for every field in our menu system (M1-M4) --
// guarantees genuinely uniform font/box rendering rather than each
// callback having its own copy-pasted drawing code (which is exactly how
// the font-size inconsistency happened: some callbacks used FONT_SMALL,
// others fell through to the framework's default, larger FONT_FIELD_*).
// Always draws label on top, given display string below, both in the
// smaller FONT_SMALL/FONT_SMALL_FIELD_VALUE fonts.
void draw_menu_field(struct field *f, cairo_t *gfx, const char *display){
	if (f_focus == f)
		fill_rect(gfx, f->x, f->y, f->width, f->height, COLOR_FIELD_SELECTED);
	else
		fill_rect(gfx, f->x, f->y, f->width, f->height, COLOR_BACKGROUND);
	if (f_focus == f)
		rect(gfx, f->x, f->y, f->width-1, f->height, SELECTED_LINE, 2);
	else if (f_hover == f)
		rect(gfx, f->x, f->y, f->width, f->height, COLOR_SELECTED_BOX, 1);
	else
		rect(gfx, f->x, f->y, f->width, f->height, COLOR_CONTROL_BOX, 1);

	int label_height = font_table[FONT_SMALL].height;
	int value_height = font_table[FONT_SMALL_FIELD_VALUE].height;
	int y = f->y + ((f->height - label_height - value_height) / 2);
	if (y < 1) y = 1;

	int w = measure_text(gfx, f->label, FONT_SMALL);
	draw_text(gfx, f->x + f->width/2 - w/2, y, f->label, FONT_SMALL);
	y += label_height;

	w = measure_text(gfx, (char*)display, FONT_SMALL_FIELD_VALUE);
	draw_text(gfx, f->x + f->width/2 - w/2, y, (char*)display, FONT_SMALL_FIELD_VALUE);
}

int do_notch_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		// f->value has already been cycled by the framework's own
		// focus_field()->edit_field() mechanism (built in for any
		// FIELD_TOGGLE field) before this callback runs. Just forward
		// the resulting value -- don't flip it again here.
		char response[20], request[20];
		sprintf(request, "notch=%s", f->value);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_notch_freq(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;

		// Lock NFREQ to the actual current passband (LOW-HIGH), not just
		// the field's own static 100-3000 range. Outside LOW-HIGH, that
		// audio is already filtered out elsewhere -- a notch there would
		// have no audible effect. Also account for the current NWIDTH
		// preset so the whole notch -- not just its center -- stays
		// inside the passband. NWTH is a string (NARROW/MEDIUM/WIDE),
		// not numeric, so map it to Hz explicitly (must match sbitx.c's
		// notch_width handler: NARROW=50, MEDIUM=100, WIDE=150).
		int low = field_int("LOW");
		int high = field_int("HIGH");
		const char *width_str = field_str("NWTH");
		int width = 100;
		if (width_str){
			if (!strcmp(width_str, "NARROW")) width = 50;
			else if (!strcmp(width_str, "WIDE")) width = 150;
		}
		int half_width = width / 2;

		if (low >= 0 && v < low + half_width)
			v = low + half_width;
		if (high >= 0 && v > high - half_width)
			v = high - half_width;

		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "notch_freq=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_notch_width(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		// 2-line rendering matching the requested design exactly: the
		// abbreviated preset name itself (NARW/MEDM/WIDE) acts as the top
		// line (in place of a static "NWTH" label), Hz value as the
		// bottom line. f->value stays "NARROW"/"MEDIUM"/"WIDE" internally
		// for all the existing comparison logic -- only the DISPLAYED
		// text is abbreviated here.
		int width_hz = !strcmp(f->value, "NARROW") ? 50 :
		               !strcmp(f->value, "WIDE") ? 150 : 100;
		char hz_str[16];
		sprintf(hz_str, "%dHz", width_hz);
		char display_label[8];
		if (!strcmp(f->value, "NARROW")) strcpy(display_label, "NARW");
		else if (!strcmp(f->value, "WIDE")) strcpy(display_label, "WIDE");
		else strcpy(display_label, "MEDM");

		if (f_focus == f)
			fill_rect(gfx, f->x, f->y, f->width, f->height, COLOR_FIELD_SELECTED);
		else
			fill_rect(gfx, f->x, f->y, f->width, f->height, COLOR_BACKGROUND);
		if (f_focus == f)
			rect(gfx, f->x, f->y, f->width-1, f->height, SELECTED_LINE, 2);
		else if (f_hover == f)
			rect(gfx, f->x, f->y, f->width, f->height, COLOR_SELECTED_BOX, 1);
		else
			rect(gfx, f->x, f->y, f->width, f->height, COLOR_CONTROL_BOX, 1);

		int label_height = font_table[FONT_SMALL].height;
		int value_height = font_table[FONT_SMALL_FIELD_VALUE].height;
		int y = f->y + ((f->height - label_height - value_height) / 2);
		if (y < 1) y = 1;

		int w = measure_text(gfx, display_label, FONT_SMALL);
		draw_text(gfx, f->x + f->width/2 - w/2, y, display_label, FONT_SMALL);
		y += label_height;

		w = measure_text(gfx, hz_str, FONT_SMALL_FIELD_VALUE);
		draw_text(gfx, f->x + f->width/2 - w/2, y, hz_str, FONT_SMALL_FIELD_VALUE);

		return 1;  // handled, skip the default rendering
	}
	if (event == FIELD_EDIT){
		// The framework's built-in FIELD_TOGGLE cycling logic only
		// correctly handles 2-value lists like ON/OFF -- for this 3-value
		// list it would skip WIDE entirely. Handling FIELD_EDIT here
		// ourselves and returning 1 bypasses that broken logic.
		if (!strcmp(f->value, "NARROW"))
			strcpy(f->value, "MEDIUM");
		else if (!strcmp(f->value, "MEDIUM"))
			strcpy(f->value, "WIDE");
		else
			strcpy(f->value, "NARROW");
		update_field(f);
		char request[20], response[20];
		sprintf(request, "notch_width=%s", f->value);
		sdr_request(request, response);

		// Re-clamp NFREQ using the same rule as do_notch_freq -- map the
		// preset to its Hz value (must match sbitx.c's notch_width
		// command handler: NARROW=50, MEDIUM=100, WIDE=150) to compute
		// the half-width for the passband lock.
		int width_hz = !strcmp(f->value, "NARROW") ? 50 :
		               !strcmp(f->value, "WIDE") ? 150 : 100;

		struct field *freq_f = get_field("notch_freq");
		if (freq_f){
			int fv = atoi(freq_f->value);
			int low = field_int("LOW");
			int high = field_int("HIGH");
			int half_width = width_hz / 2;
			if (low >= 0 && fv < low + half_width)
				fv = low + half_width;
			if (high >= 0 && fv > high - half_width)
				fv = high - half_width;
			sprintf(freq_f->value, "%d", fv);
			update_field(freq_f);
			char freq_request[20], freq_response[20];
			sprintf(freq_request, "notch_freq=%d", fv);
			sdr_request(freq_request, freq_response);
		}
		return 1;
	}
	return 0;
}

int do_contour_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		// same reasoning as do_notch_toggle: f->value has already been
		// cycled correctly by the framework's built-in FIELD_TOGGLE
		// mechanism (safe here since this is a 2-value ON/OFF list) --
		// just forward it.
		char response[20], request[30];
		sprintf(request, "contour=%s", f->value);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_contour_freq(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;

		// Same passband lock as NFREQ -- CFREQ can't be set outside the
		// current LOW-HIGH window, accounting for CONW so the whole
		// bump (not just its center) stays inside the passband. CONW is
		// a string (NARROW/MEDIUM/WIDE), map to Hz explicitly (must match
		// sbitx.c's contour_width handler: NARROW=40, MEDIUM=100, WIDE=300).
		int low = field_int("LOW");
		int high = field_int("HIGH");
		const char *width_str = field_str("CONW");
		int width = 200;
		if (width_str){
			if (!strcmp(width_str, "NARROW")) width = 100;
			else if (!strcmp(width_str, "WIDE")) width = 300;
		}
		int half_width = width / 2;

		if (low >= 0 && v < low + half_width)
			v = low + half_width;
		if (high >= 0 && v > high - half_width)
			v = high - half_width;

		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "contour_freq=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_contour_width(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		int width_hz = !strcmp(f->value, "NARROW") ? 100 :
		               !strcmp(f->value, "WIDE") ? 300 : 200;
		char hz_str[16];
		sprintf(hz_str, "%dHz", width_hz);
		char display_label[8];
		if (!strcmp(f->value, "NARROW")) strcpy(display_label, "NARW");
		else if (!strcmp(f->value, "WIDE")) strcpy(display_label, "WIDE");
		else strcpy(display_label, "MEDM");

		if (f_focus == f)
			fill_rect(gfx, f->x, f->y, f->width, f->height, COLOR_FIELD_SELECTED);
		else
			fill_rect(gfx, f->x, f->y, f->width, f->height, COLOR_BACKGROUND);
		if (f_focus == f)
			rect(gfx, f->x, f->y, f->width-1, f->height, SELECTED_LINE, 2);
		else if (f_hover == f)
			rect(gfx, f->x, f->y, f->width, f->height, COLOR_SELECTED_BOX, 1);
		else
			rect(gfx, f->x, f->y, f->width, f->height, COLOR_CONTROL_BOX, 1);

		int label_height = font_table[FONT_SMALL].height;
		int value_height = font_table[FONT_SMALL_FIELD_VALUE].height;
		int y = f->y + ((f->height - label_height - value_height) / 2);
		if (y < 1) y = 1;

		int w = measure_text(gfx, display_label, FONT_SMALL);
		draw_text(gfx, f->x + f->width/2 - w/2, y, display_label, FONT_SMALL);
		y += label_height;

		w = measure_text(gfx, hz_str, FONT_SMALL_FIELD_VALUE);
		draw_text(gfx, f->x + f->width/2 - w/2, y, hz_str, FONT_SMALL_FIELD_VALUE);

		return 1;
	}
	if (event == FIELD_EDIT){
		if (!strcmp(f->value, "NARROW"))
			strcpy(f->value, "MEDIUM");
		else if (!strcmp(f->value, "MEDIUM"))
			strcpy(f->value, "WIDE");
		else
			strcpy(f->value, "NARROW");
		update_field(f);
		char request[20], response[20];
		sprintf(request, "contour_width=%s", f->value);
		sdr_request(request, response);

		// Map preset to Hz (must match sbitx.c's contour_width handler:
		// NARROW=100, MEDIUM=200, WIDE=300).
		int width_hz = !strcmp(f->value, "NARROW") ? 100 :
		               !strcmp(f->value, "WIDE") ? 300 : 200;

		struct field *freq_f = get_field("contour_freq");
		if (freq_f){
			int fv = atoi(freq_f->value);
			int low = field_int("LOW");
			int high = field_int("HIGH");
			int half_width = width_hz / 2;
			if (low >= 0 && fv < low + half_width)
				fv = low + half_width;
			if (high >= 0 && fv > high - half_width)
				fv = high - half_width;
			sprintf(freq_f->value, "%d", fv);
			update_field(freq_f);
			char freq_request[20], freq_response[20];
			sprintf(freq_request, "contour_freq=%d", fv);
			sdr_request(freq_request, freq_response);
		}
		return 1;
	}
	return 0;
}

int do_valley_atten(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		char display[8];
		if (!formant_protect_enabled) strcpy(display, "OFF");
		else sprintf(display, "%d", v);

		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		int is_double_tap = (last_toggle_tap == f);
		char response[20], request[20];

		if (is_double_tap){
			// Double-tap: turn off unconditionally.
			if (formant_protect_enabled){
				sprintf(request, "formant=OFF");
				sdr_request(request, response);
			}
			last_toggle_tap = NULL;   // consumed -- next tap starts a fresh sequence
		}
		else{
			// Single tap: turn on if currently off; if already on, do
			// nothing except select it, so the knob can adjust its level
			// without a plain re-tap switching it off.
			if (!formant_protect_enabled){
				sprintf(request, "formant=ON");
				sdr_request(request, response);
			}
			last_toggle_tap = f;
		}
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "valley=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// Formant valley suppression: fully independent toggle+level, separate
// from FMNT. Same double-tap convention as every other combined box.
int do_formant_valley(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, formant_valley_enabled ? f->value : "OFF");
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		int is_double_tap = (last_toggle_tap == f);
		char response[20], request[20];

		if (is_double_tap){
			// Double-tap: turn off unconditionally.
			if (formant_valley_enabled){
				sprintf(request, "formant_valley_on=OFF");
				sdr_request(request, response);
			}
			last_toggle_tap = NULL;
		}
		else{
			// Single tap: turn on if currently off; if already on, do
			// nothing except select it, so the knob can adjust its level
			// without a plain re-tap switching it off.
			if (!formant_valley_enabled){
				sprintf(request, "formant_valley_on=ON");
				sdr_request(request, response);
			}
			last_toggle_tap = f;
		}
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "formant_valley=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_nb_threshold(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		char display[8];
		if (!noise_blanker_enabled) strcpy(display, "OFF");
		else sprintf(display, "%d", v);

		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		int is_double_tap = (last_toggle_tap == f);
		char response[20], request[20];

		if (is_double_tap){
			// Double-tap: turn off unconditionally.
			if (noise_blanker_enabled){
				sprintf(request, "nb=OFF");
				sdr_request(request, response);
			}
			last_toggle_tap = NULL;   // consumed -- next tap starts a fresh sequence
		}
		else{
			// Single tap: turn on if currently off; if already on, do
			// nothing except select it, so the knob can adjust its level
			// without a plain re-tap switching it off.
			if (!noise_blanker_enabled){
				sprintf(request, "nb=ON");
				sdr_request(request, response);
			}
			last_toggle_tap = f;
		}
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "nb_threshold=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_nr_threshold(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		char display[8];
		if (!baseline_nr_enabled) strcpy(display, "OFF");
		else sprintf(display, "%d", v);

		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		int is_double_tap = (last_toggle_tap == f);
		char response[20], request[20];

		if (is_double_tap){
			// Double-tap: turn off unconditionally.
			if (baseline_nr_enabled){
				sprintf(request, "nr=OFF");
				sdr_request(request, response);
			}
			last_toggle_tap = NULL;   // consumed -- next tap starts a fresh sequence
		}
		else{
			// Single tap: turn on if currently off; if already on, do
			// nothing except select it, so the knob can adjust its level
			// without a plain re-tap switching it off.
			if (!baseline_nr_enabled){
				sprintf(request, "nr=ON");
				sdr_request(request, response);
			}
			last_toggle_tap = f;
		}
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "nr_threshold=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// AINR: AI noise reduction (RNNoise). One box: the value is how much of the
// cleaned audio is mixed in (1-20, 20 = fully cleaned), 0 shows as OFF.
// Knob adjusts the amount; a tap switches it off, and a tap again brings it
// back at the amount it had before. Only affects SSB/AM reception.
// USB speaker / USB microphone (see usbaudio.c). Tap to switch on or off. If the
// USB device is not found the box goes back to OFF and a note is written to
// the console; the radio carries on with its own audio.
static int usb_toggle_press(struct field *f, const char *cmd, const char *what){
	char request[40], response[40];
	sprintf(request, "%s=%s", cmd, f->value);   // the framework has already flipped f->value
	response[0] = 0;
	sdr_request(request, response);
	if (strcmp(response, "ok")){
		strcpy(f->value, "OFF");
		char msg[100];
		sprintf(msg, "USB %s: no USB sound device found\r\n", what);
		write_console(FONT_LOG, msg);
		sprintf(request, "%s=OFF", cmd);
		sdr_request(request, response);
	}
	update_field(f);
	return 1;
}

int do_usb_spk(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS)
		return usb_toggle_press(f, "usb_spk", "speaker");
	return 0;
}

int do_usb_mic(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS)
		return usb_toggle_press(f, "usb_mic", "microphone");
	return 0;
}

#ifdef SBITX_FREEDV
// FreeDV (see freedv_ext.c). Tap or turn the knob to step through
// OFF -> 700D -> 700E -> 1600 -> RADE -> RAD2 (RADE V2, beta). Use USB or LSB with a filter of 3 kHz or wider.
static const char *fdv_names[] = {"OFF", "700D", "700E", "1600", "RADE", "RAD2"};
#define FDV_N 6

// Switch FreeDV to mode number v (index into fdv_names). Returns the mode
// actually running: v, or 0 (OFF) if it could not start. Used by the menu box
// and by the FreeDV window.
static void fdv_win_hide_cb(void);
static int fdv_select(int v){
	char request[40], response[40];
	sprintf(request, "freedv=%s", fdv_names[v]);
	response[0] = 0;
	sdr_request(request, response);
	if (strcmp(response, "ok")){
		if (v > 0){
			// the requested mode could not start (RADE not installed, say):
			// make sure the engine is really OFF, not still running the
			// previous mode while the box shows OFF
			response[0] = 0;
			sdr_request("freedv=OFF", response);
		}
		v = 0;
		write_console(FONT_LOG, "FreeDV: could not start (700D/700E/1600 need libcodec2; RADE needs rade/build_rade.sh) -- FreeDV is now OFF\r\n");
	}
	else if (v){
		char msg[120];
		sprintf(msg, "FreeDV %s on: use USB or LSB, filter 3 kHz or wider\r\n", fdv_names[v]);
		write_console(FONT_LOG, msg);
	}
	struct field *f = get_field("freedv");
	if (f){
		sprintf(f->value, "%d", v);
		update_field(f);
	}
	if (v == 0) fdv_win_hide_cb();   // FreeDV is off: the window is not needed
	return v;
}

int do_fdv_mode(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);
	if (v < 0 || v >= FDV_N) v = 0;
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, fdv_names[v]);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS || event == FIELD_EDIT){
		if (event == FIELD_EDIT && a == MIN_KEY_DOWN)
			v = (v + FDV_N - 1) % FDV_N;
		else
			v = (v + 1) % FDV_N;
		fdv_select(v);
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------
// FreeDV window (menu page 9, box FDVW). Like the FreeDV program on a PC:
// mode buttons, sync light, SNR, frequency offset, the constellation
// ("scatter") plot, received text, audio levels, and a small setup area
// (callsign sent as text, transmit level, squelch, clipping).
// The numbers come from freedv_ext.c; the radio's own spectrum and
// waterfall stay on the main screen.
// ---------------------------------------------------------------------
static struct {
	GtkWidget *win, *scatter, *panel, *modebtn[FDV_N], *call, *txlevel, *squelch, *clip, *txlabel, *sqlabel;
	guint timer;
	struct freedv_ext_stats st;
	int building;                      // true while the controls are being set up
} fw;

static void fdv_win_css(void){
	static int done = 0;
	if (done) return;
	done = 1;
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css,
		"#fdvwin { background-color: #1e1e1e; }"
		"#fdvwin label { color: #ffffff; font-weight: bold; }"
		"#fdvwin button { min-height: 34px; background-image: none; background-color: #000000;"
		"  color: #ffffff; border: 2px solid #c8c8c8; border-radius: 4px; }"
		"#fdvwin button label { color: #ffffff; }"
		"#fdvwin button:checked { background-color: #1f6f2f; }"
		"#fdvwin button:hover { background-color: #303030; }"
		"#fdvwin entry { background-color: #000000; color: #ffffff; border: 2px solid #c8c8c8;"
		"  min-height: 30px; }"
		"#fdvwin scale trough { min-height: 8px; background-color: #d8d8d8; border: 1px solid #000000; }"
		"#fdvwin scale slider { min-width: 26px; min-height: 26px; background-image: none;"
		"  background-color: #ffffff; border: 2px solid #000000; border-radius: 6px; }",
		-1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
}

static void fdv_text(cairo_t *cr, double x, double y, double size, const char *s){
	cairo_set_font_size(cr, size);
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, s);
}

static void fdv_bar(cairo_t *cr, double x, double y, double w, double h, double frac, double r, double g, double b){
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_rectangle(cr, x, y, w, h); cairo_fill(cr);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_rectangle(cr, x, y, w * frac, h); cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1); cairo_stroke(cr);
}

static gboolean fdv_scatter_draw(GtkWidget *w, cairo_t *cr, gpointer d){
	int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
	cairo_set_line_width(cr, 1);
	cairo_move_to(cr, W / 2, 0); cairo_line_to(cr, W / 2, H);
	cairo_move_to(cr, 0, H / 2); cairo_line_to(cr, W, H / 2);
	cairo_stroke(cr);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	if (fw.st.has_scatter){
		double sc = (W < H ? W : H) / 2.0 * 0.9;
		cairo_set_source_rgba(cr, 0.2, 1.0, 0.3, 0.8);
		for (int i = 0; i < fw.st.nsym; i++){
			cairo_arc(cr, W / 2 + fw.st.sx[i] * sc, H / 2 - fw.st.sy[i] * sc, 2.0, 0, 6.2832);
			cairo_fill(cr);
		}
	}
	else {
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		fdv_text(cr, 12, H / 2 - 8, 14, !fw.st.active ? "FreeDV is OFF" :
			(fw.st.mode[0] == 'R' ? "RADE has no scatter plot" : "waiting for a signal..."));
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	fdv_text(cr, 6, 16, 12, "Constellation");
	return FALSE;
}

static gboolean fdv_panel_draw(GtkWidget *w, cairo_t *cr, gpointer d){
	int W = gtk_widget_get_allocated_width(w);
	struct freedv_ext_stats *s = &fw.st;
	char t[80];
	cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
	cairo_paint(cr);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

	// sync light
	if (!s->active) cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
	else if (s->sync) cairo_set_source_rgb(cr, 0.1, 0.9, 0.2);
	else cairo_set_source_rgb(cr, 0.9, 0.15, 0.1);
	cairo_arc(cr, 22, 22, 13, 0, 6.2832); cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	fdv_text(cr, 44, 29, 18, !s->active ? "OFF" : (s->sync ? "SYNC" : "SEARCHING"));
	if (s->active){
		fdv_text(cr, W - 70, 29, 18, s->mode);
	}

	// SNR with a bar from -5 to +20 dB
	if (s->active && s->sync) sprintf(t, "SNR %+.1f dB", s->snr); else strcpy(t, "SNR  --");
	fdv_text(cr, 10, 62, 16, t);
	fdv_bar(cr, 10, 70, W - 20, 12, (s->snr + 5.0) / 25.0, 0.2, 0.8, 0.3);

	// offsets
	if (s->active && s->sync) sprintf(t, "Freq offset %+.0f Hz", s->foff); else strcpy(t, "Freq offset  --");
	fdv_text(cr, 10, 106, 14, t);
	if (s->active && s->sync && s->has_text) sprintf(t, "Clock offset %+.0f ppm   Sync %.0f%%", s->clock_ppm, s->sync_metric * 100);
	else strcpy(t, "Clock offset  --");
	fdv_text(cr, 10, 126, 14, t);

	// audio levels, -60 .. 0 dB
	sprintf(t, "RX audio %.0f dB", s->rx_db);
	fdv_text(cr, 10, 152, 13, t);
	fdv_bar(cr, 10, 158, W - 20, 10, (s->rx_db + 60.0) / 60.0, 0.3, 0.6, 1.0);
	sprintf(t, "TX mic %.0f dB", s->tx_db);
	fdv_text(cr, 10, 188, 13, t);
	fdv_bar(cr, 10, 194, W - 20, 10, (s->tx_db + 60.0) / 60.0, 1.0, 0.6, 0.2);

	// received text
	cairo_set_source_rgb(cr, 1, 1, 1);
	fdv_text(cr, 10, 226, 13, "Received text:");
	cairo_set_source_rgb(cr, 1.0, 0.9, 0.3);
	if (s->active && !s->has_text && s->mode[0] == 'R') strcpy(t, "(not available with RADE)");
	else strncpy(t, s->rxtext, sizeof(t) - 1), t[sizeof(t) - 1] = 0;
	fdv_text(cr, 10, 246, 15, t);
	return FALSE;
}

static gboolean fdv_win_tick(gpointer d){
	if (!fw.win || !gtk_widget_get_visible(fw.win))
		return TRUE;
	freedv_ext_get_stats(&fw.st);
	gtk_widget_queue_draw(fw.scatter);
	gtk_widget_queue_draw(fw.panel);
	// keep the mode buttons in step with the radio (the menu box can change it too)
	struct field *f = get_field("freedv");
	int v = f ? atoi(f->value) : 0;
	fw.building = 1;
	for (int i = 0; i < FDV_N; i++)
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fw.modebtn[i])) != (i == v))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fw.modebtn[i]), i == v);
	fw.building = 0;
	return TRUE;
}

static void on_fdv_mode_btn(GtkToggleButton *tb, gpointer d){
	if (fw.building || !gtk_toggle_button_get_active(tb))
		return;
	int v = GPOINTER_TO_INT(d);
	fdv_select(v);
}

static void on_fdv_call_set(GtkWidget *w, gpointer d){
	freedv_ext_set_callsign(gtk_entry_get_text(GTK_ENTRY(fw.call)));
	gtk_entry_set_text(GTK_ENTRY(fw.call), freedv_ext_get_callsign());   // shows the cleaned-up version
}
static void on_fdv_txlevel(GtkRange *r, gpointer d){
	char t[48];
	sprintf(t, "TX level: %d %%", (int)gtk_range_get_value(r));
	gtk_label_set_text(GTK_LABEL(fw.txlabel), t);
	if (!fw.building) freedv_ext_set_tx_level((int)gtk_range_get_value(r));
}
static void on_fdv_squelch(GtkRange *r, gpointer d){
	char t[64];
	sprintf(t, "Squelch: mute below SNR %+d dB", (int)gtk_range_get_value(r));
	gtk_label_set_text(GTK_LABEL(fw.sqlabel), t);
	if (!fw.building) freedv_ext_set_squelch((float)gtk_range_get_value(r));
}
static void on_fdv_clip(GtkToggleButton *tb, gpointer d){
	if (!fw.building) freedv_ext_set_clip(gtk_toggle_button_get_active(tb));
}
static void on_fdv_close(GtkButton *b, gpointer d){
	gtk_widget_hide(fw.win);
}

// redraw the FDVW box whenever the FreeDV window opens or closes, however that happens
static void fdv_win_refresh_box(GtkWidget *w, gpointer d){
	struct field *bf = get_field("#freedv_window");
	if (bf) update_field(bf);
}

static void fdv_win_open(void){
	if (fw.win){
		gtk_window_present(GTK_WINDOW(fw.win));
		return;
	}
	fdv_win_css();
	fw.building = 1;
	fw.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_name(fw.win, "fdvwin");
	gtk_window_set_title(GTK_WINDOW(fw.win), "FreeDV");
	if (window)
		gtk_window_set_transient_for(GTK_WINDOW(fw.win), GTK_WINDOW(window));
	gtk_window_set_keep_above(GTK_WINDOW(fw.win), TRUE);
	gtk_window_set_position(GTK_WINDOW(fw.win), GTK_WIN_POS_CENTER_ON_PARENT);
	gtk_window_set_default_size(GTK_WINDOW(fw.win), 720, 420);
	g_signal_connect(fw.win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
	g_signal_connect(fw.win, "show", G_CALLBACK(fdv_win_refresh_box), NULL);
	g_signal_connect(fw.win, "hide", G_CALLBACK(fdv_win_refresh_box), NULL);

	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
	gtk_container_add(GTK_CONTAINER(fw.win), hbox);

	// left: constellation and the status panel
	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	fw.scatter = gtk_drawing_area_new();
	gtk_widget_set_size_request(fw.scatter, 300, 150);
	g_signal_connect(fw.scatter, "draw", G_CALLBACK(fdv_scatter_draw), NULL);
	gtk_box_pack_start(GTK_BOX(left), fw.scatter, TRUE, TRUE, 0);
	fw.panel = gtk_drawing_area_new();
	gtk_widget_set_size_request(fw.panel, 300, 256);
	g_signal_connect(fw.panel, "draw", G_CALLBACK(fdv_panel_draw), NULL);
	gtk_box_pack_start(GTK_BOX(left), fw.panel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), left, TRUE, TRUE, 0);

	// right: mode buttons and setup
	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_size_request(right, 340, -1);
	gtk_box_pack_start(GTK_BOX(right), gtk_label_new("Mode"), FALSE, FALSE, 0);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
	for (int i = 0; i < FDV_N; i++){
		fw.modebtn[i] = gtk_toggle_button_new_with_label(fdv_names[i]);
		gtk_grid_attach(GTK_GRID(grid), fw.modebtn[i], i % 3, i / 3, 1, 1);
		g_signal_connect(fw.modebtn[i], "toggled", G_CALLBACK(on_fdv_mode_btn), GINT_TO_POINTER(i));
	}
	gtk_box_pack_start(GTK_BOX(right), grid, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(right), gtk_label_new("Setup"), FALSE, FALSE, 4);
	GtkWidget *crow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	fw.call = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(fw.call), 15);
	gtk_entry_set_text(GTK_ENTRY(fw.call), freedv_ext_get_callsign());
	gtk_entry_set_placeholder_text(GTK_ENTRY(fw.call), "your callsign (sent as text)");
	GtkWidget *cset = gtk_button_new_with_label("Set");
	g_signal_connect(cset, "clicked", G_CALLBACK(on_fdv_call_set), NULL);
	g_signal_connect(fw.call, "activate", G_CALLBACK(on_fdv_call_set), NULL);
	gtk_box_pack_start(GTK_BOX(crow), fw.call, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(crow), cset, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), crow, FALSE, FALSE, 0);

	fw.txlabel = gtk_label_new("TX level");
	gtk_box_pack_start(GTK_BOX(right), fw.txlabel, FALSE, FALSE, 0);
	fw.txlevel = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20, 100, 5);
	gtk_scale_set_digits(GTK_SCALE(fw.txlevel), 0);
	gtk_scale_set_draw_value(GTK_SCALE(fw.txlevel), FALSE);
	g_signal_connect(fw.txlevel, "value-changed", G_CALLBACK(on_fdv_txlevel), NULL);
	gtk_range_set_value(GTK_RANGE(fw.txlevel), freedv_ext_get_tx_level());
	gtk_box_pack_start(GTK_BOX(right), fw.txlevel, FALSE, FALSE, 0);

	fw.sqlabel = gtk_label_new("Squelch");
	gtk_box_pack_start(GTK_BOX(right), fw.sqlabel, FALSE, FALSE, 0);
	fw.squelch = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -6, 6, 1);
	gtk_scale_set_digits(GTK_SCALE(fw.squelch), 0);
	gtk_scale_set_draw_value(GTK_SCALE(fw.squelch), FALSE);
	g_signal_connect(fw.squelch, "value-changed", G_CALLBACK(on_fdv_squelch), NULL);
	gtk_range_set_value(GTK_RANGE(fw.squelch), freedv_ext_get_squelch());
	gtk_box_pack_start(GTK_BOX(right), fw.squelch, FALSE, FALSE, 0);

	fw.clip = gtk_check_button_new_with_label("Clip transmit peaks (700D/700E/1600)");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fw.clip), freedv_ext_get_clip());
	g_signal_connect(fw.clip, "toggled", G_CALLBACK(on_fdv_clip), NULL);
	gtk_box_pack_start(GTK_BOX(right), fw.clip, FALSE, FALSE, 0);

	GtkWidget *close_btn = gtk_button_new_with_label("Close");
	g_signal_connect(close_btn, "clicked", G_CALLBACK(on_fdv_close), NULL);
	gtk_box_pack_end(GTK_BOX(right), close_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), right, FALSE, FALSE, 0);

	fw.building = 0;
	memset(&fw.st, 0, sizeof(fw.st));
	if (!fw.timer)
		fw.timer = g_timeout_add(100, fdv_win_tick, NULL);
	gtk_widget_show_all(fw.win);
	gtk_window_present(GTK_WINDOW(fw.win));
}

static void fdv_win_hide(void){
	if (fw.win && gtk_widget_get_visible(fw.win))
		gtk_widget_hide(fw.win);
}
static void fdv_win_hide_cb(void){ fdv_win_hide(); }

int do_fdv_window(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int shown = fw.win && gtk_widget_get_visible(fw.win);
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, shown ? "ON" : "OFF");   // tap: ON opens the window, OFF closes it
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		if (shown) fdv_win_hide();      // a second press closes the window
		else fdv_win_open();
		update_field(f);
		return 1;
	}
	return 0;
}

int do_fdv_status(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		char display[16] = "OFF";   // FreeDV is off
		struct field *m = get_field("freedv");
		if (m && atoi(m->value) > 0){
			char response[60] = "";
			int sync = 0;
			float snr = 0;
			sdr_request("freedv_status=1", response);
			if (sscanf(response, "%d %f", &sync, &snr) == 2){
				if (sync)
					sprintf(display, "%+d", (int)floor(snr + 0.5));
				else
					strcpy(display, "SRCH");
			}
		}
		draw_menu_field(f, gfx, display);
		return 1;
	}
	return 0;
}

#endif

// The key a USB PTT microphone's button sends (most of them act as a keyboard).
static const char *usb_ptt_names[] = {"OFF", "F9", "F10", "F11", "F12"};
static int usb_ptt_keyval(void){
	struct field *f = get_field("usb_ptt");
	int v = f ? atoi(f->value) : 0;
	switch (v){
		case 1: return GDK_KEY_F9;
		case 2: return GDK_KEY_F10;
		case 3: return GDK_KEY_F11;
		case 4: return GDK_KEY_F12;
	}
	return 0;
}

int do_usb_ptt(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);
	if (v < 0 || v > 4) v = 0;
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, usb_ptt_names[v]);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS || event == FIELD_EDIT){
		if (event == FIELD_EDIT && a == MIN_KEY_DOWN)
			v = (v + 4) % 5;
		else
			v = (v + 1) % 5;
		sprintf(f->value, "%d", v);
		update_field(f);
		return 1;
	}
	return 0;
}

int do_ai_nr(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	static int last_on_level = 10;   // level restored when tapped back on
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		char display[8];
		if (v <= 0) strcpy(display, "OFF");
		else sprintf(display, "%d", v);
		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		char request[20], response[20];
		if (v > 0){
			last_on_level = v;
			v = 0;
		}
		else
			v = last_on_level;
		sprintf(f->value, "%d", v);
		sprintf(request, "ai_nr=%d", v);
		sdr_request(request, response);
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "ai_nr=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_mic_comp(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		char display[8];
		if (!mic_comp_enabled) strcpy(display, "OFF");
		else sprintf(display, "%d", v);

		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		int is_double_tap = (last_toggle_tap == f);
		char response[20], request[20];

		if (is_double_tap){
			// Double-tap: turn off unconditionally.
			if (mic_comp_enabled){
				sprintf(request, "mic_comp_on=OFF");
				sdr_request(request, response);
			}
			last_toggle_tap = NULL;   // consumed -- next tap starts a fresh sequence
		}
		else{
			// Single tap: turn on if currently off; if already on, do
			// nothing except select it, so the knob can adjust its level
			// without a plain re-tap switching it off.
			if (!mic_comp_enabled){
				sprintf(request, "mic_comp_on=ON");
				sdr_request(request, response);
			}
			last_toggle_tap = f;
		}
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "mic_comp=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

int do_audio_boost(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		char display[8];
		if (!audio_boost_enabled) strcpy(display, "OFF");
		else sprintf(display, "%d", v);

		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		int is_double_tap = (last_toggle_tap == f);
		char response[20], request[20];

		if (is_double_tap){
			// Double-tap: turn off unconditionally.
			if (audio_boost_enabled){
				sprintf(request, "audio_boost_on=OFF");
				sdr_request(request, response);
			}
			last_toggle_tap = NULL;   // consumed -- next tap starts a fresh sequence
		}
		else{
			// Single tap: turn on if currently off; if already on, do
			// nothing except select it, so the knob can adjust its level
			// without a plain re-tap switching it off.
			if (!audio_boost_enabled){
				sprintf(request, "audio_boost_on=ON");
				sdr_request(request, response);
			}
			last_toggle_tap = f;
		}
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "audio_boost=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// VOX: simple on/off toggle, matching NOTC/CONT's pattern (no associated
// level of its own -- threshold and hangtime are separate boxes).
int do_vox_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		// f->value already correctly toggled by the framework's own
		// focus_field()->edit_field() mechanism (built in for any
		// FIELD_TOGGLE field) before this callback runs. Just forward
		// the resulting value -- don't flip it again here.
		char response[20], request[20];
		sprintf(request, "vox_on=%s", f->value);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// VOX: sensitivity threshold (0-20, higher = requires louder speech).
// Knob only.
int do_vox_threshold(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "vox_threshold=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// VOX: hang time before unkeying after speech stops (0-20, mapped to
// 0-2000ms in sbitx.c). Knob only.
int do_vox_hangtime(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "vox_hangtime=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// EQ_POPUP_BEGIN
// ---------------------------------------------------------------------
// Equalizer popups. Two independent windows, one for RX and one for TX,
// since the two usually want different settings. Each is an ordinary
// non-modal GTK window (so you can keep listening/operating and watch the
// effect while adjusting) with an on/off button, six vertical sliders
// (-20..+20 dB) and Flat / Close buttons. This is the first real GTK
// window in this codebase -- everything else is drawn directly into the
// main drawing area -- which is why it's kept small and self-contained.
//
// The windows hold NO state of their own. Every slider/toggle change goes
// through set_field() on a hidden field (eq_rx_b0.., eq_rx_on, ...), which
// both stores it for persistence and forwards it to the DSP. The windows
// are just a view of those fields.
// ---------------------------------------------------------------------
#define EQ_UI_BANDS 6
#define EQ_UI_MAX_DB 20

static const char *eq_band_names[EQ_UI_BANDS] = {"300", "600", "1k", "1.5k", "2k", "2.8k"};

struct eq_popup {
	GtkWidget *win;
	GtkWidget *toggle;
	GtkWidget *scales[EQ_UI_BANDS];
};
static struct eq_popup eq_popups[2];   // [0] = RX, [1] = TX

// band < 0 -> the on/off field, otherwise that band's gain field
static void eq_field_name(char *out, int is_tx, int band){
	if (band < 0)
		sprintf(out, "eq_%s_on", is_tx ? "tx" : "rx");
	else
		sprintf(out, "eq_%s_b%d", is_tx ? "tx" : "rx", band);
}

static void on_eq_scale_changed(GtkRange *range, gpointer data){
	int code = GPOINTER_TO_INT(data);
	int is_tx = code / 16, band = code % 16;
	double d = gtk_range_get_value(range);
	int v = (int)(d < 0 ? d - 0.5 : d + 0.5);
	char name[24], val[16];

	eq_field_name(name, is_tx, band);
	sprintf(val, "%d", v);
	set_field(name, val);
}

// Show explicit signs (+3, -4, 0) so cut vs boost reads at a glance.
static gchar *on_eq_format_value(GtkScale *scale, gdouble value, gpointer data){
	int v = (int)(value < 0 ? value - 0.5 : value + 0.5);
	if (v == 0)
		return g_strdup("0");
	return g_strdup_printf("%+d", v);
}

static void on_eq_toggled(GtkToggleButton *tb, gpointer data){
	int is_tx = GPOINTER_TO_INT(data);
	int on = gtk_toggle_button_get_active(tb);
	char name[24];

	eq_field_name(name, is_tx, -1);
	set_field(name, on ? "1" : "0");
	gtk_button_set_label(GTK_BUTTON(tb), on ? "EQ ON" : "EQ OFF");
	// keep the ON/OFF shown on the menu button in step
	struct field *btn = get_field(is_tx ? "#eq_tx_open" : "#eq_rx_open");
	if (btn)
		update_field(btn);
}

static void on_eq_flat(GtkButton *b, gpointer data){
	int is_tx = GPOINTER_TO_INT(data), i;
	for (i = 0; i < EQ_UI_BANDS; i++)
		gtk_range_set_value(GTK_RANGE(eq_popups[is_tx].scales[i]), 0);   // each fires on_eq_scale_changed
}

static void on_eq_close(GtkButton *b, gpointer data){
	gtk_widget_hide(eq_popups[GPOINTER_TO_INT(data)].win);
}

// Bigger touch targets than the stock theme gives, scoped to these
// windows only (#eqwin) so nothing else in the app is affected.
static void eq_apply_css(void){
	static int done = 0;
	if (done)
		return;
	done = 1;
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css,
		// dark window, white text
		"#eqwin { background-color: #1e1e1e; }"
		"#eqwin label { color: #ffffff; font-weight: bold; }"
		// buttons: black with white text and a light edge
		"#eqwin button { min-height: 36px; background-image: none; background-color: #000000;"
		"  color: #ffffff; border: 2px solid #c8c8c8; border-radius: 4px; }"
		"#eqwin button label { color: #ffffff; }"
		"#eqwin button#eqtoggle label { color: #000000; }"
		"#eqwin button:hover, #eqwin button:active { background-color: #303030; }"
		// EQ on/off: grey, black text; a lighter grey when on
		"#eqwin button#eqtoggle { background-image: none; background-color: #808080; color: #000000; }"
		"#eqwin button#eqtoggle:checked { background-color: #c0c0c0; color: #000000; }"
		// sliders: light bar the thumb runs along, big white thumb with black edge
		"#eqwin scale trough { min-width: 8px; min-height: 8px; background-color: #d8d8d8;"
		"  border: 1px solid #000000; }"
		"#eqwin scale slider { min-width: 30px; min-height: 30px; background-image: none;"
		"  background-color: #ffffff; border: 2px solid #000000; border-radius: 6px; }"
		// dB scale: bright tick marks with white numbers
		"#eqwin scale mark indicator, #eqwin scale marks indicator { min-width: 10px; min-height: 2px;"
		"  background-color: #ffffff; }"
		"#eqwin scale mark label, #eqwin scale marks label { color: #ffffff; font-size: 12px; }",
		-1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
}

static void eq_popup_open(int is_tx){
	struct eq_popup *p = &eq_popups[is_tx];
	char name[24];
	struct field *f;
	int i;

	if (p->win){
		gtk_window_present(GTK_WINDOW(p->win));
		return;
	}

	eq_apply_css();

	p->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_name(p->win, "eqwin");
	gtk_window_set_title(GTK_WINDOW(p->win), is_tx ? "TX Equalizer" : "RX Equalizer");
	if (window)
		gtk_window_set_transient_for(GTK_WINDOW(p->win), GTK_WINDOW(window));
	gtk_window_set_modal(GTK_WINDOW(p->win), FALSE);
	gtk_window_set_keep_above(GTK_WINDOW(p->win), TRUE);
	gtk_window_set_position(GTK_WINDOW(p->win), GTK_WIN_POS_CENTER_ON_PARENT);
	gtk_window_set_default_size(GTK_WINDOW(p->win), 540, 340);
	// X button hides rather than destroys, so the window is reused
	g_signal_connect(p->win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
	gtk_container_add(GTK_CONTAINER(p->win), vbox);

	// header: title + on/off
	GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *title = gtk_label_new(is_tx ? "TX equalizer  (dB)" : "RX equalizer  (dB)");
	gtk_widget_set_halign(title, GTK_ALIGN_START);
	p->toggle = gtk_toggle_button_new_with_label("EQ OFF");
	gtk_widget_set_name(p->toggle, "eqtoggle");
	gtk_widget_set_size_request(p->toggle, 100, 40);
	gtk_box_pack_start(GTK_BOX(hdr), title, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(hdr), p->toggle, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hdr, FALSE, FALSE, 0);

	// six sliders, each with its center frequency underneath
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	for (i = 0; i < EQ_UI_BANDS; i++){
		GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
			-EQ_UI_MAX_DB, EQ_UI_MAX_DB, 1);
		gtk_range_set_inverted(GTK_RANGE(sc), TRUE);        // up = boost
		gtk_range_set_round_digits(GTK_RANGE(sc), 0);
		gtk_scale_set_digits(GTK_SCALE(sc), 0);
		// No fill from the bottom: this is cut/boost around 0 dB, not a level,
		// so a half-filled bar at flat would be misleading.
		gtk_scale_set_has_origin(GTK_SCALE(sc), FALSE);
		g_signal_connect(sc, "format-value", G_CALLBACK(on_eq_format_value), NULL);
		gtk_scale_set_draw_value(GTK_SCALE(sc), TRUE);
		gtk_scale_set_value_pos(GTK_SCALE(sc), GTK_POS_BOTTOM);
		// dB scale down the left of each slider: numbers at every 10 dB,
		// plus a tick (no number, the slider shows its own) every 5 dB
		for (int mk = -EQ_UI_MAX_DB; mk <= EQ_UI_MAX_DB; mk += 5){
			if (mk % 10 == 0){
				char mt[8];
				if (mk == 0) strcpy(mt, "0"); else sprintf(mt, "%+d", mk);
				gtk_scale_add_mark(GTK_SCALE(sc), mk, GTK_POS_LEFT, mt);
			}
			else
				gtk_scale_add_mark(GTK_SCALE(sc), mk, GTK_POS_LEFT, NULL);
		}
		gtk_scale_add_mark(GTK_SCALE(sc), 0, GTK_POS_RIGHT, NULL);   // 0 dB reference line across
		gtk_widget_set_size_request(sc, -1, 190);

		// start from the stored (restored-at-startup) value, BEFORE
		// connecting the signal, so opening the window doesn't re-send it
		eq_field_name(name, is_tx, i);
		f = get_field(name);
		gtk_range_set_value(GTK_RANGE(sc), f ? atoi(f->value) : 0);
		g_signal_connect(sc, "value-changed", G_CALLBACK(on_eq_scale_changed),
			GINT_TO_POINTER(is_tx * 16 + i));

		GtkWidget *lbl = gtk_label_new(eq_band_names[i]);
		gtk_box_pack_start(GTK_BOX(col), sc, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(col), lbl, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
		p->scales[i] = sc;
	}
	gtk_box_pack_start(GTK_BOX(vbox), row, TRUE, TRUE, 0);

	// bottom: Flat (zero all six) and Close
	GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *flat = gtk_button_new_with_label("Flat");
	GtkWidget *close_btn = gtk_button_new_with_label("Close");
	gtk_widget_set_size_request(flat, 90, 40);
	gtk_widget_set_size_request(close_btn, 90, 40);
	g_signal_connect(flat, "clicked", G_CALLBACK(on_eq_flat), GINT_TO_POINTER(is_tx));
	g_signal_connect(close_btn, "clicked", G_CALLBACK(on_eq_close), GINT_TO_POINTER(is_tx));
	gtk_box_pack_start(GTK_BOX(btns), flat, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(btns), close_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), btns, FALSE, FALSE, 0);

	// on/off: set from the stored value, then connect
	eq_field_name(name, is_tx, -1);
	f = get_field(name);
	int on = (f && !strcmp(f->value, "1")) ? 1 : 0;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->toggle), on);
	gtk_button_set_label(GTK_BUTTON(p->toggle), on ? "EQ ON" : "EQ OFF");
	g_signal_connect(p->toggle, "toggled", G_CALLBACK(on_eq_toggled), GINT_TO_POINTER(is_tx));

	gtk_widget_show_all(p->win);
	gtk_window_present(GTK_WINDOW(p->win));
}
// EQ_POPUP_END

// Menu button for either EQ: shows ON/OFF from the stored field, and a
// tap opens that side's popup.
static int eq_menu_button(struct field *f, cairo_t *gfx, int event, int is_tx){
	if (event == FIELD_DRAW){
		struct field *on = get_field(is_tx ? "eq_tx_on" : "eq_rx_on");
		draw_menu_field(f, gfx, (on && !strcmp(on->value, "1")) ? "ON" : "OFF");
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		eq_popup_open(is_tx);
		return 1;
	}
	return 0;
}

int do_eq_rx_open(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	return eq_menu_button(f, gfx, event, 0);
}

int do_eq_tx_open(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	return eq_menu_button(f, gfx, event, 1);
}

// Voice Recorder: channel selector (0-4, displayed as CH1-CH5). Knob
// adjusts which channel is selected. A double-tap (same shared pattern as
// the other combined boxes) erases the currently selected channel's
// recording -- combines what used to be a separate ERSE box into this
// one, freeing a slot for VOLM. Changing channel or erasing is blocked in
// sbitx.c while record/playback is active.
int do_rec_channel(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		char display[8];
		sprintf(display, "CH%d", v + 1);
		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		if (last_toggle_tap == f){
			// Double-tap: erase the currently selected channel.
			char response[20], request[20];
			sprintf(request, "erase=%d", 1);
			sdr_request(request, response);
			last_toggle_tap = NULL;
		}
		else{
			last_toggle_tap = f;
		}
		update_field(f);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "rec_channel=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// Voice Recorder: RECD toggle. FIELD_NUMBER (not FIELD_TOGGLE) so the
// displayed value is set manually AFTER sending the command and re-reading
// the actual resulting state -- never assumed -- since sbitx.c's safety
// guards (e.g. can't record while playing) can reject the request, and a
// FIELD_TOGGLE would have already flipped the display regardless.
int do_rec_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, rec_active ? "ON" : "OFF");
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		int is_double_tap = (last_toggle_tap == f);
		char response[20], request[20];

		if (is_double_tap){
			if (rec_active){
				sprintf(request, "rec=OFF");
				sdr_request(request, response);
			}
			last_toggle_tap = NULL;
		}
		else{
			if (!rec_active){
				sprintf(request, "rec=ON");
				sdr_request(request, response);
			}
			last_toggle_tap = f;
		}
		update_field(f);   // redraw reads rec_active fresh -- reflects whatever sbitx.c actually did
		return 1;
	}
	return 0;
}

// Voice Recorder: PLAY toggle. Same manual-display approach as RECD, for
// the same reason (playing an empty channel, or while recording, is
// rejected in sbitx.c).
int do_play_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	static unsigned int play_last_tap_time = 0;

	if (event == FIELD_DRAW){
		const char *display;
		if (play_active) display = "ON";
		else if (rec_length[rec_channel_selected] == 0) display = "EMPT";
		else display = "OFF";
		draw_menu_field(f, gfx, display);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		unsigned int now = millis();
		int is_rapid_double_tap = (now - play_last_tap_time < PLAY_DOUBLE_TAP_MS);
		play_last_tap_time = now;

		char response[20], request[20];
		if (is_rapid_double_tap && play_active){
			// Genuinely rapid second tap while playing -- explicit stop.
			sprintf(request, "play=OFF");
			sdr_request(request, response);
		}
		else{
			// Every other tap -- whether starting fresh or a normal-paced
			// repeat while already playing -- always retriggers. sbitx.c's
			// own command handler decides fresh-start (30ms) vs repeat
			// (10ms) timing; this callback's only job is to always send
			// the request, never silently do nothing.
			sprintf(request, "play=ON");
			sdr_request(request, response);
		}
		update_field(f);
		return 1;
	}
	return 0;
}

// ERAS: tap once -> shows SURE?, tap again within 3 s -> erases channel.
static unsigned int rec_erase_armed_at = 0;
int do_rec_erase(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int armed = rec_erase_armed_at && (millis() - rec_erase_armed_at < 3000);
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, armed ? "SURE?" : "ERAS");
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		if (armed){
			char response[20];
			sdr_request("erase=1", response);
			rec_erase_armed_at = 0;
		}
		else
			rec_erase_armed_at = millis();
		update_field(f);
		return 1;
	}
	return 0;
}

// EXPT: write the selected channel to a WAV file and copy it to a USB stick.
int do_rec_export(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, "EXPT");
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		char response[20];
		sdr_request("rec_export=1", response);
		update_field(f);
		return 1;
	}
	return 0;
}

// Voice Recorder: playback volume (0-20, 10 = unity gain). Knob only.
// Applies to both what's actually transmitted and the local monitor feed.
int do_rec_volume(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "rec_volume=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// SWR Sweep: drive cap adjuster. Knob only -- deliberately conservative
// range (1-30) pending a wattmeter-verified value; the real safety
// enforcement is the live fwdpower cutoff inside run_swr_sweep() itself.
int do_sweep_drive(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "sweep_drive=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// SWR Sweep: number of readings to average (1-10). Knob only.
int do_sweep_iterations(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	int v = atoi(f->value);

	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == FIELD_EDIT){
		if (a == MIN_KEY_UP && v + f->step <= f->max)
			v += f->step;
		else if (a == MIN_KEY_DOWN && v - f->step >= f->min)
			v -= f->step;
		sprintf(f->value, "%d", v);
		update_field(f);
		char request[20], response[20];
		sprintf(request, "sweep_iterations=%d", v);
		sdr_request(request, response);
		return 1;
	}
	return 0;
}

// SWR Sweep: trigger. Tapping starts a run at the current VFO frequency
// (blocking briefly while it runs, matching calibrate_band_power()'s own
// blocking pattern in this codebase) -- shows RUN while active, GO
// otherwise. sbitx.c itself refuses to start a run while already
// transmitting or already running, so this is safe to tap freely.
// SWR Sweep: trigger. Tapping while idle starts a sweep (non-blocking --
// runs incrementally inside sound_process(), so the UI stays fully
// responsive throughout). Tapping again WHILE RUNNING aborts it
// immediately -- this is the missing abort capability. Shows RUN while
// active, GO otherwise.
int do_sweep_trigger(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, sweep_running ? "RUN" : "GO");
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		char response[20], request[20];
		if (sweep_running)
			sprintf(request, "sweep_abort=%d", 1);
		else
			sprintf(request, "sweep=%d", 1);
		sdr_request(request, response);
		update_field(f);
		return 1;
	}
	return 0;
}

// SWR Sweep: current VFO frequency display, read-only. Shows in kHz to
// fit the box. This is exactly the frequency the sweep will measure at --
// makes it visible on this menu page rather than only elsewhere on screen.
int do_sweep_freq_display(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		char display[16];
		sprintf(display, "%d", freq_hdr / 1000);
		draw_menu_field(f, gfx, display);
		return 1;
	}
	return 0;
}

// Positions every menu-paged field: fields belonging to the current page
// go to their on-screen slot, everything else moves off-screen (x=1000,
// y=-1000, the existing convention this codebase already uses to hide a
// field -- see draw_field()'s "if (f->x <= -1000) return;" check).
// Adding a new feature to a page later just means adding one more
// field_move() pair inside the matching "if (current_menu_page == N)"
// block below -- the paging mechanism itself doesn't need to change.
void update_menu_page(){
	int row_y = menu_row_y;
	int page = menu_system_visible ? current_menu_page : 0;  // 0 matches no page -> everything hidden

	// Row geometry, computed against the actual screen_width (800px by
	// default) rather than guessed -- MENU/KBD sit at screen_width-47, so
	// the row must end with a safety margin before that column. Capped at
	// 48px (reduced further, matching the smaller font now used
	// uniformly across every field in the menu system).
	int row_start = 465;
	int row_end = screen_width - 47 - 10;  // 10px clearance before MENU
	int box4_w = (row_end - row_start - 3*3) / 4;  // 4 boxes, 3 gaps of 3px
	if (box4_w > 48) box4_w = 48;  // cap even if more room happens to exist
	int gap = 3;
	int row_h = 36;  // reduced from 40, matching the smaller font

	// Page 1: Notch (3 boxes) + MIC Compression (1 combined box) = 4/4, full.
	if (page == 1){
		field_move("NOTC", row_start, row_y, box4_w, row_h);
		field_move("NWTH", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
		field_move("NFRQ", row_start + (box4_w+gap)*2, row_y, box4_w, row_h);
		field_move("MCMP", row_start + (box4_w+gap)*3, row_y, box4_w, row_h);
	} else {
		field_move("NOTC", 1000, -1000, box4_w, row_h);
		field_move("NWTH", 1000, -1000, box4_w, row_h);
		field_move("NFRQ", 1000, -1000, box4_w, row_h);
		field_move("MCMP", 1000, -1000, box4_w, row_h);
	}

	// Page 2: Contour (3 boxes) + Audio Boost (1 combined box) = 4/4, full.
	// Same geometry as Page 1.
	if (page == 2){
		field_move("CONT", row_start, row_y, box4_w, row_h);
		field_move("CONW", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
		field_move("CFRQ", row_start + (box4_w+gap)*2, row_y, box4_w, row_h);
		field_move("ABST", row_start + (box4_w+gap)*3, row_y, box4_w, row_h);
	} else {
		field_move("CONT", 1000, -1000, box4_w, row_h);
		field_move("CONW", 1000, -1000, box4_w, row_h);
		field_move("CFRQ", 1000, -1000, box4_w, row_h);
		field_move("ABST", 1000, -1000, box4_w, row_h);
	}

	// Page 3: the four receive noise/voice DSP controls together -- NBVL
	// (impulse noise blanker), NRVL (noise reduction), FMNT (formant peak
	// boost), FVLY (valley suppression). Each a single combined box (tap to
	// toggle off/on, knob to adjust level). Full page, same sizing as the
	// other 4-box pages.
	if (page == 3){
		field_move("NBVL", row_start, row_y, box4_w, row_h);
		field_move("NRVL", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
		field_move("FMNT", row_start + (box4_w+gap)*2, row_y, box4_w, row_h);
		field_move("FVLY", row_start + (box4_w+gap)*3, row_y, box4_w, row_h);
	} else {
		field_move("NBVL", 1000, -1000, box4_w, row_h);
		field_move("NRVL", 1000, -1000, box4_w, row_h);
		field_move("FMNT", 1000, -1000, box4_w, row_h);
		field_move("FVLY", 1000, -1000, box4_w, row_h);
	}

	// Page 4: AINR (RNNoise AI noise reduction), then the optional USB audio:
	// USBS = USB speaker, USBM = USB microphone, UPTT = which key the USB
	// mic's PTT button sends (OFF, F9, F10, F11 or F12). Full page.
	if (page == 4){
		field_move("AINR", row_start, row_y, box4_w, row_h);
		field_move("USBS", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
		field_move("USBM", row_start + (box4_w+gap)*2, row_y, box4_w, row_h);
		field_move("UPTT", row_start + (box4_w+gap)*3, row_y, box4_w, row_h);
	} else {
		field_move("AINR", 1000, -1000, box4_w, row_h);
		field_move("USBS", 1000, -1000, box4_w, row_h);
		field_move("USBM", 1000, -1000, box4_w, row_h);
		field_move("UPTT", 1000, -1000, box4_w, row_h);
	}

	// Page 5: Voice Recorder (CHAN/RECD/PLAY/VOLM, 4 boxes, full) -- for
	// pre-recorded CQ calls or any other TX audio. Erase is now a
	// double-tap on CHAN (see do_rec_channel), freeing this slot for VOLM.
	// Uses the same shared box4_w/row_h as every other full 4-box menu
	// (M1/M2/M6), for consistent sizing across all of them.
	if (page == 5){
		int w6 = (row_end - row_start - 5*gap) / 6;
		if (w6 > 42) w6 = 42;
		field_move("CHAN", row_start, row_y, w6, row_h);
		field_move("RECD", row_start + (w6+gap)*1, row_y, w6, row_h);
		field_move("PLAY", row_start + (w6+gap)*2, row_y, w6, row_h);
		field_move("VOLM", row_start + (w6+gap)*3, row_y, w6, row_h);
		field_move("ERAS", row_start + (w6+gap)*4, row_y, w6, row_h);
		field_move("EXPT", row_start + (w6+gap)*5, row_y, w6, row_h);
	} else {
		field_move("ERAS", 1000, -1000, box4_w, row_h);
		field_move("EXPT", 1000, -1000, box4_w, row_h);
		field_move("CHAN", 1000, -1000, box4_w, row_h);
		field_move("RECD", 1000, -1000, box4_w, row_h);
		field_move("PLAY", 1000, -1000, box4_w, row_h);
		field_move("VOLM", 1000, -1000, box4_w, row_h);
	}

	// Page 6: SWR Sweep (SWFR/SWIT/SWPW/SWPT, 4 boxes, full). SWFR shows
	// the current VFO frequency (the sweep always measures there). SWPT
	// triggers repeated readings, printed live to the console/log area
	// (the empty left panel) along with the final average and standard
	// deviation, rather than a results box on this menu.
	if (page == 6){
		field_move("SWFR", row_start, row_y, box4_w, row_h);
		field_move("SWIT", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
		field_move("SWPW", row_start + (box4_w+gap)*2, row_y, box4_w, row_h);
		field_move("SWPT", row_start + (box4_w+gap)*3, row_y, box4_w, row_h);
	} else {
		field_move("SWFR", 1000, -1000, box4_w, row_h);
		field_move("SWIT", 1000, -1000, box4_w, row_h);
		field_move("SWPW", 1000, -1000, box4_w, row_h);
		field_move("SWPT", 1000, -1000, box4_w, row_h);
	}

	// Page 7: VOX (VOXT/VXTH/VXHT, 3 boxes used, 1 blank) -- automatic
	// voice-operated TX, keys on speech, unkeys after the hang time.
	if (page == 7){
		field_move("VOXT", row_start, row_y, box4_w, row_h);
		field_move("VXTH", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
		field_move("VXHT", row_start + (box4_w+gap)*2, row_y, box4_w, row_h);
	} else {
		field_move("VOXT", 1000, -1000, box4_w, row_h);
		field_move("VXTH", 1000, -1000, box4_w, row_h);
		field_move("VXHT", 1000, -1000, box4_w, row_h);
	}

	// Page 8: Equalizer (EQRX/EQTX, 2 boxes used) -- each opens its own
	// popup window with six sliders; settings are independent per side.
	if (page == 8){
		field_move("EQRX", row_start, row_y, box4_w, row_h);
		field_move("EQTX", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
	} else {
		field_move("EQRX", 1000, -1000, box4_w, row_h);
		field_move("EQTX", 1000, -1000, box4_w, row_h);
	}

#ifdef SBITX_FREEDV
	// Page 9: FreeDV digital voice. FDVM = mode (OFF, 700D, 700E, 1600),
	// FDVS = receiver status: SRCH while searching, or the signal-to-noise
	// ratio in dB (e.g. +7) once it has locked on.
	if (page == 9){
		field_move("FDVM", row_start, row_y, box4_w, row_h);
		field_move("FDVS", row_start + (box4_w+gap)*1, row_y, box4_w, row_h);
		field_move("FDVW", row_start + (box4_w+gap)*2, row_y, box4_w, row_h);
	} else {
		fdv_win_hide();   // leaving the FreeDV page closes its window
		field_move("FDVM", 1000, -1000, box4_w, row_h);
		field_move("FDVS", 1000, -1000, box4_w, row_h);
		field_move("FDVW", 1000, -1000, box4_w, row_h);
	}
#endif

	// One function per page, maximum 4 boxes, never split across pages.
	// Any future page using 4 boxes MUST use box4_w (or narrower), not a
	// hardcoded width, to stay guaranteed clear of MENU/KBD.
}

int do_menu_toggle(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){
		draw_menu_field(f, gfx, f->value);
		return 1;
	}
	if (event == GDK_BUTTON_PRESS){
		current_menu_page++;
		if (current_menu_page > MENU_PAGE_COUNT)
			current_menu_page = 1;
		sprintf(f->value, "M%d", current_menu_page);
		update_field(f);
		update_menu_page();
		return 1;
	}
	if (event == FIELD_EDIT){
		// knob support: turn either direction to move forward/back through pages
		if (a == MIN_KEY_UP){
			current_menu_page++;
			if (current_menu_page > MENU_PAGE_COUNT)
				current_menu_page = 1;
		}
		else if (a == MIN_KEY_DOWN){
			current_menu_page--;
			if (current_menu_page < 1)
				current_menu_page = MENU_PAGE_COUNT;
		}
		sprintf(f->value, "M%d", current_menu_page);
		update_field(f);
		update_menu_page();
		return 1;
	}
	return 0;
}



void open_url(char *url){
	char temp_line[200];

	sprintf(temp_line, "chromium-browser --log-leve=3 "
	"--enable-features=OverlayScrollbar %s"
	"  >/dev/null 2> /dev/null &", url);
	execute_app(temp_line);
}

void qrz(const char *callsign){
	char 	url[1000];

	sprintf(url, "https://qrz.com/DB/%s &", callsign);
	open_url(url);
}

int do_macro(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	char buff[256], *mode;
	char contact_callsign[100];

	strcpy(contact_callsign, get_field("#contact_callsign")->value);

	if(event == GDK_BUTTON_PRESS){
		int fn_key = atoi(f->cmd+3); // skip past the '#mf' and read the function key number

/*		if (!strcmp(f->cmd, "#mfkbd")){
			set_ui(LAYOUT_KBD);
			return 1;
		}
		else if (!strcmp(f->cmd, "#mfqrz") && strlen(contact_callsign) > 0){
			qrz(contact_callsign);
			return 1;
		}
		else 
*/
	 	macro_exec(fn_key, buff);
	
		mode = get_field("r1:mode")->value;

		//add the end of transmission to the expanded buffer for the fldigi modes
		if (!strcmp(mode, "RTTY") || !strcmp(mode, "PSK31")){
			strcat(buff, "^r");
			tx_on(TX_SOFT);
		}

		if (!strcmp(mode, "FT8") && strlen(buff)){
			ft8_tx(buff, atoi(get_field("#tx_pitch")->value));
			set_field("#text_in", "");
			//write_console(FONT_LOG_TX, buff);
		}
		else if (strlen(buff)){
			set_field("#text_in", buff);
			//put it in the text buffer and hope it gets transmitted!
		}
		return 1;
	}
	else if (event == FIELD_DRAW){
		int width, offset, text_length, line_start, y;	
		char this_line[MAX_FIELD_LENGTH];
		int text_line_width = 0;

		fill_rect(gfx, f->x, f->y, f->width,f->height, COLOR_BACKGROUND);
		rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

		width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
		offset = f->width/2 - width/2;
		if (strlen(f->value) == 0)
			draw_text(gfx, f->x +5, f->y+13 , f->label , FONT_FIELD_LABEL);
		else {
			if (strlen(f->label)){
				draw_text(gfx, f->x+5, f->y+5 ,  f->label, FONT_FIELD_LABEL);
				draw_text(gfx, f->x+5 , f->y+f->height - 20 , f->value , f->font_index);
			}
			else
				draw_text(gfx, f->x+offset , f->y+5, f->value , f->font_index);
			}	
		return 1;
	}

	return 0;
}

int do_record(struct field *f, cairo_t *gfx, int event, int a, int b, int c){
	if (event == FIELD_DRAW){

		if (f_focus == f)
			rect(gfx, f->x, f->y, f->width-1,f->height, COLOR_SELECTED_BOX, 2);
		else if (f_hover == f)
			rect(gfx, f->x, f->y, f->width,f->height, COLOR_SELECTED_BOX, 1);
		else 
			rect(gfx, f->x, f->y, f->width,f->height, COLOR_CONTROL_BOX, 1);

		int width = measure_text(gfx, f->label, FONT_FIELD_LABEL);
		int offset = f->width/2 - width/2;
		int	label_y = f->y + ((f->height 
			- font_table[FONT_FIELD_LABEL].height - 5  
			- font_table[FONT_FIELD_VALUE].height)/2);
		draw_text(gfx, f->x + offset, label_y, f->label, FONT_FIELD_LABEL);


		char duration[12];
		label_y += font_table[FONT_FIELD_LABEL].height;

		if (record_start){
			width = measure_text(gfx, f->value, f->font_index);
			offset = f->width/2 - width/2;
			time_t duration_seconds = time(NULL) - record_start;
			int minutes = duration_seconds/60;
			int seconds = duration_seconds % 60;
			sprintf(duration, "%d:%02d", minutes, seconds); 	
		}
		else
			strcpy(duration, "OFF");
		width = measure_text(gfx, duration, FONT_FIELD_VALUE);
		draw_text(gfx, f->x + f->width/2 - width/2, label_y, duration, f->font_index);
		return 1;
	}
	return 0;
}

void tx_on(int trigger){
	char response[100];

	if (trigger != TX_SOFT && trigger != TX_PTT){
		puts("Error: tx_on trigger should be SOFT or PTT");
		return;
	}

	struct field *f = get_field("r1:mode");
	if (f){
		if (!strcmp(f->value, "CW"))
			tx_mode = MODE_CW;
		else if (!strcmp(f->value, "CWR"))
			tx_mode = MODE_CWR;
		else if (!strcmp(f->value, "USB"))
			tx_mode = MODE_USB;
		else if (!strcmp(f->value, "LSB"))
			tx_mode = MODE_LSB;
		else if (!strcmp(f->value, "NBFM"))
			tx_mode = MODE_NBFM;
		else if (!strcmp(f->value, "AM"))
			tx_mode = MODE_AM;
		else if (!strcmp(f->value, "2TONE"))
			tx_mode = MODE_2TONE;
		else if (!strcmp(f->value, "DIGITAL"))
			tx_mode = MODE_DIGITAL;
	}

	if (in_tx == 0){
		sdr_request("tx=on", response);	
		in_tx = trigger; //can be PTT or softswitch
		char response[20];
		struct field *freq = get_field("r1:freq");
		set_operating_freq(atoi(freq->value), response);
		update_field(get_field("r1:freq"));
		printf("TX\n");
	}

	tx_start_time = millis();
}

void tx_off(){
	char response[100];

	modem_abort();

	if (in_tx){
		sdr_request("tx=off", response);	
		in_tx = 0;
		sdr_request("key=up", response);
		char response[20];
		struct field *freq = get_field("r1:freq");
		set_operating_freq(atoi(freq->value), response);
		update_field(get_field("r1:freq"));
		printf("RX\n");
	}
	sound_input(0); //it is a low overhead call, might as well be sure
}


static int layout_handler(void* user, const char* section, 
            const char* name, const char* value)
{
	//the section is the field's name

	printf("setting %s:%s to %d\n", section, name, atoi(value));
	struct field *f = get_field(section);
	if (!strcmp(name, "x"))
		f->x = atoi(value);
	else if (!strcmp(name, "y"))
		f->y = atoi(value);
	else if (!strcmp(name, "width"))
		f->width = atoi(value);
	else if (!strcmp(name, "height"))
		f->height = atoi(value);	
}

void set_ui(int id){
	struct field *f = get_field("#kbd_q");

	if (id == LAYOUT_KBD){
		// the "#kbd" is out of screen, get it up and "#mf" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++){
			if (!strncmp(active_layout[i].cmd, "#kbd", 4) && active_layout[i].y > 1000)
				active_layout[i].y -= 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3) && active_layout[i].y < 1000)
				active_layout[i].y += 1000;
			active_layout[i].is_dirty = 1;	
		}
	}
	if (id == LAYOUT_MACROS) {
		// the "#mf" is out of screen, get it up and "#kbd" down
		for (int i = 0; active_layout[i].cmd[0] > 0; i++){
			if (!strncmp(active_layout[i].cmd, "#kbd", 4) && active_layout[i].y < 1000)
				active_layout[i].y += 1000;
			else if (!strncmp(active_layout[i].cmd, "#mf", 3) && active_layout[i].y > 1000)
				active_layout[i].y -= 1000;
			active_layout[i].is_dirty = 1;	
		}
	}
	current_layout = id;
}

int static cw_keydown = 0;
int	static cw_hold_until = 0;
int static cw_hold_duration = 150;

static void cw_key(int state){
	char response[100];
	if (state == 1 && cw_keydown == 0){
		sdr_request("key=down", response);
		cw_keydown = 1;
	}
	else if (state == 0 && cw_keydown == 1){
		cw_keydown = 0;
	}
	//printf("cw key = %d\n", cw_keydown);
}


static int control_down = 0;
static int usb_ptt_down = 0;
static gboolean on_key_release (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	key_modifier = 0;

	// USB microphone PTT button (a function key): release = back to receive
	{
		int pk = usb_ptt_keyval();
		if (pk && event->keyval == pk){
			usb_ptt_down = 0;
			tx_off();
			return TRUE;
		}
	}

	if (event->keyval == MIN_KEY_CONTROL){
		control_down = 0;
	}

	if (event->keyval == MIN_KEY_TAB){
		tx_off();
  }

}

static gboolean on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	char request[1000], response[1000];

	// USB microphone PTT button (a function key): press = transmit. Holding the
	// key makes the keyboard repeat, so only the first press counts.
	{
		int pk = usb_ptt_keyval();
		if (pk && event->keyval == pk){
			if (!usb_ptt_down){
				usb_ptt_down = 1;
				tx_on(TX_PTT);
			}
			return TRUE;
		}
	}

	if (event->keyval == MIN_KEY_CONTROL){
		control_down = 1;
	}

	if (control_down){
		GtkClipboard *clip;
		struct field *f;	
		switch(event->keyval){
			case 'r':
				tx_off();
				break;
			case 't':
				tx_on(TX_SOFT);
				break;
			case 'm':
				if (current_layout == LAYOUT_MACROS)
					set_ui(LAYOUT_KBD);
				else
					set_ui(LAYOUT_MACROS);
				break;
			case 'q':
				tx_off();
				set_field("#record", "OFF");
				save_user_settings(1);
				exit(0);
				break;
			case 'c':
				f = get_field("#text_in");
				clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				gtk_clipboard_set_text(clip, f->value, strlen(f->value));
				break; 
			case 'l':
				enter_qso();
				break;
			case 'v':
				clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				if (clip){
					int i = 0;
					gchar *text = gtk_clipboard_wait_for_text(clip);
					f = get_field("#text_in");
					if (text){
						i = strlen(f->value);
						while(i < MAX_FIELD_LENGTH-1 && *text){
							if (*text >= ' ' || *text == '\n' || 
											(*text >= ' ' && *text <= 128))
								f->value[i++] = *text;  
							text++;	
						}
						f->value[i] = 0;
						update_field(f);
					}
				}
			break;
		}
		return FALSE;
	}

	if (f_focus && f_focus->value_type == FIELD_TEXT){
		edit_field(f_focus, event->keyval); 
		return FALSE;
	}
		
//	printf("keyPress %x %x\n", event->keyval, event->state);
	//key_modifier = event->keyval;
	switch(event->keyval){
		case MIN_KEY_ESC:
			modem_abort();
			tx_off();
			call_wipe();
			break;
		case MIN_KEY_UP:
			if (f_focus == NULL && f_hover > active_layout){
				hover_field(f_hover - 1);
				//printf("Up, hover %s\n", f_hover->cmd);
			}else if (f_focus){
				edit_field(f_focus, MIN_KEY_UP);
			}
			break;
		case MIN_KEY_DOWN:
			if (f_focus == NULL && f_hover && strcmp(f_hover->cmd, "")){
				hover_field(f_hover + 1);
				//printf("Down, hover %d\n", f_hover);
			}
			else if (f_focus){
				edit_field(f_focus, MIN_KEY_DOWN);
			}
			break;
		case 65507:
			key_modifier |= event->keyval;
			//printf("key_modifier set to %d\n", key_modifier);
			break;
		default:
			//by default, all text goes to the text_input control
			if (event->keyval == MIN_KEY_ENTER)
				edit_field(get_field("#text_in"), '\n');
			else if (MIN_KEY_F1 <= event->keyval && event->keyval <= MIN_KEY_F12){
				int fn_key = event->keyval - MIN_KEY_F1 + 1;
				char fname[10];
				sprintf(fname, "#mf%d", fn_key);
				do_macro(get_field(fname), NULL, GDK_BUTTON_PRESS, 0, 0, 0);
			} 
			else
				edit_field(get_field("#text_in"), event->keyval);
			//if (f_focus)
			//	edit_field(f_focus, event->keyval); 
			//printf("key = %d (%c)\n", event->keyval, (char)event->keyval); 	
	}
  return FALSE; 
}

static gboolean on_scroll (GtkWidget *widget, GdkEventScroll *event, gpointer data) {
	
	if (f_focus){
		if (event->direction == 0){
     if (!strcmp(get_field("reverse_scrolling")->value, "ON")){
	  		edit_field(f_focus, MIN_KEY_DOWN);
      } else {
		  	edit_field(f_focus, MIN_KEY_UP);
      }
		} else {
      if (!strcmp(get_field("reverse_scrolling")->value, "ON")){
			  edit_field(f_focus, MIN_KEY_UP);
      } else {
			  edit_field(f_focus, MIN_KEY_DOWN);
      }
   }
	}
		
}


static gboolean on_window_state (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	mouse_down = 0;
}

static gboolean on_mouse_release (GtkWidget *widget, GdkEventButton *event, gpointer data) {
	struct field *f;

	mouse_down = 0;
	if (event->type == GDK_BUTTON_RELEASE && event->button == GDK_BUTTON_PRIMARY){
		if(f_focus->fn)
			f_focus->fn(f_focus, NULL, GDK_BUTTON_RELEASE, 
				(int)(event->x), (int)(event->y),0); 	
		//printf("mouse release at %d, %d\n", (int)(event->x), (int)(event->y));
	}
  /* We've handled the event, stop processing */
  return TRUE;
}


static gboolean on_mouse_move (GtkWidget *widget, GdkEventButton *event, gpointer data) {
	char buff[100];

	if (!mouse_down)
		return false;

	int x = (int)(event->x);
	int y = (int)(event->y);

	// if a control is in focus and it handles the mouse drag, then just do that
	// else treat it as a spin up/down of the control
	if (f_focus){

			if (!f_focus->fn ||  !f_focus->fn(f_focus, NULL, GDK_MOTION_NOTIFY, event->x, event->y, 0)){
				//just emit up or down
				if(last_mouse_x < x || last_mouse_y > y)
					edit_field(f_focus, MIN_KEY_UP);
				else if (last_mouse_x > x || last_mouse_y < y)
					edit_field(f_focus, MIN_KEY_DOWN);
			}
		}
	last_mouse_x = x;
	last_mouse_y = y;

	return true;
}

static gboolean on_mouse_press (GtkWidget *widget, GdkEventButton *event, gpointer data) {
	struct field *f;

	if (event->type == GDK_BUTTON_RELEASE){
		mouse_down = 0;
		//puts("mouse up in on_mouse_press");
	}
	else if (event->type == GDK_BUTTON_PRESS /*&& event->button == GDK_BUTTON_PRIMARY*/){

		//printf("mouse event at %d, %d\n", (int)(event->x), (int)(event->y));
		for (int i = 0; active_layout[i].cmd[0] > 0; i++) {
			f = active_layout + i;
			if (f->x < event->x && event->x < f->x + f->width 
					&& f->y < event->y && event->y < f->y + f->height){
				if (strncmp(f->cmd, "#kbd", 4))
					focus_field(f);
				else
					do_control_action(f->label);
				if (f->fn){
					//we get out of the loop just to prevent two buttons from responding
					if (f->fn(f, NULL, GDK_BUTTON_PRESS, event->x, event->y, event->button))
						break;	
				}
			} 
		}
		last_mouse_x = (int)event->x;
		last_mouse_y = (int)event->y;
		mouse_down = 1;
	}
  /* We've handled the event, stop processing */
  return FALSE;
}


/*
Turns out (after two days of debugging) that GTK is not thread-safe and
we cannot invalidate the spectrum from another thread .
This redraw is called from another thread. Hence, we set a flag here 
that is read by a timer tick from the main UI thread and the window
is posted a redraw signal that in turn triggers the redraw_all routine.
Don't ask me, I only work around here.
*/
void redraw(){
	struct field *f;
	f = get_field("#console");
	f->is_dirty = 1;
	f = get_field("#text_in");
	f->is_dirty = 1;
}

/* hardware specific routines */

void init_gpio_pins(){
	for (int i = 0; i < 15; i++){
		pinMode(pins[i], INPUT);
		pullUpDnControl(pins[i], PUD_UP);
	}

	pinMode(PTT, INPUT);
	pullUpDnControl(PTT, PUD_UP);
	pinMode(DASH, INPUT);
	pullUpDnControl(DASH, PUD_UP);
}

uint8_t dec2bcd(uint8_t val){
	return ((val/10 * 16) + (val %10));
}

uint8_t bcd2dec(uint8_t val){
	return ((val/16 * 10) + (val %16));
}

void rtc_read(){
	uint8_t rtc_time[10];

	i2cbb_write_i2c_block_data(DS3231_I2C_ADD, 0, 0, NULL);

	int e =  i2cbb_read_i2c_block_data(DS3231_I2C_ADD, 0, 8, rtc_time);
	if (e <= 0){
		printf("RTC not detected\n");
		return;
	}
	for (int i = 0; i < 7; i++)
		rtc_time[i] = bcd2dec(rtc_time[i]);

	char buff[100];
	
	//convert to julian
	struct tm t;
	time_t gm_now;

	t.tm_year 	= rtc_time[6] + 2000 - 1900;
	t.tm_mon 	= rtc_time[5] - 1;
	t.tm_mday 	= rtc_time[4];
	t.tm_hour 	= rtc_time[2];
	t.tm_min		= rtc_time[1];
	t.tm_sec		= rtc_time[0];		

	time_t tjulian = mktime(&t);
	
	tzname[0] = tzname[1] = "GMT";
	timezone = 0;
	daylight = 0;
	setenv("TZ", "UTC", 1);	
	gm_now = mktime(&t);

	write_console(FONT_LOG, "RTC detected\n");
	time_delta =(long)gm_now -(long)(millis()/1000l);
}


void rtc_write(int year, int month, int day, int hours, int minutes, int seconds){
	uint8_t rtc_time[10];

	rtc_time[0] = dec2bcd(seconds);
	rtc_time[1] = dec2bcd(minutes);
	rtc_time[2] = dec2bcd(hours);
	rtc_time[3] = 0;
	rtc_time[4] = dec2bcd(day);
	rtc_time[5] = dec2bcd(month);
	rtc_time[6] = dec2bcd(year - 2000);

	for (uint8_t i = 0; i < 7; i++){
  	int e = i2cbb_write_byte_data(DS3231_I2C_ADD, i, rtc_time[i]);
		if (e)
			printf("rtc_write: error writing DS3231 register at %d index\n", i);
	}

/*	int e =  i2cbb_write_i2c_block_data(DS1307_I2C_ADD, 0, 7, rtc_time);
	if (e < 0){
		printf("RTC not written: %d\n", e);
		return;
	}
*/
}

//this will copy the computer time
//to the rtc
void rtc_sync(){
	time_t t = time(NULL);
	struct tm *t_utc = gmtime(&t);

	printf("Checking for valid NTP time ...");
	if (system("ntpstat") != 0){
		printf(".. not found.\n");
		return;
	}
	printf("Syncing RTC to %04d-%02d-%02d %02d:%02d:%02d\n", 
		t_utc->tm_year + 1900,  t_utc->tm_mon + 1, t_utc->tm_mday, 
		t_utc->tm_hour, t_utc->tm_min, t_utc->tm_sec);

	rtc_write( t_utc->tm_year + 1900,  t_utc->tm_mon + 1, t_utc->tm_mday, 
		t_utc->tm_hour, t_utc->tm_min, t_utc->tm_sec);
}

int key_poll(){
	int key = CW_IDLE;
	int input_method = get_cw_input_method();

	if (input_method == CW_IAMBIC || input_method == CW_IAMBICB){	
		if (digitalRead(PTT) == LOW)
			key |= CW_DASH;
		if (digitalRead(DASH) == LOW)
			key |= CW_DOT;
	}
	//straight key
	else if (digitalRead(PTT) == LOW || digitalRead(DASH) == LOW)
			key = CW_DOWN;

	//printf("key %d\n", key);
	return key;
}

void enc_init(struct encoder *e, int speed, int pin_a, int pin_b){
	e->pin_a = pin_a;
	e->pin_b = pin_b;
	e->speed = speed;
	e->history = 5;
}

int enc_state (struct encoder *e) {
	return (digitalRead(e->pin_a) ? 1 : 0) + (digitalRead(e->pin_b) ? 2: 0);
}

int enc_read(struct encoder *e) {
  int result = 0; 
  int newState;
  
  newState = enc_state(e); // Get current state  
    
  if (newState != e->prev_state)
     delay (1);
  
  if (enc_state(e) != newState || newState == e->prev_state)
    return 0; 

  //these transitions point to the encoder being rotated anti-clockwise
  if ((e->prev_state == 0 && newState == 2) || 
    (e->prev_state == 2 && newState == 3) || 
    (e->prev_state == 3 && newState == 1) || 
    (e->prev_state == 1 && newState == 0)){
      e->history--;
      //result = -1;
    }
  //these transitions point to the enccoder being rotated clockwise
  if ((e->prev_state == 0 && newState == 1) || 
    (e->prev_state == 1 && newState == 3) || 
    (e->prev_state == 3 && newState == 2) || 
    (e->prev_state == 2 && newState == 0)){
      e->history++;
    }
  e->prev_state = newState; // Record state for next pulse interpretation
  if (e->history > e->speed){
    result = 1;
    e->history = 0;
  }
  if (e->history < -e->speed){
    result = -1;
    e->history = 0;
  }
  return result;
}

static int tuning_ticks = 0;
void tuning_isr(void){
	int tuning = enc_read(&enc_b);
	if (tuning < 0)
		tuning_ticks++;
	if (tuning > 0)
		tuning_ticks--;	
}

void query_swr(){
	uint8_t response[4];
	int16_t vfwd, vref;
	int  vswr;
	char buff[20];

	if (!in_tx)
		return;
	if(i2cbb_read_i2c_block_data(0x8, 0, 4, response) == -1)
		return;

	vfwd = vref = 0;

	memcpy(&vfwd, response, 2);
	memcpy(&vref, response+2, 2);
	if (vref >= vfwd)
		vswr = 100;
	else
		vswr = (10*(vfwd + vref))/(vfwd-vref);
	sprintf(buff,"%d", (vfwd * 40)/68);
	set_field("#fwdpower", buff);		
	sprintf(buff, "%d", vswr);
	set_field("#vswr", buff);
}

void oled_toggle_band(){
	unsigned int freq_now = field_int("FREQ");
	//choose the next band 
	int  band_now = 1;
	for (int i = 0; i < sizeof(band_stack)/sizeof(struct band); i++){
		if (band_stack[i].start <= freq_now && freq_now <= band_stack[i].stop)
			band_now = i;	
	}
	if (band_now == (sizeof(band_stack)/sizeof(struct band)) -1)
		change_band("80M");
	else
		change_band(band_stack[band_now+1].name); 
}

//if oled is detected, it will display the ip address on the oled
// andwait for the tuning knob to be pressed to resume 

void oled_setup(){
	char ip_str[1000];

	while(digitalRead(ENC1_SW) == HIGH){
		ip_str[0] = 0;
		FILE *pf = popen("hostname -I", "r");
		if (pf){
			fgets(ip_str, 100, pf);
			pclose(pf);
			//terminate the string at the first space
			char *p = strchr(ip_str, ' ');
			if (p)
				*p = 0;
			oled_clear();
			oled_write(0,0, "Hi, zBitx is up on");
			oled_write(0,1, ip_str);
			oled_write(0, 2, "Press Func to start");
			oled_refresh();
		}
		delay(100);
	}
	
	oled_clear();
	oled_write(0, 3, "Starting...");
	oled_refresh();
}


char oled_screen_text[1000] = {0};
void oled_update(){
	char buff[1000];
	char const *mode;
	char const *p;

	//draw out the radio display 
	if (in_tx)
		strcpy(buff, "T");
	else if (!strcmp(field_str("SPLIT"), "ON"))
		strcpy(buff, "S");
	else if (!strcmp(field_str("VFO"), "A"))
		strcpy(buff, "A");
	else	
		strcpy(buff, "B");
	strcat(buff, ":");
	strcat(buff, freq_with_separators(field_str("FREQ")));
	
	mode = field_str("MODE");
	char *q = buff + strlen(buff);
	strncpy(q, p, 5);
	*(q + 5) = 0; 
	strcat(buff, "\n>Audio:");
	p = field_str("AUDIO");
	strcat(buff, p);
	strcat(buff, "\n Drive:");
	strcat(buff, field_str("DRIVE"));
	strcat(buff, "\n BW   :");
	strcat(buff, field_str("BW"));
	if (!strcmp(mode, "FT8")){
		strcat(buff, "\n Pitch:");
		strcat(buff, field_str("TX_PITCH"));
	}
	else if (!strcmp(mode, "LSB") || !strcmp(mode, "USB")){
		strcat(buff, "\n Mic  :");
		strcat(buff, field_str("MIC"));
	}
	else {
		strcat(buff, "\n Pitch:");
		strcat(buff, field_str("PITCH"));
	}	

	if (!strncmp(buff, oled_screen_text, sizeof(oled_screen_text)))
		return;
	strcpy(oled_screen_text, buff);
	oled_clear();
	
	p = buff;
	for (int i = 0; i < 8; i++){
		p = oled_write(0, i, p);
		if(*p == '\n')
			p++;
		else
			break;
	}
	
	oled_refresh();		
}

void hw_init(){
	wiringPiSetup();
	init_gpio_pins();

	enc_init(&enc_a, ENC_FAST, ENC1_B, ENC1_A);
	enc_init(&enc_b, ENC_FAST, ENC2_A, ENC2_B);

	int e = g_timeout_add(1, ui_tick, NULL);

	wiringPiISR(ENC2_A, INT_EDGE_BOTH, tuning_isr);
	wiringPiISR(ENC2_B, INT_EDGE_BOTH, tuning_isr);
	if (!oled_init()){
		oled_available = 1;
		oled_setup();
	}
}

void hamlib_tx(int tx_input){
  if (tx_input){
    sound_input(1);
		tx_on(TX_SOFT);
	}
  else {
    sound_input(0);
		tx_off();
	}
}


int get_cw_delay(){
	return atoi(get_field("#cwdelay")->value);
}

int get_cw_input_method(){
	struct field *f = get_field("#cwinput");
	if (!strcmp(f->value, "KEYBOARD"))
		return CW_KBD;
	else if (!strcmp(f->value, "IAMBIC"))
		return CW_IAMBIC;
	else if (!strcmp(f->value, "IAMBICB"))
		return CW_IAMBICB;
	else
		return CW_STRAIGHT;
}

int get_pitch(){
	struct field *f = get_field("rx_pitch");
	return atoi(f->value);
}

int get_cw_tx_pitch(){
	struct field *f = get_field("#tx_pitch");
	return atoi(f->value);
}

int get_data_delay(){
	return data_delay;
}

int get_wpm(){
	struct field *f = get_field("#tx_wpm");
	return atoi(f->value);
}

long get_freq(){
	return atol(get_field("r1:freq")->value);
}



void bin_dump(int length, uint8_t *data){
	printf("i2c: ");
	for (int i = 0; i < length; i++)
		printf("%x ", data[i]);
	printf("\n");
}

int  web_get_console(char *buff, int max){
	char c;
	int i;

	if (q_length(&q_web) == 0)
		return 0;
	strcpy(buff, "CONSOLE ");
	buff += strlen("CONSOLE ");
	for (i = 0; (c = q_read(&q_web)) && i < max; i++){
		if (c < 128 && c >= ' ')
			*buff++ = c;
	}
	*buff = 0;
	return i;
}

void web_get_spectrum(char *buff){

  int n_bins = (int)((1.0 * spectrum_span) / 46.875);
  //the center frequency is at the center of the lower sideband,
  //i.e, three-fourth way up the bins.
  int starting_bin = (3 *MAX_BINS)/4 - n_bins/2;
  int ending_bin = starting_bin + n_bins;

  int j = 3;
  if (in_tx){
    strcpy(buff, "TX ");
    for (int i = 0; i < MOD_MAX; i++){
      int y = (2 * mod_display[i]) + 32;
      if (y > 127)
        buff[j++] = 127;
      else if(y > 0 && y <= 95)
        buff[j++] = y + 32;
      else
        buff[j++] = ' ';
    }
  }
  else{
    strcpy(buff, "RX ");
    for (int i = starting_bin; i <= ending_bin; i++){
      int y = spectrum_plot[i] + waterfall_offset;
      if (y > 95)
        buff[j++] = 127;
      else if(y >= 0 )
        buff[j++] = y + 32;
      else
        buff[j++] = ' ';
    }
  }

  buff[j++] = 0;
  return;
}

void set_radio_mode(char *mode){
	char umode[10], request[100], response[100];
	int i;

	printf("Mode: %s\n", mode);
	for (i = 0; i < sizeof(umode) - 1 && *mode; i++)
		umode[i] = toupper(*mode++);
	umode[i] = 0;

	sprintf(request, "r1:mode=%s", umode);
	sdr_request(request, response);
	if (strcmp(response, "ok")){
		printf("mode %d: unavailable\n", umode);
		return;
	}
	int new_bandwidth = 3000;
	switch(mode_id(umode)){
		case MODE_CW:
		case MODE_CWR:
			new_bandwidth = field_int("BW_CW");
			break;
		case MODE_LSB:
		case MODE_USB:
		case MODE_AM:
			new_bandwidth = field_int("BW_VOICE");
			break;
		case MODE_FT8:
			new_bandwidth = 4000;
			break;
		default:
			new_bandwidth = field_int("BW_DIGITAL");
	}
	layout_ui();
	//let the bw control trigger the filter
	char bw_str[10];
	sprintf(bw_str, "%d", new_bandwidth);
	field_set("BW", bw_str);

	struct field *f = get_field_by_label("MODE");
	if (strcmp(f->value, umode))
		field_set("MODE", umode);
}


gboolean ui_tick(gpointer gook){
	int static ticks = 0;

	ticks++;

	while (q_length(&q_remote_commands) > 0){
		//read each command until the 
		char remote_cmd[1000];
		int c, i;
		for (i = 0; i < sizeof(remote_cmd)-2 &&  (c = q_read(&q_remote_commands)) > 0; i++){
			remote_cmd[i] = c;
		}
		remote_cmd[i] = 0;

		//echo the keystrokes for chatty modes like cw/rtty/psk31/etc
		if (!strncmp(remote_cmd, "key ", 4))
			for (int i = 4; remote_cmd[i] > 0; i++)
				edit_field(get_field("#text_in"), remote_cmd[i]);	
		else {
			cmd_exec(remote_cmd);
			settings_updated = 1; //save the settings
		}
	}

		for (struct field *f = active_layout; f->cmd[0] > 0; f++){
			if (f->is_dirty){
				if (f->y >= 0){
					GdkRectangle r;
					r.x = f->x;
					r.y = f->y;
					r.width = f->width;
					r.height = f->height;
					invalidate_rect(r.x, r.y, r.width, r.height);
				}
			}
		}
  //char message[100];
	
	// check the tuning knob
	struct field *f = get_field("r1:freq");

	while (tuning_ticks > 0){
		edit_field(f, MIN_KEY_DOWN);
		tuning_ticks--;
    //sprintf(message, "tune-\r\n");
    //write_console(FONT_LOG, message);

	}

	while (tuning_ticks < 0){
		edit_field(f, MIN_KEY_UP);
		tuning_ticks++;
    //sprintf(message, "tune+\r\n");
    //write_console(FONT_LOG, message);
	}


	// VOX asks (from the audio thread) to key or unkey; do it here, the same
	// way the PTT does, so the screen, meters and frequency all follow.
	if (vox_ui_request){
		int rq = vox_ui_request;
		vox_ui_request = 0;
		if (rq == 1 && !in_tx)
			tx_on(TX_SOFT);
		else if (rq == 2 && in_tx == TX_SOFT)
			tx_off();
	}

#ifdef SBITX_FREEDV
	// FreeDV status box: refresh a few times a second while its page is showing
	{
		static int fdv_ticks = 0;
		if (++fdv_ticks >= 250){
			fdv_ticks = 0;
			if (menu_system_visible && current_menu_page == 9)
				update_field(get_field("#freedv_status"));
		}
	}
#endif

	if (ticks % 20 == 0){
  	modem_poll(mode_id(get_field("r1:mode")->value));
	}

	int tick_count = 100;
	switch(mode_id(field_str("MODE"))){
		case MODE_CW:
		case MODE_CWR:
			tick_count = 50;
			break;
		case MODE_FT8:
			tick_count = 200;
			break;
		default:
			tick_count = 100; 
	}
	if (ticks >= tick_count){

		char response[6], cmd[10];
		cmd[0] = 1;

		if(in_tx){
			char buff[10];

			sprintf(buff,"%d", fwdpower);
			set_field("#fwdpower", buff);		
			sprintf(buff, "%d", vswr);
			set_field("#vswr", buff);
		}

		struct field *f = get_field("spectrum");
		update_field(f);	//move this each time the spectrum watefall index is moved
		f = get_field("waterfall");
		update_field(f);
		update_titlebar();

		if (digitalRead(ENC1_SW) == 0){
			//flip between mode and volume
			if (f_focus && !strcmp(f_focus->label, "AUDIO"))
				focus_field(get_field("r1:mode"));
			else
				focus_field(get_field("r1:volume"));
			printf("Focus is on %s\n", f_focus->label);
		}
		
		if (digitalRead(ENC2_SW) == 0)
			oled_toggle_band();

		if (record_start)
			update_field(get_field("#record"));

		// alternate character from the softkeyboard upon long press
		if (f_focus && focus_since + 500 < millis() 
						&& !strncmp(f_focus->cmd, "#kbd_", 5) && mouse_down){
			//emit the symbol
			struct field *f_text = f_focus; //get_field("#text_in");
			//replace the previous character with teh shifted one
			edit_field(f_text,MIN_KEY_BACKSPACE); 
			edit_field(f_text, f_focus->label[0]);
			focus_since = millis();
		}

    // check if low and high settings are stepping on each other
    char new_value[20];
    while (atoi(get_field("r1:low")->value) > atoi(get_field("r1:high")->value)){
      sprintf(new_value, "%d", atoi(get_field("r1:high")->value)+get_field("r1:high")->step);
      set_field("r1:high",new_value);
    }


    static char last_mouse_pointer_value[16];

    int cursor_type;

    if (strcmp(get_field("mouse_pointer")->value, last_mouse_pointer_value)){
      sprintf(last_mouse_pointer_value,get_field("mouse_pointer")->value);
      if (!strcmp(last_mouse_pointer_value,"BLANK")){
        cursor_type = GDK_BLANK_CURSOR;
      } else if (!strcmp(last_mouse_pointer_value,"RIGHT")){
        cursor_type = GDK_RIGHT_PTR;
      } else if (!strcmp(last_mouse_pointer_value,"CROSSHAIR")){
        cursor_type = GDK_CROSSHAIR;
      } else {
        cursor_type = GDK_LEFT_PTR;
      }
      GdkCursor* new_cursor;
      new_cursor = gdk_cursor_new_for_display (gdk_display_get_default(),cursor_type);
      gdk_window_set_cursor(gdk_get_default_root_window(), new_cursor);
    }

		if (oled_available)
			oled_update();
		ticks = 0;
  }
	//update_field(get_field("#text_in")); //modem might have extracted some text

  hamlib_slice();
	remote_slice();
	save_user_settings(0);

 
	f = get_field("r1:mode");
	//straight key in CW
	if (f && (!strcmp(f->value, "2TONE") || !strcmp(f->value, "LSB") 
	|| !strcmp(f->value, "AM") || !strcmp(f->value, "USB"))){
		if (digitalRead(PTT) == LOW && in_tx == 0)
			tx_on(TX_PTT);
		else if (digitalRead(PTT) == HIGH && in_tx  == TX_PTT)
			tx_off();
	}

	int scroll = enc_read(&enc_a);
	if (scroll && f_focus){
		if (scroll < 0)
			edit_field(f_focus, MIN_KEY_DOWN);
		else
			edit_field(f_focus, MIN_KEY_UP);
	}	
	return TRUE;
}

void ui_init(int argc, char *argv[]){
 
  gtk_init( &argc, &argv );

	//we are using two deprecated functions here
	//if anyone has a hack around them, do submit it
/*
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	screen_width = gdk_screen_width();
	screen_height = gdk_screen_height();
#pragma pop
*/
	q_init(&q_web, 1000);

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 480);
  gtk_window_set_default_size(GTK_WINDOW(window), screen_width, screen_height);
  gtk_window_set_title( GTK_WINDOW(window), "sBITX" );
	gtk_window_set_icon_from_file(GTK_WINDOW(window), "/home/pi/sbitx/sbitx_icon.png", NULL);

  display_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(display_area, 500, 400);
  gtk_container_add( GTK_CONTAINER(window), display_area );

  g_signal_connect( G_OBJECT(window), "destroy", G_CALLBACK( gtk_main_quit ), NULL );
  g_signal_connect( G_OBJECT(display_area), "draw", G_CALLBACK( on_draw_event ), NULL );
  g_signal_connect (G_OBJECT (window), "key_press_event", G_CALLBACK (on_key_press), NULL);
  g_signal_connect (G_OBJECT (window), "key_release_event", G_CALLBACK (on_key_release), NULL);
  g_signal_connect (G_OBJECT (window), "window_state_event", G_CALLBACK (on_window_state), NULL);
	g_signal_connect (G_OBJECT(display_area), "button_press_event", G_CALLBACK (on_mouse_press), NULL);
	g_signal_connect (G_OBJECT(window), "button_release_event", G_CALLBACK (on_mouse_release), NULL);
	g_signal_connect (G_OBJECT(display_area), "motion_notify_event", G_CALLBACK (on_mouse_move), NULL);
	g_signal_connect (G_OBJECT(display_area), "scroll_event", G_CALLBACK (on_scroll), NULL);
	g_signal_connect(G_OBJECT(window), "configure_event", G_CALLBACK(on_resize), NULL);

  /* Ask to receive events the drawing area doesn't normally
   * subscribe to. In particular, we need to ask for the
   * button press and motion notify events that want to handle.
   */
  gtk_widget_set_events (display_area, gtk_widget_get_events (display_area)
                                     | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK 
																			| GDK_SCROLL_MASK
                                     | GDK_POINTER_MOTION_MASK);

  gtk_widget_show_all(window);
	layout_ui();	
	focus_field(get_field("r1:volume"));
	webserver_start();
	f_last_text = get_field_by_label("TEXT");
}

/* handle modem callbacks for more data */


int get_tx_data_byte(char *c){
	//take out the first byte and return it to the modem
	struct field *f = get_field("#text_in");
	int length = strlen(f->value);

	if (f->value[0] == '\\' || !length)
		return 0;
	if (length){
		*c = f->value[0];
		//now shift the buffer down, hopefully, this copies the trailing null too
		for (int i = 0; i < length; i++)
			f->value[i] = f->value[i+1];
	}
	f->is_dirty = 1;
	f->update_remote = 1;
	//update_field(f);
	return length;
}

int get_tx_data_length(){
	struct field *f = get_field("#text_in");

	if (strlen(f->value) == 0)
		return 0;

	if (f->value[0] != COMMAND_ESCAPE)
		return strlen(get_field("#text_in")->value);
	else
		return 0;
}

int is_in_tx(){
	return in_tx;
}

/* handle the ui request and update the controls */

void change_band(char *request){
	int i, old_band, new_band; 
	int max_bands = sizeof(band_stack)/sizeof(struct band);
	long new_freq, old_freq;
	char buff[100];

	//find the band that has just been selected, the first char is #, we skip it
	for (new_band = 0; new_band < max_bands; new_band++)
		if (!strcmp(request, band_stack[new_band].name))
			break;

	//continue if the band is legit
	if (new_band == max_bands)
		return;

	// find out the tuned frequency
	struct field *f = get_field("r1:freq");
	old_freq = atol(f->value);
	f = get_field("r1:mode");
	int old_mode = mode_id(f->value);
	if (old_mode == -1)
		return;

	//first, store this frequency in the appropriate bin
	for (old_band = 0; old_band < max_bands; old_band++)
		if (band_stack[old_band].start <= old_freq && old_freq <= band_stack[old_band].stop)
				break;

	int stack = band_stack[old_band].index;
	if (stack < 0 || stack >= STACK_DEPTH)
		stack = 0;
	if (old_band < max_bands){
		//update the old band setting 
		if (stack >= 0 && stack < STACK_DEPTH){
				band_stack[old_band].freq[stack] = old_freq;
				band_stack[old_band].mode[stack] = old_mode;
		}
	}

	//if we are still in the same band, move to the next position
	if (new_band == old_band){
		stack = ++band_stack[new_band].index;
		//move the stack and wrap the band around
		if (stack >= STACK_DEPTH)
			stack = 0;
		band_stack[new_band].index = stack;
	}
	stack = band_stack[new_band].index;
	sprintf(buff, "%d", band_stack[new_band].freq[stack]);
	char resp[100];
	set_operating_freq(band_stack[new_band].freq[stack], resp);
	field_set("FREQ", buff);
	field_set("MODE", mode_name[band_stack[new_band].mode[stack]]);	
	update_field(get_field("r1:mode"));

  // this fixes bug with filter settings not being applied after a band change, not sure why it's a bug - k3ng 2022-09-03
//  set_field("r1:low",get_field("r1:low")->value);
//  set_field("r1:high",get_field("r1:high")->value);

	abort_tx();
}

void utc_set(char *args, int update_rtc){
	int n[7], i;
	char *p, *q;
	struct tm t;
	time_t gm_now;

	i = 0;
	p =  strtok(args, "-/;: ");
	if (p){
		n[0] = atoi(p);
		for (i = 1; i < 7; i++){
			p = strtok(NULL, "-/;: ");
			if (!p)
				break;
			n[i] = atoi(p);
		}
	}	

	if (i != 6 ){
		write_console(FONT_LOG, 
			"Sets the current UTC Time for logging etc.\nUsage \\utc yyyy mm dd hh mm ss\nWhere\n"
			"  yyyy is a four digit year like 2022\n"
			"  mm is two digit month [1-12]\n"
			"  dd is two digit day of the month [0-31]\n"
			"  hh is two digit hour in 24 hour format (UTC)\n"
			"  mm is two digit minutes in 24 hour format(UTC)\n"
			"  ss is two digit seconds in [0-59]\n"
			"ex: \\utc 2022 07 14 8:40:00\n"); 
			return;
	}

	rtc_write(n[0], n[1], n[2], n[3], n[4], n[5]);

	if (n[0] < 2000)
		n[0] += 2000;
	t.tm_year = n[0] - 1900;
	t.tm_mon = n[1] - 1;
	t.tm_mday = n[2]; 
	t.tm_hour = n[3];
	t.tm_min = n[4];
	t.tm_sec = n[5];

	tzname[0] = tzname[1] = "GMT";
	timezone = 0;
	daylight = 0;
	setenv("TZ", "UTC", 1);	
	gm_now = mktime(&t);

	write_console(FONT_LOG, "UTC time is set\n");
	time_delta =(long)gm_now -(long)(millis()/1000l);
	printf("time_delta = %ld\n", time_delta);
}



void meter_calibrate(){
	//we change to 40 meters, cw
	printf("starting meter calibration\n"
	"1. Attach a power meter and a dummy load to the antenna\n"
	"2. Adjust the drive until you see 40 watts on the power meter\n"
	"3. Press the tuning knob to confirm.\n");

	set_field("r1:freq", "7035000");
	set_radio_mode("CW");	
	struct field *f_bridge = get_field("bridge");
	set_field("bridge", "100");	
	focus_field(f_bridge);
}

void do_control_action(char *cmd){	
	char request[1000], response[1000], buff[100];

	strcpy(request, cmd);			//don't mangle the original, thank you

	if (!strcmp(request, "CLOSE")){
		gtk_window_iconify(GTK_WINDOW(window));
	}
	else if (!strcmp(request, "OFF")){
		tx_off();
		set_field("#record", "OFF");
		save_user_settings(1);
		exit(0);
	}
	else if (!strcmp(request, "SET"))
		settings_ui(window);
	else if (!strcmp(request, "LOG"))
		logbook_list_open();
	else if (!strncmp(request, "BW ",3)){
		int bw = atoi(request+3);	
		set_filter_high_low(bw); //calls do_control_action again to set LOW and HIGH
		//we have to save this as well
		save_bandwidth(bw);
	}
	else if (!strcmp(request, "WIPE"))
		call_wipe();
	else if (!strcmp(request, "ESC")){
		//empty the text buffer
		modem_abort();
		tx_off();
		call_wipe();
		field_set("TEXT", "");
		modem_abort();
		tx_off();
	}
	else if (!strcmp(request, "TX")){	
		tx_on(TX_SOFT);
	}
	else if (!strcmp(request, "WEB")){
		open_url("http://127.0.0.1:8080");
	}
	else if (!strcmp(request, "RX")){
		tx_off();
	}
	else if (!strncmp(request, "RIT", 3))
		update_field(get_field("r1:freq"));
	else if (!strncmp(request, "SPLIT", 5)){
		update_field(get_field("r1:freq"));	
		if (!strcmp(get_field("#vfo")->value, "B"))
			set_field("#vfo", "A");
	}
	else if (!strcmp(request, "VFO B")){
		struct field *f = get_field("r1:freq");
		struct field *vfo = get_field("#vfo");
		struct field *vfo_a = get_field("#vfo_a_freq");
		struct field *vfo_b = get_field("#vfo_b_freq");
		if (!strcmp(vfo->value, "B")){
			//vfo_a_freq = atoi(f->value);
			strcpy(vfo_a->value, f->value);
			//sprintf(buff, "%d", vfo_b_freq);
			set_field("r1:freq", vfo_b->value);
			settings_updated++;
		}
	}
	else if (!strcmp(request, "VFO A")){
		struct field *f = get_field("r1:freq");
		struct field *vfo = get_field("#vfo");
		struct field *vfo_a = get_field("#vfo_a_freq");
		struct field *vfo_b = get_field("#vfo_b_freq");
		//printf("vfo old %s, new %s\n", vfo->value, request);
		if (!strcmp(vfo->value, "A")){
		//	vfo_b_freq = atoi(f->value);
			strcpy(vfo_b->value, f->value);
	//		sprintf(buff, "%d", vfo_a_freq);
			set_field("r1:freq", vfo_a->value);
			settings_updated++;
		}
	}
	else if (!strcmp(request, "KBD ON")){
		layout_ui();
	
	}
	else if (!strcmp(request, "KBD OFF")){
		layout_ui();
	}
	else if (!strcmp(request, "SAVE")){
			enter_qso();
	}
	//tuning step
  else if (!strcmp(request, "STEP 1M"))
    tuning_step = 1000000;
	else if (!strcmp(request, "STEP 100K"))
		tuning_step = 100000;
	else if (!strcmp(request, "STEP 10K"))
		tuning_step = 10000;
	else if (!strcmp(request, "STEP 1K"))
		tuning_step = 1000;
	else if (!strcmp(request, "STEP 500H"))
		tuning_step = 500;
	else if (!strcmp(request, "STEP 100H"))
		tuning_step = 100;
	else if (!strcmp(request, "STEP 10H"))
		tuning_step = 10;

	//spectrum bandwidth
	else if (!strcmp(request, "SPAN 2.5K"))
		spectrum_span = 2500;
	else if (!strcmp(request, "SPAN 6K"))
		spectrum_span = 6000;
	else if (!strcmp(request, "SPAN 10K"))
		spectrum_span = 10000;
	else if (!strcmp(request, "SPAN 25K"))
		spectrum_span = 25000;
		
	//handle the band stacking
	else if (!strcmp(request, "80M") || 
		!strcmp(request, "40M") || 
		!strcmp(request, "30M") || 
		!strcmp(request, "20M") || 
		!strcmp(request, "17M") || 
		!strcmp(request, "15M") || 
		!strcmp(request, "12M") || 
		!strcmp(request, "10M")){
		change_band(request); 		
	}
	else if (!strcmp(request, "REC ON")){
		char fullpath[200];	//dangerous, find the MAX_PATH and replace 200 with it

		char *path = getenv("HOME");
		time(&record_start);
		struct tm *tmp = localtime(&record_start);
		sprintf(fullpath, "%s/sbitx/audio/%04d%02d%02d-%02d%02d-%02d.wav", path, 
			tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec); 

		char request[300], response[100];
		sprintf(request, "record=%s", fullpath);
		sdr_request(request, response);
		sprintf(request, "Recording:%s\n", fullpath);
		write_console(FONT_LOG, request);
	}
	else if (!strcmp(request, "REC OFF")){
		sdr_request("record", "off");
		write_console(FONT_LOG, "Recording stopped\n");
		record_start = 0;
	}
	else if (!strcmp(request, "QRZ") && strlen(field_str("CALL")) > 0)
		qrz(field_str("CALL"));
	else {
		//send this to the radio core
		char args[MAX_FIELD_LENGTH];
		char exec[20];
		int i, j;

  	args[0] = 0;

		//copy the exec
		for (i = 0; *cmd > ' ' && i < sizeof(exec) - 1; i++)
			exec[i] = *cmd++;
		exec[i] = 0; 

		//skip the spaces
		while(*cmd == ' ')
			cmd++;

		j = 0;
		for (i = 0; *cmd && i < sizeof(args) - 1; i++){
			if (*cmd > ' ')
					j = i;
			args[i] = *cmd++;
		}
		args[++j] = 0;
		
		//translate the frequency of operating depending upon rit, split, etc.
		if (!strncmp(request, "FREQ", 4))
			set_operating_freq(atoi(request+5), response);
		else if (!strncmp(request, "MODE ", 5)){
			set_radio_mode(request+5);
			update_field(get_field("r1:mode"));
		}
		else{
			struct field *f = get_field_by_label(exec); 
			if (f){
				sprintf(request, "%s=%s", f->cmd, args);
				sdr_request(request, response);
			}
		}
	}
}

/*
	These are user/remote entered commands.
	The command format is "CMD VALUE", the CMD is an all uppercase text
	that matches the label of a control.
	The value is taken as a string past the separator space all the way
	to the end of the string, including any spaces.

	It also handles many commands that don't map to a control
	like metercal or txcal, etc.
*/
void cmd_exec(char *cmd){
	int i, j;
	int mode = mode_id(get_field("r1:mode")->value);

	char args[MAX_FIELD_LENGTH];
	char exec[20];

  args[0] = 0;

	//copy the exec
	for (i = 0; *cmd > ' ' && i < sizeof(exec) - 1; i++)
		exec[i] = *cmd++;
	exec[i] = 0; 

	//skip the spaces
	while(*cmd == ' ')
		cmd++;

	j = 0;
	for (i = 0; *cmd && i < sizeof(args) - 1; i++){
		if (*cmd > ' ')
				j = i;
		args[i] = *cmd++;
	}
	args[++j] = 0;

	char response[100];

	if (!strcmp(exec, "FT8")){
		ft8_process(args, FT8_START_QSO);
	}
	else if (!strcmp(exec, "callsign")){
		strcpy(get_field("#mycallsign")->value,args); 
		sprintf(response, "\n[Your callsign is set to %s]\n", get_field("#mycallsign")->value);
		write_console(FONT_LOG, response);
	}
	else if (!strcmp(exec, "metercal")){
		meter_calibrate();
	}
	else if (!strcmp(exec, "abort"))
		abort_tx();
	else if (!strcmp(exec, "rtc"))
		rtc_read();
	else if (!strcmp(exec, "txcal")){
		char response[10];
		sdr_request("txcal=", response);
	}
	else if (!strcmp(exec, "grid")){	
		set_field("#mygrid", args);
		sprintf(response, "\n[Your grid is set to %s]\n", get_field("#mygrid")->value);
		write_console(FONT_LOG, response);
	}
	else if (!strcmp(exec, "utc")){
		utc_set(args, 1);
	}
	else if (!strcmp(exec, "logbook")){
		char fullpath[200];	//dangerous, find the MAX_PATH and replace 200 with it
		char *path = getenv("HOME");
		sprintf(fullpath, "mousepad %s/sbitx/data/logbook.txt", path); 
		execute_app(fullpath);
	}
	else if (!strcmp(exec, "clear")){
		console_init();
	}
	else if(!strcmp(exec, "macro") || !strcmp(exec, "MACRO")){
		if (!strcmp(args, "list"))
			macro_list(NULL);
		else if (!macro_load(args, NULL)){
			set_ui(LAYOUT_MACROS);
			set_field("#current_macro", args);
		}
		else if (strlen(get_field("#current_macro")->value)){
			write_console(FONT_LOG, "current macro is ");
			write_console(FONT_LOG, get_field("#current_macro")->value);
			write_console(FONT_LOG, "\n");
		}
		else
			write_console(FONT_LOG, "macro file not loaded\n");
	}
	else if (!strcmp(exec, "qso"))
		enter_qso(args);
	else if (!strcmp(exec, "exchange")){
		set_field("#contest_serial", "0");
		set_field("#sent_exchange", "");

		if (strlen(args)){
			set_field("#sent_exchange", args);
			if (atoi(args))
				set_field("#contest_serial", args);
		}
		write_console(FONT_LOG, "Exchange set to [");
		write_console(FONT_LOG, get_field("#sent_exchange")->value);
		write_console(FONT_LOG, "]\n");
	}
	else if(!strcmp(exec, "freq") || !strcmp(exec, "f")){
		long freq = atol(args);
		if (freq == 0){
			write_console(FONT_LOG, "Usage: \f xxxxx (in Hz or KHz)\n");
		}
		else if (freq < 30000)
			freq *= 1000;

		if (freq > 0){
			char freq_s[20];
			sprintf(freq_s, "%ld",freq);
			set_field("r1:freq", freq_s);
		}
	}
  else if (!strcmp(exec, "exit")){
    tx_off();
    set_field("#record", "OFF");
    save_user_settings(1);
    exit(0);
  }
	else if (!strcmp(exec, "qrz")){
		if(strlen(args))
			qrz(args);
		else
			write_console(FONT_LOG, "/qrz [callsign]\n");
	}
	else if (!strcmp(exec, "mode") || !strcmp(exec, "m") || !strcmp(exec, "MODE")){
		set_radio_mode(args);
		update_field(get_field("r1:mode"));
	}
	else if (!strcmp(exec, "t"))
		tx_on(TX_SOFT);
	else if (!strcmp(exec, "r"))
		tx_off();
// added rtx for web remote tx function coming soon
        else if (!strcmp(exec, "rtx")) {
                tx_on(TX_SOFT);
                sound_input(1);
            }
	else if (!strcmp(exec, "telnet")){
		if (strlen(args) > 5) 
			telnet_open(args);
		else
			telnet_open(get_field("#telneturl")->value);
	}
	else if (!strcmp(exec, "tclose"))
		telnet_close(args);
	else if (!strcmp(exec, "tel"))
		telnet_write(args);
	else if (!strcmp(exec, "txpitch")){
		if (strlen(args)){
			int t = atoi(args);	
			if (t > 100 && t < 4000)
				set_field("#tx_pitch", args);
			else
				write_console(FONT_LOG, "cw pitch should be 100-4000");
		}
		char buff[100];
		sprintf(buff, "txpitch is set to %d Hz\n", get_cw_tx_pitch());
		write_console(FONT_LOG, buff);
	}
/*	else if (!strcmp(exec, "PITCH")){
		struct field *f = get_field_by_label(exec);
		field_set("PITCH", args);
		focus_field(f);
	}
*/
	
	else if (exec[0] == 'F' && isdigit(exec[1])){
		char buff[1000];
		printf("executing macro %s\n", exec);
		do_macro(get_field_by_label(exec), NULL, GDK_BUTTON_PRESS, 0, 0, 0);
		//macro_exec(atoi(exec+1), buff);
		//if (strlen(buff))
		//	set_field("#text_in", buff);
	}
	else {
		char field_name[32];
		//conver the string to upper if not already so
		for (char *p = exec; *p; p++)
			*p =  toupper(*p);
		struct field *f = get_field_by_label(exec);
		if (f){
			//convert all the letters to uppercase
			for(char *p = args; *p; p++)
				*p = toupper(*p);
			if(set_field(f->cmd, args)){
				write_console(FONT_LOG, "Invalid setting:");
      } else {
				//this is an extract from focus_field()
				//it shifts the focus to the updated field
				//without toggling/jumping the value 
				struct field *prev_hover = f_hover;
				struct field *prev_focus = f_focus;
				f_focus = NULL;
				f_focus = f_hover = f;
				focus_since = millis();
				update_field(f_hover);
      }
		}
	}
	save_user_settings(0);
}

// From https://stackoverflow.com/questions/5339200/how-to-create-a-single-instance-application-in-c-or-c
void ensure_single_instance(){
	int pid_file = open("/tmp/sbitx.pid", O_CREAT | O_RDWR, 0666);
	int rc = flock(pid_file, LOCK_EX | LOCK_NB);
	if(rc) {
    if(EWOULDBLOCK == errno){
			printf("Another instance of sbitx is already running\n");
			exit(0);
		}	
	}
}

int main( int argc, char* argv[] ) {

	puts(VER_STR);
	printf("Custom build compiled %s %s (colour meters, red/blue trace, NRVL MMSE)\n", __DATE__, __TIME__);
	active_layout = main_controls;

//	ensure_single_instance();

	//unlink any pending ft8 transmission
	unlink("/home/pi/sbitx/ft8tx_float.raw");
	call_wipe();
	
	ui_init(argc, argv);
	hw_init();
	console_init();

	q_init(&q_remote_commands, 1000); //not too many commands
	q_init(&q_tx_text, 100); //best not to have a very large q 
	setup();

	rtc_sync();


	struct field *f;
	f = active_layout;

	//initialize the modulation display

	tx_mod_max = get_field("spectrum")->width;
	tx_mod_buff = malloc(sizeof(int32_t) * tx_mod_max);
	memset(tx_mod_buff, 0, sizeof(int32_t) * tx_mod_max);
	tx_mod_index = 0;
	init_waterfall();

	//set the radio to some decent defaults
	do_control_action("FREQ 7100000");
	do_control_action("MODE LSB");	
	do_control_action("STEP 1K");	
  do_control_action("SPAN 25K");

	strcpy(vfo_a_mode, "USB");
	strcpy(vfo_b_mode, "LSB");
	set_field("#mycallsign", "VU2LCH");
	//vfo_a_freq = 14000000;
	//vfo_b_freq = 7000000;
	
	f = get_field("spectrum");
	update_field(f);
	set_volume(20000000);

	set_field("r1:freq", "7000000");
	set_field("r1:mode", "USB");
	set_field("tx_gain", "24");
	set_field("tx_power", "40");
	set_field("r1:gain", "41");
	set_field("r1:volume", "85");

	char directory[200];	//dangerous, find the MAX_PATH and replace 200 with it
	char *path = getenv("HOME");
	strcpy(directory, path);
	strcat(directory, "/sbitx/data/user_settings.ini");
  if (ini_parse(directory, user_settings_handler, NULL)<0){
    printf("Unable to load ~/sbitx/data/user_settings.ini\n"
		"Loading default.ini instead\n");
		strcpy(directory, path);
		strcat(directory, "/sbitx/data/default_settings.ini");
  	ini_parse(directory, user_settings_handler, NULL);
  }

	sync_dsp_levels();

	//the logger fields may have an unfinished qso details
	call_wipe();

	if (strlen(get_field("#current_macro")->value))
		macro_load(get_field("#current_macro")->value, NULL);

	char buff[1000];

	//now set the frequency of operation and more to vfo_a
  set_field("r1:freq", get_field("#vfo_a_freq")->value);

	console_init();
	write_console(FONT_LOG, VER_STR);
  write_console(FONT_LOG, "\r\nEnter \\help for help\r\n");

	if (strcmp(get_field("#mycallsign")->value, "NOBODY")){
		sprintf(buff, "\nWelcome %s\nYour grid is %s\n", 
		get_field("#mycallsign")->value, get_field("#mygrid")->value);
		write_console(FONT_LOG, buff);
	}
	else 
		write_console(FONT_LOG, "Set your callsign with '\\callsign [yourcallsign]'\n"
		"Set your 6 letter grid with '\\grid [yourgrid]\n");

	set_field("#text_in", "");
	field_set("REC", "OFF");
	field_set("KBD", "OFF");

	// you don't want to save the recently loaded settings
	settings_updated = 0;
  hamlib_start();
	remote_start();

	rtc_read();

//	open_url("http://127.0.0.1:8080");
//	execute_app("chromium-browser --log-leve=3 "
//	"--enable-features=OverlayScrollbar http://127.0.0.1:8080"
//	"  &>/dev/null &");
  gtk_main();
  
  return 0;
}

