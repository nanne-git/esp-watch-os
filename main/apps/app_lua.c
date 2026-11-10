/*
 * Lua app runtime. See lua_apps.h for the folder layout and docs/APPS.md for the API.
 *
 * Design rules:
 *  - Every Lua call happens in LVGL context (event callbacks, lv_timers) or while holding
 *    the UI lock, so the single lua_State is never used by two tasks at once.
 *  - All LVGL objects an app creates live under the app's root container. Closing the app
 *    first deletes those objects (their DELETE events drop the Lua references), then closes
 *    the Lua state - in that order, so no LVGL callback can reach freed Lua memory.
 *  - Errors never crash the watch: the app is stopped and the error is shown on its page.
 *    Stopping is deferred with lv_async_call because the error may happen deep inside a
 *    Lua call that is still running.
 */
#include "lua_apps.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "apps.h"
#include "audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "ui.h"
#include "weather.h"
#include "wifi_svc.h"

static const char *TAG = "luaapp";

#define TIME_LIMIT_MS   1500         /* one callback may run at most this long */
#define MAX_TIMERS      16
#define HTTP_MAX_BODY   (32 * 1024)
#define HTTP_MAX_JOBS   3
#define CONTENT_H       (UI_H - 64)

typedef enum { K_LABEL, K_BUTTON, K_BOX, K_SLIDER, K_SWITCH, K_BAR, K_ARC, K_IMAGE, K_ROW } kind_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *label;          /* text part (label itself, or a button's label) */
    kind_t    kind;
    int       on_click;       /* Lua registry refs, LUA_NOREF if unset */
    int       on_change;
    bool      click_cb, change_cb;
} lobj_t;

typedef struct {
    lv_timer_t *t;
    int         ref;
    uint32_t    id;
    bool        repeat;
} ltimer_t;

static struct {
    lua_State *L;
    uint32_t   gen;             /* bumps every time a Lua state is closed */
    bool       dead;            /* failed: no more Lua calls, error is being shown */
    bool       page_open;
    int        page_depth;      /* ui_page_depth() of our page */
    lv_obj_t  *root;
    char       id[32];
    char       dir[80];
    lua_app_info_t info;
    size_t     mem_used, mem_limit;
    int64_t    deadline;
    int        objs_ref;        /* registry table: lightuserdata(lv_obj) -> userdata */
    bool       awake_held;
    cJSON     *store;           /* storage.get/set, saved in data.json */
    char      *err;
    ltimer_t   timers[MAX_TIMERS];
    uint32_t   timer_seq;
} s_app = { .objs_ref = LUA_NOREF };

static volatile int s_http_jobs;

static void app_start(void);
static void app_stop_state(void);
static void show_error(void);

/* ======================================================================== helpers */

static bool valid_name(const char *n)
{
    if (!n || !n[0] || n[0] == '.' || strlen(n) > 60) return false;
    for (const char *p = n; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    }
    return true;
}

static uint32_t parse_color(const char *s, uint32_t def)
{
    if (!s) return def;
    if (*s == '#') s++;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    return end != s ? (uint32_t)v : def;
}

static const struct { const char *name; const char *sym; } SYMBOLS[] = {
    { "audio", LV_SYMBOL_AUDIO }, { "video", LV_SYMBOL_VIDEO }, { "list", LV_SYMBOL_LIST },
    { "ok", LV_SYMBOL_OK }, { "close", LV_SYMBOL_CLOSE }, { "power", LV_SYMBOL_POWER },
    { "settings", LV_SYMBOL_SETTINGS }, { "home", LV_SYMBOL_HOME }, { "download", LV_SYMBOL_DOWNLOAD },
    { "drive", LV_SYMBOL_DRIVE }, { "refresh", LV_SYMBOL_REFRESH }, { "mute", LV_SYMBOL_MUTE },
    { "volume", LV_SYMBOL_VOLUME_MAX }, { "image", LV_SYMBOL_IMAGE }, { "edit", LV_SYMBOL_EDIT },
    { "prev", LV_SYMBOL_PREV }, { "play", LV_SYMBOL_PLAY }, { "pause", LV_SYMBOL_PAUSE },
    { "stop", LV_SYMBOL_STOP }, { "next", LV_SYMBOL_NEXT }, { "eject", LV_SYMBOL_EJECT },
    { "left", LV_SYMBOL_LEFT }, { "right", LV_SYMBOL_RIGHT }, { "plus", LV_SYMBOL_PLUS },
    { "minus", LV_SYMBOL_MINUS }, { "eye", LV_SYMBOL_EYE_OPEN }, { "warning", LV_SYMBOL_WARNING },
    { "shuffle", LV_SYMBOL_SHUFFLE }, { "up", LV_SYMBOL_UP }, { "down", LV_SYMBOL_DOWN },
    { "loop", LV_SYMBOL_LOOP }, { "folder", LV_SYMBOL_DIRECTORY }, { "upload", LV_SYMBOL_UPLOAD },
    { "call", LV_SYMBOL_CALL }, { "cut", LV_SYMBOL_CUT }, { "copy", LV_SYMBOL_COPY },
    { "save", LV_SYMBOL_SAVE }, { "bars", LV_SYMBOL_BARS }, { "mail", LV_SYMBOL_ENVELOPE },
    { "charge", LV_SYMBOL_CHARGE }, { "paste", LV_SYMBOL_PASTE }, { "bell", LV_SYMBOL_BELL },
    { "keyboard", LV_SYMBOL_KEYBOARD }, { "gps", LV_SYMBOL_GPS }, { "file", LV_SYMBOL_FILE },
    { "wifi", LV_SYMBOL_WIFI }, { "battery", LV_SYMBOL_BATTERY_FULL }, { "bluetooth", LV_SYMBOL_BLUETOOTH },
    { "trash", LV_SYMBOL_TRASH }, { "backspace", LV_SYMBOL_BACKSPACE }, { "sd", LV_SYMBOL_SD_CARD },
};

static const char *symbol_by_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(SYMBOLS) / sizeof(SYMBOLS[0]); i++) {
        if (strcasecmp(SYMBOLS[i].name, name) == 0) return SYMBOLS[i].sym;
    }
    return NULL;
}

static const lv_font_t *font_by_size(int s)
{
    if (s <= 12) return &lv_font_montserrat_12;
    if (s <= 14) return &lv_font_montserrat_14;
    if (s <= 16) return &lv_font_montserrat_16;
    if (s <= 18) return &lv_font_montserrat_18;
    if (s <= 20) return &lv_font_montserrat_20;
    if (s <= 24) return &lv_font_montserrat_24;
    if (s <= 28) return &lv_font_montserrat_28;
    if (s <= 32) return &lv_font_montserrat_32;
    if (s <= 40) return &lv_font_montserrat_36;
    return &lv_font_montserrat_48;
}

static char *read_file(const char *path, size_t max, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    if (len_out) *len_out = n;
    return buf;
}

/* ======================================================================== app list */

