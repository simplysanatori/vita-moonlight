#pragma once

#include <stdbool.h>
#include <vita2d.h>

#include <psp2/touch.h>

// Ends the frame the menu is drawing and shows it. A caller about to hand vita2d
// to the stream (resuming it) does this first, so the stream's frames never
// start inside the menu's.
void ui_end();

#define WIDTH 960
#define HEIGHT 544

#define ICON_LEFT_ARROW   "\xef\x95\x8c"
#define ICON_RIGHT_ARROW  "\xef\x95\x93"
#define ICON_LEFT_RIGHT_ARROWS ICON_LEFT_ARROW ICON_RIGHT_ARROW
#define ICON_NETWORK      "\xef\xa6\x8c"
#define ICON_SPRINTING    "\xeF\xa5\xaD"


// What a row's state means; the UI decides how to show it
typedef enum {
  MENU_STATUS_NONE,
  MENU_STATUS_WARNING,  // something to look at, such as Bluetooth being on
  MENU_STATUS_ONLINE,   // a PC that answers
  MENU_STATUS_OFFLINE,  // a PC that doesn't
  MENU_STATUS_CHANGED,  // a PC that answers at a new address
} menu_status;

// One row of a menu: what it says and what state it's in, not how it looks
typedef struct menu_entry {
  int id;
  char *name;
  char *suffix;          // extra text on the right, such as an address
  char subname[1024];    // the current value, on the right
  bool disabled, separator;
  bool adjustable;       // left and right change the value
  bool checked;          // the current choice among options
  menu_status status;
} menu_entry;

typedef struct menu_geom {
  int x, y, width, height, el, total_y;
} menu_geom;

// Vita control symbols, drawn as shapes because the UI font has no glyphs for them
enum {
  // Face buttons, in their usual colours
  BUTTON_GLYPH_CROSS,
  BUTTON_GLYPH_CIRCLE,
  BUTTON_GLYPH_TRIANGLE,
  BUTTON_GLYPH_SQUARE,
  // D-pad, with the named direction highlighted, or all four for BUTTON_GLYPH_DPAD
  BUTTON_GLYPH_DPAD,
  BUTTON_GLYPH_DPAD_UP,
  BUTTON_GLYPH_DPAD_DOWN,
  BUTTON_GLYPH_DPAD_LEFT,
  BUTTON_GLYPH_DPAD_RIGHT,
  // Labelled buttons; these are wider than they are tall
  BUTTON_GLYPH_L,
  BUTTON_GLYPH_R,
  BUTTON_GLYPH_L2,
  BUTTON_GLYPH_R2,
  BUTTON_GLYPH_START,
  BUTTON_GLYPH_SELECT,
  // Round controls
  BUTTON_GLYPH_PS,
  BUTTON_GLYPH_LEFT_STICK,
  BUTTON_GLYPH_RIGHT_STICK,
  BUTTON_GLYPH_L3,
  BUTTON_GLYPH_R3,
  // Touch surfaces
  BUTTON_GLYPH_FRONT_TOUCH,
  BUTTON_GLYPH_REAR_TOUCH,
  BUTTON_GLYPH_COUNT
};

// Longest line wrap_text() produces, in bytes
#define WRAP_LINE_MAX 256

extern vita2d_font *font;

// Every menu row is this tall, and no menu box is taller than the default one
#define MENU_ROW_HEIGHT 24
#define MENU_MAX_HEIGHT 400

struct menu_geom make_geom_centered(int w, int h);
struct menu_geom make_geom_fitted(int w, int rows);

#define SCE_CTRL_HOLD 0x80000000

// What a press means, whatever the buttons: confirm and cancel follow Swap X & O,
// and L and R arrive on either of their bits. Screens act on these, so the
// controls are decided in one place and any UI can drive the same screens.
enum {
  UI_INTENT_UP        = 1u << 0,
  UI_INTENT_DOWN      = 1u << 1,
  UI_INTENT_LEFT      = 1u << 2,
  UI_INTENT_RIGHT     = 1u << 3,
  UI_INTENT_CONFIRM   = 1u << 4,
  UI_INTENT_CANCEL    = 1u << 5,
  UI_INTENT_HELP      = 1u << 6,  // triangle
  UI_INTENT_SECONDARY = 1u << 7,  // square
  UI_INTENT_TAB_PREV  = 1u << 8,  // L
  UI_INTENT_TAB_NEXT  = 1u << 9,  // R
  UI_INTENT_START     = 1u << 10,
  UI_INTENT_SELECT    = 1u << 11,
  // Set when the intents repeat because the buttons are being held
  UI_INTENT_REPEAT    = 1u << 31,
};

typedef struct input_data {
  int buttons;           // raw, for the rare check that needs a particular button
  unsigned int intents;  // UI_INTENT_* flags
  SceTouchData touch;
} input_data;

// Reads this frame's buttons and touch, with their intents
input_data read_input(void);

// Pressed this frame, and not repeating from being held: for actions
static inline bool ui_pressed(const input_data *input, unsigned int intent) {
  return (input->intents & intent) != 0 && (input->intents & UI_INTENT_REPEAT) == 0;
}

// Pressed, or repeating while held: for moving through lists and values
static inline bool ui_held(const input_data *input, unsigned int intent) {
  return (input->intents & intent) != 0;
}

typedef int (*gui_loop_callback) (int, void *, const input_data *);
typedef int (*gui_back_callback) (void *);
typedef void (*gui_draw_callback) (void);

bool was_button_pressed(short id);
bool is_button_down(short id);
bool is_rectangle_touched(const SceTouchData *touch, int lx, int ly, int rx, int ry);

int display_menu(
        menu_entry menu[],
        int total_elements,
        menu_geom *geom_ptr,
        gui_loop_callback loop_callback,
        gui_back_callback back_callback,
        gui_draw_callback draw_callback,
        void *context
        );

void display_alert(
        char *message,
        char *button_captions[],
        int buttons_count,
        gui_loop_callback cb,
        void *context
        );

void display_error(char *format, ...);
// Shows a larger dialog with a title and explanatory text, closed with
// confirm, cancel or triangle
void display_help(const char *title, const char *message);

// Draws a control symbol size pixels tall whose bottom sits on the text
// baseline y, and returns its width (labelled buttons are wider than size)
int draw_button_glyph(int glyph, int x, int y, int size);

// Splits text into lines no wider than max_width at the UI font size, breaking
// at spaces and newlines. Returns the number of lines written to lines.
int wrap_text(const char *text, int max_width, char lines[][WRAP_LINE_MAX], int max_lines);

void flash_message(char *format, ...);

void guilib_init(gui_loop_callback global_loop_cb, gui_draw_callback global_draw_cb);

// Declaración de display_confirm para uso global
int display_confirm(const char* message);
