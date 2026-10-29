#include "ui_settings.h"

#include "guilib.h"
#include "ime.h"
#include "ui_keyboard.h"

#include "../config.h"
#include "../input/vita.h"
#include "../input/swap_shoulder_buttons.h"
#include "../video/vita.h"
#include "../debug.h"
#include "../stats.h"
#include "../connection.h"
#include "../bench.h"
#include "ui_connect.h"
#include "../input/touchabsolute.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <psp2/ctrl.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include <psp2/videodec.h>
#include <vita2d.h>
#include <Limelight.h>
#include "debug.h"
#include "../util.h"
#include "ui_recommended.h"
#include "../settings.h"
extern char* strdup(const char*);

static unsigned int settings_special_codes[] = {0,
  // special
  INPUT_TYPE_DEF_NAME | INPUT_TYPE_SPECIAL,
  INPUT_SPECIAL_KEY_PAUSE | INPUT_TYPE_SPECIAL,
  INPUT_SPECIAL_KEY_KEYBOARD | INPUT_TYPE_SPECIAL,
  // gamepad
  INPUT_TYPE_DEF_NAME | INPUT_TYPE_GAMEPAD,
  SPECIAL_FLAG | INPUT_TYPE_GAMEPAD,
  LB_FLAG | INPUT_TYPE_GAMEPAD,
  RB_FLAG | INPUT_TYPE_GAMEPAD,
  LS_CLK_FLAG | INPUT_TYPE_GAMEPAD,
  RS_CLK_FLAG | INPUT_TYPE_GAMEPAD,
  LEFT_TRIGGER | INPUT_TYPE_ANALOG,
  RIGHT_TRIGGER | INPUT_TYPE_ANALOG,
  // mouse
  INPUT_TYPE_DEF_NAME | INPUT_TYPE_MOUSE,
  BUTTON_LEFT | INPUT_TYPE_MOUSE,
  BUTTON_RIGHT | INPUT_TYPE_MOUSE,
  BUTTON_MIDDLE | INPUT_TYPE_MOUSE,
  BUTTON_X1 | INPUT_TYPE_MOUSE,
  BUTTON_X2 | INPUT_TYPE_MOUSE,
  // keyboard
  INPUT_TYPE_DEF_NAME | INPUT_TYPE_KEYBOARD,
  27,    73,  77,  9,
  112,  113,  114,  115,  116,  117,  118,  119, 120,   121,   122,   123
};

// settings_special_names debe ser visible globalmente para sizeof
char *settings_special_names[] = {
  "None",
  // special
  "Special inputs",
  "Pause stream",
  "Open keyboard",
  // gamepad
  "Gamepad buttons",
  "Special (XBox button)",
  "LB", "RB", "LS", "RS", "LT", "RT",
  // mouse
  "Mouse buttons",
  "Left",
  "Right",
  "Middle(wheel)",
  "X1(4th)",
  "X2(5th)",
  // keyboard
  "Keyboard input codes",
  "Esc",
  "I", "M", "Tab",
  "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"
};

/*
 * Deadzone
 */

#define lerp(value, from_max, to_max) ((((value*10) * (to_max*10))/(from_max*10))/10)
static void deadzone_draw() {
  int vertical = (WIDTH - config.back_deadzone.left - config.back_deadzone.right) / 2 + config.back_deadzone.left,
      horizontal = (HEIGHT - config.back_deadzone.top - config.back_deadzone.bottom) / 2 + config.back_deadzone.top;

  vita2d_draw_rectangle(
      config.back_deadzone.left,
      config.back_deadzone.top,
      WIDTH - config.back_deadzone.right - config.back_deadzone.left,
      HEIGHT - config.back_deadzone.bottom - config.back_deadzone.top,
      0x3000ff00
      );

  vita2d_draw_line(vertical, config.back_deadzone.top, vertical, HEIGHT - config.back_deadzone.bottom, 0xffffffff);
  vita2d_draw_line(config.back_deadzone.left, horizontal, WIDTH - config.back_deadzone.right, horizontal, 0xffffffff);

  SceTouchData touch_data;
  sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch_data, 1);

  for (int i = 0; i < touch_data.reportNum; i++) {
    int x = lerp(touch_data.report[i].x, 1919, 960);
    int y = lerp(touch_data.report[i].y, 1087, 544);
    if (x < config.back_deadzone.left || x > WIDTH - config.back_deadzone.right)
      continue;

    if (y < config.back_deadzone.top || y > HEIGHT - config.back_deadzone.bottom)
      continue;

    vita2d_draw_fill_circle(x, y, 30, 0xffffffff);
  }
}

