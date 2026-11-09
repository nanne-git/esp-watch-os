/*
 * BLE: Nordic UART Service (NUS) peripheral via NimBLE.
 * Lines from the phone go to Gadgetbridge parsing (GB(...)/setTime) or the text file commands.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void ble_svc_start(void);
bool ble_svc_send(const void *data, size_t len);
bool ble_svc_connected(void);
bool ble_svc_advertising(void);
void ble_svc_pause_adv_for_transfer(void);   /* web transfers: free the shared radio */
void ble_svc_apply_settings(void);           /* name / on-off changed */
void ble_svc_get_addr(char *buf, size_t len);