static void read_manifest(const char *id, lua_app_info_t *a)
{
    memset(a, 0, sizeof(*a));
    strlcpy(a->id, id, sizeof(a->id));
    strlcpy(a->name, id, sizeof(a->name));
    a->color = 0x5E5CE6;
    a->memory_kb = 1024;
    strlcpy(a->version, "1.0", sizeof(a->version));

    char path[128];
    snprintf(path, sizeof(path), LUA_APPS_DIR "/%s/manifest.json", id);
    char *txt = read_file(path, 2048, NULL);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const char *icon = NULL;
    if (j) {
        cJSON *v;
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "name"))) strlcpy(a->name, v->valuestring, sizeof(a->name));
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "version"))) strlcpy(a->version, v->valuestring, sizeof(a->version));
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "description"))) strlcpy(a->description, v->valuestring, sizeof(a->description));
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "memory_kb"))) a->memory_kb = v->valueint;
        v = cJSON_GetObjectItem(j, "color");
        if (cJSON_IsString(v)) a->color = parse_color(v->valuestring, a->color);
        else if (cJSON_IsNumber(v)) a->color = (uint32_t)v->valuedouble;
        if (cJSON_IsString(v = cJSON_GetObjectItem(j, "icon"))) icon = v->valuestring;
    }
    if (a->memory_kb < 128) a->memory_kb = 128;
    if (a->memory_kb > 4096) a->memory_kb = 4096;

    const char *sym = symbol_by_name(icon);
    if (sym) {
        strlcpy(a->icon, sym, sizeof(a->icon));
    } else if (icon && valid_name(icon) && strcasestr(icon, ".png")) {
        snprintf(a->icon, sizeof(a->icon), "S:" LUA_APPS_DIR "/%s/%s", id, icon);
        a->icon_is_image = true;
    } else {
        /* default icon: first letter of the name */
        a->icon[0] = (char)((a->name[0] >= 'a' && a->name[0] <= 'z') ? a->name[0] - 32 : a->name[0]);
        a->icon[1] = '\0';
    }
    cJSON_Delete(j);
}

static int cmp_apps(const void *x, const void *y)
{
    return strcasecmp(((const lua_app_info_t *)x)->name, ((const lua_app_info_t *)y)->name);
}

int lua_apps_scan(lua_app_info_t *out, int max)
{
    if (!fm_is_mounted()) return 0;
    DIR *d = opendir(LUA_APPS_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    char path[128];
    struct stat st;
    while (n < max && (e = readdir(d)) != NULL) {
        if (!valid_name(e->d_name) || strlen(e->d_name) >= sizeof(out->id)) continue;
        snprintf(path, sizeof(path), LUA_APPS_DIR "/%s/main.lua", e->d_name);
        if (stat(path, &st) != 0) continue;
        read_manifest(e->d_name, &out[n++]);
    }
    closedir(d);
    qsort(out, n, sizeof(*out), cmp_apps);
    return n;
}

bool lua_apps_get(const char *id, lua_app_info_t *out)
{
    if (!valid_name(id) || !fm_is_mounted()) return false;
    char path[128];
    struct stat st;
    snprintf(path, sizeof(path), LUA_APPS_DIR "/%s/main.lua", id);
    if (stat(path, &st) != 0) return false;
    read_manifest(id, out);
    return true;
}

cJSON *lua_apps_json(void)
{
    cJSON *o = cJSON_CreateObject();
    char running[32];
    if (lua_apps_running(running, sizeof(running))) cJSON_AddStringToObject(o, "running", running);
    else cJSON_AddNullToObject(o, "running");
    cJSON *arr = cJSON_AddArrayToObject(o, "apps");
    lua_app_info_t *list = calloc(32, sizeof(*list));
    int n = list ? lua_apps_scan(list, 32) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "id", list[i].id);
        cJSON_AddStringToObject(a, "name", list[i].name);
        cJSON_AddStringToObject(a, "version", list[i].version);
        cJSON_AddStringToObject(a, "description", list[i].description);
        char c[8];
        snprintf(c, sizeof(c), "#%06lX", (unsigned long)(list[i].color & 0xFFFFFF));
        cJSON_AddStringToObject(a, "color", c);
        cJSON_AddNumberToObject(a, "memory_kb", list[i].memory_kb);
        cJSON_AddItemToArray(arr, a);
    }
    free(list);
    return o;
}

/* ======================================================================== sandbox: memory + time */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    if (!ptr) osize = 0;            /* for new blocks osize is a type tag, not a size */
    if (nsize == 0) {
        heap_caps_free(ptr);
        s_app.mem_used -= osize;
        return NULL;
    }
    if (nsize > osize && s_app.mem_used - osize + nsize > s_app.mem_limit) {
        return NULL;                /* Lua turns this into a "not enough memory" error */
    }
    void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) s_app.mem_used = s_app.mem_used - osize + nsize;
    return p;
}

static void l_hook(lua_State *L, lua_Debug *ar)
{
    if (esp_timer_get_time() > s_app.deadline) {
        luaL_error(L, "app is not responding (a callback ran longer than %d ms - endless loop?)", TIME_LIMIT_MS);
    }
}

static int l_traceback(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

static void fail_async(void *arg)
{
    if ((uint32_t)(uintptr_t)arg != s_app.gen || !s_app.L) return;
    app_stop_state();
    show_error();
}

static void app_fail(const char *msg)
{
    if (s_app.dead) return;
    s_app.dead = true;
    ESP_LOGE(TAG, "[%s] %s", s_app.id, msg ? msg : "?");
    free(s_app.err);
    s_app.err = strndup(msg ? msg : "unknown error", 1500);
    lv_async_call(fail_async, (void *)(uintptr_t)s_app.gen);
}

/* stack: function, nargs arguments. Pops them. Returns false on error. */
static bool app_pcall(int nargs)
{
    lua_State *L = s_app.L;
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, l_traceback);
    lua_insert(L, base);
    int64_t saved = s_app.deadline;
    s_app.deadline = esp_timer_get_time() + TIME_LIMIT_MS * 1000LL;
    int r = lua_pcall(L, nargs, 0, base);
    s_app.deadline = saved;               /* nested call: the outer one keeps its limit */
    if (r != LUA_OK) {
        app_fail(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_remove(L, base);
    return r == LUA_OK;
}

static bool can_call(void) { return s_app.L && !s_app.dead; }

/* call a Lua function stored in the registry */
static void call_ref(int ref, int nargs_on_stack)
{
    lua_State *L = s_app.L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (nargs_on_stack) lua_insert(L, -1 - nargs_on_stack);
    app_pcall(nargs_on_stack);
}

/* ======================================================================== objects */

#define OBJ_MT "watch.obj"

static void forget_obj(lobj_t *lo)
{
    lua_State *L = s_app.L;
    if (lo->on_click != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lo->on_click);
    if (lo->on_change != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lo->on_change);
    lo->on_click = lo->on_change = LUA_NOREF;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_app.objs_ref);
    lua_pushlightuserdata(L, lo->obj);
    lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lo->obj = NULL;
    lo->label = NULL;
}

static void obj_event_cb(lv_event_t *e)
{
    lobj_t *lo = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        if (s_app.L) forget_obj(lo);       /* only while the state is alive (see header) */
        return;
    }
    if (!can_call() || !lo->obj) return;
    lua_State *L = s_app.L;
    if (code == LV_EVENT_CLICKED && lo->on_click != LUA_NOREF) {
        call_ref(lo->on_click, 0);
    } else if (code == LV_EVENT_VALUE_CHANGED && lo->on_change != LUA_NOREF) {
        if (lo->kind == K_SWITCH) {
            lua_pushboolean(L, lv_obj_has_state(lo->obj, LV_STATE_CHECKED));
        } else if (lo->kind == K_ARC) {
            lua_pushinteger(L, lv_arc_get_value(lo->obj));
        } else {
            lua_pushinteger(L, lv_slider_get_value(lo->obj));
        }
        call_ref(lo->on_change, 1);
    }
}

