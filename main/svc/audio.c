#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

static const char *TAG = "audio";

static esp_codec_dev_handle_t s_spk, s_mic;
static SemaphoreHandle_t      s_lock;          /* speaker device ownership */
static bool                   s_spk_open;
static uint32_t               s_spk_rate;
static uint8_t                s_spk_ch, s_spk_bits;
static bool                   s_player_ready;
static volatile player_state_t s_pstate = PLAYER_IDLE;
static char                   s_current[256];
static int64_t                s_play_start_us, s_play_paused_acc_us, s_pause_at_us;
static player_done_cb_t       s_done_cb;
static volatile bool          s_user_stop;

/* ------------------------------------------------------------------ speaker device */

static bool spk_open(uint32_t rate, uint8_t bits, uint8_t ch)
{
    if (!s_spk) {
        return false;
    }
    if (s_spk_open && rate == s_spk_rate && bits == s_spk_bits && ch == s_spk_ch) {
        return true;
    }
    if (s_spk_open) {
        esp_codec_dev_close(s_spk);
        s_spk_open = false;
    }
    esp_codec_dev_sample_info_t fs = { .sample_rate = rate, .channel = ch, .bits_per_sample = bits };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed (%lu Hz)", (unsigned long)rate);
        return false;
    }
    s_spk_open = true;
    s_spk_rate = rate;
    s_spk_bits = bits;
    s_spk_ch = ch;
    audio_apply_volume();
    return true;
}

static void spk_close(void)
{
    if (s_spk_open) {
        esp_codec_dev_close(s_spk);
        s_spk_open = false;
    }
}

void audio_apply_volume(void)
{
    if (s_spk && s_spk_open) {
        const settings_t *s = settings();
        esp_codec_dev_set_out_vol(s_spk, s->muted ? 0 : s->volume);
    }
}

static void on_settings(void)
{
    audio_apply_volume();
}

/* ------------------------------------------------------------------ player callbacks */

static esp_err_t pl_write(void *buf, size_t len, size_t *written, uint32_t timeout_ms)
{
    if (s_spk_open && esp_codec_dev_write(s_spk, buf, len) == ESP_CODEC_DEV_OK) {
        *written = len;
        return ESP_OK;
    }
    *written = 0;
    return ESP_FAIL;
}

static esp_err_t pl_clk(uint32_t rate, uint32_t bits, i2s_slot_mode_t ch)
{
    return spk_open(rate, bits, ch == I2S_SLOT_MODE_STEREO ? 2 : 1) ? ESP_OK : ESP_FAIL;
}

static esp_err_t pl_mute(AUDIO_PLAYER_MUTE_SETTING setting)
{
    if (s_spk && s_spk_open) {
        esp_codec_dev_set_out_mute(s_spk, setting == AUDIO_PLAYER_MUTE);
    }
    return ESP_OK;
}

static void pl_event(audio_player_cb_ctx_t *ctx)
{
    switch (ctx->audio_event) {
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
        s_pstate = PLAYER_PLAYING;
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
        s_pstate = PLAYER_PAUSED;
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
        if (s_pstate != PLAYER_IDLE) {
            s_pstate = PLAYER_IDLE;
            if (!s_user_stop && s_done_cb) {
                s_done_cb();          /* finished on its own: let the app play the next track */
            }
            s_user_stop = false;
        }
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE:
        ESP_LOGW(TAG, "unsupported file: %s", s_current);
        s_pstate = PLAYER_IDLE;
        break;
    default:
        break;
    }
}

static bool player_ensure(void)
{
    if (s_player_ready) {
        return true;
    }
    audio_player_config_t cfg = {
        .mute_fn = pl_mute,
        .clk_set_fn = pl_clk,
        .write_fn = pl_write,
        .priority = 6,
        .coreID = 1,
    };
    if (audio_player_new(cfg) != ESP_OK) {
        ESP_LOGE(TAG, "player init failed");
        return false;
    }
    audio_player_callback_register(pl_event, NULL);
    s_player_ready = true;
    return true;
}

/* ------------------------------------------------------------------ player API */

bool player_play(const char *path)
{
    if (rec_active()) {
        rec_stop();
    }
    audio_beep_stop();
    if (!player_ensure()) {
        return false;
    }
    if (s_pstate != PLAYER_IDLE) {
        s_user_stop = true;
        audio_player_stop();
        for (int i = 0; i < 50 && s_pstate != PLAYER_IDLE; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "can't open %s", path);
        return false;
    }
    strlcpy(s_current, path, sizeof(s_current));
    s_user_stop = false;
    s_play_start_us = esp_timer_get_time();
    s_play_paused_acc_us = 0;
    s_pstate = PLAYER_PLAYING;
    if (audio_player_play(f) != ESP_OK) {         /* the player closes the file */
        s_pstate = PLAYER_IDLE;
        return false;
    }
    return true;
}

void player_pause(void)
{
    if (s_pstate == PLAYER_PLAYING) {
        audio_player_pause();
        s_pause_at_us = esp_timer_get_time();
        s_pstate = PLAYER_PAUSED;
    }
}

void player_resume(void)
{
    if (s_pstate == PLAYER_PAUSED) {
        audio_player_resume();
        s_play_paused_acc_us += esp_timer_get_time() - s_pause_at_us;
        s_pstate = PLAYER_PLAYING;
    }
}

void player_stop(void)
{
    if (s_player_ready && s_pstate != PLAYER_IDLE) {
        s_user_stop = true;
        audio_player_stop();
    }
    s_pstate = PLAYER_IDLE;
}

player_state_t player_state(void) { return s_pstate; }
const char *player_current(void) { return s_current; }
void player_set_done_cb(player_done_cb_t cb) { s_done_cb = cb; }