static int deadzone_loop(int cursor, void *context, const input_data *input) {
  menu_entry *menu = context;

  bool left = ui_held(input, UI_INTENT_LEFT);
  bool right = ui_held(input, UI_INTENT_RIGHT);

  int delta = left ? -15 : (right ? 15 : 0);
  switch (cursor) {
    case 0: config.back_deadzone.top += delta; break;
    case 1: config.back_deadzone.right += delta; break;
    case 2: config.back_deadzone.bottom += delta; break;
    case 3: config.back_deadzone.left += delta; break;
  }

  char current[256];

  int numbers[] = {
    config.back_deadzone.top,
    config.back_deadzone.right,
    config.back_deadzone.bottom,
    config.back_deadzone.left
  };
  for (int i = 0; i < 4; i++) {
    sprintf(current, "%dpx", numbers[i]);
    strcpy(menu[i].subname, current);
  }

  return 0;
}

static int deadzone_settings_menu() {
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

  menu_entry menu[16];
  int idx = 0;
  // In the order deadzone_loop adjusts them (it used to label 1 and 3 the wrong way round)
  menu[idx++] = (menu_entry) { .name = "Top: ", .disabled = false, .id = 0, .adjustable = true };
  menu[idx++] = (menu_entry) { .name = "Right: ", .disabled = false, .id = 1, .adjustable = true };
  menu[idx++] = (menu_entry) { .name = "Bottom: ", .disabled = false, .id = 2, .adjustable = true };
  menu[idx++] = (menu_entry) { .name = "Left: ", .disabled = false, .id = 3, .adjustable = true };

  menu_geom geom = make_geom_centered(250, 120);
  geom.x = 50;
  geom.y = 50;
  geom.el = 25;
  return display_menu(menu, idx, &geom, &deadzone_loop, NULL, &deadzone_draw, &menu);
}

/*
 * Special keys
 */

static int special_keys_ord(char *text) {
  bool did_find = false;
  int i = 0;
  for (; i < sizeof(settings_special_codes) / sizeof(int); i++) {
    if (strcmp(settings_special_names[i], text) == 0) {
      did_find = true;
      break;
    }
  }

  if (did_find) {
    return settings_special_codes[i];
  } else {
    return 0;
  }
}

static void special_keys_name(int ord, char *text) {
  if (ord == 0) {
    strcpy(text, "None");
    return;
  }

  bool did_find = false;
  int i = 0;
  for (; i < sizeof(settings_special_codes) / sizeof(int); i++) {
    if (settings_special_codes[i] == ord) {
      did_find = true;
      break;
    }
  }

  if (did_find) {
    strcpy(text, settings_special_names[i]);
  } else {
    sprintf(text, "%d", ord);
  }
}

enum {
  SETTINGS_SELECT_SPECIAL_KEY_MANUAL = -1000
};

enum {
  SETTINGS_SPECIAL_KEYS_OFFSET,
  SETTINGS_SPECIAL_KEYS_SIZE
};

enum {
  SETTINGS_SPECIAL_KEYS_NW_VIEW = 3
};

static int select_special_key_loop(int id, void *context, const input_data *input) {
  int *code = context;

  if (!ui_pressed(input, UI_INTENT_CONFIRM)) {
    return 0;
  }
  if (id != SETTINGS_SELECT_SPECIAL_KEY_MANUAL) {
    *code = id;
    return 1;
  }
  char key_code_value[512];
  if (ime_dialog_number(key_code_value, "Enter key code:", "") == 0) {
      int key_code = atoi(key_code_value);
      if (key_code) {
        *code = key_code;
        return 1;
      } else {
        display_error("Incorrect key code entered: %s", key_code_value);
      }
  }

  return 0;
}

static int select_special_key_menu(int *code) {
  // TODO: sizeof(codes) / sizeof(int) ?
  menu_entry menu[64];
  int idx = 0;
  for (int i = 0; i < sizeof(settings_special_codes) / sizeof(int); i++) {
    unsigned int id = settings_special_codes[i];
    menu[idx++] = (menu_entry) {
      .id = id,
      .name = malloc(sizeof(char) * 256),
      .disabled = (id >= INPUT_TYPE_DEF_NAME)
    };
    special_keys_name(id, menu[idx-1].name);
  }

  menu[idx++] = (menu_entry) { .name = "", .disabled = true, .separator = true };
  menu[idx++] = (menu_entry) { .name = "Enter manually ...", .id = SETTINGS_SELECT_SPECIAL_KEY_MANUAL };

  int return_code = display_menu(menu, idx, NULL, &select_special_key_loop, NULL, NULL, code);
  for (int i = 0; i < sizeof(settings_special_codes) / sizeof(int); i++) {
    free(menu[i].name);
  }

  return return_code;
}

