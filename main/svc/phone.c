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
 * Watch -> phone (JSON + "\n"):
 *   {"t":"status","bat":85,"chg":0}  {"t":"music","n":"play"}
 *   {"t":"call","n":"REJECT"}  {"t":"findPhone","n":true}  {"t":"notify","id":1,"n":"DISMISS"}
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
#include "freertos/semphr.h"
#include "hw.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"

static const char *TAG = "phone";

static notif_t         s_notifs[NOTIF_MAX];
static int             s_notif_count;
static call_t          s_call;
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
    s_call.active = false;
    unlock();
    haptic_stop();
    if (was) {
        ui_on_call(false);
    }
}

static void send_json(cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    if (txt) {
        size_t n = strlen(txt);
        char *line = malloc(n + 2);
        if (line) {
            memcpy(line, txt, n);
            line[n] = '\n';
            ble_svc_send(line, n + 1);
            free(line);
        }
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
            haptic_stop();
        } else {
            call_end();                  /* end / reject / outgoing finished */
        }
    } else if (strcmp(t, "musicinfo") == 0) {
        phone_music_set_info(jstr(o, "artist"), jstr(o, "track"), (int)jnum(o, "dur", 0));
    } else if (strcmp(t, "musicstate") == 0) {
        phone_music_set_state(strcmp(jstr(o, "state"), "play") == 0, (int)jnum(o, "position", 0));
    } else if (strcmp(t, "find") == 0) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "n");
        if (cJSON_IsTrue(n)) {
            static const uint16_t buzz[] = { 300, 200, 300, 200, 300, 200, 300 };
            haptic_pattern(buzz, 7);
            ui_toast("Your phone is looking for the watch");
        } else {
            haptic_stop();
        }
    } else {
        ESP_LOGD(TAG, "unhandled GB message t=%s", t);
    }
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
        char *json = strndup(start, end - start);
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
