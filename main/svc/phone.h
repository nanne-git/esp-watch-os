/*
 * Phone link data (fed by Gadgetbridge over BLE): notifications, calls, phone music,
 * plus watch->phone actions. The UI reads these stores; services call ui hooks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define NOTIF_MAX 30

typedef struct {
    uint32_t id;
    time_t   when;
    char     app[24];
    char     title[64];
    char     body[200];
} notif_t;

/* notifications (newest first) */
int  notif_count(void);
bool notif_get(int index, notif_t *out);
void notif_add(const notif_t *n);           /* also triggers toast + vibration (respects DND) */
void notif_remove_id(uint32_t id);
void notif_clear(void);
bool notif_dnd_active(void);

/* calls */
typedef struct {
    bool active;
    char name[48];
    char number[24];
} call_t;
void call_get(call_t *out);
void call_set_incoming(const char *name, const char *number);
void call_end(void);
void call_action(const char *action);       /* "ACCEPT", "REJECT", "IGNORE", "END" -> phone */

/* phone music */
typedef struct {
    bool  valid;
    bool  playing;
    char  artist[48];
    char  track[64];
    int   duration_s;
    int   position_s;
} phone_music_t;
void phone_music_get(phone_music_t *out);
void phone_music_set_info(const char *artist, const char *track, int dur);
void phone_music_set_state(bool playing, int position);
void phone_music_cmd(const char *cmd);      /* "play","pause","next","previous","volumeup","volumedown" */

/* misc phone actions */
void phone_find(bool ring);
void phone_send_battery(void);
bool phone_connected(void);

/* Gadgetbridge line handler (one complete line from BLE) */
void gb_handle_line(const char *line);