/* wrap a new LVGL object: pushes the userdata */
static lobj_t *wrap(lua_State *L, lv_obj_t *obj, lv_obj_t *label, kind_t kind)
{
    lobj_t *lo = lua_newuserdatauv(L, sizeof(lobj_t), 0);
    memset(lo, 0, sizeof(*lo));
    lo->obj = obj;
    lo->label = label;
    lo->kind = kind;
    lo->on_click = lo->on_change = LUA_NOREF;
    luaL_setmetatable(L, OBJ_MT);
    /* keep it alive as long as the LVGL object exists */
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_app.objs_ref);
    lua_pushlightuserdata(L, obj);
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lv_obj_add_event_cb(obj, obj_event_cb, LV_EVENT_DELETE, lo);
    return lo;
}

static lobj_t *check_obj(lua_State *L, int i)
{
    lobj_t *lo = luaL_checkudata(L, i, OBJ_MT);
    if (!lo->obj) luaL_error(L, "this object was deleted");
    return lo;
}

static void set_click(lua_State *L, lobj_t *lo, int fn_idx)
{
    if (lo->on_click != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lo->on_click);
    lo->on_click = LUA_NOREF;
    if (lua_isfunction(L, fn_idx)) {
        lua_pushvalue(L, fn_idx);
        lo->on_click = luaL_ref(L, LUA_REGISTRYINDEX);
        lv_obj_add_flag(lo->obj, LV_OBJ_FLAG_CLICKABLE);
        if (!lo->click_cb) {
            lv_obj_add_event_cb(lo->obj, obj_event_cb, LV_EVENT_CLICKED, lo);
            lo->click_cb = true;
        }
    }
}

static void set_change(lua_State *L, lobj_t *lo, int fn_idx)
{
    if (lo->on_change != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lo->on_change);
    lo->on_change = LUA_NOREF;
    if (lua_isfunction(L, fn_idx)) {
        lua_pushvalue(L, fn_idx);
        lo->on_change = luaL_ref(L, LUA_REGISTRYINDEX);
        if (!lo->change_cb) {
            lv_obj_add_event_cb(lo->obj, obj_event_cb, LV_EVENT_VALUE_CHANGED, lo);
            lo->change_cb = true;
        }
    }
}

/* "50%", "content" or a number of pixels */
static bool opt_size(lua_State *L, int t, const char *key, int32_t *out)
{
    bool ok = true;
    int ty = lua_getfield(L, t, key);
    if (ty == LUA_TNUMBER) {
        *out = (int32_t)lua_tointeger(L, -1);
    } else if (ty == LUA_TSTRING) {
        const char *s = lua_tostring(L, -1);
        if (strcmp(s, "content") == 0) *out = LV_SIZE_CONTENT;
        else if (strchr(s, '%')) *out = lv_pct(atoi(s));
        else *out = atoi(s);
    } else {
        ok = false;
    }
    lua_pop(L, 1);
    return ok;
}

static bool opt_int(lua_State *L, int t, const char *key, lua_Integer *out)
{
    bool ok = lua_getfield(L, t, key) == LUA_TNUMBER;
    if (ok) *out = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return ok;
}

static const char *opt_str(lua_State *L, int t, const char *key, char *buf, size_t size)
{
    const char *r = NULL;
    if (lua_getfield(L, t, key) == LUA_TSTRING) {
        strlcpy(buf, lua_tostring(L, -1), size);
        r = buf;
    }
    lua_pop(L, 1);
    return r;
}

static lv_align_t align_by_name(const char *s)
{
    static const struct { const char *n; lv_align_t a; } A[] = {
        { "center", LV_ALIGN_CENTER }, { "top", LV_ALIGN_TOP_MID }, { "bottom", LV_ALIGN_BOTTOM_MID },
        { "left", LV_ALIGN_LEFT_MID }, { "right", LV_ALIGN_RIGHT_MID }, { "top_left", LV_ALIGN_TOP_LEFT },
        { "top_right", LV_ALIGN_TOP_RIGHT }, { "bottom_left", LV_ALIGN_BOTTOM_LEFT },
        { "bottom_right", LV_ALIGN_BOTTOM_RIGHT },
    };
    for (size_t i = 0; i < sizeof(A) / sizeof(A[0]); i++) {
        if (strcmp(A[i].n, s) == 0) return A[i].a;
    }
    return LV_ALIGN_CENTER;
}

