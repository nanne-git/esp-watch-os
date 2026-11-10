/*
 * Hardware layer for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 * Everything on the shared I2C bus (GPIO14/15) uses the BSP's bus handle.
 *
 *   AXP2101  power/battery  0x34     PCF85063A RTC   0x51
 *   QMI8658  motion sensor  0x6B     motor  GPIO18   BOOT GPIO0   PWR GPIO10
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Call once after bsp_display_start() (which creates the I2C bus). */
void hw_init(void);

/* ---- power (AXP2101) ---- */
typedef struct {
    bool     present;        /* chip answered */
    bool     battery;        /* battery connected */
    bool     usb;            /* USB power present */
    bool     charging;
    bool     full;
    int      percent;        /* 0..100, -1 unknown */
    int      voltage_mv;
    float    temp_c;         /* chip temperature */
} power_status_t;

void power_get(power_status_t *out);
void power_off(void);                 /* cuts power (wake with PWR button) */
void power_restart(void);

/* ---- RTC ---- */
bool rtc_read(struct tm *out_utc);    /* RTC keeps UTC */
bool rtc_write(const struct tm *utc);

/* ---- motion sensor ---- */
uint32_t imu_steps_today(void);
void     imu_reset_steps(void);
void     imu_add_steps(uint32_t n);         /* restore the count after a restart */
bool     imu_ok(void);
typedef void (*imu_wake_cb_t)(void);
void imu_set_raise_cb(imu_wake_cb_t cb);   /* called when the wrist is raised */

/* ---- vibration ---- */
void haptic_pulse(uint16_t ms);
void haptic_pattern(const uint16_t *on_off_ms, int count);   /* on,off,on,off... */
void haptic_stop(void);

/* ---- buttons ---- */
typedef enum { BTN_BOOT, BTN_PWR } button_id_t;
typedef enum { BTN_SHORT, BTN_LONG } button_event_t;
typedef void (*button_cb_t)(button_id_t id, button_event_t ev);
void buttons_set_cb(button_cb_t cb);
