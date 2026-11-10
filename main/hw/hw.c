#include "hw.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "pcf85063a.h"
#include "qmi8658.h"
#undef M_PI          /* the driver defines its own; use the libc one */
#include <math.h>

static const char *TAG = "hw";

#define MOTOR_GPIO GPIO_NUM_18
#define BOOT_GPIO  GPIO_NUM_0
#define PWR_GPIO   GPIO_NUM_10

/* ======================================================================== AXP2101 */

#define AXP_ADDR        0x34
#define AXP_STATUS1     0x00   /* b5 VBUS good, b3 battery present */
#define AXP_STATUS2     0x01   /* b6:5 current dir (01 charge, 10 discharge), b2:0 charge state */
#define AXP_COMMON_CFG  0x10   /* b0 software power off */
#define AXP_ADC_EN      0x30   /* b0 vbat, b4 die temp */
#define AXP_VBAT_H      0x34
#define AXP_TDIE_H      0x3C
#define AXP_BAT_PERCENT 0xA4
#define AXP_GAUGE_CFG   0x18   /* b3 fuel gauge enable */

static i2c_master_dev_handle_t s_axp;
static SemaphoreHandle_t       s_i2c_lock;   /* our own transactions on the shared bus */

static esp_err_t axp_read(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_axp) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_axp, &reg, 1, buf, len, 50);
}

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    if (!s_axp) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_axp, b, 2, 50);
}

static void axp_init(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, AXP_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not found");
        return;
    }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AXP_ADDR,
                                .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    axp_read(AXP_ADC_EN, &v, 1);
    axp_write(AXP_ADC_EN, v | 0x01 | 0x10);          /* battery voltage + die temp ADC */
    axp_read(AXP_GAUGE_CFG, &v, 1);
    axp_write(AXP_GAUGE_CFG, v | 0x08);              /* fuel gauge on */
    /* charge gently: 300 mA constant current (reg 0x62: 9 = 300 mA) keeps the battery and
     * the power chip cooler while on USB; 4.2 V target is the chip default */
    axp_read(0x62, &v, 1);
    axp_write(0x62, (v & ~0x1F) | 9);
    ESP_LOGI(TAG, "AXP2101 ready");
}

void power_get(power_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->percent = -1;
    uint8_t st1, st2, pct, vb[2], td[2];
    xSemaphoreTake(s_i2c_lock, portMAX_DELAY);
    bool ok = axp_read(AXP_STATUS1, &st1, 1) == ESP_OK && axp_read(AXP_STATUS2, &st2, 1) == ESP_OK;
    if (ok) {
        o->present = true;
        o->usb = st1 & 0x20;
        o->battery = st1 & 0x08;
        uint8_t dir = (st2 >> 5) & 0x03;
        uint8_t chg = st2 & 0x07;
        o->charging = o->battery && dir == 0x01 && chg >= 1 && chg <= 3;
        o->full = o->battery && chg == 4;
        if (o->battery && axp_read(AXP_BAT_PERCENT, &pct, 1) == ESP_OK && pct <= 100) {
            o->percent = pct;
        }
        if (axp_read(AXP_VBAT_H, vb, 2) == ESP_OK) {
            o->voltage_mv = ((vb[0] & 0x3F) << 8) | vb[1];
        }
        if (axp_read(AXP_TDIE_H, td, 2) == ESP_OK) {
            int raw = ((td[0] & 0x3F) << 8) | td[1];
            o->temp_c = 22.0f + (7274 - raw) / 20.0f;   /* datasheet formula */
        }
    }
    xSemaphoreGive(s_i2c_lock);
}

void power_off(void)
{
    ESP_LOGW(TAG, "power off");
    uint8_t v = 0;
    xSemaphoreTake(s_i2c_lock, portMAX_DELAY);
    axp_read(AXP_COMMON_CFG, &v, 1);
    axp_write(AXP_COMMON_CFG, v | 0x01);
    xSemaphoreGive(s_i2c_lock);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();      /* only reached on USB power without battery cut-off */
}

void power_restart(void)
{
    esp_restart();
}

/* ======================================================================== RTC */

static pcf85063a_dev_t s_rtc;
static bool            s_rtc_ok;

