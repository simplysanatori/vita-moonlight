#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device.h"
#include "client.h"

// The connection to a host: who it is, its apps, and starting and stopping
// streams, without any menus or dialogs. Errors come back as text for the
// caller to show however its UI does.

// The connected host, and its apps once read
extern SERVER_DATA server;
extern PAPP_LIST server_applist;

// Reads the connected host's app list again into server_applist, freeing
// the list it replaces. Call it only while no menu shows the old list's
// names. Returns GS_OK, or a GS_ error with the old list kept.
int session_refresh_applist(void);

// Records which host the connection is to, by the name it was paired under
void session_set_host(const char *name);

// The id of the app called name in list, or -1; and the name of app id,
// copied into name (returning 0 if there isn't one)
int get_app_id(PAPP_LIST list, char *name);
int get_app_name(PAPP_LIST list, int id, char *name);

// Starts streaming appId on the connected host. Returns 0, or a negative
// code with a message in err.
int session_start_stream(int appId, char *err, size_t err_len);

// Connects to a paired host and reads its app list, without any menus or
// dialogs. Returns 0, or a negative code with a message in err.
int session_open_headless(device_info_t *info, char *err, size_t err_len);
// True when connected to a host (its app list loaded) and nothing is streaming
bool session_server_ready(void);
// The connected host's name, or an empty string
const char *session_server_name(void);
// Finds the benchmark app on the connected host: an exact, case-insensitive
// match on preferred, otherwise the first app with "bench" in its name.
// Returns its id with its name in found, or -1 if there isn't one.
int session_find_bench_app(const char *preferred, char *found, size_t found_len);
// Finds an app on the connected host by name, ignoring case. Returns its id,
// or -1 if there isn't one.
int session_find_app(const char *name);
// Starts streaming an app on the connected host. Returns 0, or a negative
// code with a message in err.
int session_start_app_by_id(int app_id, char *err, size_t err_len);
// After a stream started without the host menu has ended, restores the
// connection state that menu expects
void session_return_to_server(void);
// Quits the app running on the host the last connection used
int session_quit_app(void);
