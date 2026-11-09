/*
 * Persistent settings (NVS). One struct, saved as a blob, plus a field table
 * so the web API can read/write settings as JSON by name.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#define SETTINGS_STR_SHORT 33
#define SETTINGS_STR_LONG  65

typedef struct {
    /* display */
    uint8_t  brightness;          /* 5..100 % */
    uint16_t screen_timeout_s;    /* 0 = never */
    bool     raise_to_wake;
    bool     always_on;           /* dim clock instead of black when idle */
    uint8_t  watchface;           /* 0 digital, 1 analog, 2 minimal */
    uint32_t accent_color;        /* 0xRRGGBB */

    /* sound & vibration */
    uint8_t  volume;              /* 0..100 */
    bool     muted;
    bool     vibration;
    bool     haptic_touch;

    /* date & time */
    char     timezone[SETTINGS_STR_SHORT];   /* POSIX TZ, e.g. "IST-5:30" */
    bool     time_24h;
    bool     auto_time;           /* NTP when online */

    /* wi-fi */
    bool     wifi_on;
    bool     hotspot_on;
    char     ap_ssid[SETTINGS_STR_SHORT];
    char     ap_pass[SETTINGS_STR_LONG];
    char     hostname[SETTINGS_STR_SHORT];

    /* bluetooth */
    bool     bt_on;
    char     bt_name[SETTINGS_STR_SHORT];   /* Gadgetbridge needs the "Bangle.js" prefix */

    /* web server / remote management */
    bool     web_on;
    char     web_pass[SETTINGS_STR_SHORT];  /* empty = no password */

    /* notifications */
    bool     notif_on;
    bool     notif_wake;
    bool     dnd;
    bool     dnd_scheduled;
    uint16_t dnd_start_min;       /* minutes after midnight */
    uint16_t dnd_end_min;

    /* power */
    bool     power_save;

    /* updates */
    char     ota_repo[SETTINGS_STR_LONG];   /* "owner/repo" */
    char     ota_token[100];                /* GitHub read-only token (private repo) */
    bool     ota_auto_check;

    /* weather & health */
    float    latitude;
    float    longitude;
    char     city[SETTINGS_STR_SHORT];
    uint16_t step_goal;
    bool     move_reminder;       /* nudge after an hour without walking (09-21 h) */
} settings_t;

typedef void (*settings_cb_t)(void);

void settings_init(void);

/* Current settings (read-only view). */
const settings_t *settings(void);

/* Change settings: get a writable copy, edit, then commit (saves + notifies listeners). */
settings_t *settings_edit(void);
void settings_commit(void);

void settings_on_change(settings_cb_t cb);
void settings_factory_reset(void);

/* JSON for the web API. Secrets (passwords/tokens) are replaced by "***" on output. */
cJSON *settings_to_json(void);
/* Apply known keys from a JSON object; returns number of fields changed. */
int settings_from_json(const cJSON *obj);
