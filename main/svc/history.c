#include "history.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw.h"
#include "nvs.h"

static const char *TAG = "history";

#define DIR_PATH "/sdcard/History"

static history_day_t s_days[HISTORY_DAYS];   /* finished days, oldest first */
static uint32_t      s_today;                /* YYYYMMDD being counted */
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ymd(const struct tm *t) { return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday; }

static bool time_valid(struct tm *lt)
{
    time_t now = time(NULL);
    localtime_r(&now, lt);
    return lt->tm_year + 1900 >= 2025;
}

/* ------------------------------------------------------------------ NVS */

static void nvs_save(uint32_t steps)
{
    nvs_handle_t h;
    if (nvs_open("watch", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "st_date", s_today);
    nvs_set_u32(h, "st_cnt", steps);
    nvs_set_blob(h, "st_days", s_days, sizeof(s_days));
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(uint32_t *date, uint32_t *steps)
{
    *date = *steps = 0;
    nvs_handle_t h;
    if (nvs_open("watch", NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u32(h, "st_date", date);
    nvs_get_u32(h, "st_cnt", steps);
    size_t len = sizeof(s_days);
    if (nvs_get_blob(h, "st_days", s_days, &len) != ESP_OK || len != sizeof(s_days)) {
        memset(s_days, 0, sizeof(s_days));
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------ CSV on the SD card */

static void csv_append(const char *file, const char *header, const char *line)
{
    if (!fm_is_mounted()) return;
    mkdir(DIR_PATH, 0775);
    char path[96];
    snprintf(path, sizeof(path), DIR_PATH "/%s", file);
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) return;
    if (fresh) fprintf(f, "%s\n", header);
    fprintf(f, "%s\n", line);      /* one short line per write: a power cut loses at most this line */
    fclose(f);
}

static void finish_day(uint32_t date, uint32_t steps)
{
    if (!date) return;
    portENTER_CRITICAL(&s_mux);
    memmove(&s_days[0], &s_days[1], sizeof(s_days) - sizeof(s_days[0]));
    s_days[HISTORY_DAYS - 1].date = date;
    s_days[HISTORY_DAYS - 1].steps = steps;
    portEXIT_CRITICAL(&s_mux);
    char line[48];
    snprintf(line, sizeof(line), "%04lu-%02lu-%02lu,%lu", (unsigned long)(date / 10000), (unsigned long)(date / 100 % 100),
             (unsigned long)(date % 100), (unsigned long)steps);
    csv_append("daily.csv", "date,steps", line);
    ESP_LOGI(TAG, "day %lu finished: %lu steps", (unsigned long)date, (unsigned long)steps);
}

/* ------------------------------------------------------------------ task */

static void history_task(void *arg)
{
    struct tm lt;
    while (!time_valid(&lt)) {            /* clock comes from RTC / NTP / phone */
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    uint32_t saved_date, saved_steps;
    nvs_load(&saved_date, &saved_steps);
    s_today = ymd(&lt);
    if (saved_date == s_today) {
        imu_add_steps(saved_steps);       /* restart during the day: continue counting */
        ESP_LOGI(TAG, "restored %lu steps", (unsigned long)saved_steps);
    } else if (saved_date && saved_date < s_today) {
        finish_day(saved_date, saved_steps);   /* the watch was off at midnight */
    }

    uint32_t last_saved = UINT32_MAX, last_steps = imu_steps_today(), hour_start_steps = last_steps;
    int last_hour = lt.tm_hour, minute = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
        minute++;
        if (!time_valid(&lt)) continue;
        uint32_t today = ymd(&lt);
        uint32_t steps = imu_steps_today();
        char t[24], line[96];
        strftime(t, sizeof(t), "%Y-%m-%d %H:%M", &lt);

        if (lt.tm_hour != last_hour || today != s_today) {
            /* hourly step record (for the hour that just ended) */
            uint32_t in_hour = (today != s_today ? last_steps : steps) - hour_start_steps;
            char file[32];
            strftime(file, sizeof(file), "steps-%Y-%m.csv", &lt);
            snprintf(line, sizeof(line), "%s,%lu,%lu", t, (unsigned long)in_hour,
                     (unsigned long)(today != s_today ? last_steps : steps));
            csv_append(file, "time,steps_this_hour,steps_today", line);
            last_hour = lt.tm_hour;
            hour_start_steps = today != s_today ? 0 : steps;
        }
        if (today != s_today) {
            /* midnight: the step counter reset itself; record yesterday's last value */
            finish_day(s_today, last_steps);
            s_today = today;
            last_saved = UINT32_MAX;
        }
        if (minute % 10 == 0) {
            power_status_t p;
            power_get(&p);
            if (p.present && p.percent >= 0) {
                char file[32];
                strftime(file, sizeof(file), "battery-%Y-%m.csv", &lt);
                snprintf(line, sizeof(line), "%s,%d,%d,%d,%.1f", t, p.percent, p.voltage_mv, p.charging ? 1 : 0, p.temp_c);
                csv_append(file, "time,percent,millivolts,charging,temp_c", line);
            }
        }
        /* flash wear: save at most every 5 minutes and only when the count changed */
        if (minute % 5 == 0 && steps != last_saved) {
            nvs_save(steps);
            last_saved = steps;
        }
        last_steps = steps;
    }
}

int history_days(history_day_t *out, int n)
{
    if (n <= 0) return 0;
    int k = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = HISTORY_DAYS - (n - 1); i < HISTORY_DAYS; i++) {
        if (i >= 0) out[k++] = s_days[i];
        else out[k++] = (history_day_t){ 0, 0 };
    }
    portEXIT_CRITICAL(&s_mux);
    out[k].date = s_today;
    out[k].steps = imu_steps_today();
    return k + 1;
}

void history_start(void)
{
    xTaskCreate(history_task, "history", 3072, NULL, 2, NULL);
}
