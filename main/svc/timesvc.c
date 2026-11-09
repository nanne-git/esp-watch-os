#include "timesvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "hw.h"
#include "settings.h"

static const char *TAG = "time";

static bool s_valid;

bool timesvc_valid(void)
{
    return s_valid || time(NULL) > 1700000000;
}

void timesvc_apply_tz(void)
{
    setenv("TZ", settings()->timezone, 1);
    tzset();
}

static void set_system(time_t utc)
{
    struct timeval tv = { .tv_sec = utc };
    settimeofday(&tv, NULL);
    s_valid = true;
}

void timesvc_set_utc(time_t utc)
{
    set_system(utc);
    struct tm t;
    gmtime_r(&utc, &t);
    rtc_write(&t);
    ESP_LOGI(TAG, "time set, RTC updated");
}

static void on_sntp(struct timeval *tv)
{
    s_valid = true;
    struct tm t;
    gmtime_r(&tv->tv_sec, &t);
    rtc_write(&t);
    ESP_LOGI(TAG, "NTP sync OK, RTC updated");
}

static void on_settings(void)
{
    timesvc_apply_tz();
}

void timesvc_init(void)
{
    timesvc_apply_tz();
    struct tm t;
    if (rtc_read(&t)) {
        /* RTC holds UTC: convert without the local time zone */
        setenv("TZ", "UTC0", 1);
        tzset();
        time_t utc = mktime(&t);
        timesvc_apply_tz();
        set_system(utc);
        ESP_LOGI(TAG, "clock loaded from RTC");
    }
    settings_on_change(on_settings);

    if (settings()->auto_time) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        sntp_set_time_sync_notification_cb(on_sntp);
        esp_sntp_init();      /* syncs automatically whenever the network is up */
    }
}

void timesvc_format_time(char *buf, size_t len, bool with_ampm)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (settings()->time_24h) {
        snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
        int h = lt.tm_hour % 12;
        snprintf(buf, len, with_ampm ? "%d:%02d %s" : "%d:%02d", h ? h : 12, lt.tm_min,
                 lt.tm_hour < 12 ? "AM" : "PM");
    }
}
