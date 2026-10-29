#include "settings.h"

#include "config.h"
#include "connection.h"
#include "stats.h"
#include "util.h"
#include "video/vita.h"
#include "input/touchabsolute.h"
#include "input/swap_shoulder_buttons.h"
#include "session.h"
#include "wifiturbo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/videodec.h>
#include <Limelight.h>

extern char *strdup(const char *);

// Read by the input code, which swaps the shoulder buttons while it's set
bool swap_shoulder_buttons = false;

const char *const SETTINGS_TAB_NAMES[SETTINGS_TAB_COUNT] = {"Stream", "System", "Controls", "Developer", "About"};

bool settings_tab_shown(int tab) {
  return tab != SETTINGS_TAB_DEVELOPER || config.developer_mode;
}

bool settings_streaming(void) {
  int status = connection_get_status();
  return status == LI_CONNECTED || status == LI_MINIMIZED;
}

/*
 * Toggles on a config field
 */

#define TOGGLE(NAME, FIELD) \
  static bool NAME##_on(void) { return (FIELD) != 0; } \
  static bool NAME##_flip(int step) { (void)step; FIELD = !(FIELD); return true; }

TOGGLE(sops, config.sops)
TOGGLE(frame_inval, config.enable_ref_frame_invalidation)
TOGGLE(stream_optimize, config.stream.streamingRemotely)
TOGGLE(vblank_wait, config.enable_vita_vblank_wait)
TOGGLE(fill_screen, config.center_region_only)
TOGGLE(frame_pacer, config.enable_frame_pacer)
TOGGLE(decode_yuv, config.decode_yuv)
TOGGLE(local_audio, config.localaudio)
TOGGLE(debug_log, config.save_debug_log)
TOGGLE(powersave, config.disable_powersave)
TOGGLE(boost_clocks, config.boost_clocks)
TOGGLE(wifiturbo_notices, config.wifiturbo_notices)
TOGGLE(jp_layout, config.jp_layout)
TOGGLE(input_sync, config.input_sync_to_controller)
TOGGLE(motion, config.enable_motion_controls)
TOGGLE(sprint, config.enable_double_tap_sprint)
TOGGLE(psbutton, config.enable_psbutton_capture)
TOGGLE(touch_keys, config.enable_front_touchzones)
TOGGLE(direct_decode, config.direct_decode)

/*
 * Resolution and frame rate
 */

#define MAX_RESOLUTION 9
static const int RESOLUTIONS[MAX_RESOLUTION][2] = {
  {960, 540},   // 16:9, QHD
  {960, 544},   // VITA original
  {1024, 576},  // 16:9
  {1152, 648},  // 16:9
  {1280, 540},  // 21:9
  {1280, 720},  // 16:9, 720p HD
  {1366, 768},  // 16:9, WXGA
  {1600, 900},  // 16:9, 900p HD+
  {1920, 1080}, // 16:9, FHD
};

// The resolutions this Vita's decoder accepts, found the first time they're needed
static int supported_resolutions[MAX_RESOLUTION];
static int supported_resolution_count = 0;

static void find_supported_resolutions(void) {
  if (supported_resolution_count > 0 || vitavideo_initialized()) {
    return;
  }
  for (int i = 0; i < MAX_RESOLUTION; i++) {
    SceVideodecQueryInitInfoHwAvcdec dec;
    dec.size = sizeof(SceVideodecQueryInitInfoHwAvcdec);
    dec.horizontal = VITA_DECODER_RESOLUTION(RESOLUTIONS[i][0]);
    dec.vertical = VITA_DECODER_RESOLUTION(RESOLUTIONS[i][1]);
    dec.numOfRefFrames = 5;
    dec.numOfStreams = 1;
    int ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &dec);
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    if (ret >= 0) {
      supported_resolutions[supported_resolution_count++] = i;
    }
  }
}

// Moves from the option at current by step, stopping at either end. An
// option that isn't in the list moves to the last one.
static int step_clamped(int current, int count, int step) {
  if (current < 0) {
    return count - 1;
  }
  int next = current + step;
  return next < 0 ? 0 : (next >= count ? count - 1 : next);
}

static void resolution_value(char *out, size_t size) {
  snprintf(out, size, "%dx%d", config.stream.width, config.stream.height);
}

