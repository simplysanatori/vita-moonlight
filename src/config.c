/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015, 2016 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <ini.h>
#include "input/vita.h"
#include "input/keyboardkeys.h"
#include "debug.h"
#include "stats.h"
#include "bench.h"

extern char* strdup(const char*);

#include <psp2/kernel/sysmem.h>

#define MOONLIGHT_PATH "/moonlight"
#define USER_PATHS "."
#define DEFAULT_CONFIG_DIR "/.config"
#define DEFAULT_CACHE_DIR "/.cache"

#define DEFAULT_PACKET_SIZE 1024
#define MIN_PACKET_SIZE 256
#define MAX_PACKET_SIZE 1392

#define write_config_string(fd, key, value) fprintf(fd, "%s = %s\n", key, value)
#define write_config_int(fd, key, value) fprintf(fd, "%s = %d\n", key, value)
#define write_config_hex(fd, key, value) fprintf(fd, "%s = %X\n", key, value)
#define write_config_bool(fd, key, value) fprintf(fd, "%s = %s\n", key, value?"true":"false");
#define write_config_float(fd, key, value) fprintf(fd, "%s = %f\n", key, value)
#define write_config_section(fd, key) fprintf(fd, "\n[%s]\n", key)

CONFIGURATION config;
char *config_path;

bool inputAdded = false;
static bool mapped = true;
const char* audio_device = NULL;

