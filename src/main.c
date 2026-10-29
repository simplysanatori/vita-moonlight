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

#include "connection.h"
#include "configuration.h"
#include "audio.h"
#include "psp2/net/netctl.h"
#include "video.h"
#include "config.h"
#include "platform.h"

#include "input/vita.h"
#include "input/touchabsolute.h"
#include "input/keyboardkeys.h"
#include "keyboardsystem.h"

#include <Limelight.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include "curl/curl.h"

#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>

#include <psp2/net/net.h>


#include <psp2/io/stat.h>

#include <psp2/shellutil.h>
#include <psp2/sysmodule.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/rtc.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>

#include "client.h"
#include "graphics.h"
#include "device.h"
#include "gui/ui.h"
#include "util.h"
#include "power/vita.h"
#include "input/motion.h"

#include "debug.h"
#include "stats.h"
#include "bench.h"
#include "wifiturbo.h"
#include "check_dir.h"

#define VITA_NET_MEM_SIZE 1 * 1024 * 1024

SceNetInitParam net_param = {
  .memory = NULL,
  .size = VITA_NET_MEM_SIZE,
  .flags = 0
};

void loop_forever(void) {
  while (connection_is_ready()) {
    sceKernelDelayThread(100 * 1000);
  }
}

static void vita_init() {
  sceShellUtilInitEvents(0);

  // Seed OpenSSL with Sony-grade random number generator
  char random_seed[0x40] = {0};
  sceKernelGetRandomNumber(random_seed, sizeof(random_seed));
  RAND_seed(random_seed, sizeof(random_seed));
  OpenSSL_add_all_algorithms();

  // This is only used for PIN codes, doesn't really matter
  srand(time(NULL));

  #ifdef __vita__
  printf("Vita Moonlight %d.%d.%d (%s)\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, COMPILE_OPTIONS);
  #endif

  int ret;

  // Load Vita network 
  net_param.memory = malloc(net_param.size);
  if (net_param.memory == NULL) {
    printf("Could not allocate net memory!");
    loop_forever();
  }
 
  ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  if (ret < 0) {
    printf("Net module was unable to load!");
    loop_forever();
  }

  ret = sceNetInit(&net_param);
  if (ret < 0) {
    printf("Net init failed!");
    loop_forever();
  }
  
  ret = sceNetCtlInit();
  if (ret < 0) {
    printf("Net Ctl init failed!");
    loop_forever();
  }

  ret = curl_global_init(CURL_GLOBAL_ALL);
  if (ret < 0) {
    printf("CURL init failed!");
    loop_forever();
  }

  ret = vita_debug_init();
  if (ret != true) {
    printf("Debug log mutex init failed!");
    loop_forever();
  }

  if (!vitastats_init()) {
    printf("Stats mutex init failed!");
    loop_forever();
  }

  if (!vitabench_init()) {
    printf("Benchmark mutex init failed!");
    loop_forever();
  }
}

// Pair under the Vita's own nickname (Settings > Account > Nickname), so the
// host's paired-client list shows e.g. "Juan's Vita" instead of a placeholder.
// Not fatal: on any failure pairing falls back to "PS Vita".
static void set_pairing_device_name(void) {
  SceAppUtilInitParam init_param = {0};
  SceAppUtilBootParam boot_param = {0};
  // An error here can mean AppUtil is already initialised; the read below
  // still decides whether we have a name
  sceAppUtilInit(&init_param, &boot_param);

  SceChar8 nickname[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE] = {0};
  int ret = sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME,
                                           nickname, sizeof(nickname));
  if (ret < 0 || nickname[0] == '\0') {
    vita_debug_log("Couldn't read the Vita nickname (0x%08X), pairing as PS Vita\n", ret);
    return;
  }
  gs_set_device_name((const char *)nickname);
}


int main(int argc, char* argv[]) {
  psvDebugScreenInit();
  vita_init();

  if (!vitapower_init()) {
    printf("Failed to init power!");
    loop_forever();
  }

  if (!vitainput_init()) {
    printf("Failed to init input!");
    loop_forever();
  }

  if (!vita_motion_init()) {
    printf("Failed to init motion input!");
    loop_forever();
  }

  char out_path[MOONLIGHT_PATH_MAX] = {0};
  char out_key_dir[MOONLIGHT_PATH_MAX] = {0};
  check_and_create_moonlight_dir(out_path, out_key_dir);
  config_path = out_path;
  strcpy(config.key_dir, out_key_dir);
  vita_debug_log("[Moonlight] Carpeta seleccionada: %s", config.key_dir);
  // Ya no se guarda config antes de inicializar todos los valores
  config_parse(argc, argv, &config);

  // Restaurar estado de Absolute Touch al iniciar
  touchabsolute_enable(config.touchscreen_mode == 2);

  // Aplicar el layout guardado después de cargar la config
  keyboardsystem_set_layout((KeyboardLayout)config.keyboard_layout);

  vitapower_config(config);
  vitainput_config(config);

  char log_path[MOONLIGHT_PATH_MAX] = {0};
  snprintf(log_path, sizeof(log_path), "%s/moonlight.log", config.key_dir);
  config.log_file = fopen(log_path, "w");
  // After the log opens, so a failure to read the nickname is recorded
  set_pairing_device_name();
  // Once, here on the one thread running, so nothing else has to ask the plugin
  wifiturbo_init();

  load_all_known_devices();
  vita_debug_log_thread("main");

  gui_loop();
}
