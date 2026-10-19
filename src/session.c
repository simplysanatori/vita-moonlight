#include "session.h"

#include "config.h"
#include "connection.h"
#include "debug.h"
#include "netcount.h"
#include "platform.h"
#include "video.h"
#include "power/vita.h"
#include "input/vita.h"

#include "client.h"
#include "errors.h"
#include "Limelight.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// The connection to a host and its apps, without any menus or dialogs, so
// any UI (and the benchmark and Auto-detect) can use it

SERVER_DATA server;
PAPP_LIST server_applist;
// Name of the host the current connection is to, as it was paired
static char current_host[256] = "";

void session_set_host(const char *name) {
  snprintf(current_host, sizeof(current_host), "%s", name);
}

// Lists session_open_headless() replaced while a menu may still show their
// names (it can run from Settings, inside the PC's menu). They're freed with
// the list that menu replaces next, once nothing can read them.
static PAPP_LIST replaced_applists = NULL;

static void free_app_list(PAPP_LIST list) {
  while (list != NULL) {
    PAPP_LIST next = list->next;
    free(list->name);
    free(list);
    list = next;
  }
}

int session_refresh_applist(void) {
  PAPP_LIST list = NULL;
  int ret = gs_applist(&server, &list);
  if (ret != GS_OK) {
    return ret;
  }
  free_app_list(server_applist);
  free_app_list(replaced_applists);
  replaced_applists = NULL;
  server_applist = list;
  return GS_OK;
}

int get_app_id(PAPP_LIST list, char *name) {
  while (list != NULL) {
    if (strcmp(list->name, name) == 0)
      return list->id;

    list = list->next;
  }
  return -1;
}

int get_app_name(PAPP_LIST list, int id, char *name, size_t size) {
  while (list != NULL) {
    if (list->id == id) {
      // The name comes from the host, so it can be any length
      snprintf(name, size, "%s", list->name);
      return 1;
    }

    list = list->next;
  }

  return 0;
}

// Set by start_stream once the host has launched the app, even if the stream
// then fails to start
static bool start_launched_app = false;

static int start_stream(int appId, char *err, size_t err_len) {
  start_launched_app = false;
  // TODO support force controller id
  vita_debug_log("Running gs_start_app...");
  int ret = gs_start_app(&server, &config.stream, appId, config.sops, config.localaudio, 1);
  if (ret < 0) {
    if (ret == GS_NOT_SUPPORTED_4K)
      snprintf(err, err_len, "Server doesn't support 4K");
    else if (ret == GS_NOT_SUPPORTED_MODE)
      snprintf(err, err_len, "Server doesn't support %dx%d (%d fps)", config.stream.width, config.stream.height, config.stream.fps);
    else
      snprintf(err, err_len, "Errorcode starting app: %d", ret);

    return ret;
  }
  vita_debug_log("App started.");
  start_launched_app = true;

  enum platform system = VITA;
  int drFlags = 0;

  if (config.fullscreen)
    drFlags |= DISPLAY_FULLSCREEN;

  DECODER_RENDERER_CALLBACKS *video_callback = platform_get_video(system);
  if (config.direct_decode) {
    video_callback->capabilities |= CAPABILITY_DIRECT_SUBMIT;
  } else {
    video_callback->capabilities &= ~CAPABILITY_DIRECT_SUBMIT;
  }
  if (config.enable_ref_frame_invalidation) {
    video_callback->capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC;
  } else {
    video_callback->capabilities &= ~CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC;
  }

  // --- Ajuste para soporte Host Resolution: no modificar resolución si es -1 ---
  // Eliminados g_requested_width y g_requested_height, lógica simplificada
  int orig_width = config.stream.width;
  int orig_height = config.stream.height;
  // Si se seleccionó una resolución específica, se mantiene; si es -1, se deja al host
  // (No se modifica config.stream.width/height aquí)

  int reorder_wait_ms = config.video_reorder_wait_ms < 0 ? 0 : config.video_reorder_wait_ms;
  LiSetVideoReorderWaitUs((uint32_t)reorder_wait_ms * 1000);

  vita_debug_log("Running LiStartConnection...");
  ret = LiStartConnection(&server.serverInfo, &config.stream, &connection_callbacks,
                          video_callback, platform_get_audio(system),
                          NULL, drFlags, NULL, 0);
  vita_debug_log("Connection started.");

  // Restaurar resolución real para el framebuffer/render
  config.stream.width = orig_width;
  config.stream.height = orig_height;

  if (ret == 0) {
    server.currentGame = appId;
    return 0;
  }

  const char* connection_failed_stage_name = LiGetStageName(connection_stage);
  snprintf(err, err_len, "Failed to start stream:\nFailed stage: %s\n(error code %d)",
           connection_failed_stage_name, ret);
  return ret < 0 ? ret : -1;
}
// Starts streaming appId. Returns 0, or a negative code with a message in err,
// so callers decide whether to show it (the benchmark runs without dialogs).
int session_start_stream(int appId, char *err, size_t err_len) {
  // The launch and the RTSP handshake run at the stream's clocks; once it
  // starts, vitapower_start() keeps them up until it ends
  vitapower_busy_begin();
  int ret = start_stream(appId, err, err_len);
  vitapower_busy_end();
  return ret;
}