static int ini_handle(void *out, const char *section, const char *name,
                      const char *value) {
#define HEX(v) strtol((v), NULL, 16)
#define INT(v) atoi((v))
#define BOOL(v) strcmp((v), "true") == 0
#define STR(v) strdup((v))
#define FLT(v) atof((v))

  PCONFIGURATION config = (PCONFIGURATION)out;
  if (strcmp(section, "backtouchscreen_deadzone") == 0) {
    if (strcmp(name, "top") == 0) {
      config->back_deadzone.top = INT(value);
    } else if (strcmp(name, "right") == 0) {
      config->back_deadzone.right = INT(value);
    } else if (strcmp(name, "bottom") == 0) {
      config->back_deadzone.bottom = INT(value);
    } else if (strcmp(name, "left") == 0) {
      config->back_deadzone.left = INT(value);
    }
    else if (strcmp(name, "controller_type") == 0) {
      config->controller_type = INT(value);
    } else if (strcmp(name, "swap_shoulder_buttons") == 0) {
      config->swap_shoulder_buttons = BOOL(value);
    } else if (strcmp(name, "key_dir") == 0) {
      strncpy(config->key_dir, value, sizeof(config->key_dir)-1);
      config->key_dir[sizeof(config->key_dir)-1] = '\0';
    }
  } else if (strcmp(section, "special_keys") == 0) {
    if (strcmp(name, "nw") == 0) {
      config->special_keys.nw = HEX(value);
    } else if (strcmp(name, "ne") == 0) {
      config->special_keys.ne = HEX(value);
    } else if (strcmp(name, "sw") == 0) {
      config->special_keys.sw = HEX(value);
    } else if (strcmp(name, "se") == 0) {
      config->special_keys.se = HEX(value);
    } else if (strcmp(name, "offset") == 0) {
      config->special_keys.offset = INT(value);
    } else if (strcmp(name, "size") == 0) {
      config->special_keys.size = INT(value);
    }
  } else {
    if (strcmp(name, "address") == 0) {
      config->address = STR(value);
    } else if (strcmp(name, "width") == 0) {
      config->stream.width = INT(value);
    } else if (strcmp(name, "height") == 0) {
      config->stream.height = INT(value);
    } else if (strcmp(name, "fps") == 0) {
      config->stream.fps = INT(value);
    } else if (strcmp(name, "bitrate") == 0) {
      config->stream.bitrate = INT(value);
    } else if (strcmp(name, "packetsize") == 0) {
      config->stream.packetSize = INT(value);
    } else if (strcmp(name, "sops") == 0) {
      config->sops = BOOL(value);
    } else if (strcmp(name, "localaudio") == 0) {
      config->localaudio = BOOL(value);
    } else if (strcmp(name, "enable_frame_pacer") == 0) {
      config->enable_frame_pacer = BOOL(value);
    } else if (strcmp(name, "video_reorder_wait_ms") == 0) {
      config->video_reorder_wait_ms = INT(value);
    } else if (strcmp(name, "center_region_only") == 0) {
      config->center_region_only = BOOL(value);
    } else if (strcmp(name, "disable_powersave") == 0) {
      config->disable_powersave = BOOL(value);
    } else if (strcmp(name, "boost_clocks") == 0) {
      config->boost_clocks = BOOL(value);
    } else if (strcmp(name, "pin_threads") == 0) {
      config->pin_threads = BOOL(value);
    } else if (strcmp(name, "direct_decode") == 0) {
      config->direct_decode = BOOL(value);
    } else if (strcmp(name, "input_sync_to_controller") == 0) {
      config->input_sync_to_controller = BOOL(value);
    } else if (strcmp(name, "developer_mode") == 0) {
      config->developer_mode = BOOL(value);
    } else if (strcmp(name, "bench_seconds") == 0) {
      config->bench_seconds = INT(value);
    } else if (strcmp(name, "bench_app") == 0) {
      config->bench_app = STR(value);
    } else if (strcmp(name, "bench_host") == 0) {
      config->bench_host = STR(value);
    } else if (strcmp(name, "bench_label") == 0) {
      config->bench_label = STR(value);
    } else if (strcmp(name, "bench_autorun") == 0) {
      config->bench_autorun = BOOL(value);
    } else if (strcmp(name, "bench_quit_app") == 0) {
      config->bench_quit_app = BOOL(value);
    } else if (strcmp(name, "bench_input") == 0) {
      config->bench_input = STR(value);
    } else if (strcmp(name, "bench_grab") == 0) {
      config->bench_grab = BOOL(value);
    } else if (strcmp(name, "bench_idr_hz") == 0) {
      config->bench_idr_hz = INT(value);
    } else if (strcmp(name, "decode_yuv") == 0) {
      config->decode_yuv = BOOL(value);
    } else if (strcmp(name, "jp_layout") == 0) {
      config->jp_layout = BOOL(value);
    } else if (strcmp(name, "stats_overlay") == 0) {
      config->stats_overlay = INT(value);
    } else if (strcmp(name, "app_list_style") == 0) {
      config->app_list_style = INT(value);
    } else if (strcmp(name, "show_fps") == 0) {
      // Older configs only had an FPS on/off switch
      if (BOOL(value) && config->stats_overlay == STATS_OVERLAY_OFF) {
        config->stats_overlay = STATS_OVERLAY_FPS;
      }
    } else if (strcmp(name, "save_debug_log") == 0) {
      config->save_debug_log = BOOL(value);
    } else if (strcmp(name, "mapping") == 0) {
      config->mapping = STR(value);
    } else if (strcmp(name, "mouse_acceleration") == 0) {
      config->mouse_acceleration = INT(value);
    } else if (strcmp(name, "enable_ref_frame_invalidation") == 0) {
      config->enable_ref_frame_invalidation = BOOL(value);
    } else if (strcmp(name, "enable_remote_stream_optimization") == 0) {
      config->stream.streamingRemotely = INT(value);
    } else if (strcmp(name, "enable_vita_vblank_wait") == 0) {
      config->enable_vita_vblank_wait = BOOL(value);
    } else if (strcmp(name, "enable_motion_controls") == 0) {
      config->enable_motion_controls = BOOL(value);
    } else if (strcmp(name, "enable_front_touchzones") == 0) {
      config->enable_front_touchzones = BOOL(value);
    } else if(strcmp(name, "enable_psbutton_capture") == 0) {
      config->enable_psbutton_capture = BOOL(value);
    } else if (strcmp(name, "enable_double_tap_sprint") == 0) {
      config->enable_double_tap_sprint = BOOL(value);
    } else if (strcmp(name, "double_tap_sprint_step_time") == 0) {
      config->double_tap_sprint_step_time = INT(value);
    } else if (strcmp(name, "motion_controls_scalar_x") == 0) {
      config->motion_controls_scalar_x = FLT(value);
    } else if (strcmp(name, "motion_controls_scalar_y") == 0) {
      config->motion_controls_scalar_y = FLT(value);
    } else if (strcmp(name, "keyboard_layout") == 0) {
      config->keyboard_layout = INT(value);
    } else if (strcmp(name, "touchscreen_mode") == 0) {
      config->touchscreen_mode = atoi(value);
    } else if (strcmp(name, "controller_type") == 0) {
      config->controller_type = INT(value);
    } else if (strcmp(name, "swap_shoulder_buttons") == 0) {
      config->swap_shoulder_buttons = BOOL(value);
    }
  }
  return 0;
}

