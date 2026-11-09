/*
 * Time: RTC at boot -> system clock, NTP when Wi-Fi is up (writes back to RTC),
 * phone time from Gadgetbridge. Time zone is a POSIX TZ string from settings.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

void timesvc_init(void);              /* load RTC, apply time zone, start SNTP */
void timesvc_set_utc(time_t utc);     /* from phone / manual set; also updates RTC */
bool timesvc_valid(void);             /* clock has been set at least once */
void timesvc_apply_tz(void);          /* re-read time zone from settings */

/* "12:05" / "12:05 PM" etc. according to the 12/24h setting */
void timesvc_format_time(char *buf, size_t len, bool with_ampm);