static bool resolution_change(int step) {
  // The decoder is set up while a stream runs, so it can't be queried then
  if (vitavideo_initialized()) {
    return false;
  }
  find_supported_resolutions();
  if (supported_resolution_count == 0) {
    return false;
  }
  int current = -1;
  for (int i = 0; i < supported_resolution_count; i++) {
    const int *res = RESOLUTIONS[supported_resolutions[i]];
    if (res[0] == config.stream.width && res[1] == config.stream.height) {
      current = i;
    }
  }
  const int *res = RESOLUTIONS[supported_resolutions[step_clamped(current, supported_resolution_count, step)]];
  config.stream.width = res[0];
  config.stream.height = res[1];
  return true;
}

static const int FRAME_RATES[] = {24, 30, 40, 50, 60};
#define FRAME_RATE_COUNT ((int)(sizeof(FRAME_RATES) / sizeof(FRAME_RATES[0])))

static void fps_value(char *out, size_t size) {
  snprintf(out, size, "%d", config.stream.fps);
}

static bool fps_change(int step) {
  int current = -1;
  for (int i = 0; i < FRAME_RATE_COUNT; i++) {
    if (FRAME_RATES[i] == config.stream.fps) {
      current = i;
    }
  }
  config.stream.fps = FRAME_RATES[step_clamped(current, FRAME_RATE_COUNT, step)];
  return true;
}

/*
 * Bitrate
 */

// The Bitrate row's steps, in kbps, then Custom, which enables the Custom
// bitrate row for any value in kbps, as the setting has always been entered.
// A saved value that isn't a step opens on Custom unchanged, so upgrading
// never alters anyone's bitrate.
static const int BITRATE_STEPS_KBPS[] = {5000, 10000, 15000, 20000, 25000, 30000};
#define BITRATE_STEP_COUNT ((int)(sizeof(BITRATE_STEPS_KBPS) / sizeof(BITRATE_STEPS_KBPS[0])))
static bool bitrate_custom = false;

static int bitrate_step_index(int kbps) {
  for (int i = 0; i < BITRATE_STEP_COUNT; i++) {
    if (BITRATE_STEPS_KBPS[i] == kbps) {
      return i;
    }
  }
  return -1;
}

void settings_sync_bitrate(void) {
  bitrate_custom = bitrate_step_index(config.stream.bitrate) < 0;
}

static void bitrate_value(char *out, size_t size) {
  int kbps = config.stream.bitrate;
  if (bitrate_custom) {
    snprintf(out, size, "Custom");
  } else if (kbps % 1000 == 0) {
    snprintf(out, size, "%d Mbps", kbps / 1000);
  } else {
    snprintf(out, size, "%.1f Mbps", kbps / 1000.0f);
  }
}

static bool bitrate_change(int step) {
  // The steps, then Custom as the last position
  int position = bitrate_custom ? BITRATE_STEP_COUNT : bitrate_step_index(config.stream.bitrate);
  if (position < 0) {
    position = BITRATE_STEP_COUNT;
  }
  position = (position + (step < 0 ? BITRATE_STEP_COUNT : 1)) % (BITRATE_STEP_COUNT + 1);
  bitrate_custom = position == BITRATE_STEP_COUNT;
  if (!bitrate_custom) {
    config.stream.bitrate = BITRATE_STEPS_KBPS[position];
  }
  return true;
}

static bool bitrate_is_custom(void) {
  return bitrate_custom;
}

static void custom_bitrate_value(char *out, size_t size) {
  snprintf(out, size, "%d kbps", config.stream.bitrate);
}

/*
 * Other choices
 */

static const int PACKET_SIZES[] = {1024, 1392};
#define PACKET_SIZE_COUNT ((int)(sizeof(PACKET_SIZES) / sizeof(PACKET_SIZES[0])))

static void packet_size_value(char *out, size_t size) {
  snprintf(out, size, "%d bytes", config.stream.packetSize);
}