static int special_keys_loop(int id, void *context, const input_data *input) {
  bool left = ui_held(input, UI_INTENT_LEFT);
  bool right = ui_held(input, UI_INTENT_RIGHT);
  int selected_ord = -1;
  int delta = left ? -15 : (right ? 15 : 0);

  switch (id) {
    case SETTINGS_SPECIAL_KEYS_OFFSET:
      config.special_keys.offset += delta;
      break;
    case SETTINGS_SPECIAL_KEYS_SIZE:
      config.special_keys.size += delta;
      break;
    default:
      if (!ui_pressed(input, UI_INTENT_CONFIRM)) {
        break;
      }
      select_special_key_menu(&selected_ord);
      if (selected_ord != -1) {
        switch (id) {
          case TOUCHSEC_SPECIAL_NW:
            config.special_keys.nw = selected_ord;
            break;
          case TOUCHSEC_SPECIAL_NE:
            config.special_keys.ne = selected_ord;
            break;
          case TOUCHSEC_SPECIAL_SW:
            config.special_keys.sw = selected_ord;
            break;
          case TOUCHSEC_SPECIAL_SE:
            config.special_keys.se = selected_ord;
            break;
        }
      }
      break;
  }

  menu_entry *menu = context;

  int idx = 0;
  sprintf(menu[idx++].subname, "%d", config.special_keys.offset);
  sprintf(menu[idx++].subname, "%d", config.special_keys.size);

  idx++;
  special_keys_name(config.special_keys.nw, menu[idx++].subname);
  special_keys_name(config.special_keys.ne, menu[idx++].subname);
  special_keys_name(config.special_keys.sw, menu[idx++].subname);
  special_keys_name(config.special_keys.se, menu[idx++].subname);

  return 0;
}

static void special_keys_draw() {
  int special_offset = config.special_keys.offset,
      special_size = config.special_keys.size;

  unsigned int color = 0xff006000;

  for (int i = TOUCHSEC_SPECIAL_NW; i <= TOUCHSEC_SPECIAL_SE; i++) {
    switch (i) {
      case TOUCHSEC_SPECIAL_SW:
        vita2d_draw_rectangle(
            special_offset,
            HEIGHT - special_size - special_offset,
            special_size,
            special_size,
            color);
        // fallthrough
      case TOUCHSEC_SPECIAL_SE:
        vita2d_draw_rectangle(
            WIDTH - special_size - special_offset,
            HEIGHT - special_size - special_offset,
            special_size,
            special_size,
            color);
        // fallthrough
      case TOUCHSEC_SPECIAL_NW:
        vita2d_draw_rectangle(
            special_offset,
            special_offset,
            special_size,
            special_size,
            color);
        // fallthrough
      case TOUCHSEC_SPECIAL_NE:
        vita2d_draw_rectangle(
            WIDTH - special_size - special_offset,
            special_offset,
            special_size,
            special_size,
            color);
    }
  }

}

static int special_keys_menu() {
  menu_entry menu[16];
  int idx = 0;

  menu[idx++] = (menu_entry) { .name = "Offset", .id = SETTINGS_SPECIAL_KEYS_OFFSET };
  menu[idx++] = (menu_entry) { .name = "Size", .id = SETTINGS_SPECIAL_KEYS_SIZE };
  menu[idx++] = (menu_entry) { .name = "Assignments", .disabled = true, .separator = false };
  menu[idx++] = (menu_entry) { .name = "Top left", .id = TOUCHSEC_SPECIAL_NW };
  menu[idx++] = (menu_entry) { .name = "Top right", .id = TOUCHSEC_SPECIAL_NE };
  menu[idx++] = (menu_entry) { .name = "Bottom left", .id = TOUCHSEC_SPECIAL_SW };
  menu[idx++] = (menu_entry) { .name = "Bottom right", .id = TOUCHSEC_SPECIAL_SE };

  return display_menu(menu, idx, NULL, &special_keys_loop, NULL, &special_keys_draw, &menu);
}



