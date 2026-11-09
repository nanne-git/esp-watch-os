#include "apps.h"

#include <string.h>
#include <strings.h>

#include "ui.h"

const app_def_t g_apps[] = {
    { "Settings",   LV_SYMBOL_SETTINGS,  0x636366, app_settings_open },
    { "Notifs",     LV_SYMBOL_BELL,      0xFF3B30, app_notifications_open },
    { "Files",      LV_SYMBOL_DIRECTORY, 0x0A84FF, app_files_open },
    { "Music",      LV_SYMBOL_AUDIO,     0xFF2D55, app_music_open },
    { "Gallery",    LV_SYMBOL_IMAGE,     0xFF9F0A, app_gallery_open },
    { "Video",      LV_SYMBOL_VIDEO,     0xBF5AF2, app_video_open },
    { "Recorder",   LV_SYMBOL_STOP,      0xFF453A, app_recorder_open },
    { "Alarms",     LV_SYMBOL_BELL,      0xFF9500, app_alarms_open },
    { "Timer",      LV_SYMBOL_LOOP,      0x30B0C7, app_timer_open },
    { "Stopwatch",  LV_SYMBOL_PLAY,      0x34C759, app_stopwatch_open },
    { "Activity",   LV_SYMBOL_SHUFFLE,   0x30D158, app_steps_open },
    { "Weather",    LV_SYMBOL_IMAGE,     0x64D2FF, app_weather_open },
    { "Phone music", LV_SYMBOL_AUDIO,    0xFF375F, app_phone_music_open },
    { "Find phone", LV_SYMBOL_CALL,      0x5E5CE6, app_findphone_open },
    { "Flashlight", LV_SYMBOL_CHARGE,    0xFFD60A, app_flashlight_open },
    { "Calculator", LV_SYMBOL_KEYBOARD,  0x8E8E93, app_calculator_open },
};
const int g_app_count = sizeof(g_apps) / sizeof(g_apps[0]);

static bool ext_is(const char *path, const char *const *exts)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return false;
    }
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* implemented in the individual apps */
void music_open_file(const char *abs_path);
void gallery_open_file(const char *abs_path);
void video_open_file(const char *abs_path);
void textview_open_file(const char *abs_path);

void app_open_file(const char *abs_path)
{
    static const char *audio[] = { ".mp3", ".wav", NULL };
    static const char *image[] = { ".jpg", ".jpeg", ".png", ".bmp", NULL };
    static const char *video[] = { ".mjpeg", ".mjpg", ".avi", ".mp4", ".mov", NULL };
    static const char *text[] = { ".txt", ".log", ".md", ".csv", ".json", ".ini", ".cfg", NULL };
    if (ext_is(abs_path, audio)) {
        music_open_file(abs_path);
    } else if (ext_is(abs_path, image)) {
        gallery_open_file(abs_path);
    } else if (ext_is(abs_path, video)) {
        video_open_file(abs_path);
    } else if (ext_is(abs_path, text)) {
        textview_open_file(abs_path);
    } else {
        ui_toast("No app can open this file type");
    }
}
