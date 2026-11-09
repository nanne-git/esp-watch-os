/*
 * App registry: everything in the launcher. Each app's open() pushes its own page(s).
 */
#pragma once

#include <stdint.h>

typedef struct {
    const char *name;
    const char *icon;      /* LV_SYMBOL_* */
    uint32_t    color;
    void      (*open)(void);
} app_def_t;

extern const app_def_t g_apps[];
extern const int       g_app_count;

void app_settings_open(void);
void app_files_open(void);
void app_files_open_path(const char *card_path);
void app_music_open(void);
void app_gallery_open(void);
void app_video_open(void);
void app_recorder_open(void);
void app_alarms_open(void);
void app_timer_open(void);
void app_stopwatch_open(void);
void app_steps_open(void);
void app_weather_open(void);
void app_flashlight_open(void);
void app_findphone_open(void);
void app_phone_music_open(void);
void app_calculator_open(void);
void app_notifications_open(void);
void app_about_open(void);

/* open a file with the right app (from the file manager) */
void app_open_file(const char *abs_path);