// --- Controller Type Selection ---
/*
 * The Settings screen, listing the settings described in src/settings.c
 */

#define BITRATE_CUSTOM_MIN_KBPS 500
#define BITRATE_CUSTOM_MAX_KBPS 100000

static int settings_back(void *context);

// Returned by settings_loop to rebuild the rows, for example after another
// tab is chosen or developer options are shown or hidden
#define SETTINGS_REBUILD 0x42

static int settings_tab = SETTINGS_TAB_STREAM;

// Must hold the most rows any tab lists (about 18 today). Each is about 1 KB
// of the main thread stack, so don't oversize it.
#define SETTINGS_MAX_ROWS 32

// The rows on screen, which use their setting's index in SETTINGS as their id
static menu_entry *settings_rows = NULL;
static int settings_row_count = 0;

static void move_tab(int step) {
  do {
    settings_tab = (settings_tab + step + SETTINGS_TAB_COUNT) % SETTINGS_TAB_COUNT;
  } while (!settings_tab_shown(settings_tab));
}

// Fills in every row's value and whether it can be selected, from the settings
static void refresh_rows(void) {
  for (int i = 0; i < settings_row_count; i++) {
    menu_entry *row = &settings_rows[i];
    const setting_def *setting = &SETTINGS[row->id];
    setting_value(setting, row->subname, sizeof(row->subname));
    bool selectable = setting->kind != SETTING_NOTE && setting->kind != SETTING_HEADER;
    row->disabled = !selectable || (setting->available != NULL && !setting->available());
  }
}

static void show_settings_help(const setting_def *setting) {
  char text[1024];
  setting_help(setting, text, sizeof(text));
  display_help(setting->label, text);
}

// Asks for a whole number, returning false if the player cancels
static bool ask_number(const char *title, int current, int *result) {
  char value[512];
  char initial[16];
  snprintf(initial, sizeof(initial), "%d", current);
  if (ime_dialog_number(value, title, initial) != 0) {
    return false;
  }
  *result = atoi(value);
  return true;
}

// Carries out an action row. Returns 0, SETTINGS_REBUILD, or
// SETTINGS_RESTART_STREAM.
static int run_action(const setting_def *setting) {
  int number;
  switch (setting->action) {
    case SETTING_ACTION_RECOMMENDED: {
      // Rebuilt whatever happened: the values may have changed, and
      // auto-detect may have streamed, as a benchmark run does
      int bitrate = config.stream.bitrate;
      ui_recommended_settings_menu();
      if (config.stream.bitrate != bitrate) {
        // A preset was applied: show it on its step rather than as Custom
        settings_sync_bitrate();
      }
      if (ui_recommended_take_restart()) {
        // Save and leave, so whoever opened Settings restarts the stream
        settings_back(NULL);
        return SETTINGS_RESTART_STREAM;
      }
      return SETTINGS_REBUILD;
    }
    case SETTING_ACTION_CUSTOM_BITRATE:
      if (ask_number("Bitrate in kbps (10000 = 10 Mbps)", config.stream.bitrate, &number)) {
        if (number >= BITRATE_CUSTOM_MIN_KBPS && number <= BITRATE_CUSTOM_MAX_KBPS) {
          config.stream.bitrate = number;
        } else {
          display_error("Enter a bitrate from %d to %d kbps.", BITRATE_CUSTOM_MIN_KBPS, BITRATE_CUSTOM_MAX_KBPS);
        }
      }
      return 0;
    case SETTING_ACTION_SPRINT_TIME: {
      char value[512];
      if (ime_dialog_number(value, "Enter Sprint Step Time in Milliseconds", "") == 0) {
        int step_time = atoi(value);
        if (step_time) {
          config.double_tap_sprint_step_time = step_time;
        } else {
          display_error("Incorrect step time entered: %s", value);
        }
      }
      return 0;
    }
    case SETTING_ACTION_BACK_DEADZONE:
      deadzone_settings_menu();
      return 0;
    case SETTING_ACTION_SPECIAL_KEYS:
      special_keys_menu();
      return 0;
    case SETTING_ACTION_KEYBOARD_LAYOUT:
      keyboard_layout_menu();
      return 0;
    case SETTING_ACTION_RUN_BENCHMARK:
      ui_settings_save_config();
      vitabench_run(false);
      return SETTINGS_REBUILD;
    case SETTING_ACTION_HIDE_DEVELOPER:
      config.developer_mode = false;
      ui_settings_save_config();
      return SETTINGS_REBUILD;
    case SETTING_ACTION_SHOW_HELP:
      show_settings_help(setting);
      return 0;
    case SETTING_ACTION_NONE:
      break;
  }
  return 0;
}

