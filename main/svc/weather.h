/*
 * Weather from open-meteo.com (free, no API key) using latitude/longitude from settings.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

typedef struct {
    bool   valid;
    float  temp_c;
    float  feels_c;
    int    code;           /* WMO weather code */
    float  tmax, tmin;
    int    humidity;
    float  wind_kmh;
    int    daily_code[5];
    float  daily_max[5], daily_min[5];
    time_t updated;
} weather_t;

void        weather_start(void);
void        weather_get(weather_t *out);
void        weather_refresh(void);           /* async */
const char *weather_desc(int code);