/* style/layout options table: x y w h align color bg font radius pad gap layout border opacity text_align */
static void apply_opts(lua_State *L, int t, lobj_t *lo)
{
    if (!lua_istable(L, t)) return;
    lv_obj_t *o = lo->obj;
    lv_obj_t *txt = lo->label ? lo->label : o;
    int32_t w, h;
    lua_Integer v, x = 0, y = 0;
    char s[24];
    bool has_w = opt_size(L, t, "w", &w), has_h = opt_size(L, t, "h", &h);
    if (has_w) lv_obj_set_width(o, w);
    if (has_h) lv_obj_set_height(o, h);
    if (has_w && lo->kind == K_LABEL) lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    bool hx = opt_int(L, t, "x", &x), hy = opt_int(L, t, "y", &y);
    if (opt_str(L, t, "align", s, sizeof(s))) lv_obj_align(o, align_by_name(s), (int32_t)x, (int32_t)y);
    else if (hx || hy) lv_obj_set_pos(o, (int32_t)x, (int32_t)y);
    if (opt_int(L, t, "color", &v)) lv_obj_set_style_text_color(txt, lv_color_hex((uint32_t)v), 0);
    if (opt_int(L, t, "bg", &v)) {
        lv_obj_set_style_bg_color(o, lv_color_hex((uint32_t)v), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    }
    if (opt_int(L, t, "font", &v)) lv_obj_set_style_text_font(txt, font_by_size((int)v), 0);
    if (opt_int(L, t, "radius", &v)) lv_obj_set_style_radius(o, (int32_t)v, 0);
    if (opt_int(L, t, "pad", &v)) lv_obj_set_style_pad_all(o, (int32_t)v, 0);
    if (opt_int(L, t, "gap", &v)) {
        lv_obj_set_style_pad_row(o, (int32_t)v, 0);
        lv_obj_set_style_pad_column(o, (int32_t)v, 0);
    }
    if (opt_int(L, t, "border", &v)) {
        lv_obj_set_style_border_color(o, lv_color_hex((uint32_t)v), 0);
        lv_obj_set_style_border_width(o, 2, 0);
    }
    if (opt_int(L, t, "opacity", &v)) lv_obj_set_style_opa(o, (lv_opa_t)(v < 0 ? 0 : v > 255 ? 255 : v), 0);
    if (opt_str(L, t, "text_align", s, sizeof(s))) {
        lv_obj_set_style_text_align(txt, strcmp(s, "center") == 0 ? LV_TEXT_ALIGN_CENTER :
                                    strcmp(s, "right") == 0 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    }
    if (opt_str(L, t, "layout", s, sizeof(s))) {
        bool row = strcmp(s, "row") == 0;
        lv_obj_set_flex_flow(o, row ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    if (lua_getfield(L, t, "scroll") == LUA_TBOOLEAN) {
        if (lua_toboolean(L, -1)) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(o, LV_DIR_VER);
        } else {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
    lua_pop(L, 1);
}

/* parent from opts.parent, else the app's root */
static lv_obj_t *opt_parent(lua_State *L, int t)
{
    lv_obj_t *p = s_app.root;
    if (lua_istable(L, t)) {
        if (lua_getfield(L, t, "parent") != LUA_TNIL) p = check_obj(L, -1)->obj;
        lua_pop(L, 1);
    }
    return p;
}

/* ---- object methods ---- */

static int m_text(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    if (lo->kind == K_ROW) {
        ui_row_set_value(lo->obj, luaL_optstring(L, 2, ""));
        lua_settop(L, 1);
        return 1;
    }
    if (!lo->label) return luaL_error(L, "this object has no text");
    if (lua_isnoneornil(L, 2)) {
        lua_pushstring(L, lv_label_get_text(lo->label));
        return 1;
    }
    lv_label_set_text(lo->label, luaL_tolstring(L, 2, NULL));
    lua_settop(L, 1);
    return 1;
}

static int m_value(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    bool set = !lua_isnoneornil(L, 2);
    switch (lo->kind) {
    case K_SWITCH:
        if (!set) { lua_pushboolean(L, lv_obj_has_state(lo->obj, LV_STATE_CHECKED)); return 1; }
        if (lua_toboolean(L, 2)) lv_obj_add_state(lo->obj, LV_STATE_CHECKED);
        else lv_obj_remove_state(lo->obj, LV_STATE_CHECKED);
        break;
    case K_SLIDER:
        if (!set) { lua_pushinteger(L, lv_slider_get_value(lo->obj)); return 1; }
        lv_slider_set_value(lo->obj, (int32_t)luaL_checkinteger(L, 2), LV_ANIM_OFF);
        break;
    case K_BAR:
        if (!set) { lua_pushinteger(L, lv_bar_get_value(lo->obj)); return 1; }
        lv_bar_set_value(lo->obj, (int32_t)luaL_checkinteger(L, 2), LV_ANIM_ON);
        break;
    case K_ARC:
        if (!set) { lua_pushinteger(L, lv_arc_get_value(lo->obj)); return 1; }
        lv_arc_set_value(lo->obj, (int32_t)luaL_checkinteger(L, 2));
        break;
    default:
        return luaL_error(L, "value() works on slider, switch, bar and arc");
    }
    lua_settop(L, 1);
    return 1;
}

static int m_set(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    apply_opts(L, 2, lo);
    lua_settop(L, 1);
    return 1;
}

static int m_color(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    lv_obj_set_style_text_color(lo->label ? lo->label : lo->obj, lv_color_hex((uint32_t)luaL_checkinteger(L, 2)), 0);
    lua_settop(L, 1);
    return 1;
}

static int m_bg(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    lv_obj_set_style_bg_color(lo->obj, lv_color_hex((uint32_t)luaL_checkinteger(L, 2)), 0);
    lv_obj_set_style_bg_opa(lo->obj, LV_OPA_COVER, 0);
    lua_settop(L, 1);
    return 1;
}

static int m_pos(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    lv_obj_set_pos(lo->obj, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3));
    lua_settop(L, 1);
    return 1;
}

static int m_size(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    lv_obj_set_size(lo->obj, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3));
    lua_settop(L, 1);
    return 1;
}

static int m_align(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    lv_obj_align(lo->obj, align_by_name(luaL_checkstring(L, 2)), (int32_t)luaL_optinteger(L, 3, 0),
                 (int32_t)luaL_optinteger(L, 4, 0));
    lua_settop(L, 1);
    return 1;
}

static int m_show(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    bool on = lua_isnone(L, 2) || lua_toboolean(L, 2);
    if (on) lv_obj_remove_flag(lo->obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(lo->obj, LV_OBJ_FLAG_HIDDEN);
    lua_settop(L, 1);
    return 1;
}

static int m_hide(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    lv_obj_add_flag(lo->obj, LV_OBJ_FLAG_HIDDEN);
    lua_settop(L, 1);
    return 1;
}

static int m_on_click(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    set_click(L, lo, 2);
    lua_settop(L, 1);
    return 1;
}

static int m_on_change(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    set_change(L, lo, 2);
    lua_settop(L, 1);
    return 1;
}

static int m_clear(lua_State *L)
{
    lobj_t *lo = check_obj(L, 1);
    lv_obj_clean(lo->obj);
    lua_settop(L, 1);
    return 1;
}

static int m_delete(lua_State *L)
{
    lobj_t *lo = luaL_checkudata(L, 1, OBJ_MT);
    if (lo->obj) lv_obj_delete(lo->obj);     /* DELETE event forgets it */
    return 0;
}

static int m_exists(lua_State *L)
{
    lobj_t *lo = luaL_checkudata(L, 1, OBJ_MT);
    lua_pushboolean(L, lo->obj != NULL);
    return 1;
}

static const luaL_Reg OBJ_METHODS[] = {
    { "text", m_text }, { "value", m_value }, { "set", m_set }, { "color", m_color }, { "bg", m_bg },
    { "pos", m_pos }, { "size", m_size }, { "align", m_align }, { "show", m_show }, { "hide", m_hide },
    { "on_click", m_on_click }, { "on_change", m_on_change }, { "clear", m_clear },
    { "delete", m_delete }, { "exists", m_exists }, { NULL, NULL },
};

/* ======================================================================== ui.* */

static lobj_t *new_label(lua_State *L, lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
    return wrap(L, l, l, K_LABEL);
}

static int ui_l_label(lua_State *L)
{
    const char *text = luaL_optstring(L, 1, "");
    lobj_t *lo = new_label(L, opt_parent(L, 2), text);
    apply_opts(L, 2, lo);
    return 1;
}

static int ui_l_button(lua_State *L)
{
    const char *text = luaL_optstring(L, 1, "");
    lv_obj_t *b = lv_button_create(opt_parent(L, 3));
    lv_obj_set_style_bg_color(b, ui_accent(), 0);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_pad_ver(b, 16, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    lobj_t *lo = wrap(L, b, l, K_BUTTON);
    set_click(L, lo, 2);
    apply_opts(L, 3, lo);
    return 1;
}

static int ui_l_box(lua_State *L)
{
    lv_obj_t *o = lv_obj_create(opt_parent(L, 1));
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(o, lv_color_hex(UI_TEXT), 0);
    lobj_t *lo = wrap(L, o, NULL, K_BOX);
    apply_opts(L, 1, lo);
    return 1;
}

static int ui_l_slider(lua_State *L)
{
    int32_t mn = (int32_t)luaL_checkinteger(L, 1), mx = (int32_t)luaL_checkinteger(L, 2);
    int32_t val = (int32_t)luaL_optinteger(L, 3, mn);
    lv_obj_t *s = lv_slider_create(opt_parent(L, 5));
    lv_obj_set_width(s, 300);
    lv_slider_set_range(s, mn, mx);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, ui_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lobj_t *lo = wrap(L, s, NULL, K_SLIDER);
    set_change(L, lo, 4);
    apply_opts(L, 5, lo);
    return 1;
}

static int ui_l_switch(lua_State *L)
{
    lv_obj_t *s = lv_switch_create(opt_parent(L, 3));
    lv_obj_set_size(s, 72, 40);
    lv_obj_set_style_bg_color(s, ui_accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (lua_toboolean(L, 1)) lv_obj_add_state(s, LV_STATE_CHECKED);
    lobj_t *lo = wrap(L, s, NULL, K_SWITCH);
    set_change(L, lo, 2);
    apply_opts(L, 3, lo);
    return 1;
}

static int ui_l_bar(lua_State *L)
{
    lv_obj_t *b = lv_bar_create(opt_parent(L, 4));
    lv_obj_set_size(b, 300, 16);
    lv_bar_set_range(b, (int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2));
    lv_bar_set_value(b, (int32_t)luaL_optinteger(L, 3, 0), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, ui_accent(), LV_PART_INDICATOR);
    lobj_t *lo = wrap(L, b, NULL, K_BAR);
    apply_opts(L, 4, lo);
    return 1;
}

static int ui_l_arc(lua_State *L)
{
    lv_obj_t *a = lv_arc_create(opt_parent(L, 5));
    lv_obj_set_size(a, 220, 220);
    lv_arc_set_rotation(a, 135);
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, (int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2));
    lv_arc_set_value(a, (int32_t)luaL_optinteger(L, 3, 0));
    lv_obj_set_style_arc_color(a, ui_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, 18, LV_PART_MAIN);
    lobj_t *lo = wrap(L, a, NULL, K_ARC);
    if (lua_isfunction(L, 4)) {
        set_change(L, lo, 4);
    } else {                                   /* display only */
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    }
    apply_opts(L, 5, lo);
    return 1;
}

static int ui_l_image(lua_State *L)
{
    const char *file = luaL_checkstring(L, 1);
    if (!valid_name(file)) return luaL_error(L, "image: use a file name inside the app folder");
    char src[128];
    snprintf(src, sizeof(src), "S:%s/%s", s_app.dir, file);
    lv_obj_t *img = lv_image_create(opt_parent(L, 2));
    lv_image_set_src(img, src);
    lobj_t *lo = wrap(L, img, NULL, K_IMAGE);
    apply_opts(L, 2, lo);
    return 1;
}

/* ui.row(text, {value=, icon=, on_click=, parent=}) - a settings-style list row */
static int ui_l_row(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    char icon_name[24] = "", value[64] = "";
    bool t = lua_istable(L, 2);
    if (t) {
        opt_str(L, 2, "icon", icon_name, sizeof(icon_name));
        opt_str(L, 2, "value", value, sizeof(value));
    }
    lv_obj_t *parent = opt_parent(L, 2);
    lv_obj_t *r = ui_row(parent, symbol_by_name(icon_name), lv_color_to_u32(ui_accent()) & 0xFFFFFF, text,
                         value[0] ? value : NULL, NULL, NULL);
    lobj_t *lo = wrap(L, r, NULL, K_ROW);
    if (t) {
        lua_getfield(L, 2, "on_click");
        set_click(L, lo, lua_gettop(L));
        lua_pop(L, 1);
    }
    return 1;
}

/* ui.list() - full-page scrolling column, the usual parent for rows */
static int ui_l_list(lua_State *L)
{
    lv_obj_t *o = lv_obj_create(opt_parent(L, 1));
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(o, 16, 0);
    lv_obj_set_style_pad_bottom(o, 40, 0);
    lv_obj_set_style_pad_row(o, 8, 0);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lobj_t *lo = wrap(L, o, NULL, K_BOX);
    apply_opts(L, 1, lo);
    return 1;
}

static int ui_l_toast(lua_State *L)
{
    ui_toast(luaL_checkstring(L, 1));
    return 0;
}

static int ui_l_clear(lua_State *L)
{
    lv_obj_clean(s_app.root);
    return 0;
}

static int ui_l_icon(lua_State *L)
{
    const char *s = symbol_by_name(luaL_checkstring(L, 1));
    lua_pushstring(L, s ? s : "?");
    return 1;
}

static int ui_l_accent(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)lv_color_to_u32(ui_accent()) & 0xFFFFFF);
    return 1;
}

/* pending dialog callbacks (keyboard / confirm / choose) */
typedef struct {
    uint32_t gen;
    int      ref;
} pending_t;

static pending_t *pending_new(lua_State *L, int fn_idx)
{
    luaL_checktype(L, fn_idx, LUA_TFUNCTION);
    pending_t *p = malloc(sizeof(*p));
    if (!p) luaL_error(L, "out of memory");
    lua_pushvalue(L, fn_idx);
    p->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    p->gen = s_app.gen;
    return p;
}

static bool pending_take(pending_t *p)
{
    bool ok = p->gen == s_app.gen && can_call();
    if (ok) {
        lua_rawgeti(s_app.L, LUA_REGISTRYINDEX, p->ref);
        luaL_unref(s_app.L, LUA_REGISTRYINDEX, p->ref);
    }
    free(p);
    return ok;      /* true: the function is on the stack */
}

static void kb_done(const char *text, void *user)
{
    if (pending_take(user)) {
        lua_pushstring(s_app.L, text);
        app_pcall(1);
    }
}

static int ui_l_keyboard(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    const char *init = luaL_optstring(L, 2, "");
    pending_t *p = pending_new(L, 3);
    ui_keyboard(title, init, false, kb_done, p);
    return 0;
}

static void confirm_yes(void *user)
{
    if (pending_take(user)) app_pcall(0);
}

static int ui_l_confirm(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    pending_t *p = pending_new(L, 3);
    ui_confirm(title, msg, luaL_optstring(L, 4, "OK"), UI_BLUE, confirm_yes, p);
    return 0;
}

static void choose_done(int index, void *user)
{
    if (pending_take(user)) {
        lua_pushinteger(s_app.L, index + 1);
        app_pcall(1);
    }
}

static int ui_l_choose(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = (int)luaL_len(L, 2);
    if (n < 1 || n > 20) return luaL_error(L, "choose: 1..20 options");
    /* ui_choose keeps the pointers until the page closes: store copies in the dialog's pending */
    static char opts_buf[20][40];
    static const char *opts[20];
    for (int i = 0; i < n; i++) {
        lua_geti(L, 2, i + 1);
        strlcpy(opts_buf[i], luaL_tolstring(L, -1, NULL), sizeof(opts_buf[i]));
        lua_pop(L, 2);
        opts[i] = opts_buf[i];
    }
    int current = (int)luaL_optinteger(L, 4, 1) - 1;
    pending_t *p = pending_new(L, 3);
    ui_choose(title, opts, n, current, choose_done, p);
    return 0;
}

static const luaL_Reg UI_FUNCS[] = {
    { "label", ui_l_label }, { "button", ui_l_button }, { "box", ui_l_box }, { "slider", ui_l_slider },
    { "switch", ui_l_switch }, { "bar", ui_l_bar }, { "arc", ui_l_arc }, { "image", ui_l_image },
    { "row", ui_l_row }, { "list", ui_l_list }, { "toast", ui_l_toast }, { "clear", ui_l_clear },
    { "icon", ui_l_icon }, { "accent", ui_l_accent }, { "keyboard", ui_l_keyboard },
    { "confirm", ui_l_confirm }, { "choose", ui_l_choose }, { NULL, NULL },
};

/* ======================================================================== timer.* */

static void timer_cb(lv_timer_t *t)
{
    ltimer_t *lt = lv_timer_get_user_data(t);
    if (!can_call()) return;
    int ref = lt->ref;
    if (!lt->repeat) {
        lv_timer_delete(lt->t);
        lt->t = NULL;
        lua_rawgeti(s_app.L, LUA_REGISTRYINDEX, ref);
        luaL_unref(s_app.L, LUA_REGISTRYINDEX, ref);
        lt->ref = LUA_NOREF;
        app_pcall(0);
    } else {
        call_ref(ref, 0);
    }
}

static int timer_new(lua_State *L, bool repeat)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 20) ms = 20;
    for (int i = 0; i < MAX_TIMERS; i++) {
        ltimer_t *lt = &s_app.timers[i];
        if (lt->t) continue;
        lua_pushvalue(L, 2);
        lt->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lt->repeat = repeat;
        lt->id = ++s_app.timer_seq;
        lt->t = lv_timer_create(timer_cb, (uint32_t)ms, lt);
        lua_pushinteger(L, lt->id);
        return 1;
    }
    return luaL_error(L, "too many timers (max %d)", MAX_TIMERS);
}

static int timer_every(lua_State *L) { return timer_new(L, true); }
static int timer_after(lua_State *L) { return timer_new(L, false); }

static int timer_cancel(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    for (int i = 0; i < MAX_TIMERS; i++) {
        ltimer_t *lt = &s_app.timers[i];
        if (lt->t && lt->id == (uint32_t)id) {
            lv_timer_delete(lt->t);
            lt->t = NULL;
            luaL_unref(L, LUA_REGISTRYINDEX, lt->ref);
            lt->ref = LUA_NOREF;
        }
    }
    return 0;
}

static const luaL_Reg TIMER_FUNCS[] = {
    { "every", timer_every }, { "after", timer_after }, { "cancel", timer_cancel }, { NULL, NULL },
};

/* ======================================================================== watch.* */

static int w_vibrate(lua_State *L)
{
    lua_Integer ms = luaL_optinteger(L, 1, 40);
    haptic_pulse((uint16_t)(ms < 1 ? 1 : ms > 1000 ? 1000 : ms));
    return 0;
}

static int w_beep(lua_State *L)
{
    lua_Integer hz = luaL_optinteger(L, 1, 1000), ms = luaL_optinteger(L, 2, 100);
    audio_beep((int)(hz < 100 ? 100 : hz > 8000 ? 8000 : hz), (int)(ms < 10 ? 10 : ms > 2000 ? 2000 : ms));
    return 0;
}

static void set_int(lua_State *L, const char *k, lua_Integer v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); }
static void set_num(lua_State *L, const char *k, lua_Number v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); }
static void set_bool(lua_State *L, const char *k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); }
static void set_str(lua_State *L, const char *k, const char *v) { lua_pushstring(L, v); lua_setfield(L, -2, k); }