int player_elapsed_s(void)
{
    if (s_pstate == PLAYER_IDLE) {
        return 0;
    }
    int64_t now = s_pstate == PLAYER_PAUSED ? s_pause_at_us : esp_timer_get_time();
    return (int)((now - s_play_start_us - s_play_paused_acc_us) / 1000000);
}

/* ------------------------------------------------------------------ beeps */

static volatile int s_beep_freq, s_beep_ms;
static volatile bool s_beep_stop;
static TaskHandle_t s_beep_task;

static void beep_task(void *arg)
{
    const int rate = 16000;
    int16_t *buf = malloc(256 * sizeof(int16_t));
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!buf || s_pstate != PLAYER_IDLE || rec_active()) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (spk_open(rate, 16, 1)) {
            s_beep_stop = false;
            int total = rate * s_beep_ms / 1000;
            float phase = 0, step = 2 * (float)M_PI * s_beep_freq / rate;
            for (int done = 0; done < total && !s_beep_stop; done += 256) {
                for (int i = 0; i < 256; i++) {
                    /* square-ish tone is louder on a small speaker; soft edges avoid clicks */
                    float env = (done + i < 400) ? (done + i) / 400.0f : 1.0f;
                    buf[i] = (int16_t)(sinf(phase) > 0 ? 9000 * env : -9000 * env);
                    phase += step;
                    if (phase > 2 * (float)M_PI) phase -= 2 * (float)M_PI;
                }
                esp_codec_dev_write(s_spk, buf, 256 * sizeof(int16_t));
            }
            memset(buf, 0, 256 * sizeof(int16_t));
            esp_codec_dev_write(s_spk, buf, 256 * sizeof(int16_t));
        }
        xSemaphoreGive(s_lock);
    }
}

void audio_beep(int freq, int ms)
{
    if (settings()->muted || !s_beep_task) {
        return;
    }
    s_beep_freq = freq;
    s_beep_ms = ms;
    xTaskNotifyGive(s_beep_task);
}

void audio_beep_stop(void)
{
    s_beep_stop = true;
}

/* ------------------------------------------------------------------ recorder */

static volatile bool s_rec;
static volatile int  s_rec_level;
static int64_t       s_rec_start;
static char          s_rec_path[256];

static void wav_header(FILE *f, uint32_t data_bytes)
{
    const uint32_t rate = 16000, byte_rate = rate * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4) = 36 + data_bytes;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 1;        /* PCM */
    *(uint16_t *)(h + 22) = 1;        /* mono */
    *(uint32_t *)(h + 24) = rate;
    *(uint32_t *)(h + 28) = byte_rate;
    *(uint16_t *)(h + 32) = 2;
    *(uint16_t *)(h + 34) = 16;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = data_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void rec_task(void *arg)
{
    FILE *f = fopen(s_rec_path, "wb");
    int16_t *buf = malloc(1024 * sizeof(int16_t));
    uint32_t bytes = 0;
    if (!f || !buf) {
        ESP_LOGE(TAG, "recorder: can't open %s", s_rec_path);
        s_rec = false;
    } else {
        wav_header(f, 0);
        esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 1, .bits_per_sample = 16 };
        if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "mic open failed");
            s_rec = false;
        } else {
            esp_codec_dev_set_in_gain(s_mic, 30.0f);
        }
    }
    while (s_rec) {
        if (esp_codec_dev_read(s_mic, buf, 1024 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t sum = 0;
        for (int i = 0; i < 1024; i++) {
            sum += (int32_t)buf[i] * buf[i];
        }
        int rms = (int)sqrtf((float)(sum / 1024));
        int lvl = rms * 100 / 6000;
        s_rec_level = lvl > 100 ? 100 : lvl;
        if (fwrite(buf, 1, 1024 * sizeof(int16_t), f) != 1024 * sizeof(int16_t)) {
            ESP_LOGW(TAG, "recorder: write failed (card full?)");
            break;
        }
        bytes += 1024 * sizeof(int16_t);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    if (f) {
        wav_header(f, bytes);
        fclose(f);
    }
    free(buf);
    s_rec = false;
    s_rec_level = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "recording saved: %s (%lu bytes)", s_rec_path, (unsigned long)bytes);
    vTaskDelete(NULL);
}

bool rec_start(const char *path)
{
    if (s_rec || !s_mic) {
        return false;
    }
    player_stop();
    audio_beep_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    spk_close();                       /* the mic re-clocks the shared I2S bus */
    strlcpy(s_rec_path, path, sizeof(s_rec_path));
    s_rec = true;
    s_rec_start = esp_timer_get_time();
    if (xTaskCreatePinnedToCore(rec_task, "rec", 4096, NULL, 6, NULL, 1) != pdPASS) {
        s_rec = false;
        xSemaphoreGive(s_lock);
        return false;
    }
    return true;
}

void rec_stop(void)
{
    if (s_rec) {
        s_rec = false;
        for (int i = 0; i < 50; i++) {         /* wait for the file to be finished */
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                xSemaphoreGive(s_lock);
                break;
            }
        }
    }
}

bool rec_active(void) { return s_rec; }
int rec_seconds(void) { return s_rec ? (int)((esp_timer_get_time() - s_rec_start) / 1000000) : 0; }
int rec_level(void) { return s_rec_level; }

/* ------------------------------------------------------------------ init */

void audio_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    ESP_LOGI(TAG, "speaker %s, microphone %s", s_spk ? "ok" : "missing", s_mic ? "ok" : "missing");
    settings_on_change(on_settings);
    xTaskCreate(beep_task, "beep", 3072, NULL, 5, &s_beep_task);
}
