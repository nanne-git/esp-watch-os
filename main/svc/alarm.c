#include "alarm.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw.h"
#include "nvs.h"
#include "settings.h"
#include "timesvc.h"
#include "ui.h"

static const char *TAG = "alarm";

static alarm_t s_alarms[ALARM_MAX];
static int     s_count;
static volatile bool s_ringing;
static time_t  s_snooze_until;
static char    s_ring_label[24];

/* ------------------------------------------------------------------ storage */

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("watch", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "alarms", s_alarms, sizeof(alarm_t) * s_count);
        nvs_set_i32(h, "alarm_n", s_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("watch", NVS_READONLY, &h) == ESP_OK) {
        int32_t n = 0;
        if (nvs_get_i32(h, "alarm_n", &n) == ESP_OK && n > 0 && n <= ALARM_MAX) {
            size_t len = sizeof(alarm_t) * n;
            if (nvs_get_blob(h, "alarms", s_alarms, &len) == ESP_OK) {
                s_count = n;
            }
        }
        nvs_close(h);
    }
}

int alarm_count(void) { return s_count; }

bool alarm_get(int i, alarm_t *out)
{
    if (i < 0 || i >= s_count) {
        return false;
    }
    *out = s_alarms[i];
    return true;
}

void alarm_set(int i, const alarm_t *a)
{
    if (i >= 0 && i < s_count) {
        s_alarms[i] = *a;
        save();
    }
}

int alarm_add(const alarm_t *a)
{
    if (s_count >= ALARM_MAX) {
        return -1;
    }
    s_alarms[s_count] = *a;
    s_count++;
    save();
    return s_count - 1;
}

void alarm_delete(int i)
{
    if (i < 0 || i >= s_count) {
        return;
    }
    memmove(&s_alarms[i], &s_alarms[i + 1], (s_count - i - 1) * sizeof(alarm_t));
    s_count--;
    save();
}

