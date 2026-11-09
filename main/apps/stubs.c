/* Temporary placeholders - each is replaced by a real app file as it gets written. */
#include "apps.h"
#include "ui.h"

static void soon_build(lv_obj_t *c, void *ctx)
{
    lv_obj_t *l = ui_label(c, "Coming soon", &lv_font_montserrat_24, UI_MUTED);
    lv_obj_set_style_pad_top(l, 120, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}
#define STUB(fn, title) __attribute__((weak)) void fn(void) { ui_page_push(title, soon_build, NULL, NULL); }
#define STUB_PATH(fn, title) __attribute__((weak)) void fn(const char *p) { ui_page_push(title, soon_build, NULL, NULL); }

STUB(app_settings_open, "Settings")
STUB(app_files_open, "Files")
STUB_PATH(app_files_open_path, "Files")
STUB(app_music_open, "Music")
STUB_PATH(music_open_file, "Music")
STUB(app_gallery_open, "Gallery")
STUB_PATH(gallery_open_file, "Gallery")
STUB(app_video_open, "Video")
STUB_PATH(video_open_file, "Video")
STUB_PATH(textview_open_file, "Text")
STUB(app_recorder_open, "Recorder")
STUB(app_alarms_open, "Alarms")
STUB(app_timer_open, "Timer")
STUB(app_stopwatch_open, "Stopwatch")
STUB(app_steps_open, "Activity")
STUB(app_weather_open, "Weather")
STUB(app_flashlight_open, "Flashlight")
STUB(app_findphone_open, "Find phone")
STUB(app_phone_music_open, "Phone music")
STUB(app_calculator_open, "Calculator")
STUB(app_about_open, "About")