// Drawn around the settings box: the tabs above it, and the help hint below,
// so the help is discoverable without taking a row
static void settings_draw(void) {
  const char *hint = "Help";
  const int glyph_size = 14;
  const int gap = 6;

  // The tab bar, above the box on the left, level with the clock on the right
  const int tab_spacing = 16;
  int tab_x = 480 - 300;
  int tab_y = 272 - 200 - 10;
  tab_x += draw_button_glyph(BUTTON_GLYPH_L, tab_x, tab_y, glyph_size) + gap * 2;
  for (int tab = 0; tab < SETTINGS_TAB_COUNT; tab++) {
    if (!settings_tab_shown(tab)) {
      continue;
    }
    unsigned int colour = tab == settings_tab ? 0xff00ff00 : 0xffaaaaaa;
    vita2d_font_draw_text(font, tab_x, tab_y, colour, 18, SETTINGS_TAB_NAMES[tab]);
    tab_x += vita2d_font_text_width(font, 18, SETTINGS_TAB_NAMES[tab]) + tab_spacing;
  }
  draw_button_glyph(BUTTON_GLYPH_R, tab_x - tab_spacing + gap * 2, tab_y, glyph_size);

  int width = glyph_size + gap + vita2d_font_text_width(font, 18, hint);
  // The settings menu uses the default 600x400 box, centred on the 960x544 screen
  int x = 480 + 300 - width;
  int y = 272 + 200 + 26;
  x += draw_button_glyph(BUTTON_GLYPH_TRIANGLE, x, y, glyph_size) + gap;
  vita2d_font_draw_text(font, x, y, 0xffffffff, 18, hint);
}

static int settings_loop(int id, void *context, const input_data *input) {
  const setting_def *setting = &SETTINGS[id];

  if (ui_pressed(input, UI_INTENT_HELP)) {
    // read_buttons() only reports newly pressed buttons, so check L and R are held directly
    SceCtrlData pad = {0};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    // The menus' sceCtrlPeekBufferPositive reports the shoulder buttons on the
    // L2/R2 bits (the old LTRIGGER/RTRIGGER names), so accept either pair
    if ((pad.buttons & (SCE_CTRL_L1 | SCE_CTRL_L2)) && (pad.buttons & (SCE_CTRL_R1 | SCE_CTRL_R2))) {
      config.developer_mode = !config.developer_mode;
      ui_settings_save_config();
      display_error(config.developer_mode
                    ? "Developer options are now shown, in the Developer tab."
                    : "Developer options are hidden.");
      settings_tab = config.developer_mode ? SETTINGS_TAB_DEVELOPER : SETTINGS_TAB_STREAM;
      return SETTINGS_REBUILD;
    }
    show_settings_help(setting);
    return 0;
  }

  // L and R switch tabs, after triangle so the developer combination (L + R +
  // triangle) wins when R and triangle land in the same frame. Pressing both
  // shoulders together moves back and forth and lands on the same tab.
  bool left_tab = ui_pressed(input, UI_INTENT_TAB_PREV);
  bool right_tab = ui_pressed(input, UI_INTENT_TAB_NEXT);
  if (left_tab != right_tab) {
    move_tab(left_tab ? -1 : 1);
    return SETTINGS_REBUILD;
  }

  bool confirm = ui_pressed(input, UI_INTENT_CONFIRM);
  bool changed = false;
  switch (setting->kind) {
    case SETTING_TOGGLE:
      changed = confirm && setting->change(1);
      break;
    case SETTING_CHOICE: {
      bool repeats = (setting->flags & SETTING_HELD_REPEATS) != 0;
      bool left = repeats ? ui_held(input, UI_INTENT_LEFT) : ui_pressed(input, UI_INTENT_LEFT);
      bool right = repeats ? ui_held(input, UI_INTENT_RIGHT) : ui_pressed(input, UI_INTENT_RIGHT);
      right = right || (confirm && (setting->flags & SETTING_CONFIRM_STEPS));
      if (left != right) {
        changed = setting->change(left ? -1 : 1);
      }
      break;
    }
    case SETTING_ACTION:
      if (confirm) {
        int ret = run_action(setting);
        if (ret) {
          return ret;
        }
        changed = true;
      }
      break;
    case SETTING_INFO:
    case SETTING_NOTE:
    case SETTING_HEADER:
      break;
  }

  if (changed) {
    if (setting->flags & SETTING_SAVE_AT_ONCE) {
      ui_settings_save_config();
    }
    refresh_rows();
  }
  return 0;
}

