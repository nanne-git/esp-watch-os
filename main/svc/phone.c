/*
 * Gadgetbridge "Bangle.js" protocol (Android companion app).
 *
 * Phone -> watch (one line each, usually prefixed with 0x10):
 *   setTime(1700000000);E.setTimeZone(5.5);...
 *   GB({"t":"notify","id":1,"src":"WhatsApp","title":"Ann","body":"hi"})
 *   GB({"t":"notify-","id":1})
 *   GB({"t":"call","cmd":"incoming","name":"Ann","number":"+91..."})
 *   GB({"t":"musicinfo","artist":"..","track":"..","dur":200})
 *   GB({"t":"musicstate","state":"play","position":12})
 *   GB({"t":"find","n":true})
 * Watch -> phone (one JSON object per line, ending in "\r\n"):
 *   {"t":"status","bat":85,"chg":0}  {"t":"music","n":"play"}
 *   {"t":"call","n":"REJECT"}  {"t":"findPhone","n":true}  {"t":"notify","id":1,"n":"DISMISS"}
 *
 * Two Gadgetbridge quirks this file has to deal with:
 *  - Gadgetbridge cuts lines at "\n" and then drops the character BEFORE the "\n" (it expects
 *    Espruino's "\r\n"). A line ending in plain "\n" loses its closing '}' and Gadgetbridge shows
 *    "Malformed JSON from Bangle.js". ble_svc_send() therefore always terminates lines with "\r\n".
 *  - What Gadgetbridge sends inside GB(...) is JavaScript, not strict JSON: strings may contain
 *    \xNN, \v, octal \0..\377 escapes, raw Latin-1 bytes, and whole strings may be sent as
 *    atob("base64"). gb_js_to_json() rewrites that into real JSON before cJSON parses it.
 */
#include "phone.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_svc.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "esp_app_desc.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "hw.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"
#include "weather.h"

static const char *TAG = "phone";

static notif_t         s_notifs[NOTIF_MAX];
static int             s_notif_count;
static call_t          s_call;
static bool            s_call_answered, s_call_user_ended;
static phone_music_t   s_music;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

/* ------------------------------------------------------------------ notifications */

int notif_count(void) { return s_notif_count; }

bool notif_get(int i, notif_t *out)
{
    lock();
    bool ok = i >= 0 && i < s_notif_count;
    if (ok) {
        *out = s_notifs[i];
    }
    unlock();
    return ok;
}