static int w_time(lua_State *L)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    lua_createtable(L, 0, 9);
    set_int(L, "hour", lt.tm_hour);
    set_int(L, "min", lt.tm_min);
    set_int(L, "sec", lt.tm_sec);
    set_int(L, "day", lt.tm_mday);
    set_int(L, "month", lt.tm_mon + 1);
    set_int(L, "year", lt.tm_year + 1900);
    set_int(L, "wday", lt.tm_wday == 0 ? 7 : lt.tm_wday);   /* 1 = Monday .. 7 = Sunday */
    set_int(L, "yday", lt.tm_yday + 1);
    set_int(L, "epoch", (lua_Integer)now);
    return 1;
}

static int w_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int w_battery(lua_State *L)
{
    power_status_t p;
    power_get(&p);
    lua_createtable(L, 0, 5);
    set_int(L, "percent", p.percent);
    set_bool(L, "charging", p.charging);
    set_bool(L, "usb", p.usb);
    set_int(L, "voltage", p.voltage_mv);
    set_num(L, "temp", p.temp_c);
    return 1;
}

static int w_steps(lua_State *L)
{
    lua_pushinteger(L, imu_steps_today());
    return 1;
}

static int w_weather(lua_State *L)
{
    weather_t w;
    weather_get(&w);
    lua_createtable(L, 0, 7);
    set_bool(L, "valid", w.valid);
    set_num(L, "temp", w.temp_c);
    set_num(L, "feels", w.feels_c);
    set_num(L, "high", w.tmax);
    set_num(L, "low", w.tmin);
    set_int(L, "humidity", w.humidity);
    set_str(L, "text", weather_desc(w.code));
    return 1;
}

