/*
 * Audio: speaker (ES8311) for music/beeps, microphones (ES7210) for the recorder.
 * They share one I2S bus, so playback and recording don't run at the same time.
 */
#pragma once

#include <stdbool.h>

void audio_init(void);
void audio_apply_volume(void);            /* from settings (volume / muted) */

/* tones (alarms, timer) - stops music playback */
void audio_beep(int freq_hz, int ms);
void audio_beep_stop(void);

/* music player: MP3 / WAV files */
typedef enum { PLAYER_IDLE, PLAYER_PLAYING, PLAYER_PAUSED } player_state_t;
typedef void (*player_done_cb_t)(void);
bool           player_play(const char *abs_path);
void           player_pause(void);
void           player_resume(void);
void           player_stop(void);
player_state_t player_state(void);
const char    *player_current(void);
int            player_elapsed_s(void);
void           player_set_done_cb(player_done_cb_t cb);   /* track finished on its own */

/* recorder: 16 kHz mono 16-bit WAV */
bool rec_start(const char *abs_path);
void rec_stop(void);
bool rec_active(void);
int  rec_seconds(void);
int  rec_level(void);                     /* 0..100 live input level */