bool notif_dnd_active(void)
{
    const settings_t *s = settings();
    if (s->dnd) {
        return true;
    }
    if (!s->dnd_scheduled || !timesvc_valid()) {
        return false;
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int m = lt.tm_hour * 60 + lt.tm_min;
    if (s->dnd_start_min <= s->dnd_end_min) {
        return m >= s->dnd_start_min && m < s->dnd_end_min;
    }
    return m >= s->dnd_start_min || m < s->dnd_end_min;   /* over midnight */
}

void notif_add(const notif_t *n)
{
    if (!settings()->notif_on) {
        return;
    }
    lock();
    /* replace an existing id (updated notification) */
    for (int i = 0; i < s_notif_count; i++) {
        if (s_notifs[i].id == n->id) {
            memmove(&s_notifs[i], &s_notifs[i + 1], (s_notif_count - i - 1) * sizeof(notif_t));
            s_notif_count--;
            break;
        }
    }
    if (s_notif_count == NOTIF_MAX) {
        s_notif_count--;
    }
    memmove(&s_notifs[1], &s_notifs[0], s_notif_count * sizeof(notif_t));
    s_notifs[0] = *n;
    if (!s_notifs[0].when) {
        s_notifs[0].when = time(NULL);
    }
    s_notif_count++;
    unlock();

    bool quiet = notif_dnd_active();
    if (!quiet && settings()->vibration) {
        static const uint16_t pat[] = { 120, 120, 120 };
        haptic_pattern(pat, 3);
    }
    ui_on_notification(n, quiet);
}

void notif_remove_id(uint32_t id)
{
    lock();
    for (int i = 0; i < s_notif_count; i++) {
        if (s_notifs[i].id == id) {
            memmove(&s_notifs[i], &s_notifs[i + 1], (s_notif_count - i - 1) * sizeof(notif_t));
            s_notif_count--;
            break;
        }
    }
    unlock();
    ui_on_data_changed();
}

void notif_clear(void)
{
    lock();
    s_notif_count = 0;
    unlock();
    ui_on_data_changed();
}

/* ------------------------------------------------------------------ calls */

void call_get(call_t *out)
{
    lock();
    *out = s_call;
    unlock();
}

void call_set_incoming(const char *name, const char *number)
{
    lock();
    s_call.active = true;
    s_call_answered = false;
    s_call_user_ended = false;
    strlcpy(s_call.name, name && *name ? name : "Unknown", sizeof(s_call.name));
    strlcpy(s_call.number, number ? number : "", sizeof(s_call.number));
    unlock();
    if (settings()->vibration && !notif_dnd_active()) {
        static const uint16_t ring[] = { 600, 400, 600, 400, 600, 400, 600, 400, 600 };
        haptic_pattern(ring, 9);
    }
    ui_on_call(true);
}

void call_end(void)
{
    lock();
    bool was = s_call.active;
    call_t c = s_call;
    s_call.active = false;
    unlock();
    haptic_stop();
    if (was) {
        ui_on_call(false);
        if (!s_call_answered && !s_call_user_ended) {        /* rang out: missed call */
            notif_t n = { 0 };
            n.id = esp_random();
            strlcpy(n.app, "Phone", sizeof(n.app));
            strlcpy(n.title, "Missed call", sizeof(n.title));
            snprintf(n.body, sizeof(n.body), "%s %s", c.name, c.number);
            notif_add(&n);
        }
    }
}

/* One JSON object -> one line. cJSON_PrintUnformatted never emits raw newlines/control chars
 * (they are escaped), and ble_svc_send() appends the "\r\n" Gadgetbridge needs. */
static void send_json(cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    if (txt) {
        ble_svc_send(txt, strlen(txt));
        cJSON_free(txt);
    }
    cJSON_Delete(o);
}

static void send_tn(const char *t, const char *n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", t);
    cJSON_AddStringToObject(o, "n", n);
    send_json(o);
}

void call_action(const char *action)
{
    send_tn("call", action);
    if (strcmp(action, "ACCEPT") == 0) {
        s_call_answered = true;
    } else {
        s_call_user_ended = true;
    }
    if (strcmp(action, "ACCEPT") != 0) {
        call_end();
    } else {
        haptic_stop();
    }
}

/* ------------------------------------------------------------------ phone music */

void phone_music_get(phone_music_t *out)
{
    lock();
    *out = s_music;
    unlock();
}

void phone_music_set_info(const char *artist, const char *track, int dur)
{
    lock();
    s_music.valid = true;
    strlcpy(s_music.artist, artist ? artist : "", sizeof(s_music.artist));
    strlcpy(s_music.track, track ? track : "", sizeof(s_music.track));
    s_music.duration_s = dur;
    unlock();
    ui_on_data_changed();
}

void phone_music_set_state(bool playing, int pos)
{
    lock();
    s_music.valid = true;
    s_music.playing = playing;
    s_music.position_s = pos;
    unlock();
    ui_on_data_changed();
}

void phone_music_cmd(const char *cmd)
{
    send_tn("music", cmd);
}

/* ------------------------------------------------------------------ misc */

void phone_find(bool ring)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "findPhone");
    cJSON_AddBoolToObject(o, "n", ring);
    send_json(o);
}

void phone_send_battery(void)
{
    power_status_t p;
    power_get(&p);
    if (p.percent < 0) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "status");
    cJSON_AddNumberToObject(o, "bat", p.percent);
    cJSON_AddNumberToObject(o, "volt", p.voltage_mv / 1000.0);
    cJSON_AddNumberToObject(o, "chg", p.charging ? 1 : 0);
    send_json(o);
}

void phone_send_version(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ver");
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "hw", "ESP-Watch");
    send_json(o);
}

bool phone_connected(void)
{
    return ble_svc_connected();
}