static int w_wifi(lua_State *L)
{
    wifi_status_t ws;
    wifi_svc_status(&ws);
    lua_createtable(L, 0, 4);
    set_bool(L, "connected", ws.state == WIFI_ST_CONNECTED);
    set_str(L, "ssid", ws.ssid);
    set_str(L, "ip", ws.ip);
    set_int(L, "rssi", ws.rssi);
    return 1;
}

static int w_keep_awake(lua_State *L)
{
    bool on = lua_toboolean(L, 1);
    if (on != s_app.awake_held) {
        ui_keep_awake(on);
        s_app.awake_held = on;
    }
    return 0;
}

static const luaL_Reg WATCH_FUNCS[] = {
    { "vibrate", w_vibrate }, { "beep", w_beep }, { "time", w_time }, { "millis", w_millis },
    { "battery", w_battery }, { "steps", w_steps }, { "weather", w_weather }, { "wifi", w_wifi },
    { "keep_awake", w_keep_awake }, { NULL, NULL },
};

/* ======================================================================== json.* */

static cJSON *lua_to_json(lua_State *L, int idx, int depth)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: return cJSON_CreateNull();
    case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER: return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING: return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        if (depth > 16) luaL_error(L, "json: nested too deeply");
        lua_Integer n = luaL_len(L, idx);
        if (n > 0) {
            cJSON *a = cJSON_CreateArray();
            for (lua_Integer i = 1; i <= n; i++) {
                lua_geti(L, idx, i);
                cJSON_AddItemToArray(a, lua_to_json(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return a;
        }
        cJSON *o = cJSON_CreateObject();
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                cJSON_AddItemToObject(o, lua_tostring(L, -2), lua_to_json(L, -1, depth + 1));
            }
            lua_pop(L, 1);
        }
        return o;
    }
    default:
        return cJSON_CreateNull();
    }
}

static void json_to_lua(lua_State *L, const cJSON *j, int depth)
{
    luaL_checkstack(L, 4, "json");
    if (!j || cJSON_IsNull(j) || depth > 32) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(j)) {
        lua_pushboolean(L, cJSON_IsTrue(j));
    } else if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        if (d == (double)(lua_Integer)d) lua_pushinteger(L, (lua_Integer)d);
        else lua_pushnumber(L, d);
    } else if (cJSON_IsString(j)) {
        lua_pushstring(L, j->valuestring);
    } else if (cJSON_IsArray(j)) {
        lua_createtable(L, cJSON_GetArraySize(j), 0);
        int i = 1;
        for (const cJSON *c = j->child; c; c = c->next) {
            json_to_lua(L, c, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else {
        lua_createtable(L, 0, 4);
        for (const cJSON *c = j->child; c; c = c->next) {
            json_to_lua(L, c, depth + 1);
            lua_setfield(L, -2, c->string);
        }
    }
}

static int json_encode(lua_State *L)
{
    cJSON *j = lua_to_json(L, 1, 0);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return luaL_error(L, "json: out of memory");
    lua_pushstring(L, s);
    free(s);
    return 1;
}

static int json_decode(lua_State *L)
{
    cJSON *j = cJSON_Parse(luaL_checkstring(L, 1));
    if (!j) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid JSON");
        return 2;
    }
    json_to_lua(L, j, 0);
    cJSON_Delete(j);
    return 1;
}

static const luaL_Reg JSON_FUNCS[] = { { "encode", json_encode }, { "decode", json_decode }, { NULL, NULL } };

/* ======================================================================== storage.* */

static void store_load(void)
{
    if (s_app.store) return;
    char path[128];
    snprintf(path, sizeof(path), "%s/data.json", s_app.dir);
    char *txt = read_file(path, 64 * 1024, NULL);
    s_app.store = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (!cJSON_IsObject(s_app.store)) {
        cJSON_Delete(s_app.store);
        s_app.store = cJSON_CreateObject();
    }
}

static bool store_save(void)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/data.json", s_app.dir);
    char *s = cJSON_PrintUnformatted(s_app.store);
    if (!s) return false;
    FILE *f = fopen(path, "wb");
    bool ok = f && fputs(s, f) >= 0;
    if (f) fclose(f);
    free(s);
    return ok;
}

