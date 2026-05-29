// ota.h — BLE-driven OTA firmware update for ClauLi.
//
// Protocol over the OTA Control characteristic (writes from daemon → ESP,
// notifications from ESP → daemon):
//
//   Commands (daemon → ESP, written to OTA_CONTROL):
//     0x01 [size:4 LE]   BEGIN  — start an upload of N bytes
//     0x02               END    — finalize + commit + scheduled reboot
//     0x03               ABORT  — discard partial upload
//
//   Notifications (ESP → daemon, on OTA_CONTROL):
//     0x10               READY      — ready to receive chunks
//     0x11 [recv:4 LE]   PROGRESS   — bytes received so far (~4 Hz)
//     0x12               DONE       — committed, reboot in ~1.5 s
//     0xE0 [err:1]       ERR_BEGIN  — Update.begin failed (esp_err code)
//     0xE1 [err:1]       ERR_WRITE  — Update.write failed
//     0xE2 [err:1]       ERR_END    — Update.end failed
//
// Bulk data goes through the OTA_DATA characteristic (write-without-response)
// to amortize round-trip latency. See ble.cpp for the GATT registration.

#pragma once
#include <Arduino.h>

enum {
    OTA_NOTIFY_READY     = 0x10,
    OTA_NOTIFY_PROGRESS  = 0x11,
    OTA_NOTIFY_DONE      = 0x12,
    OTA_NOTIFY_ERR_BEGIN = 0xE0,
    OTA_NOTIFY_ERR_WRITE = 0xE1,
    OTA_NOTIFY_ERR_END   = 0xE2,
};

// Notification callback shape: ble.cpp uses this to send notifies over GATT.
typedef void (*ota_notify_cb_t)(uint8_t status, const uint8_t* payload, size_t len);
void ota_set_notify_cb(ota_notify_cb_t cb);

// Status accessors
bool     ota_in_progress(void);
uint32_t ota_received_bytes(void);
uint32_t ota_expected_bytes(void);

// Host-issued commands (called by ble.cpp on the BLE thread)
void ota_cmd_begin(uint32_t total_size);
void ota_cmd_data (const uint8_t* data, size_t len);
void ota_cmd_end  (void);
void ota_cmd_abort(void);

// Called from main loop — handles deferred reboot after DONE.
void ota_tick(void);