/* ------------------------------------------------------------------ protocol parser */

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void handle_gb(cJSON *o)
{
    const char *t = jstr(o, "t");
    if (strcmp(t, "notify") == 0) {
        notif_t n = { 0 };
        n.id = (uint32_t)jnum(o, "id", esp_random());
        strlcpy(n.app, *jstr(o, "src") ? jstr(o, "src") : "Phone", sizeof(n.app));
        const char *title = *jstr(o, "title") ? jstr(o, "title") : jstr(o, "sender");
        strlcpy(n.title, title, sizeof(n.title));
        strlcpy(n.body, *jstr(o, "body") ? jstr(o, "body") : jstr(o, "subject"), sizeof(n.body));
        notif_add(&n);
    } else if (strcmp(t, "notify-") == 0) {
        notif_remove_id((uint32_t)jnum(o, "id", 0));
    } else if (strcmp(t, "call") == 0) {
        const char *cmd = jstr(o, "cmd");
        if (strcmp(cmd, "incoming") == 0) {
            call_set_incoming(jstr(o, "name"), jstr(o, "number"));
        } else if (strcmp(cmd, "start") == 0 || strcmp(cmd, "accept") == 0) {
            s_call_answered = true;
            haptic_stop();
        } else {
            call_end();                  /* end / reject / outgoing finished */
        }
    } else if (strcmp(t, "musicinfo") == 0) {
        phone_music_set_info(jstr(o, "artist"), jstr(o, "track"), (int)jnum(o, "dur", 0));
    } else if (strcmp(t, "musicstate") == 0) {
        phone_music_set_state(strcmp(jstr(o, "state"), "play") == 0, (int)jnum(o, "position", 0));
    } else if (strcmp(t, "weather") == 0) {
        double k = jnum(o, "temp", 0);
        if (k > 150) {
            weather_set_from_phone(k - 273.15, (int)jnum(o, "code", 800), (int)jnum(o, "hum", 0),
                                   jnum(o, "wind", 0), jnum(o, "hi", k) - 273.15, jnum(o, "lo", k) - 273.15);
        }
    } else if (strcmp(t, "find") == 0) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "n");
        if (cJSON_IsTrue(n)) {
            static const uint16_t buzz[] = { 300, 200, 300, 200, 300, 200, 300 };
            haptic_pattern(buzz, 7);
            ui_toast("Your phone is looking for the watch");
        } else {
            haptic_stop();
        }
    } else if (strcmp(t, "is_gps_active") == 0) {
        /* Gadgetbridge asks whether the watch wants phone GPS: we have no GPS app, say no */
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "t", "gps_power");
        cJSON_AddBoolToObject(r, "status", false);
        send_json(r);
    } else {
        ESP_LOGD(TAG, "unhandled GB message t=%s", t);
    }
}

/* ---- Gadgetbridge "JSON-ish" -> strict JSON ---- */

typedef struct {
    char  *buf;
    size_t len, cap;
} sbuf_t;

static bool sb_put(sbuf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 256);
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *nb = realloc(b->buf, cap);
        if (!nb) {
            return false;
        }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return true;
}