static int st_get(lua_State *L)
{
    const char *k = luaL_checkstring(L, 1);
    store_load();
    cJSON *v = cJSON_GetObjectItem(s_app.store, k);
    if (v) json_to_lua(L, v, 0);
    else lua_pushvalue(L, 2);       /* default */
    return 1;
}

static int st_set(lua_State *L)
{
    const char *k = luaL_checkstring(L, 1);
    store_load();
    cJSON_DeleteItemFromObject(s_app.store, k);
    if (!lua_isnil(L, 2)) cJSON_AddItemToObject(s_app.store, k, lua_to_json(L, 2, 0));
    lua_pushboolean(L, store_save());
    return 1;
}

static bool file_path(lua_State *L, int i, char *out, size_t size)
{
    const char *name = luaL_checkstring(L, i);
    if (!valid_name(name)) luaL_error(L, "file name must be inside the app folder: %s", name);
    snprintf(out, size, "%s/%s", s_app.dir, name);
    return true;
}

static int st_read(lua_State *L)
{
    char path[128];
    file_path(L, 1, path, sizeof(path));
    size_t n;
    char *txt = read_file(path, 256 * 1024, &n);
    if (!txt) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, txt, n);
    free(txt);
    return 1;
}

static int st_write_mode(lua_State *L, const char *mode)
{
    char path[128];
    file_path(L, 1, path, sizeof(path));
    size_t n;
    const char *data = luaL_checklstring(L, 2, &n);
    FILE *f = fopen(path, mode);
    bool ok = f && fwrite(data, 1, n, f) == n;
    if (f) fclose(f);
    lua_pushboolean(L, ok);
    return 1;
}

static int st_write(lua_State *L) { return st_write_mode(L, "wb"); }
static int st_append(lua_State *L) { return st_write_mode(L, "ab"); }

static int st_remove(lua_State *L)
{
    char path[128];
    file_path(L, 1, path, sizeof(path));
    lua_pushboolean(L, remove(path) == 0);
    return 1;
}

static const luaL_Reg STORAGE_FUNCS[] = {
    { "get", st_get }, { "set", st_set }, { "read", st_read }, { "write", st_write },
    { "append", st_append }, { "remove", st_remove }, { NULL, NULL },
};

/* ======================================================================== http.* (async) */

typedef struct {
    uint32_t gen;
    int      ref;
    char    *url;
    char    *body;        /* POST body or NULL */
    char    *ctype;
    char    *resp;
    size_t   len;
    bool     truncated;
} http_job_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    http_job_t *j = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && j->resp) {
        size_t room = HTTP_MAX_BODY - j->len;
        size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        if (n < (size_t)e->data_len) j->truncated = true;
        memcpy(j->resp + j->len, e->data, n);
        j->len += n;
    }
    return ESP_OK;
}

static void http_task(void *arg)
{
    http_job_t *j = arg;
    j->resp = heap_caps_malloc(HTTP_MAX_BODY + 1, MALLOC_CAP_SPIRAM);
    int status = -1;
    esp_http_client_config_t cfg = {
        .url = j->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .event_handler = http_evt,
        .user_data = j,
        .method = j->body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c && j->resp) {
        if (j->body) {
            esp_http_client_set_header(c, "Content-Type", j->ctype ? j->ctype : "application/json");
            esp_http_client_set_post_field(c, j->body, (int)strlen(j->body));
        }
        if (esp_http_client_perform(c) == ESP_OK) status = esp_http_client_get_status_code(c);
    }
    if (c) esp_http_client_cleanup(c);
    if (j->resp) j->resp[j->len] = '\0';

    if (ui_lock()) {
        if (j->gen == s_app.gen && can_call()) {
            lua_State *L = s_app.L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, j->ref);
            luaL_unref(L, LUA_REGISTRYINDEX, j->ref);
            lua_pushinteger(L, status);
            if (j->resp && status > 0) lua_pushlstring(L, j->resp, j->len);
            else lua_pushnil(L);
            app_pcall(2);
        }
        ui_unlock();
    }
    free(j->resp);
    free(j->url);
    free(j->body);
    free(j->ctype);
    free(j);
    s_http_jobs--;
    vTaskDelete(NULL);
}

static int http_start(lua_State *L, const char *url, const char *body, const char *ctype, int fn_idx)
{
    luaL_checktype(L, fn_idx, LUA_TFUNCTION);
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return luaL_error(L, "http: url must start with http:// or https://");
    }
    if (s_http_jobs >= HTTP_MAX_JOBS) return luaL_error(L, "http: too many requests at once");
    http_job_t *j = calloc(1, sizeof(*j));
    if (!j) return luaL_error(L, "out of memory");
    j->url = strdup(url);
    j->body = body ? strdup(body) : NULL;
    j->ctype = ctype ? strdup(ctype) : NULL;
    j->gen = s_app.gen;
    lua_pushvalue(L, fn_idx);
    j->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_http_jobs++;
    if (xTaskCreate(http_task, "lua_http", 8192, j, 3, NULL) != pdPASS) {
        s_http_jobs--;
        luaL_unref(L, LUA_REGISTRYINDEX, j->ref);
        free(j->url);
        free(j->body);
        free(j->ctype);
        free(j);
        return luaL_error(L, "http: out of memory");
    }
    return 0;
}

/* http.get(url, function(status, body) end) */
static int http_get(lua_State *L) { return http_start(L, luaL_checkstring(L, 1), NULL, NULL, 2); }

/* http.post(url, body, function(status, body) end [, content_type]) */
static int http_post(lua_State *L)
{
    return http_start(L, luaL_checkstring(L, 1), luaL_checkstring(L, 2), luaL_optstring(L, 4, NULL), 3);
}

static const luaL_Reg HTTP_FUNCS[] = { { "get", http_get }, { "post", http_post }, { NULL, NULL } };

/* ======================================================================== app.*, print, require, os */

static void exit_async(void *arg)
{
    if ((uint32_t)(uintptr_t)arg == s_app.gen && s_app.page_open && ui_page_depth() == s_app.page_depth) {
        ui_page_pop();
    }
}

static int a_exit(lua_State *L)
{
    lv_async_call(exit_async, (void *)(uintptr_t)s_app.gen);
    return 0;
}

static int a_memory(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(s_app.mem_used / 1024));
    lua_pushinteger(L, (lua_Integer)(s_app.mem_limit / 1024));
    return 2;
}

static int l_print(lua_State *L)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    ESP_LOGI("app", "[%s] %s", s_app.id, lua_tostring(L, -1));
    return 0;
}

