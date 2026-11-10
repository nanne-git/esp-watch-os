/*
 * BLE: Nordic UART Service (NUS) peripheral via NimBLE.
 * Lines from the phone go to Gadgetbridge parsing (GB(...)/setTime) or the text file commands.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void ble_svc_start(void);
/* Send ONE text line to the phone (NUS TX). The line ending is added/normalised to "\r\n"
 * (Gadgetbridge drops the char before "\n"), the message is queued and sent whole by a TX task,
 * so lines from different tasks never interleave. Safe to call from any task, incl. the BLE host.
 * Returns false if not connected/subscribed or the queue is full. */
bool ble_svc_send(const void *data, size_t len);
bool ble_svc_connected(void);
bool ble_svc_advertising(void);
void ble_svc_pause_adv_for_transfer(void);   /* web transfers: free the shared radio */
void ble_svc_apply_settings(void);           /* name / on-off changed */
void ble_svc_get_addr(char *buf, size_t len);