/* append one Latin-1 character (0..255) inside a JSON string, escaped if needed */
static bool sb_char(sbuf_t *b, unsigned c)
{
    char tmp[8];
    if (c == '"' || c == '\\') {
        tmp[0] = '\\';
        tmp[1] = (char)c;
        return sb_put(b, tmp, 2);
    }
    if (c < 0x20 || c >= 0x7f) {
        snprintf(tmp, sizeof(tmp), "\\u%04x", c & 0xff);
        return sb_put(b, tmp, 6);
    }
    tmp[0] = (char)c;
    return sb_put(b, tmp, 1);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* atob("...") at s (s points at 'a'); returns chars consumed, 0 if not an atob call */
static size_t conv_atob(const char *s, const char *end, sbuf_t *b)
{
    if (end - s < 8 || strncmp(s, "atob(\"", 6) != 0) {
        return 0;
    }
    const char *q = memchr(s + 6, '"', end - (s + 6));
    if (!q || q + 1 >= end || q[1] != ')') {
        return 0;
    }
    size_t in_len = q - (s + 6);
    size_t out_len = 0;
    unsigned char *bin = malloc(in_len * 3 / 4 + 4);
    if (!bin) {
        return 0;
    }
    bool ok = mbedtls_base64_decode(bin, in_len * 3 / 4 + 4, &out_len,
                                    (const unsigned char *)s + 6, in_len) == 0;
    ok = ok && sb_put(b, "\"", 1);
    for (size_t i = 0; ok && i < out_len; i++) {
        ok = sb_char(b, bin[i]);
    }
    ok = ok && sb_put(b, "\"", 1);
    free(bin);
    return ok ? (size_t)(q + 2 - s) : 0;
}

/* Returns a malloc'd strict-JSON copy of Gadgetbridge's GB(...) payload, or NULL. */
static char *gb_js_to_json(const char *s, size_t n)
{
    sbuf_t b = { 0 };
    const char *end = s + n;
    bool in_str = false;
    bool ok = sb_put(&b, "", 0);
    while (ok && s < end) {
        unsigned char c = (unsigned char)*s;
        if (!in_str) {
            if (c == '"') {
                in_str = true;
                ok = sb_put(&b, "\"", 1);
                s++;
            } else if (c == 'a') {
                size_t used = conv_atob(s, end, &b);
                if (used) {
                    s += used;
                } else {
                    ok = sb_put(&b, s++, 1);
                }
            } else {
                ok = sb_put(&b, s++, 1);
            }
            continue;
        }
        /* inside a string */
        if (c == '"') {
            in_str = false;
            ok = sb_put(&b, "\"", 1);
            s++;
        } else if (c != '\\') {
            ok = sb_char(&b, c);           /* raw Latin-1 / control bytes get \u00XX */
            s++;
        } else if (s + 1 >= end) {
            s++;                           /* dangling backslash: drop */
        } else {
            char e = s[1];
            s += 2;
            switch (e) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': {
                char tmp[2] = { '\\', e };
                ok = sb_put(&b, tmp, 2);
                break;
            }
            case 'v':
                ok = sb_char(&b, 0x0b);
                break;
            case 'u':
                if (end - s >= 4 && hexval(s[0]) >= 0 && hexval(s[1]) >= 0 && hexval(s[2]) >= 0 &&
                    hexval(s[3]) >= 0) {
                    ok = sb_put(&b, "\\u", 2) && sb_put(&b, s, 4);
                    s += 4;
                } else {
                    ok = sb_char(&b, 'u');
                }
                break;
            case 'x':
                if (end - s >= 2 && hexval(s[0]) >= 0 && hexval(s[1]) >= 0) {
                    ok = sb_char(&b, (unsigned)(hexval(s[0]) * 16 + hexval(s[1])));
                    s += 2;
                } else {
                    ok = sb_char(&b, 'x');
                }
                break;
            default:
                if (e >= '0' && e <= '7') {           /* octal: up to 3 digits, max 0377 */
                    unsigned v = (unsigned)(e - '0');
                    for (int k = 0; k < 2 && s < end && *s >= '0' && *s <= '7' && v * 8 + (*s - '0') <= 255; k++) {
                        v = v * 8 + (unsigned)(*s++ - '0');
                    }
                    ok = sb_char(&b, v);
                } else {
                    ok = sb_char(&b, (unsigned char)e);   /* JS: unknown escape = the char */
                }
                break;
            }
        }
    }
    if (!ok) {
        free(b.buf);
        return NULL;
    }
    return b.buf;
}

static void apply_phone_timezone(double hours)
{
    /* POSIX TZ has the sign inverted: UTC+5:30 -> "<+0530>-5:30" */
    int total = (int)lround(hours * 60);
    int a = abs(total);
    char tz[33];
    snprintf(tz, sizeof(tz), "<%c%02d%02d>%c%d:%02d", total >= 0 ? '+' : '-', a / 60, a % 60,
             total >= 0 ? '-' : '+', a / 60, a % 60);
    if (strcmp(tz, settings()->timezone) != 0) {
        settings_t *s = settings_edit();
        strlcpy(s->timezone, tz, sizeof(s->timezone));
        settings_commit();
        ESP_LOGI(TAG, "time zone from phone: %s", tz);
    }
}

void gb_handle_line(const char *line)
{
    /* setTime(...) and E.setTimeZone(...) arrive as JavaScript */
    const char *st = strstr(line, "setTime(");
    if (st) {
        time_t utc = (time_t)strtoll(st + 8, NULL, 10);
        if (utc > 1600000000) {
            timesvc_set_utc(utc);
        }
        const char *tz = strstr(line, "setTimeZone(");
        if (tz && settings()->auto_time) {
            apply_phone_timezone(strtod(tz + 12, NULL));
        }
        phone_send_battery();
    }

    const char *gb = strstr(line, "GB(");
    if (gb) {
        const char *start = gb + 3;
        const char *end = strrchr(start, ')');
        if (!end) {
            return;
        }
        char *json = gb_js_to_json(start, end - start);
        if (!json) {
            return;
        }
        cJSON *o = cJSON_Parse(json);
        free(json);
        if (o) {
            handle_gb(o);
            cJSON_Delete(o);
        } else {
            ESP_LOGW(TAG, "bad GB json: %.60s", start);
        }
    }
}
