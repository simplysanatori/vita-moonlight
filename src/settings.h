#pragma once

#include <stdbool.h>
#include <stddef.h>

// Every setting in the Settings screen, described once: where it's listed,
// what it's called, its help, how its value reads and how it changes. This
// is data, with no drawing or dialogs, so any UI can list and change the
// settings the same way (src/gui/ui_settings.c is today's).

// Settings is split into tabs
enum {
  SETTINGS_TAB_ALL = -1,  // listed above the tabs, on every one
  SETTINGS_TAB_STREAM,
  SETTINGS_TAB_SYSTEM,
  SETTINGS_TAB_CONTROLS,
  SETTINGS_TAB_DEVELOPER,
  SETTINGS_TAB_ABOUT,
  SETTINGS_TAB_COUNT,
};

typedef enum {
  SETTING_TOGGLE,  // confirm switches it on and off
  SETTING_CHOICE,  // left and right step through the options
  SETTING_ACTION,  // confirm does something, carried out by the UI
  SETTING_INFO,    // a line of information, selectable for its help
  SETTING_NOTE,    // a message that can't be selected
  SETTING_HEADER,  // a heading for the rows below it
} setting_kind;

// Things the UI carries out, since they open dialogs or other screens
typedef enum {
  SETTING_ACTION_NONE,
  SETTING_ACTION_RECOMMENDED,     // the recommended settings screen
  SETTING_ACTION_CUSTOM_BITRATE,  // enter a bitrate in kbps
  SETTING_ACTION_SPRINT_TIME,     // enter the double tap time in ms
  SETTING_ACTION_BACK_DEADZONE,   // the rear touch dead zone screen
  SETTING_ACTION_SPECIAL_KEYS,    // the touch corner buttons screen
  SETTING_ACTION_KEYBOARD_LAYOUT, // the keyboard layout screen
  SETTING_ACTION_RUN_BENCHMARK,
  SETTING_ACTION_HIDE_DEVELOPER,
  SETTING_ACTION_SHOW_HELP,       // nothing to change: confirm explains it
  SETTING_ACTION_WIFITURBO_APPLY, // the Wi-Fi Turbo plugin's settings for streaming, after a confirmation
} setting_action;

enum {
  SETTING_NEEDS_NEW_STREAM = 1 << 0,  // agreed with the PC at stream start, so hidden mid-stream
  SETTING_WARNING          = 1 << 1,  // something the player should look at
  SETTING_CONFIRM_STEPS    = 1 << 2,  // confirm also steps a choice forward
  SETTING_HELD_REPEATS     = 1 << 3,  // holding left or right keeps stepping
  SETTING_SAVE_AT_ONCE     = 1 << 4,  // saved as soon as it changes
};

typedef struct setting_def {
  int tab;
  setting_kind kind;
  unsigned int flags;
  const char *label;
  // Help shown with triangle, or NULL to use help_with() (or for none)
  const char *help;
  void (*help_with)(char *out, size_t size);
  // Whether it's listed now, or NULL for always
  bool (*listed)(void);
  // Whether it can be selected now, or NULL for always
  bool (*available)(void);
  // Toggles: whether it's on, shown as yes or no
  bool (*is_on)(void);
  // Writes the current value, or NULL for none
  void (*value)(char *out, size_t size);
  // A value that never changes, for information rows
  const char *fixed_value;
  // Toggles (step 1) and choices (step -1 or 1). Returns true if it changed.
  bool (*change)(int step);
  setting_action action;
} setting_def;

extern const setting_def SETTINGS[];
extern const int SETTINGS_COUNT;

extern const char *const SETTINGS_TAB_NAMES[SETTINGS_TAB_COUNT];

// Whether a tab is shown: Developer only in developer mode
bool settings_tab_shown(int tab);

// Whether a stream is running or paused, when settings that need a new
// stream are hidden
bool settings_streaming(void);

// Whether the setting is listed now on the given tab (rows on every tab
// included), taking mid-stream and its own condition into account
bool setting_listed(const setting_def *setting, int tab);

// Writes the current value as shown, or an empty string for none
void setting_value(const setting_def *setting, char *out, size_t size);

// Writes the help for a setting
void setting_help(const setting_def *setting, char *out, size_t size);

// The bitrate's Custom state: the Bitrate row is on Custom, which enables
// the Custom bitrate row, whenever the value isn't one of the steps
void settings_sync_bitrate(void);
