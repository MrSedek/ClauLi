// ota.cpp — see ota.h for protocol details.
//
// Implementation notes:
// - Uses the Arduino Update library which wraps esp_ota_*. It picks the
//   inactive OTA slot automatically (set via partition table — see
//   platformio.ini `board_build.partitions = min_spiffs.csv`).
// - Reboot is deferred ~1.5 s after DONE so the notify packet has time to
//   leave and the daemon can close its end of the link gracefully.
// - PROGRESS notifies are throttled to ~4 Hz to keep BLE traffic from
//   competing with the data stream.

#include "ota.h"
#include "emo2.h"
#include <Update.h>

static ota_notify_cb_t notify_cb = nullptr;
static bool     in_progress      = false;
static uint32_t expected         = 0;
static uint32_t received         = 0;
static uint32_t last_progress_ms = 0;
static uint32_t last_chunk_ms    = 0;   // last time we got OTA data bytes
static uint32_t reboot_at_ms     = 0;   // 0 = no pending reboot
// If the daemon stalls / link drops mid-stream, leaving us stuck in
// `in_progress` waiting forever — abort after this much silence + notify
// the daemon so the web UI shows a clear error instead of just freezing.
#define OTA_IDLE_TIMEOUT_MS 8000

static inline void send_notify(uint8_t status, const uint8_t* payload, size_t len) {
    if (notify_cb) notify_cb(status, payload, len);
}

static inline void send_notify_u32(uint8_t status, uint32_t val) {
    uint8_t buf[4] = {
        (uint8_t)(val & 0xFF),
        (uint8_t)((val >> 8) & 0xFF),
        (uint8_t)((val >> 16) & 0xFF),
        (uint8_t)((val >> 24) & 0xFF),
    };
    send_notify(status, buf, 4);
}

void ota_set_notify_cb(ota_notify_cb_t cb) { notify_cb = cb; }

bool     ota_in_progress  (void) { return in_progress; }
uint32_t ota_received_bytes(void) { return received; }
uint32_t ota_expected_bytes(void) { return expected; }

void ota_cmd_begin(uint32_t total_size) {
    if (in_progress) {
        Update.abort();
        in_progress = false;
    }
    Serial.printf("OTA: BEGIN size=%lu\n", (unsigned long)total_size);
    received = 0;
    expected = total_size;
    // U_FLASH = application image. Update.begin sizes the partition and
    // refuses if the image is too large.
    if (!Update.begin(total_size, U_FLASH)) {
        uint8_t err = (uint8_t)Update.getError();
        Serial.printf("OTA: begin failed err=%u (%s)\n", err, Update.errorString());
        send_notify(OTA_NOTIFY_ERR_BEGIN, &err, 1);
        emo2_set_ota_progress(3, 0);
        return;
    }
    in_progress = true;
    last_progress_ms = millis();
    last_chunk_ms    = millis();   // idle-timeout anchor
    send_notify(OTA_NOTIFY_READY, nullptr, 0);
    emo2_set_ota_progress(0, 0);   // show "OTA 0%" overlay
}

void ota_cmd_data(const uint8_t* data, size_t len) {
    if (!in_progress || !data || !len) return;
    size_t wrote = Update.write((uint8_t*)data, len);
    if (wrote != len) {
        uint8_t err = (uint8_t)Update.getError();
        Serial.printf("OTA: write failed wrote=%u/%u err=%u (%s)\n",
                      (unsigned)wrote, (unsigned)len, err, Update.errorString());
        send_notify(OTA_NOTIFY_ERR_WRITE, &err, 1);
        emo2_set_ota_progress(3, 0);
        Update.abort();
        in_progress = false;
        return;
    }
    received += len;
    uint32_t now = millis();
    last_chunk_ms = now;
    if (now - last_progress_ms >= 250) {
        last_progress_ms = now;
        send_notify_u32(OTA_NOTIFY_PROGRESS, received);
        // Mirror the percent on the ESP screen so the user has a visual cue
        // even if they don't have the web UI open.
        uint8_t pct = expected ? (uint8_t)((uint64_t)received * 100 / expected) : 0;
        emo2_set_ota_progress(1, pct);
    }
}

void ota_cmd_end(void) {
    if (!in_progress) return;
    Serial.printf("OTA: END received=%lu expected=%lu\n",
                  (unsigned long)received, (unsigned long)expected);
    send_notify_u32(OTA_NOTIFY_PROGRESS, received);
    // Sanity check BEFORE calling Update.end. If the BLE link dropped a
    // chunk silently (write-without-response had this bug — daemon now uses
    // write-with-response), we'd otherwise hit esp_image_verify failure
    // inside Update.end and the user sees the opaque "code 9" error.
    // Surface a clearer error with custom code 0xFE = "INCOMPLETE".
    if (received != expected) {
        Serial.printf("OTA: incomplete — got %lu of %lu bytes\n",
                      (unsigned long)received, (unsigned long)expected);
        uint8_t err = 0xFE;   // custom: incomplete transfer (not from Update lib)
        send_notify(OTA_NOTIFY_ERR_END, &err, 1);
        emo2_set_ota_progress(3, 0);
        Update.abort();
        in_progress = false;
        return;
    }
    if (!Update.end(true)) {
        uint8_t err = (uint8_t)Update.getError();
        Serial.printf("OTA: end failed err=%u (%s)\n", err, Update.errorString());
        send_notify(OTA_NOTIFY_ERR_END, &err, 1);
        emo2_set_ota_progress(3, 0);   // error
        in_progress = false;
        return;
    }
    in_progress = false;
    send_notify(OTA_NOTIFY_DONE, nullptr, 0);
    emo2_set_ota_progress(2, 100);     // done — stays visible till reboot
    reboot_at_ms = millis() + 1500;
}

void ota_cmd_abort(void) {
    if (!in_progress) return;
    Update.abort();
    in_progress = false;
    received = expected = 0;
    Serial.println("OTA: aborted");
}

void ota_tick(void) {
    if (reboot_at_ms && (int32_t)(millis() - reboot_at_ms) >= 0) {
        Serial.println("OTA: rebooting into new image");
        delay(50);
        ESP.restart();
    }
    // Idle-timeout watchdog. If we're streaming and the daemon stops sending
    // chunks (link dropped, daemon crashed, macOS suspend, etc.) the loop
    // would otherwise sit forever waiting. Abort with a clear error code
    // (0xFD) so the daemon + web UI report "stalled" instead of just hanging.
    if (in_progress &&
        (millis() - last_chunk_ms) > OTA_IDLE_TIMEOUT_MS) {
        Serial.printf("OTA: idle %lums — aborting (got %lu/%lu bytes)\n",
                      (unsigned long)(millis() - last_chunk_ms),
                      (unsigned long)received, (unsigned long)expected);
        uint8_t err = 0xFD;   // custom: idle-timeout (daemon went silent)
        send_notify(OTA_NOTIFY_ERR_END, &err, 1);
        Update.abort();
        in_progress = false;
        emo2_set_ota_progress(3, 0);
    }
}
