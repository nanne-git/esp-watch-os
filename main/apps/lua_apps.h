/*
 * Lua apps: third-party apps installed at runtime from the SD card.
 *
 *   /sdcard/apps/<id>/main.lua         required - the app's code
 *   /sdcard/apps/<id>/manifest.json    optional - {"name","icon","color","version","description","memory_kb"}
 *   /sdcard/apps/<id>/icon.png         optional - 80x80 launcher icon ("icon": "icon.png")
 *
 * Only one Lua app runs at a time, inside the LVGL task, sandboxed (no io/os/debug,
 * memory limit, 1.5 s time limit per callback). API reference: docs/APPS.md
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#define LUA_APPS_DIR "/sdcard/apps"

typedef struct {
    char     id[32];          /* folder name */
    char     name[32];
    char     icon[64];        /* LV_SYMBOL_* text, or "S:/sdcard/apps/<id>/icon.png" when icon_is_image */
    bool     icon_is_image;
    uint32_t color;
    char     version[16];
    char     description[96];
    int      memory_kb;
} lua_app_info_t;

/* any task: list installed apps (sorted by name). Returns the count. */
int  lua_apps_scan(lua_app_info_t *out, int max);
bool lua_apps_get(const char *id, lua_app_info_t *out);
cJSON *lua_apps_json(void);                 /* {"running": id|null, "apps": [...]} */

/* LVGL context: open an app page */
void lua_app_open(const char *id);

/* any task (take the UI lock themselves) */
bool lua_apps_run(const char *id);          /* open, or restart if already open (hot reload) */
void lua_apps_stop(void);                   /* close the running app */
bool lua_apps_remove(const char *id);       /* stop if running + delete its folder */
bool lua_apps_running(char *id_out, size_t size);