void alarm_days_str(uint8_t days, char *buf, size_t len)
{
    static const char *d[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (days == 0) {
        snprintf(buf, len, "Once");
    } else if (days == 0x7F) {
        snprintf(buf, len, "Every day");
    } else if (days == 0x3E) {
        snprintf(buf, len, "Weekdays");
    } else if (days == 0x41) {
        snprintf(buf, len, "Weekends");
    } else {
        buf[0] = '\0';
        for (int i = 0; i < 7; i++) {
            if (days & (1 << i)) {
                strlcat(buf, d[i], len);
                strlcat(buf, " ", len);
            }
        }
    }
}

/* minutes until alarm a fires from now (or -1) */
static int minutes_until(const alarm_t *a, const struct tm *lt)
{
    if (!a->enabled) {
        return -1;
    }
    int now_m = lt->tm_hour * 60 + lt->tm_min;
    int al_m = a->hour * 60 + a->min;
    for (int day = 0; day < 8; day++) {
        int wd = (lt->tm_wday + day) % 7;
        if (a->days && !(a->days & (1 << wd))) {
            continue;
        }
        int diff = day * 1440 + al_m - now_m;
        if (diff > 0) {
            return diff;
        }
    }
    return -1;
}

bool alarm_next(char *buf, size_t len)
{
    if (!timesvc_valid()) {
        return false;
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int best = -1, bi = -1;
    for (int i = 0; i < s_count; i++) {
        int m = minutes_until(&s_alarms[i], &lt);
        if (m >= 0 && (best < 0 || m < best)) {
            best = m;
            bi = i;
        }
    }
    if (bi < 0) {
        return false;
    }
    static const char *d[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    time_t at = now + best * 60;
    struct tm t;
    localtime_r(&at, &t);
    if (best < 1440 && t.tm_yday == lt.tm_yday) {
        snprintf(buf, len, "%02d:%02d", s_alarms[bi].hour, s_alarms[bi].min);
    } else {
        snprintf(buf, len, "%s %02d:%02d", d[t.tm_wday], s_alarms[bi].hour, s_alarms[bi].min);
    }
    return true;
}

/* ------------------------------------------------------------------ ringing */

static void ring(const char *label)
{
    strlcpy(s_ring_label, label, sizeof(s_ring_label));
    s_ringing = true;
    ESP_LOGI(TAG, "ringing: %s", label);
    ui_on_alarm(label);
}

void alarm_snooze(void)
{
    s_ringing = false;
    haptic_stop();
    audio_beep_stop();
    s_snooze_until = time(NULL) + 5 * 60;
}

void alarm_dismiss(void)
{
    s_ringing = false;
    haptic_stop();
    audio_beep_stop();
    s_snooze_until = 0;
}

/* ------------------------------------------------------------------ timer & stopwatch */

static int64_t s_timer_end_us;      /* 0 = not running */
static int64_t s_timer_left_us;     /* when paused */
static bool    s_timer_paused;

void timer_start(int seconds)
{
    s_timer_end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s_timer_paused = false;
}
void timer_pause(void)
{
    if (s_timer_end_us && !s_timer_paused) {
        s_timer_left_us = s_timer_end_us - esp_timer_get_time();
        s_timer_paused = true;
    }
}
void timer_resume(void)
{
    if (s_timer_paused) {
        s_timer_end_us = esp_timer_get_time() + s_timer_left_us;
        s_timer_paused = false;
    }
}
void timer_cancel(void) { s_timer_end_us = 0; s_timer_paused = false; }
bool timer_running(void) { return s_timer_end_us != 0; }
bool timer_paused(void) { return s_timer_paused; }
int timer_remaining(void)
{
    if (!s_timer_end_us) return 0;
    int64_t left = s_timer_paused ? s_timer_left_us : s_timer_end_us - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

static int64_t s_sw_start, s_sw_acc;
static bool    s_sw_running;
static int64_t s_laps[20];
static int     s_lap_count;

void sw_start(void) { if (!s_sw_running) { s_sw_start = esp_timer_get_time(); s_sw_running = true; } }
void sw_stop(void) { if (s_sw_running) { s_sw_acc += esp_timer_get_time() - s_sw_start; s_sw_running = false; } }
void sw_reset(void) { s_sw_running = false; s_sw_acc = 0; s_lap_count = 0; }
bool sw_running(void) { return s_sw_running; }
int64_t sw_elapsed_ms(void)
{
    int64_t us = s_sw_acc + (s_sw_running ? esp_timer_get_time() - s_sw_start : 0);
    return us / 1000;
}
void sw_lap(void)
{
    if (s_lap_count < 20) {
        s_laps[s_lap_count++] = sw_elapsed_ms();
    }
}
int sw_laps(int64_t *out, int max)
{
    int n = s_lap_count < max ? s_lap_count : max;
    memcpy(out, s_laps, n * sizeof(int64_t));
    return n;
}

/* ------------------------------------------------------------------ checker task */

static void alarm_task(void *arg)
{
    int last_min = -1;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        /* countdown timer */
        if (s_timer_end_us && !s_timer_paused && esp_timer_get_time() >= s_timer_end_us) {
            s_timer_end_us = 0;
            static const uint16_t pat[] = { 400, 200, 400, 200, 400, 200, 400 };
            haptic_pattern(pat, 7);
            audio_beep(880, 1500);
            ui_on_timer_done();
        }
        /* keep buzzing while ringing */
        if (s_ringing) {
            static int64_t last_buzz;
            int64_t t = esp_timer_get_time();
            if (t - last_buzz > 1500000) {
                last_buzz = t;
                static const uint16_t pat[] = { 500, 250, 500 };
                haptic_pattern(pat, 3);
                audio_beep(1000, 600);
            }
        }
        if (!timesvc_valid()) {
            continue;
        }
        time_t now = time(NULL);
        if (s_snooze_until && now >= s_snooze_until) {
            s_snooze_until = 0;
            ring(s_ring_label);
        }
        struct tm lt;
        localtime_r(&now, &lt);
        int m = lt.tm_hour * 60 + lt.tm_min;
        if (m == last_min) {
            continue;
        }
        last_min = m;
        for (int i = 0; i < s_count; i++) {
            alarm_t *a = &s_alarms[i];
            if (!a->enabled || a->hour != lt.tm_hour || a->min != lt.tm_min) {
                continue;
            }
            if (a->days && !(a->days & (1 << lt.tm_wday))) {
                continue;
            }
            if (!a->days) {          /* one-shot: disable after firing */
                a->enabled = false;
                save();
            }
            ring(a->label[0] ? a->label : "Alarm");
            break;
        }
    }
}

void alarm_init(void)
{
    load();
    xTaskCreate(alarm_task, "alarm", 3072, NULL, 4, NULL);
}