bool config_file_parse(char* filename, PCONFIGURATION config) {
  return ini_parse(filename, ini_handle, config);
}

bool config_save(const char* filename, PCONFIGURATION config) {
  FILE* fd = fopen(filename, "w");
  if (fd == NULL) {
    // A full memory card, say: keep running with the settings in memory
    vita_debug_log("Can't open configuration file: %s", filename);
    return false;
  }

  if (config->address)
    write_config_string(fd, "address", config->address);


  if (config->mapping)
    write_config_string(fd, "mapping", config->mapping);

  if (config->stream.width != 1280)
    write_config_int(fd, "width", config->stream.width);
  if (config->stream.height != 720)
    write_config_int(fd, "height", config->stream.height);
  if (config->stream.fps != 60)
    write_config_int(fd, "fps", config->stream.fps);
  if (config->stream.bitrate != -1)
    write_config_int(fd, "bitrate", config->stream.bitrate);
  if (config->stream.packetSize != DEFAULT_PACKET_SIZE)
    write_config_int(fd, "packetsize", config->stream.packetSize);
  if (!config->sops)
    write_config_bool(fd, "sops", config->sops);
  if (config->localaudio)
    write_config_bool(fd, "localaudio", config->localaudio);

  if (config->app && strcmp(config->app, "Steam") != 0)
    write_config_string(fd, "app", config->app);

  write_config_string(fd, "key_dir", config->key_dir); // Guardar key_dir en la raíz
  write_config_bool(fd, "enable_frame_pacer", config->enable_frame_pacer);
  write_config_int(fd, "video_reorder_wait_ms", config->video_reorder_wait_ms);
  write_config_bool(fd, "center_region_only", config->center_region_only);
  write_config_bool(fd, "disable_powersave", config->disable_powersave);
  write_config_bool(fd, "boost_clocks", config->boost_clocks);
  write_config_bool(fd, "pin_threads", config->pin_threads);
  write_config_bool(fd, "direct_decode", config->direct_decode);
  write_config_bool(fd, "input_sync_to_controller", config->input_sync_to_controller);
  write_config_bool(fd, "developer_mode", config->developer_mode);
  write_config_int(fd, "bench_seconds", config->bench_seconds);
  write_config_string(fd, "bench_app", config->bench_app);
  write_config_string(fd, "bench_host", config->bench_host);
  write_config_string(fd, "bench_label", config->bench_label);
  write_config_bool(fd, "bench_autorun", config->bench_autorun);
  write_config_bool(fd, "bench_quit_app", config->bench_quit_app);
  write_config_string(fd, "bench_input", config->bench_input);
  write_config_bool(fd, "bench_grab", config->bench_grab);
  write_config_int(fd, "bench_idr_hz", config->bench_idr_hz);
  write_config_bool(fd, "decode_yuv", config->decode_yuv);
  write_config_bool(fd, "jp_layout", config->jp_layout);
  write_config_int(fd, "stats_overlay", config->stats_overlay);
  write_config_int(fd, "app_list_style", config->app_list_style);
  write_config_bool(fd, "save_debug_log", config->save_debug_log);
  write_config_bool(fd, "enable_front_touchzones", config->enable_front_touchzones);

  write_config_int(fd, "mouse_acceleration", config->mouse_acceleration);
  write_config_bool(fd, "enable_ref_frame_invalidation", config->enable_ref_frame_invalidation);
  write_config_int(fd, "enable_remote_stream_optimization", config->stream.streamingRemotely);
  write_config_bool(fd, "enable_vita_vblank_wait", config->enable_vita_vblank_wait);
  write_config_bool(fd, "enable_motion_controls", config->enable_motion_controls);
  write_config_bool(fd, "enable_psbutton_capture", config->enable_psbutton_capture);
  write_config_bool(fd, "enable_double_tap_sprint", config->enable_double_tap_sprint);
  write_config_int(fd, "double_tap_sprint_step_time", config->double_tap_sprint_step_time);
  write_config_float(fd, "motion_controls_scalar_x", config->motion_controls_scalar_x);
  write_config_float(fd, "motion_controls_scalar_y", config->motion_controls_scalar_y);
  write_config_int(fd, "keyboard_layout", config->keyboard_layout);
  write_config_int(fd, "touchscreen_mode", config->touchscreen_mode);
  write_config_bool(fd, "swap_shoulder_buttons", config->swap_shoulder_buttons); // Guardar swap_shoulder_buttons en la raíz
  write_config_int(fd, "controller_type", config->controller_type); // Guardar controller_type en la raíz
  

  write_config_section(fd, "backtouchscreen_deadzone");
  write_config_int(fd, "top",     config->back_deadzone.top);
  write_config_int(fd, "right",   config->back_deadzone.right);
  write_config_int(fd, "bottom",  config->back_deadzone.bottom);
  write_config_int(fd, "left",    config->back_deadzone.left);

  write_config_section(fd, "special_keys");
  write_config_hex(fd, "nw",      config->special_keys.nw);
  write_config_hex(fd, "ne",      config->special_keys.ne);
  write_config_hex(fd, "sw",      config->special_keys.sw);
  write_config_hex(fd, "se",      config->special_keys.se);
  write_config_int(fd, "offset",  config->special_keys.offset);
  write_config_int(fd, "size",    config->special_keys.size);

  // A full card can also fail part way through the writes or the final flush
  bool written = !ferror(fd);
  if (fclose(fd) != 0)
    written = false;
  if (!written)
    vita_debug_log("Can't write configuration file: %s", filename);
  return written;
}