static int open_headless(device_info_t *info, char *err, size_t err_len) {
  char key_dir[4096];
  snprintf(key_dir, sizeof(key_dir), "%s/%s", config.key_dir, info->name);

  // Try the address that worked last time first
  const char *addresses[2] = {
    info->prefer_external ? info->external : info->internal,
    info->prefer_external ? info->internal : info->external,
  };
  int ret = GS_ERROR;
  for (int i = 0; i < 2 && ret != GS_OK; i++) {
    if (addresses[i][0] == '\0') {
      continue;
    }
    // Same as the interactive path: skip the host display-mode check, which
    // Sunshine's mode list doesn't reliably pass (e.g. 960x540 at 50 fps)
    ret = gs_init(&server, (char *)addresses[i], info->port, key_dir, 0, true);
  }
  if (ret != GS_OK) {
    snprintf(err, err_len, "Can't connect to %s: %s", info->name, gs_error ? gs_error : "no response");
    return -1;
  }
  snprintf(current_host, sizeof(current_host), "%s", info->name);
  connection_reset();

  // Forget the previous PC's apps first: if this PC's list can't be read,
  // a stale list would let a later attempt start one of those IDs here.
  // Not freed yet: menu rows may still point at its names.
  if (server_applist != NULL) {
    PAPP_LIST last = server_applist;
    while (last->next != NULL) {
      last = last->next;
    }
    last->next = replaced_applists;
    replaced_applists = server_applist;
    server_applist = NULL;
  }
  if (gs_applist(&server, &server_applist) != GS_OK) {
    snprintf(err, err_len, "Can't read the app list from %s: %s", info->name, gs_error ? gs_error : "");
    return -1;
  }
  connection_paired();
  return 0;
}

int session_open_headless(device_info_t *info, char *err, size_t err_len) {
  vitapower_busy_begin();
  int ret = open_headless(info, err, err_len);
  vitapower_busy_end();
  return ret;
}

bool session_server_ready(void) {
  int status = connection_get_status();
  return server.paired && server_applist != NULL && (status == LI_READY || status == LI_PAIRED);
}

const char *session_server_name(void) {
  return current_host;
}

static bool contains_ignoring_case(const char *haystack, const char *needle) {
  size_t n = strlen(needle);
  for (; *haystack; haystack++) {
    if (strncasecmp(haystack, needle, n) == 0) {
      return true;
    }
  }
  return false;
}

int session_find_bench_app(const char *preferred, char *found, size_t found_len) {
  PAPP_LIST match = NULL;
  for (PAPP_LIST app = server_applist; app != NULL && match == NULL; app = app->next) {
    if (preferred && preferred[0] && strcasecmp(app->name, preferred) == 0) {
      match = app;
    }
  }
  for (PAPP_LIST app = server_applist; app != NULL && match == NULL; app = app->next) {
    if (contains_ignoring_case(app->name, "bench")) {
      match = app;
    }
  }
  if (match == NULL) {
    return -1;
  }
  snprintf(found, found_len, "%s", match->name);
  return match->id;
}

int session_find_app(const char *name) {
  for (PAPP_LIST app = server_applist; app != NULL; app = app->next) {
    if (strcasecmp(app->name, name) == 0) {
      return app->id;
    }
  }
  return -1;
}

int session_start_app_by_id(int app_id, char *err, size_t err_len) {
  // Auto-detect and the benchmark quit their app afterwards. Over a running
  // app, the host would resume that one instead, and they'd then close it.
  if (server.currentGame != 0) {
    snprintf(err, err_len, "An app is already running on %s. Quit it from the "
             "PC's menu first.", session_server_name());
    return -1;
  }
  vitapower_config(config);
  vitainput_config(config);
  connection_paired();
  // Count this stream's video packets afresh (src/netcount.c)
  netcount_reset();
  int ret = session_start_stream(app_id, err, err_len);
  if (ret != 0 && start_launched_app) {
    // The app started but its stream didn't: close it again, since the host
    // was idle before (this also clears server.currentGame)
    session_quit_app();
    session_return_to_server();
  }
  return ret;
}

void session_return_to_server(void) {
  // A finished stream leaves the connection disconnected; go back to the
  // state the host's menu expects
  if (connection_get_status() == LI_DISCONNECTED) {
    connection_reset();
  }
  connection_paired();
}

int session_quit_app(void) {
  int ret = gs_quit_app(&server);
  if (ret == GS_OK) {
    // As the menu's Quit does, so the apps can be started again
    server.currentGame = 0;
  }
  return ret;
}