static bool packet_size_change(int step) {
  // 1024 is the safe default; 1392 is the largest that fits a 1500-byte MTU
  int current = -1;
  for (int i = 0; i < PACKET_SIZE_COUNT; i++) {
    if (PACKET_SIZES[i] == config.stream.packetSize) {
      current = i;
    }
  }
  // A custom value from the config file moves to the first preset
  int next = current < 0 ? 0 : (current + step + PACKET_SIZE_COUNT) % PACKET_SIZE_COUNT;
  config.stream.packetSize = PACKET_SIZES[next];
  return true;
}

static void stats_overlay_value(char *out, size_t size) {
  static const char *const names[STATS_OVERLAY_COUNT] = {"off", "fps", "full"};
  snprintf(out, size, "%s", names[config.stats_overlay]);
}

static bool stats_overlay_change(int step) {
  config.stats_overlay = (config.stats_overlay + STATS_OVERLAY_COUNT + step) % STATS_OVERLAY_COUNT;
  return true;
}

static void app_list_value(char *out, size_t size) {
  snprintf(out, size, "%s", config.app_list_style == APP_LIST_TEXT ? "text" : "box art");
}

static bool app_list_change(int step) {
  config.app_list_style = (config.app_list_style + APP_LIST_STYLE_COUNT + step) % APP_LIST_STYLE_COUNT;
  return true;
}

static const char *const CONTROLLER_TYPE_NAMES[] = {"Xbox", "PlayStation"};
static const int CONTROLLER_TYPE_VALUES[] = {1, 2};
#define CONTROLLER_TYPE_COUNT 2

static int controller_type_index(void) {
  for (int i = 0; i < CONTROLLER_TYPE_COUNT; i++) {
    if (CONTROLLER_TYPE_VALUES[i] == config.controller_type) {
      return i;
    }
  }
  return 1; // PlayStation if not found
}

static void controller_type_value(char *out, size_t size) {
  snprintf(out, size, "%s", CONTROLLER_TYPE_NAMES[controller_type_index()]);
}

static bool controller_type_change(int step) {
  int index = (controller_type_index() + CONTROLLER_TYPE_COUNT + step) % CONTROLLER_TYPE_COUNT;
  config.controller_type = CONTROLLER_TYPE_VALUES[index];
  return true;
}

static void mouse_accel_value(char *out, size_t size) {
  snprintf(out, size, "%d", config.mouse_acceleration);
}

static bool mouse_accel_change(int step) {
  int value = config.mouse_acceleration + step * 15;
  config.mouse_acceleration = value < 0 ? 0 : (value > 300 ? 300 : value);
  return true;
}

static const char *const TOUCH_MODE_NAMES[] = {"Off", "DS4 Touchpad", "Mouse Absolute", "Tablet (Sunshine)"};
#define TOUCH_MODE_COUNT 4

static void touch_mode_value(char *out, size_t size) {
  int mode = config.touchscreen_mode;
  snprintf(out, size, "%s", mode >= 0 && mode < TOUCH_MODE_COUNT ? TOUCH_MODE_NAMES[mode] : "");
}

static bool touch_mode_change(int step) {
  (void)step;
  config.touchscreen_mode = (config.touchscreen_mode + 1) % TOUCH_MODE_COUNT;
  // Absolute touch only runs in Mouse Absolute mode
  touchabsolute_enable(config.touchscreen_mode == 2);
  return true;
}

/*
 * Shoulder buttons and the mapping file, which can't both be on
 */

static bool swap_shoulders_on(void) {
  return config.swap_shoulder_buttons;
}

static bool swap_shoulders_flip(int step) {
  (void)step;
  config.swap_shoulder_buttons = !config.swap_shoulder_buttons;
  swap_shoulder_buttons = config.swap_shoulder_buttons;
  if (config.swap_shoulder_buttons && config.mapping) {
    free(config.mapping);
    config.mapping = NULL;
  }
  return true;
}

static bool mapping_on(void) {
  return config.mapping != NULL;
}

static bool mapping_flip(int step) {
  (void)step;
  if (config.mapping) {
    free(config.mapping);
    config.mapping = NULL;
  } else {
    config.mapping = strdup("mappings/vita.conf");
    config.swap_shoulder_buttons = false;
    swap_shoulder_buttons = false;
  }
  return true;
}

static void sprint_time_value(char *out, size_t size) {
  snprintf(out, size, "%u", config.double_tap_sprint_step_time);
}