/* require("name") loads name.lua from the app folder (text only, cached) */
static int l_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "app_loaded");
    if (lua_getfield(L, -1, name) != LUA_TNIL) return 1;
    lua_pop(L, 1);
    char file[72], path[160];
    snprintf(file, sizeof(file), "%s.lua", name);
    if (!valid_name(file)) return luaL_error(L, "require: bad module name '%s'", name);
    snprintf(path, sizeof(path), "%s/%s", s_app.dir, file);
    if (luaL_loadfilex(L, path, "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    return 1;
}

static int os_time(lua_State *L) { lua_pushinteger(L, (lua_Integer)time(NULL)); return 1; }
static int os_clock(lua_State *L) { lua_pushnumber(L, esp_timer_get_time() / 1e6); return 1; }

static int os_date(lua_State *L)
{
    const char *fmt = luaL_optstring(L, 1, "%c");
    time_t t = (time_t)luaL_optinteger(L, 2, (lua_Integer)time(NULL));
    struct tm tm;
    localtime_r(&t, &tm);
    if (strncmp(fmt, "*t", 2) == 0) {
        lua_pushinteger(L, (lua_Integer)t);
        lua_replace(L, 1);
        return w_time(L);
    }
    char buf[128];
    size_t n = strftime(buf, sizeof(buf), fmt, &tm);
    lua_pushlstring(L, buf, n);
    return 1;
}

static const luaL_Reg OS_FUNCS[] = { { "time", os_time }, { "clock", os_clock }, { "date", os_date }, { NULL, NULL } };

#define NEW_LIB(L, name, funcs) do { lua_newtable(L); luaL_setfuncs(L, funcs, 0); lua_setglobal(L, name); } while (0)

/* ======================================================================== lifecycle */

static void app_start(void)
{
    s_app.dead = false;
    s_app.mem_used = 0;
    s_app.mem_limit = (size_t)s_app.info.memory_kb * 1024;
    lv_obj_clean(s_app.root);

    lua_State *L = lua_newstate(l_alloc, NULL);
    if (!L) {
        free(s_app.err);
        s_app.err = strdup("not enough memory to start the app");
        show_error();
        return;
    }
    s_app.L = L;
    lua_sethook(L, l_hook, LUA_MASKCOUNT, 2000);
    lua_gc(L, LUA_GCGEN, 0, 0);

    static const luaL_Reg libs[] = {
        { LUA_GNAME, luaopen_base }, { LUA_COLIBNAME, luaopen_coroutine }, { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string }, { LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 },
        { NULL, NULL },
    };
    for (const luaL_Reg *l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    /* sandbox: no loading code from arbitrary files / bytecode */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "load");

    lua_newtable(L);
    s_app.objs_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "app_loaded");

    luaL_newmetatable(L, OBJ_MT);
    lua_newtable(L);
    luaL_setfuncs(L, OBJ_METHODS, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    NEW_LIB(L, "ui", UI_FUNCS);
    lua_getglobal(L, "ui");
    lua_pushinteger(L, UI_W); lua_setfield(L, -2, "W");
    lua_pushinteger(L, CONTENT_H); lua_setfield(L, -2, "H");
    lua_pop(L, 1);
    NEW_LIB(L, "timer", TIMER_FUNCS);
    NEW_LIB(L, "watch", WATCH_FUNCS);
    NEW_LIB(L, "json", JSON_FUNCS);
    NEW_LIB(L, "storage", STORAGE_FUNCS);
    NEW_LIB(L, "http", HTTP_FUNCS);
    NEW_LIB(L, "os", OS_FUNCS);

    lua_newtable(L);
    lua_pushcfunction(L, a_exit); lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, a_memory); lua_setfield(L, -2, "memory");
    lua_pushstring(L, s_app.id); lua_setfield(L, -2, "id");
    lua_pushstring(L, s_app.info.name); lua_setfield(L, -2, "name");
    lua_pushstring(L, s_app.info.version); lua_setfield(L, -2, "version");
    lua_setglobal(L, "app");

    lua_register(L, "print", l_print);
    lua_register(L, "require", l_require);

    char path[128];
    snprintf(path, sizeof(path), "%s/main.lua", s_app.dir);
    ESP_LOGI(TAG, "start %s (memory limit %d KB)", s_app.id, s_app.info.memory_kb);
    if (luaL_loadfilex(L, path, "t") != LUA_OK) {
        app_fail(lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    app_pcall(0);
}

/* delete the app's objects, then the Lua state (order matters, see header) */
static void app_stop_state(void)
{
    lua_State *L = s_app.L;
    if (!L) return;
    if (!s_app.dead && lua_getglobal(L, "on_close") == LUA_TFUNCTION) {
        app_pcall(0);
    } else {
        lua_pop(L, 1);
    }
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (s_app.timers[i].t) lv_timer_delete(s_app.timers[i].t);
        s_app.timers[i].t = NULL;
    }
    if (s_app.root) lv_obj_clean(s_app.root);
    s_app.L = NULL;
    lua_close(L);
    s_app.gen++;
    s_app.objs_ref = LUA_NOREF;
    if (s_app.awake_held) {
        ui_keep_awake(false);
        s_app.awake_held = false;
    }
    cJSON_Delete(s_app.store);
    s_app.store = NULL;
    ESP_LOGI(TAG, "stopped %s", s_app.id);
}

static void restart_async(void *arg)
{
    if (!s_app.page_open || !s_app.root) return;
    app_stop_state();
    lua_apps_get(s_app.id, &s_app.info);
    app_start();
}

static void restart_cb(lv_event_t *e)
{
    lv_async_call(restart_async, NULL);     /* don't delete the button inside its own click */
}

static void show_error(void)
{
    if (!s_app.root) return;
    lv_obj_t *r = s_app.root;
    lv_obj_clean(r);
    lv_obj_t *t = ui_label(r, LV_SYMBOL_WARNING " App error", &lv_font_montserrat_24, UI_RED);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *box = lv_obj_create(r);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, UI_W - 40, CONTENT_H - 140);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_t *m = ui_label(box, s_app.err ? s_app.err : "?", &lv_font_montserrat_16, 0xE5E5EA);
    lv_obj_set_width(m, lv_pct(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_t *b = ui_button(r, LV_SYMBOL_REFRESH " Restart app", UI_BLUE, restart_cb, NULL);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -24);
}

static void page_build(lv_obj_t *c, void *ctx)
{
    if (s_app.L) {           /* page rebuilt: LVGL already deleted the old objects */
        s_app.root = NULL;
        app_stop_state();
    }
    s_app.root = c;
    lv_obj_set_style_text_color(c, lv_color_hex(UI_TEXT), 0);
    app_start();
}

static void page_close(void *ctx)
{
    app_stop_state();
    s_app.root = NULL;
    s_app.page_open = false;
    free(s_app.err);
    s_app.err = NULL;
}

void lua_app_open(const char *id)
{
    lua_app_info_t info;
    if (!lua_apps_get(id, &info)) {
        ui_toast("App not found");
        return;
    }
    if (s_app.page_open) ui_go_home();   /* one Lua app at a time */
    s_app.info = info;
    strlcpy(s_app.id, id, sizeof(s_app.id));
    snprintf(s_app.dir, sizeof(s_app.dir), LUA_APPS_DIR "/%s", id);
    s_app.page_open = true;
    ui_page_push_raw(info.name, page_build, page_close, NULL);
    s_app.page_depth = ui_page_depth();
}

/* ======================================================================== control (any task) */

bool lua_apps_run(const char *id)
{
    lua_app_info_t info;
    if (!lua_apps_get(id, &info)) return false;
    ui_wake();
    if (!ui_lock()) return false;
    if (s_app.page_open && strcmp(s_app.id, id) == 0) {
        app_stop_state();                /* hot reload: same page, fresh code */
        s_app.info = info;
        app_start();
    } else {
        lua_app_open(id);
    }
    ui_unlock();
    return true;
}

void lua_apps_stop(void)
{
    if (!ui_lock()) return;
    if (s_app.page_open) ui_go_home();
    ui_unlock();
}

bool lua_apps_remove(const char *id)
{
    if (!valid_name(id)) return false;
    char running[32];
    if (lua_apps_running(running, sizeof(running)) && strcmp(running, id) == 0) lua_apps_stop();
    char path[96];
    snprintf(path, sizeof(path), LUA_APPS_DIR "/%s", id);
    return fm_remove_recursive(path) >= 0;
}

bool lua_apps_running(char *id_out, size_t size)
{
    if (!s_app.page_open) return false;
    strlcpy(id_out, s_app.id, size);
    return true;
}
