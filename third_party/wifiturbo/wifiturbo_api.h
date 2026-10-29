/*
 * wifiturbo_api.h - the part of vita-turbo-wifi's SceWifiTurbo interface that
 * Moonlight calls.
 *
 * Copied from include/wifiturbo.h and include/wifiturbo_kernel_api.h of
 * vita-turbo-wifi v0.4.1 (https://github.com/juanjsebgarcia/vita-turbo-wifi),
 * Copyright (c) 2026 Juan Garcia Alvite, MIT licence (see LICENSE beside this
 * file). Only the declarations Moonlight uses are kept, with the plugin's
 * own comments trimmed. The structs cross the user/kernel syscall boundary,
 * so their layout must match the plugin's.
 *
 * The plugin is optional. These functions are linked weakly (see
 * wifiturbo_imports.yml), and src/wifiturbo.c treats anything but
 * WQ_SELFTEST_MAGIC from wqSelfTestPing() as "plugin not installed".
 */
#ifndef WIFITURBO_API_H
#define WIFITURBO_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Round-trip cookie returned by wqSelfTestPing(): 'WTB1' */
#define WQ_SELFTEST_MAGIC   0x57544231u

#define WQ_OK                   0

/* Power profiles, WqUiState.profile */
#define WQ_PROFILE_MAX_PERF    0
#define WQ_PROFILE_BALANCED    1
#define WQ_PROFILE_MAX_BATTERY 2

/* SNR / noise floor from the driver's RSSI_INFO replies */
typedef struct WqLinkInternals {
	uint8_t  valid;            /* 1 once a reply with real dBm readings is seen */
	int16_t  noise_floor_dbm;
	int16_t  beacon_rssi_dbm;
	int16_t  data_rssi_dbm;
	int16_t  snr_db;           /* beacon RSSI - noise floor                    */
} WqLinkInternals;

/* The settings the plugin's resident module last published */
typedef struct {
	int keepwifi;
	int hud;
	int autoreconnect;
	int roam;       /* Smart Mesh Roaming */
	int ht40;
	int profile;    /* WQ_PROFILE_* */
	int devmode;
	int rxring;     /* receive buffer: 0 default / 1 enhanced / 2 maximum */
	int hudpos;
	int hudgame;    /* In-game HUD: draws over games and apps */
} WqUiState;

/* wqUiSubmit copies this many bytes from its argument, whatever the string's
 * length, so callers must pass a buffer of at least this size */
#define WQ_UI_CMD_MAX 64

/* Always returns WQ_SELFTEST_MAGIC when the kernel module is loaded */
int wqSelfTestPing(void);
/* Reads the published settings. The kernel copies its own sizeof(WqUiState),
 * which grows with the plugin, so give it a larger buffer than ours. */
int wqUiReadState(WqUiState *out);
/* Queues a settings command ("roam off", "profile maxperf", ...) for the
 * plugin's resident module. The change is saved by the plugin. */
int wqUiSubmit(const char *line);
/* Only call while connected: the first call installs a kernel hook. Like
 * wqUiReadState, the kernel copies its own sizeof(WqLinkInternals) into out,
 * so give it a buffer larger than ours. */
int wqChipObserveGetLink(WqLinkInternals *out);

#ifdef __cplusplus
}
#endif

#endif /* WIFITURBO_API_H */