static void deadzone_value(char *out, size_t size) {
  snprintf(out, size, "%dpx,%dpx,%dpx,%dpx", config.back_deadzone.top, config.back_deadzone.right,
           config.back_deadzone.bottom, config.back_deadzone.left);
}

/*
 * Help with a path in it, which depends on where the config lives
 */

static void debug_log_help(char *out, size_t size) {
  snprintf(out, size,
           "Saves a detailed log to %smoonlight.log for tracking down problems. It slows "
           "streaming down, so only turn it on when something's wrong, and turn it off again "
           "afterwards.", config.key_dir);
}

static void mapping_help(char *out, size_t size) {
  snprintf(out, size,
           "Use your own button layout from a file at %smappings/vita.conf instead of the "
           "default. There's an example file in the project's GitHub repository.",
           config.key_dir);
}

/*
 * Wi-Fi Turbo, the Wi-Fi plugin, when it is installed
 */

// Applying changes the plugin's settings and can reconnect Wi-Fi, which would
// end a stream
static bool wifiturbo_apply_listed(void) {
  return wifiturbo_present() && !settings_streaming();
}

static void wifiturbo_check_help(char *out, size_t size) {
  char details[800];
  int count = wifiturbo_advice_details(details, sizeof(details));
  if (count < 0) {
    snprintf(out, size, "Couldn't read the Wi-Fi Turbo plugin's settings.");
  } else if (count == 0) {
    snprintf(out, size, "None of the Wi-Fi Turbo settings that can interrupt a stream are set.");
  } else {
    snprintf(out, size, "%s\n\nChange these in the Vita's Settings, under Network, Wi-Fi Turbo.", details);
  }
}

/*
 * The benchmark, against the computer Settings was opened from
 */

static bool bench_not_ready(void) {
  return !settings_streaming() && !session_server_ready();
}

static bool bench_ready(void) {
  return !settings_streaming() && session_server_ready();
}

static void bench_target_value(char *out, size_t size) {
  char app_name[128];
  if (session_find_bench_app(config.bench_app, app_name, sizeof(app_name)) >= 0) {
    snprintf(out, size, "%s on %s", app_name, session_server_name());
  } else {
    snprintf(out, size, "no benchmark app on %s", session_server_name());
  }
}

static const int BENCH_DURATIONS[] = {30, 60, 120, 300};
#define BENCH_DURATION_COUNT ((int)(sizeof(BENCH_DURATIONS) / sizeof(BENCH_DURATIONS[0])))

static void bench_seconds_value(char *out, size_t size) {
  snprintf(out, size, "%d s", config.bench_seconds);
}

static bool bench_seconds_change(int step) {
  int current = 0;
  for (int i = 0; i < BENCH_DURATION_COUNT; i++) {
    if (BENCH_DURATIONS[i] == config.bench_seconds) {
      current = i;
    }
  }
  config.bench_seconds = BENCH_DURATIONS[(current + BENCH_DURATION_COUNT + step) % BENCH_DURATION_COUNT];
  return true;
}