bool rtc_read(struct tm *o)
{
    if (!s_rtc_ok) {
        return false;
    }
    pcf85063a_datetime_t d;
    xSemaphoreTake(s_i2c_lock, portMAX_DELAY);
    esp_err_t err = pcf85063a_get_time_date(&s_rtc, &d);
    xSemaphoreGive(s_i2c_lock);
    if (err != ESP_OK || d.year < 2024 || d.month < 1 || d.month > 12) {
        return false;
    }
    memset(o, 0, sizeof(*o));
    o->tm_year = d.year - 1900;
    o->tm_mon = d.month - 1;
    o->tm_mday = d.day;
    o->tm_hour = d.hour;
    o->tm_min = d.min;
    o->tm_sec = d.sec;
    return true;
}

bool rtc_write(const struct tm *t)
{
    if (!s_rtc_ok) {
        return false;
    }
    pcf85063a_datetime_t d = {
        .year = t->tm_year + 1900, .month = t->tm_mon + 1, .day = t->tm_mday,
        .dotw = t->tm_wday, .hour = t->tm_hour, .min = t->tm_min, .sec = t->tm_sec,
    };
    xSemaphoreTake(s_i2c_lock, portMAX_DELAY);
    esp_err_t err = pcf85063a_set_time_date(&s_rtc, d);
    xSemaphoreGive(s_i2c_lock);
    return err == ESP_OK;
}

/* ======================================================================== IMU: steps + raise */

static qmi8658_dev_t  s_imu;
static bool           s_imu_ok;
static volatile uint32_t s_steps;
static int            s_steps_day = -1;
static imu_wake_cb_t  s_raise_cb;

bool imu_ok(void) { return s_imu_ok; }
uint32_t imu_steps_today(void) { return s_steps; }
void imu_reset_steps(void) { s_steps = 0; }
void imu_add_steps(uint32_t n) { s_steps += n; }
void imu_set_raise_cb(imu_wake_cb_t cb) { s_raise_cb = cb; }

/*
 * 25 Hz sampling. Steps: peak detection on the low-pass-filtered acceleration
 * magnitude with a dynamic threshold and a 250 ms minimum gap.
 * Raise to wake: watch goes from "arm down / side" to "face up" (z dominant) quickly.
 */
static void imu_task(void *arg)
{
    float lp = 1000, prev = 1000, avg = 1000;
    bool rising = false;
    int64_t last_step = 0;
    float zhist[25] = { 0 };
    int zi = 0;
    int64_t last_raise = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(40));
        float x, y, z;
        xSemaphoreTake(s_i2c_lock, portMAX_DELAY);
        uint8_t raw[6];
        esp_err_t err = qmi8658_read_register(&s_imu, QMI8658_AX_L, raw, 6);
        /* +-8 g range: 4096 counts per g -> milli-g */
        x = (int16_t)((raw[1] << 8) | raw[0]) * 1000.0f / 4096.0f;
        y = (int16_t)((raw[3] << 8) | raw[2]) * 1000.0f / 4096.0f;
        z = (int16_t)((raw[5] << 8) | raw[4]) * 1000.0f / 4096.0f;
        xSemaphoreGive(s_i2c_lock);
        if (err != ESP_OK) {
            continue;
        }
        /* reset counter at local midnight */
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        if (lt.tm_year > 120) {
            if (s_steps_day != -1 && lt.tm_yday != s_steps_day) {
                s_steps = 0;
            }
            s_steps_day = lt.tm_yday;
        }

        float mag = sqrtf(x * x + y * y + z * z);
        lp = lp * 0.75f + mag * 0.25f;
        avg = avg * 0.98f + lp * 0.02f;
        int64_t t = esp_timer_get_time();
        if (lp > prev) {
            rising = true;
        } else if (rising && prev > avg + 120 && (t - last_step) > 250000) {
            /* a peak well above the running average */
            s_steps++;
            last_step = t;
            rising = false;
        } else if (lp < prev) {
            rising = false;
        }
        prev = lp;

        /* raise detection */
        float zn = z / (mag > 1 ? mag : 1);
        zhist[zi] = zn;
        zi = (zi + 1) % 25;
        float oldest = zhist[zi];                 /* ~1 s ago */
        if (s_raise_cb && zn > 0.75f && oldest < 0.3f && fabsf(mag - 1000) < 350 &&
            (t - last_raise) > 2000000) {
            last_raise = t;
            s_raise_cb();
        }
    }
}