void update_layout() {
  if (config.jp_layout) {
    config.btn_confirm = SCE_CTRL_CIRCLE;
    config.btn_cancel = SCE_CTRL_CROSS;
  }
  else {
    config.btn_confirm = SCE_CTRL_CROSS;
    config.btn_cancel = SCE_CTRL_CIRCLE;
  }
}

void config_parse(int argc, char* argv[], PCONFIGURATION config) {
  LiInitializeStreamConfiguration(&config->stream);

  config->stream.width = 1280;
  config->stream.height = 720;
  config->stream.fps = 60;
  config->stream.bitrate = -1;
  config->stream.packetSize = DEFAULT_PACKET_SIZE;
  config->stream.streamingRemotely = 0;
  config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  config->stream.supportedVideoFormats = VIDEO_FORMAT_H264;
  // The YUV path samples the decoder's output through a *_CSC0 texture
  // (src/video/vita.c), and the GPU's CSC0 conversion is BT.601 limited
  // range. Ask the host for exactly that rather than relying on common-c's
  // defaults; change these and the texture format together
  config->stream.colorSpace = COLORSPACE_REC_601;
  config->stream.colorRange = COLOR_RANGE_LIMITED;

  config->platform = "vita";
  config->model = sceKernelGetModelForCDialog();
  config->app = "Steam";
  config->action = NULL;
  config->address = NULL;
  config->config_file = NULL;
  config->sops = true;
  config->localaudio = false;
  config->fullscreen = true;
  config->unsupported_version = false;
  config->save_debug_log = false;
  config->disable_powersave = true;
  config->boost_clocks = true;
  config->pin_threads = false;
  config->direct_decode = false;
  config->input_sync_to_controller = true;
  config->developer_mode = false;
  config->bench_seconds = BENCH_DEFAULT_SECONDS;
  config->bench_app = "Moonlight Bench";
  config->bench_host = "";
  config->bench_label = "";
  config->bench_autorun = false;
  config->bench_quit_app = true;
  config->bench_input = "key";
  config->bench_grab = false;
  config->bench_idr_hz = 0;
  config->decode_yuv = true;
  config->jp_layout = false;
  config->stats_overlay = STATS_OVERLAY_OFF;
  config->app_list_style = APP_LIST_BOXART;
  // Off by default: it counts frames in ~1 s windows and drops any excess, so jitter
  // around 60 fps periodically discards a decoded frame and causes a visible hitch
  config->enable_frame_pacer = false;
  // On the Vita's Wi-Fi ~10% of video packets can arrive out of order at a
  // weaker signal; waiting for them saves frames that would otherwise freeze
  // the picture until a keyframe. At ~60% signal, 720p 20 Mbps: 23-37
  // freezes a minute with no wait, 2-8 at 10 ms, 1-3 at 20 ms, with the
  // same latency.
  config->video_reorder_wait_ms = 20;
  config->center_region_only = false;

  config->enable_front_touchzones = false;
  config->special_keys.nw = INPUT_SPECIAL_KEY_PAUSE | INPUT_TYPE_SPECIAL;
  config->special_keys.sw = SPECIAL_FLAG | INPUT_TYPE_GAMEPAD;
  config->special_keys.offset = 0;
  config->special_keys.size = 150;

  config->mouse_acceleration = 150;
  config->enable_ref_frame_invalidation = false;
  config->enable_vita_vblank_wait = false;
  config->enable_psbutton_capture = true;
  config->enable_double_tap_sprint = false;

  config->double_tap_sprint_step_time = 200;

  config->motion_controls_scalar_x = 1.2;
  config->motion_controls_scalar_y = 0.8;

  config->inputsCount = 0;
  config->mapping = NULL;
  // No sobrescribir key_dir si ya fue asignado por main.c
  // config->key_dir[0] = 0;
  // Valor por defecto: PS si no está asignado
  if (config->controller_type < 1 || config->controller_type > 4) {
    config->controller_type = 2;
  }
  // Valor por defecto: swap desactivado
  // (si no está presente en config, será false por defecto)
  // No es necesario forzar nada aquí

  char* config_file = config_path;
  if (config_file) {
    config_file_parse(config_file, config);
    vita_debug_log("[DEBUG] Configuración cargada: key_dir = %s, touchscreen_mode = %d, stats_overlay = %d", config->key_dir, config->touchscreen_mode, config->stats_overlay);
  }

  update_layout();

  if (config->config_file != NULL)
    config_save(config->config_file, config);

  // Solo asignar valor por defecto si sigue vacío
  if (config->key_dir[0] == 0x0) {
    const char *xdg_cache_dir = getenv("XDG_CACHE_DIR");
    if (xdg_cache_dir && xdg_cache_dir[0] != '\0') {
      snprintf(config->key_dir, sizeof(config->key_dir), "%s" MOONLIGHT_PATH, xdg_cache_dir);
    } else {
      const char *home_dir = getenv("HOME");
      if (home_dir && home_dir[0] != '\0') {
        snprintf(config->key_dir, sizeof(config->key_dir), "%s" DEFAULT_CACHE_DIR MOONLIGHT_PATH, home_dir);
      } else {
        snprintf(config->key_dir, sizeof(config->key_dir), "%s" DEFAULT_CACHE_DIR MOONLIGHT_PATH, "");
      }
    }
  }

  if (config->stream.fps == -1)
    config->stream.fps = config->stream.height >= 1080 ? 30 : 60;

  if (config->stream.bitrate == -1) {
    if (config->stream.height >= 1080 && config->stream.fps >= 60)
      config->stream.bitrate = 20000;
    else if (config->stream.height >= 1080 || config->stream.fps >= 60)
      config->stream.bitrate = 10000;
    else
      config->stream.bitrate = 5000;
  }

  if (config->bench_seconds < BENCH_MIN_SECONDS || config->bench_seconds > BENCH_MAX_SECONDS) {
    config->bench_seconds = BENCH_DEFAULT_SECONDS;
  }

  if (config->stats_overlay < STATS_OVERLAY_OFF || config->stats_overlay >= STATS_OVERLAY_COUNT) {
    config->stats_overlay = STATS_OVERLAY_OFF;
  }

  // A hand-edited or corrupt file could hold any number, and these index tables
  if (config->touchscreen_mode < 0 || config->touchscreen_mode > 3) {
    vita_debug_log("touchscreen_mode %d out of range, using 0", config->touchscreen_mode);
    config->touchscreen_mode = 0;
  }

  if (config->keyboard_layout < 0 || config->keyboard_layout >= KB_LAYOUT_COUNT) {
    vita_debug_log("keyboard_layout %d out of range, using 0", config->keyboard_layout);
    config->keyboard_layout = KB_LAYOUT_EN_US;
  }

  if (config->app_list_style < 0 || config->app_list_style >= APP_LIST_STYLE_COUNT) {
    config->app_list_style = APP_LIST_BOXART;
  }

  // Larger packets would be IP-fragmented on a standard 1500-byte MTU
  if (config->stream.packetSize < MIN_PACKET_SIZE || config->stream.packetSize > MAX_PACKET_SIZE) {
    vita_debug_log("packetsize %d out of range, using %d", config->stream.packetSize, DEFAULT_PACKET_SIZE);
    config->stream.packetSize = DEFAULT_PACKET_SIZE;
  }

  if (inputAdded) {
    if (!mapped) {
        fprintf(stderr, "Mapping option should be followed by the input to be mapped.\n");
        exit(-1);
    } else if (config->mapping == NULL) {
        fprintf(stderr, "Please specify mapping file as default mapping could not be found.\n");
        exit(-1);
    }
  }
}