static int settings_back(void *context) {
  ui_settings_save_config();
  update_layout();
  return 0;
}

int ui_settings_menu() {
  menu_entry menu[SETTINGS_MAX_ROWS];
  settings_rows = menu;
  // A fresh visit opens on the first tab, and on Custom bitrate when the
  // saved value isn't a step; rebuilds stay as they are
  settings_tab = SETTINGS_TAB_STREAM;
  settings_sync_bitrate();

  int ret;
  do {
    if (!settings_tab_shown(settings_tab)) {
      settings_tab = SETTINGS_TAB_STREAM;
    }
    // The rows for this tab, in the table's order
    settings_row_count = 0;
    for (int i = 0; i < SETTINGS_COUNT; i++) {
      const setting_def *setting = &SETTINGS[i];
      if (!setting_listed(setting, settings_tab)) {
        continue;
      }
      assert(settings_row_count < SETTINGS_MAX_ROWS);
      menu[settings_row_count++] = (menu_entry) {
        .id = i,
        .name = (char *)setting->label,
        .separator = setting->kind == SETTING_HEADER,
        .adjustable = setting->kind == SETTING_CHOICE,
        .status = (setting->flags & SETTING_WARNING) ? MENU_STATUS_WARNING : MENU_STATUS_NONE,
      };
    }
    refresh_rows();
    ret = display_menu(menu, settings_row_count, NULL, &settings_loop, &settings_back, &settings_draw, NULL);
  } while (ret == SETTINGS_REBUILD);
  settings_rows = NULL;
  settings_row_count = 0;
  return ret;
}

void ui_settings_save_config() {
  vita_debug_log("[DEBUG] Guardando configuración:");
  vita_debug_log("  touchscreen_mode = %d", config.touchscreen_mode);
  vita_debug_log("  swap_shoulder_buttons = %d", config.swap_shoulder_buttons);
  vita_debug_log("  controller_type = %d", config.controller_type);
  vita_debug_log("  enable_mapping = %s", config.mapping ? "yes" : "no");
  vita_debug_log("  stats_overlay = %d", config.stats_overlay);
  vita_debug_log("  enable_motion_controls = %d", config.enable_motion_controls);
  vita_debug_log("  double_tap_sprint = %d", config.enable_double_tap_sprint);
  vita_debug_log("  double_tap_sprint_step_time = %u", config.double_tap_sprint_step_time);
  vita_debug_log("  mouse_acceleration = %d", config.mouse_acceleration);
  vita_debug_log("  stream.width = %d", config.stream.width);
  vita_debug_log("  stream.height = %d", config.stream.height);
  vita_debug_log("  stream.fps = %d", config.stream.fps);
  vita_debug_log("  stream.bitrate = %d", config.stream.bitrate);
  vita_debug_log("  save_debug_log = %d", config.save_debug_log);
  vita_debug_log("  disable_powersave = %d", config.disable_powersave);
  vita_debug_log("  boost_clocks = %d", config.boost_clocks);
  vita_debug_log("  pin_threads = %d", config.pin_threads);
  vita_debug_log("  jp_layout = %d", config.jp_layout);
  vita_debug_log("  localaudio = %d", config.localaudio);
  vita_debug_log("  audio_packet_ms = %d", config.audio_packet_ms);
  vita_debug_log("  enable_frame_pacer = %d", config.enable_frame_pacer);
  vita_debug_log("  frame_pacing = %d", config.frame_pacing);
  vita_debug_log("  center_region_only = %d", config.center_region_only);
  vita_debug_log("  enable_ref_frame_invalidation = %d", config.enable_ref_frame_invalidation);
  vita_debug_log("  stream.streamingRemotely = %d", config.stream.streamingRemotely);
  vita_debug_log("  enable_vita_vblank_wait = %d", config.enable_vita_vblank_wait);
  if (!config_save(config_path, &config)) {
    display_error("Couldn't save the settings. Is the memory card full? They apply until Moonlight closes.");
    return;
  }
  vita_debug_log("[DEBUG] Configuración guardada en %s", config_path);
}