/* ======================================================================== haptics */

static esp_timer_handle_t s_motor_timer;
static uint16_t           s_pattern[16];
static int                s_pattern_len, s_pattern_pos;

static void motor_step(void *arg)
{
    if (s_pattern_pos >= s_pattern_len) {
        gpio_set_level(MOTOR_GPIO, 0);
        return;
    }
    gpio_set_level(MOTOR_GPIO, (s_pattern_pos % 2) == 0);
    esp_timer_start_once(s_motor_timer, (uint64_t)s_pattern[s_pattern_pos++] * 1000);
}

void haptic_pattern(const uint16_t *ms, int count)
{
    if (count > 16) {
        count = 16;
    }
    esp_timer_stop(s_motor_timer);
    memcpy(s_pattern, ms, count * sizeof(uint16_t));
    s_pattern_len = count;
    s_pattern_pos = 0;
    motor_step(NULL);
}

void haptic_pulse(uint16_t ms)
{
    haptic_pattern(&ms, 1);
}

void haptic_stop(void)
{
    esp_timer_stop(s_motor_timer);
    s_pattern_len = 0;
    gpio_set_level(MOTOR_GPIO, 0);
}

/* ======================================================================== buttons */

static button_cb_t s_btn_cb;
void buttons_set_cb(button_cb_t cb) { s_btn_cb = cb; }

static void button_task(void *arg)
{
    const struct { gpio_num_t gpio; int active; button_id_t id; } btns[] = {
        { BOOT_GPIO, 0, BTN_BOOT },   /* pulled up, pressed = 0 */
        { PWR_GPIO, 1, BTN_PWR },     /* inverted: pressed = 1 */
    };
    int64_t down_at[2] = { 0 };
    bool long_sent[2] = { false };
    /* ignore a PWR button that is still held from powering on */
    vTaskDelay(pdMS_TO_TICKS(1500));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int64_t t = esp_timer_get_time();
        for (int i = 0; i < 2; i++) {
            bool pressed = gpio_get_level(btns[i].gpio) == btns[i].active;
            if (pressed && !down_at[i]) {
                down_at[i] = t;
                long_sent[i] = false;
            } else if (pressed && !long_sent[i] && t - down_at[i] > 800000) {
                long_sent[i] = true;
                if (s_btn_cb) s_btn_cb(btns[i].id, BTN_LONG);
            } else if (!pressed && down_at[i]) {
                if (!long_sent[i] && t - down_at[i] > 30000 && s_btn_cb) {
                    s_btn_cb(btns[i].id, BTN_SHORT);
                }
                down_at[i] = 0;
            }
        }
    }
}

/* ======================================================================== init */

void hw_init(void)
{
    s_i2c_lock = xSemaphoreCreateMutex();
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    axp_init(bus);

    s_rtc_ok = pcf85063a_init(&s_rtc, bus, PCF85063A_ADDRESS) == ESP_OK;
    ESP_LOGI(TAG, "RTC %s", s_rtc_ok ? "ready" : "not found");

    if (qmi8658_init(&s_imu, bus, QMI8658_ADDRESS_HIGH) == ESP_OK) {
        qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_8G);
        qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_31_25HZ);   /* we read at 25 Hz */
        qmi8658_enable_accel(&s_imu, true);
        s_imu_ok = true;
        xTaskCreate(imu_task, "imu", 3072, NULL, 3, NULL);
        ESP_LOGI(TAG, "IMU ready");
    } else {
        ESP_LOGW(TAG, "IMU not found");
    }

    gpio_config_t out = { .pin_bit_mask = 1ULL << MOTOR_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(MOTOR_GPIO, 0);
    const esp_timer_create_args_t ta = { .callback = motor_step, .name = "motor" };
    esp_timer_create(&ta, &s_motor_timer);

    gpio_config_t in = { .pin_bit_mask = (1ULL << BOOT_GPIO) | (1ULL << PWR_GPIO),
                         .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE };
    gpio_config(&in);
    gpio_set_pull_mode(BOOT_GPIO, GPIO_PULLUP_ONLY);
    xTaskCreate(button_task, "buttons", 3072, NULL, 5, NULL);
}
