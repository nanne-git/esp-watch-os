/*
 * History: keeps today's steps across restarts and records data over time.
 *
 *   Flash (NVS, works without SD card): today's step count + the last 14 days' totals.
 *   SD card, CSV files you can open in Excel / Numbers / the web dashboard:
 *     /sdcard/History/daily.csv              date,steps
 *     /sdcard/History/steps-YYYY-MM.csv      time,steps_this_hour,steps_today      (hourly)
 *     /sdcard/History/battery-YYYY-MM.csv    time,percent,millivolts,charging,temp_c  (every 10 min)
 */
#pragma once

#include <stdint.h>

#define HISTORY_DAYS 14

typedef struct {
    uint32_t date;       /* YYYYMMDD, 0 = empty */
    uint32_t steps;
} history_day_t;

void history_start(void);

/* last n days, oldest first, today (live count) last. Returns how many were filled. */
int history_days(history_day_t *out, int n);