static void version_value(char *out, size_t size) {
  snprintf(out, size, "v%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
}

/*
 * The table, in the order the rows are listed
 */

// Every About row shares this help
static const char ABOUT_HELP[] =
      "About this app: its version, where the project lives and who looks after it, and the "
      "two projects it works with: Moonlight, the streaming client it's built on, and "
      "Sunshine, the host you run on your PC.";

const setting_def SETTINGS[] = {
  // Above the tabs, on every one
  {.tab = SETTINGS_TAB_ALL, .kind = SETTING_ACTION, .flags = SETTING_WARNING,
   .label = "Bluetooth is on, which can disturb Wi-Fi", .listed = vita_bluetooth_enabled,
   .action = SETTING_ACTION_SHOW_HELP,
   .help = "Bluetooth and Wi-Fi share the Vita's 2.4 GHz radio, so while Bluetooth is on the "
      "stream can stutter or lose frames, more so with a Bluetooth controller or headphones "
      "in use. If you aren't using a Bluetooth device, turn it off in the Vita's Settings, "
      "under Bluetooth Devices."},
  {.tab = SETTINGS_TAB_ALL, .kind = SETTING_NOTE,
   .label = "Settings that need a new stream return once this one ends.", .listed = settings_streaming},

  // Stream
  // Listed mid-stream too: it can restart the stream to apply its settings
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_ACTION, .label = "Apply recommended settings",
   .action = SETTING_ACTION_RECOMMENDED,
   .help = "Sets resolution, frame rate and bitrate to what worked best in testing, for your "
      "connection and for what matters most to you. Auto-detect tests the connection where "
      "you'll play and picks for you. During a stream it can restart the stream to apply "
      "them; Auto-detect itself needs the game closed first."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_CHOICE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Resolution", .value = resolution_value, .change = resolution_change,
   .help = "The picture size your PC sends. The Vita's screen is 960x544, so 960x544 or 960x540 "
      "looks sharpest for the least Wi-Fi. Bigger sizes look about the same on the Vita but "
      "need more bandwidth. Can't be changed while a game is streaming."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_CHOICE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "FPS", .value = fps_value, .change = fps_change,
   .help = "Frames per second your PC sends. 60 is the smoothest. Try 30 if the stream stutters or"
      " your Wi-Fi is weak: it needs about half the data. 24 suits films."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_CHOICE, .flags = SETTING_NEEDS_NEW_STREAM | SETTING_CONFIRM_STEPS,
   .label = "Bitrate", .value = bitrate_value, .change = bitrate_change,
   .help = "How much video is sent each second. Higher looks sharper but needs a stronger signal "
      "and adds a little delay; on the Vita's screen the picture stops improving at about 15 "
      "to 20 Mbps. 10 suits weaker Wi-Fi, 20 a strong signal. Lower it if the picture goes "
      "blocky or stutters. Custom lets you type any value below."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_ACTION, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Custom bitrate", .available = bitrate_is_custom, .value = custom_bitrate_value,
   .action = SETTING_ACTION_CUSTOM_BITRATE,
   .help = "Any bitrate, in kilobits per second (10000 = 10 Mbps), from 500 to 100000. Choose "
      "Custom on the Bitrate row above to use it."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Change graphical game settings for performance", .is_on = sops_on, .change = sops_flip,
   .help = "Lets GeForce Experience change your game's own graphics settings to suit the stream. "
      "Only does anything with NVIDIA GeForce Experience; Sunshine ignores it. Turn it off if"
      " your in-game settings keep changing by themselves."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Enable reference frame invalidation", .is_on = frame_inval_on, .change = frame_inval_flip,
   .help = "Fixes glitches from lost video faster, without waiting for a whole new picture. Needs "
      "support from your PC. If you see corrupted video that doesn't clear up, turn this off."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Enable stream optimization", .is_on = stream_optimize_on, .change = stream_optimize_flip,
   .help = "Tunes the stream for playing over the internet, away from home. Leave it off on your "
      "home network, where it only adds overhead."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE,
   .label = "Enable VITA vblank", .is_on = vblank_wait_on, .change = vblank_wait_flip,
   .help = "V-Sync. Waits for the screen to finish a refresh before showing the next frame. Stops "
      "lines tearing across the picture, but can add up to a frame of input lag. Off feels "
      "snappier, on looks cleaner."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Fill screen (crop edges)", .is_on = fill_screen_on, .change = fill_screen_flip,
   .help = "When the stream's shape doesn't match the Vita's screen, for example a 4:3 or "
      "ultrawide game, zoom in and crop the edges to fill the screen instead of showing black"
      " bars. You may lose some of the picture at the edges."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE,
   .label = "Enable frame pacer", .is_on = frame_pacer_on, .change = frame_pacer_flip,
   .help = "Throws away extra frames when more arrive than your FPS setting. It can cause a small "
      "hitch every few seconds, so off is recommended."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_CHOICE, .flags = SETTING_NEEDS_NEW_STREAM | SETTING_CONFIRM_STEPS,
   .label = "Video packet size", .value = packet_size_value, .change = packet_size_change,
   .help = "How big each chunk of video sent over Wi-Fi is. 1392 sends fewer chunks per frame, "
      "which can be smoother on a good connection. If the video breaks up or won't start, go "
      "back to 1024."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Fast colour decoding", .is_on = decode_yuv_on, .change = decode_yuv_flip,
   .help = "Lets the Vita's graphics chip handle the colour conversion while it draws each frame, "
      "which nearly halves the video decoder's work (about 2 ms less per frame). The picture "
      "looks the same. Only turn this off if colours look wrong, such as a green or pink "
      "tint."},
  {.tab = SETTINGS_TAB_STREAM, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Enable local audio", .is_on = local_audio_on, .change = local_audio_flip,
   .help = "Also plays the game's sound on your PC's speakers. Off keeps the PC quiet and sends "
      "the sound only to the Vita."},

  // System
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_TOGGLE,
   .label = "Enable debug log", .is_on = debug_log_on, .change = debug_log_flip, .help_with = debug_log_help},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_TOGGLE,
   .label = "Disable power save", .is_on = powersave_on, .change = powersave_flip,
   .help = "Stops the Vita dimming its screen or going to sleep while you stream, even when you "
      "aren't pressing anything, such as during a cutscene."},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_TOGGLE,
   .label = "Boost CPU/GPU clocks while streaming", .is_on = boost_clocks_on, .change = boost_clocks_flip,
   .help = "Runs the Vita's processor and graphics at full speed while streaming, for smoother "
      "video and sound. Uses more battery. Turn it off to save battery if your streams "
      "already run smoothly."},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_TOGGLE,
   .label = "Swap X & O for Moonlight", .is_on = jp_layout_on, .change = jp_layout_flip,
   .help = "Swaps X and O in Moonlight's own menus, so O confirms and X goes back, like on "
      "Japanese consoles. Doesn't change any buttons in your games."},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_CHOICE, .flags = SETTING_CONFIRM_STEPS,
   .label = "Stats overlay", .value = stats_overlay_value, .change = stats_overlay_change,
   .help = "Shows stream information on screen. fps: the frame rate and Wi-Fi signal. full: frame "
      "rate, bitrate, lost frames, delays and Wi-Fi signal, to help track down stutter or "
      "lag. The Wi-Fi signal turns yellow below 60%, a sign to move closer to your router. "
      "Use left and right to change."},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_CHOICE, .flags = SETTING_CONFIRM_STEPS,
   .label = "App list", .value = app_list_value, .change = app_list_change,
   .help = "How a PC's games and apps are listed when you connect to it. Box art: a row of game "
      "covers to scroll through with left and right. Text: a plain list, which uses less "
      "memory. Covers are saved on the memory card after the first visit."},
  // Only listed when the Wi-Fi Turbo plugin is installed
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_HEADER, .label = "Wi-Fi Turbo", .listed = wifiturbo_present},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_ACTION, .label = "Wi-Fi Turbo settings for streaming",
   .listed = wifiturbo_present, .value = wifiturbo_advice_value, .action = SETTING_ACTION_SHOW_HELP,
   .help_with = wifiturbo_check_help},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_TOGGLE, .listed = wifiturbo_present,
   .label = "Warn about Wi-Fi Turbo settings", .is_on = wifiturbo_notices_on, .change = wifiturbo_notices_flip,
   .help = "When a stream starts, shows a short message if a Wi-Fi Turbo setting can interrupt it, such "
      "as Smart Mesh Roaming, which scans other channels and can reconnect Wi-Fi mid-stream. "
      "Moonlight only reads those settings; it never changes them on its own."},
  {.tab = SETTINGS_TAB_SYSTEM, .kind = SETTING_ACTION, .label = "Apply recommended Wi-Fi Turbo settings",
   .listed = wifiturbo_apply_listed, .action = SETTING_ACTION_WIFITURBO_APPLY,
   .help = "Asks Wi-Fi Turbo to turn off Smart Mesh Roaming, 40 MHz and the In-game HUD, and to use Max "
      "performance, which suits streaming. The plugin saves them, so they stay that way until you "
      "change them in the Vita's Settings, under Network, Wi-Fi Turbo. Changing 40 MHz or the power "
      "profile reconnects Wi-Fi for a few seconds. Can't be used during a stream."},

  // Controls
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE,
   .label = "Sync input to controller sampling", .is_on = input_sync_on, .change = input_sync_flip,
   .help = "Reads your buttons in step with the Vita's controls, which saves battery without "
      "adding lag. If touch controls feel slower than before, turn it off."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Enable Gyroscope reporting", .is_on = motion_on, .change = motion_flip,
   .help = "Sends the Vita's motion sensor to your PC, for games with gyro aiming or motion "
      "controls. Needs a PC host that supports motion, such as Sunshine."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE,
   .label = "Enable double tap to sprint", .is_on = sprint_on, .change = sprint_flip,
   .help = "Push the left stick fully forward twice, quickly, to hold sprint (a left stick click)."
      " Handy because the Vita's sticks can't be clicked. A running icon shows while it's "
      "active."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_ACTION,
   .label = "Sprint double tap time", .value = sprint_time_value, .action = SETTING_ACTION_SPRINT_TIME,
   .help = "How quickly the two pushes must come for double tap to sprint, in milliseconds. Raise "
      "it if sprint won't start, lower it if it starts by accident."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_CHOICE, .flags = SETTING_NEEDS_NEW_STREAM | SETTING_SAVE_AT_ONCE,
   .label = "Controller type", .value = controller_type_value, .change = controller_type_change,
   .help = "Which controller your PC thinks is connected: PlayStation or Xbox. Games show button "
      "prompts to match."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE,
   .label = "Swap R1/L1 <-> R2/L2", .is_on = swap_shoulders_on, .change = swap_shoulders_flip,
   .help = "Off: L and R are L1 and R1, and the top corners of the rear touchpad are L2 and R2. "
      "On: L and R become L2 and R2, which suits shooters where you aim and fire with the "
      "triggers, and the rear corners become L1 and R1."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_CHOICE, .flags = SETTING_HELD_REPEATS,
   .label = "Mouse acceleration", .value = mouse_accel_value, .change = mouse_accel_change,
   .help = "How fast the mouse cursor moves when you use the front screen as a trackpad. Higher "
      "moves the cursor further for the same swipe."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE,
   .label = "Enable mapping file", .is_on = mapping_on, .change = mapping_flip, .help_with = mapping_help},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE,
   .label = "Enable PS button capture", .is_on = psbutton_on, .change = psbutton_flip,
   .help = "Press the PS button once to send it to your PC, which opens things like Steam's "
      "overlay. Press it twice quickly to go to the Vita's home screen as usual."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_ACTION,
   .label = "Back touchscreen deadzone", .value = deadzone_value, .action = SETTING_ACTION_BACK_DEADZONE,
   .help = "Strips around the edges of the rear touchpad that are ignored, so holding the Vita "
      "doesn't press L2, R2, L3 or R3 by accident."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE,
   .label = "Enable touchscreen special keys", .is_on = touch_keys_on, .change = touch_keys_flip,
   .help = "Turns the corners of the front screen into buttons, such as top-left for the pause "
      "menu. Choose what each corner does in Touchscreen special keys."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_ACTION,
   .label = "Touchscreen special keys", .action = SETTING_ACTION_SPECIAL_KEYS,
   .help = "Choose what each corner of the front screen does, such as opening the pause menu or "
      "pressing the PS button. Only works when Enable touchscreen special keys is on."},
  // Confirm cycles the modes, so it's a toggle with four positions
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_TOGGLE,
   .label = "Touchscreen mode", .value = touch_mode_value, .change = touch_mode_change,
   .help = "How the front screen works in games. Off: a trackpad for the mouse. DS4 Touchpad: a "
      "PlayStation controller touchpad. Mouse Absolute: the cursor jumps to your finger. "
      "Tablet: full multi-touch (Sunshine)."},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_HEADER, .label = "Keyboard"},
  {.tab = SETTINGS_TAB_CONTROLS, .kind = SETTING_ACTION,
   .label = "Keyboard layout", .action = SETTING_ACTION_KEYBOARD_LAYOUT,
   .help = "The language layout of the on-screen keyboard, which you open with START + LEFT while "
      "streaming."},

  // Developer, shown after holding L and R and pressing triangle
  {.tab = SETTINGS_TAB_DEVELOPER, .kind = SETTING_NOTE,
   .label = "The benchmark can't run during a stream.", .listed = settings_streaming},
  {.tab = SETTINGS_TAB_DEVELOPER, .kind = SETTING_NOTE,
   .label = "To benchmark, open Settings from a connected computer's menu.", .listed = bench_not_ready},
  {.tab = SETTINGS_TAB_DEVELOPER, .kind = SETTING_ACTION,
   .label = "Run benchmark", .listed = bench_ready, .value = bench_target_value,
   .action = SETTING_ACTION_RUN_BENCHMARK,
   .help = "Streams the benchmark page (tools/bench/bench.html) from this computer and measures "
      "input lag, latency, frame rate and dropped frames, then saves a report to the memory "
      "card. The computer needs a Sunshine app that shows the page; see "
      "tools/bench/README.md. Only available from a connected computer's menu."},
  {.tab = SETTINGS_TAB_DEVELOPER, .kind = SETTING_CHOICE, .flags = SETTING_CONFIRM_STEPS,
   .label = "Benchmark duration", .listed = bench_ready, .value = bench_seconds_value,
   .change = bench_seconds_change,
   .help = "How long the benchmark measures for. Longer runs smooth out Wi-Fi hiccups."},
  {.tab = SETTINGS_TAB_DEVELOPER, .kind = SETTING_TOGGLE, .flags = SETTING_NEEDS_NEW_STREAM,
   .label = "Decode on network thread (legacy)", .is_on = direct_decode_on, .change = direct_decode_flip,
   .help = "For troubleshooting only. Uses the older way of handling video, which stutters more on"
      " a busy Wi-Fi network. Leave it off unless you want to compare the two."},
  {.tab = SETTINGS_TAB_DEVELOPER, .kind = SETTING_ACTION,
   .label = "Hide developer options", .action = SETTING_ACTION_HIDE_DEVELOPER,
   .help = "Hides these developer options again. Hold L and R and press triangle in Settings to "
      "show them."},

  // About: selectable lines, so the tab can scroll and show help
  {.tab = SETTINGS_TAB_ABOUT, .kind = SETTING_INFO, .label = "Moonlight for PS Vita", .value = version_value,
   .help = ABOUT_HELP},
  {.tab = SETTINGS_TAB_ABOUT, .kind = SETTING_INFO, .label = "Project",
   .fixed_value = "github.com/xyzz/vita-moonlight", .help = ABOUT_HELP},
  {.tab = SETTINGS_TAB_ABOUT, .kind = SETTING_INFO, .label = "Maintained by",
   .fixed_value = "AorsiniYT, d3m3vilurr, xyzz", .help = ABOUT_HELP},
  {.tab = SETTINGS_TAB_ABOUT, .kind = SETTING_INFO, .label = "Built on Moonlight",
   .fixed_value = "moonlight-stream.org", .help = ABOUT_HELP},
  {.tab = SETTINGS_TAB_ABOUT, .kind = SETTING_INFO, .label = "Host for your PC",
   .fixed_value = "Sunshine: github.com/LizardByte/Sunshine", .help = ABOUT_HELP},
  {.tab = SETTINGS_TAB_ABOUT, .kind = SETTING_INFO, .label = "Licence",
   .fixed_value = "GNU GPL version 3", .help = ABOUT_HELP},
};

const int SETTINGS_COUNT = (int)(sizeof(SETTINGS) / sizeof(SETTINGS[0]));

bool setting_listed(const setting_def *setting, int tab) {
  if (setting->tab != SETTINGS_TAB_ALL && setting->tab != tab) {
    return false;
  }
  if ((setting->flags & SETTING_NEEDS_NEW_STREAM) && settings_streaming()) {
    return false;
  }
  return setting->listed == NULL || setting->listed();
}

void setting_value(const setting_def *setting, char *out, size_t size) {
  out[0] = '\0';
  if (setting->value) {
    setting->value(out, size);
  } else if (setting->is_on) {
    snprintf(out, size, "%s", setting->is_on() ? "yes" : "no");
  } else if (setting->fixed_value) {
    snprintf(out, size, "%s", setting->fixed_value);
  }
}

void setting_help(const setting_def *setting, char *out, size_t size) {
  if (setting->help_with) {
    setting->help_with(out, size);
  } else {
    snprintf(out, size, "%s", setting->help ? setting->help : "No help for this setting yet.");
  }
}
