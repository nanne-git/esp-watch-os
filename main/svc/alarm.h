/*
 * Alarms (saved in NVS) + countdown timer + stopwatch state.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALARM_MAX 8

typedef struct {
    bool    enabled;
    uint8_t hour, min;
    uint8_t days;          /* bit0 = Sunday ... bit6 = Saturday; 0 = once */
    char    label[24];
} alarm_t;

void alarm_init(void);
int  alarm_count(void);
bool alarm_get(int i, alarm_t *out);
void alarm_set(int i, const alarm_t *a);
int  alarm_add(const alarm_t *a);
void alarm_delete(int i);
bool alarm_next(char *buf, size_t len);      /* "Tue 07:00" */
void alarm_snooze(void);
void alarm_dismiss(void);
void alarm_days_str(uint8_t days, char *buf, size_t len);

/* countdown timer */
void timer_start(int seconds);
void timer_pause(void);
void timer_resume(void);
void timer_cancel(void);
int  timer_remaining(void);
bool timer_running(void);
bool timer_paused(void);

/* stopwatch */
void     sw_start(void);
void     sw_stop(void);
void     sw_reset(void);
void     sw_lap(void);
bool     sw_running(void);
int64_t  sw_elapsed_ms(void);
int      sw_laps(int64_t *out, int max);
