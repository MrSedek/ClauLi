#include "ble.h"
#include "ota.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_system.h>          // esp_reset_reason()
#include <esp_heap_caps.h>       // heap_caps_get_free_size / get_minimum_free_size

// Daemon-only mode: HID keyboard disabled so macOS never grabs ClauLi as
// a bonded keyboard (that link kept the ESP from advertising for the
// daemon). With HID off we also drop BLE bonding entirely, giving the
// daemon a single, stable, pairing-free connection. Set to 1 to restore
// the BLE HID keyboard (re-enables bonding).
#define BLE_HID_ENABLED 0

#if BLE_HID_ENABLED
#include <NimBLEHIDDevice.h>
#endif

#define DEVICE_NAME "ClauLi"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request
#define CTRL_CHAR_UUID      "4c41555a-4465-7669-6365-000000000005"  // host→device control commands

// OTA service — separate service so it can be discovered + used independently.
#define OTA_SERVICE_UUID    "4c41555a-4465-7669-6365-000000000008"
#define OTA_CTRL_CHAR_UUID  "4c41555a-4465-7669-6365-000000000009"  // commands + status
#define OTA_DATA_CHAR_UUID  "4c41555a-4465-7669-6365-00000000000a"  // bulk data

// 2 KB gives the emo2 cfg blob (~720 B today) ample headroom for future
// growth (e.g. per-state extras, additional layouts). The buffer lives in
// BSS so the extra 1.5 KB doesn't move the stack/heap watermark.
#define BLE_BUF_SIZE 2048

#if BLE_HID_ENABLED
// HID keyboard report descriptor
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (224) - Left Control
    0x29, 0xE7,  //   Usage Maximum (231) - Right GUI
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant) - Reserved byte
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x65,  //   Usage Maximum (101)
    0x81, 0x00,  //   Input (Data, Array) - Key array (6 keys)
    0xC0,        // End Collection
};
#endif  // BLE_HID_ENABLED

static NimBLEServer* server = nullptr;
#if BLE_HID_ENABLED
static NimBLEHIDDevice* hid_dev = nullptr;
#endif
static NimBLECharacteristic* input_kbd = nullptr;  // null when HID disabled
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;
static NimBLECharacteristic* ctrl_char = nullptr;
static NimBLECharacteristic* ota_ctrl_char = nullptr;
static NimBLECharacteristic* ota_data_char = nullptr;

// Shared across the NimBLE host task (server/char callbacks) and the
// Arduino loop task (ble_tick) — must be volatile like the other flags.
static volatile ble_state_t state = BLE_STATE_INIT;
static volatile bool need_advertise = false;
// Liveness watchdog: recover even if onDisconnect never fires (macOS can
// tear the link down without a clean LL terminate; the host stack then
// stays "connected" forever and never re-advertises until a power cycle).
static volatile uint32_t last_activity_ms = 0;
static volatile uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
// Backstop only: the 4s supervision timeout (set in onConnect) handles a
// genuinely dropped link via onDisconnect. This watchdog covers the rare
// "onDisconnect never fires" case. Kept long (5 min) so a healthy link
// that is merely idle — e.g. the daemon is connected but its OAuth token
// is broken, so no payload is sent — is NOT force-cycled every poll.
#define LINK_DEAD_MS 300000UL
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;
// Diagnostic bridge — fires one "[BOOT] …" notify on TX after every
// connection so the daemon can log reset reason + heap + uptime without
// needing serial cable access. Useful for catching panic / WDT crashes
// on a device the user can't physically reach.
//   diag_pending: set in onConnect, cleared after first successful send.
//   diag_send_at_ms: hold-off so the central has time to subscribe to TX
//     before we shout — sending too early gets the notify silently
//     dropped (BLE has no buffer for "send-on-subscribe").
static volatile bool     diag_pending   = false;
static volatile uint32_t diag_send_at_ms = 0;
// Captured at first connect (esp_reset_reason() is stable across the
// lifetime of the process, so we only need to read it once).
static int   diag_reset_reason = -1;
// Resolve reset reason → short ASCII label. ESP_RST_* are enum constants
// (NOT preprocessor macros), so #ifdef guards on them silently evaluate
// to false and the case falls through — that's why a previous version
// printed UNKNOWN(11) instead of USB(11). Switching to numeric literals
// dodges the entire header-availability problem and works across every
// ESP-IDF version that we'd reasonably ship against. Numbers come from
// esp_system.h's esp_reset_reason_t in ESP-IDF 5.x (Arduino-ESP32 3.x).
static const char* reset_reason_label(int r){
    switch (r) {
        case 0:  return "UNKNOWN";     // ESP_RST_UNKNOWN  — couldn't determine
        case 1:  return "POWERON";     // ESP_RST_POWERON  — VDD applied
        case 2:  return "EXT";         // ESP_RST_EXT      — external pin reset
        case 3:  return "SOFTWARE";    // ESP_RST_SW       — esp_restart()
        case 4:  return "PANIC";       // ESP_RST_PANIC    — exception / abort
        case 5:  return "INT_WDT";     // ESP_RST_INT_WDT  — interrupt WDT
        case 6:  return "TASK_WDT";    // ESP_RST_TASK_WDT — task WDT
        case 7:  return "WDT";         // ESP_RST_WDT      — other WDT
        case 8:  return "DEEPSLEEP";   // ESP_RST_DEEPSLEEP
        case 9:  return "BROWNOUT";    // ESP_RST_BROWNOUT — undervolt
        case 10: return "SDIO";        // ESP_RST_SDIO
        case 11: return "USB";         // ESP_RST_USB      — USB JTAG host re-enumeration
        case 12: return "JTAG";        // ESP_RST_JTAG
        case 13: return "EFUSE";       // ESP_RST_EFUSE
        case 14: return "PWR_GLITCH";  // ESP_RST_PWR_GLITCH
        case 15: return "CPU_LOCKUP";  // ESP_RST_CPU_LOCKUP
        default: return "OTHER";
    }
}

// Chunked-payload reassembly state. When the daemon sends a JSON blob
// larger than (MTU - 3) bytes it can't fit in one ATT write, so it splits
// the payload across multiple writes with a 1-byte type header:
//   - 0x01: FIRST frame [0x01, total_lo, total_hi, json_data...]
//   - 0x02: CONT  frame [0x02, json_data...]
// rx_received reaches rx_expected → buffer holds the complete JSON →
// data_ready is set so the main-loop consumer (e.g. emo2_apply_cfg_json)
// runs the parse exactly once per reassembled payload.
// A 0x7B ('{') first-byte still hits the legacy fast-path (single write
// = single JSON), so this is fully backward-compatible with hosts that
// don't chunk.
static size_t rx_expected = 0;   // total bytes the reassembled JSON should be
static size_t rx_received = 0;   // bytes accumulated so far in rx_buf
static volatile bool has_received_data = false;
static char mac_str[18];

// Control command queue — small ring buffer so rapid host writes don't get
// clobbered by the single-slot mailbox bug (e.g. a per-state config push that
// sends form+op+color in <1 ms would lose 2 of 3 bytes).
#define CTRL_QUEUE_SIZE 32
static volatile uint8_t  ctrl_queue[CTRL_QUEUE_SIZE];
static volatile uint8_t  ctrl_head = 0;     // producer (BLE cb) writes here
static volatile uint8_t  ctrl_tail = 0;     // consumer (main loop) reads here

static bool start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    adv->addServiceUUID(SERVICE_UUID);
#if BLE_HID_ENABLED
    adv->setAppearance(HID_KEYBOARD);
#endif
    adv->enableScanResponse(true);
    adv->setName(DEVICE_NAME);
    bool ok = adv->start();
    // start() can return false if advertising is already running — that's
    // still the desired state, so don't demote to DISCONNECTED in that case.
    if (!ok && adv->isAdvertising()) ok = true;
    state = ok ? BLE_STATE_ADVERTISING : BLE_STATE_DISCONNECTED;
    Serial.printf("BLE: advertising start=%s\n", ok ? "OK" : "FAILED");
    return ok;
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        conn_handle = info.getConnHandle();
        last_activity_ms = millis();
        // Request a short supervision timeout (4.0s) so a dropped link is
        // detected quickly and onDisconnect fires (macOS otherwise leaves
        // the peripheral hung in CONNECTED). Interval 30–60ms, latency 0.
        // 30–60 ms interval, latency 0, supervision 10 s. Originally 4 s
        // (= 400) but that tore the link down whenever macOS went quiet
        // for a couple of seconds. 10 s is the macOS-friendly default;
        // the 5-min app-level watchdog still catches genuinely dead links.
        // supervision 20 s (was 10 s). macOS CoreBluetooth normally caps the
        // negotiated timeout at its own ~5-6 s anyway, so the request is a
        // hint; but bumping it gives us headroom on platforms that honour it
        // and is harmless where it's clamped. Disconnects-every-30-60-s after
        // last batch suggest the peer is dropping during quiet periods —
        // letting it tolerate longer silence helps.
        s->updateConnParams(conn_handle, 24, 48, 0, 2000);
        Serial.printf("BLE: connected from %s\n", info.getAddress().toString().c_str());
        // Capture reset reason once (it never changes for this process).
        if (diag_reset_reason < 0) diag_reset_reason = (int)esp_reset_reason();
        // Queue a one-shot boot/heap diagnostic notify. 2000 ms hold-off
        // — bleak's macOS / CoreBluetooth path does service discovery +
        // multiple start_notify() calls in series after CONNECT, and
        // 500 ms wasn't long enough: the notify fired before subscribe
        // was active and got dropped silently (BLE has no "send-on-
        // subscribe" buffer). 2 s puts us well past bleak's typical
        // post-connect setup window on every platform.
        diag_pending    = true;
        diag_send_at_ms = millis() + 2000;
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        state = BLE_STATE_DISCONNECTED;
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        need_advertise = true;
        Serial.printf("BLE: disconnected (reason=%d)\n", reason);
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        size_t len = val.length();
        if (len == 0) return;
        const uint8_t* raw = (const uint8_t*)val.c_str();
        uint8_t hdr = raw[0];
        last_activity_ms = millis();
        has_received_data = true;

        if (hdr == 0x01) {
            // FIRST chunk: [0x01, total_lo, total_hi, json_data...]
            // Any partial reassembly in progress is abandoned — a fresh 0x01
            // is the unambiguous "start of new payload" signal.
            if (len < 3) { rx_expected = rx_received = 0; return; }
            size_t expected = (size_t)raw[1] | ((size_t)raw[2] << 8);
            if (expected == 0 || expected >= BLE_BUF_SIZE) {
                // Garbage header — drop and reset.
                rx_expected = rx_received = 0;
                Serial.printf("[RX] bad first-chunk total=%u (max %d)\n",
                              (unsigned)expected, BLE_BUF_SIZE - 1);
                return;
            }
            rx_expected = expected;
            rx_received = 0;
            size_t payload_len = len - 3;
            if (payload_len > rx_expected) payload_len = rx_expected;
            memcpy(rx_buf, raw + 3, payload_len);
            rx_received = payload_len;
        } else if (hdr == 0x02) {
            // CONTINUATION chunk: [0x02, json_data...]
            // No active reassembly → out-of-order continuation; drop.
            if (rx_expected == 0) {
                Serial.printf("[RX] orphan continuation (no active frame)\n");
                return;
            }
            size_t payload_len = len - 1;
            size_t room = rx_expected - rx_received;
            if (payload_len > room) payload_len = room;
            memcpy(rx_buf + rx_received, raw + 1, payload_len);
            rx_received += payload_len;
        } else {
            // Legacy single-write JSON (small payloads ≤ MTU-3). First byte
            // is '{' for any well-formed JSON object, so anything else falls
            // through to here — historical clients keep working.
            if (len >= BLE_BUF_SIZE) len = BLE_BUF_SIZE - 1;
            memcpy(rx_buf, raw, len);
            rx_buf[len] = '\0';
            data_ready = true;
            rx_expected = rx_received = 0;
            return;
        }

        // Reassembly finished? Null-terminate, hand off to the consumer.
        if (rx_received >= rx_expected) {
            if (rx_received >= BLE_BUF_SIZE) rx_received = BLE_BUF_SIZE - 1;
            rx_buf[rx_received] = '\0';
            data_ready = true;
            rx_expected = rx_received = 0;
        }
    }
};

// When the daemon enables notifications on the refresh char, ask for data
// if we have none yet.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        last_activity_ms = millis();
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

// CTRL command bytes (must match daemon + main.cpp dispatch)
// 0x01=Usage 0x02=BT 0x03=Splash 0x04=Cycle 0x05=Reboot 0x06=EMO
// 0x07=CycleView 0x08=NextAnim 0x10=Refresh 0x40=Lang EN 0x41=Lang RU
// 0x42=Lang EN(set) 0x43=Lang RU(set)
class CtrlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        if (val.length() >= 1) {
            uint8_t next = (ctrl_head + 1) % CTRL_QUEUE_SIZE;
            if (next != ctrl_tail) {          // queue not full
                ctrl_queue[ctrl_head] = (uint8_t)val[0];
                ctrl_head = next;
            }
            // If full (32 pending in <1ms — very unlikely), drop. Better than
            // overwriting the head and silently losing an already-queued byte.
            last_activity_ms = millis();
        }
    }
};

// OTA control: daemon writes 1-byte cmd + payload, ESP notifies status.
class OtaCtrlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        if (val.empty()) return;
        uint8_t cmd = (uint8_t)val[0];
        last_activity_ms = millis();
        switch (cmd) {
            case 0x01: {  // BEGIN [size:4 LE]
                if (val.length() < 5) return;
                uint32_t size = (uint8_t)val[1]
                              | ((uint32_t)(uint8_t)val[2] << 8)
                              | ((uint32_t)(uint8_t)val[3] << 16)
                              | ((uint32_t)(uint8_t)val[4] << 24);
                ota_cmd_begin(size);
                break;
            }
            case 0x02: ota_cmd_end();   break;  // END
            case 0x03: ota_cmd_abort(); break;  // ABORT
            default: break;
        }
    }
};

// OTA data: daemon writes raw firmware bytes (write-without-response).
class OtaDataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        if (val.empty()) return;
        ota_cmd_data((const uint8_t*)val.data(), val.length());
        last_activity_ms = millis();
    }
};

// Bridge ota.cpp's notify callback to a GATT notification on OTA_CTRL.
static void ota_notify_send(uint8_t status, const uint8_t* payload, size_t len) {
    if (!ota_ctrl_char) return;
    uint8_t buf[16];
    buf[0] = status;
    size_t n = 1;
    if (payload && len && len <= sizeof(buf) - 1) {
        memcpy(buf + 1, payload, len);
        n += len;
    }
    ota_ctrl_char->setValue(buf, n);
    ota_ctrl_char->notify();
}

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
#if BLE_HID_ENABLED
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC
#else
    // No HID → no bonding. Pairing-free link; clear any stale bond left
    // by a previous macOS pairing so the OS can't cling to it.
    NimBLEDevice::setSecurityAuth(false, false, false);
    NimBLEDevice::deleteAllBonds();
#endif

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

#if BLE_HID_ENABLED
    // --- HID keyboard service ---
    hid_dev = new NimBLEHIDDevice(server);
    hid_dev->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    hid_dev->setManufacturer("Anthropic");
    hid_dev->setPnp(0x02, 0x05AC, 0x820A, 0x0210);  // BT SIG, generic keyboard
    hid_dev->setHidInfo(0x00, 0x02);  // country=0, flags=normally connectable
    hid_dev->setBatteryLevel(100);
    input_kbd = hid_dev->getInputReport(1);  // report ID 1
#endif

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    // Control characteristic — host sends commands
    ctrl_char = svc->createCharacteristic(
        CTRL_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static CtrlCallbacks ctrlCb;
    ctrl_char->setCallbacks(&ctrlCb);

    svc->start();

    // --- OTA service (firmware update over BLE) ---
    NimBLEService* ota_svc = server->createService(OTA_SERVICE_UUID);
    ota_ctrl_char = ota_svc->createCharacteristic(
        OTA_CTRL_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    static OtaCtrlCallbacks otaCtrlCb;
    ota_ctrl_char->setCallbacks(&otaCtrlCb);
    ota_data_char = ota_svc->createCharacteristic(
        OTA_DATA_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static OtaDataCallbacks otaDataCb;
    ota_data_char->setCallbacks(&otaDataCb);
    ota_svc->start();
    ota_set_notify_cb(ota_notify_send);

    // Larger MTU = larger BLE payload per packet = much faster OTA throughput.
    // 517 is the BLE 4.2 max; the daemon negotiates it during OTA.
    NimBLEDevice::setMTU(517);
    server->start();
    // NOTE: deliberately NOT using server->advertiseOnDisconnect(true).
    // Re-advertising is driven explicitly by onDisconnect → need_advertise
    // → ble_tick (plus the isAdvertising watchdog). Enabling the stack's
    // auto-restart too would race with start_advertising()'s adv->reset()
    // and could tear down an already-running advertisement.
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    static uint32_t last_adv_ms = 0;
    const uint32_t ADV_RETRY_MS = 1000;
    uint32_t now = millis();

    // Liveness watchdog: if the stack still thinks it's CONNECTED but no
    // RX/CTRL/subscribe traffic has arrived for LINK_DEAD_MS, the link is
    // dead (macOS tore it down without a clean terminate and onDisconnect
    // never fired). Force the disconnect so we re-advertise. Trade-off:
    // with a broken token the daemon stays connected but sends no payload,
    // so the link is force-cycled ~every 90s — acceptable, self-heals once
    // the token is fixed.
    if (state == BLE_STATE_CONNECTED &&
        (now - last_activity_ms) > LINK_DEAD_MS) {
        Serial.println("BLE: link dead — forcing reconnect");
        if (server && server->getConnectedCount() > 0 &&
            conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            server->disconnect(conn_handle);
        }
        state = BLE_STATE_DISCONNECTED;
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        need_advertise = true;
    }

    // Self-healing watchdog: if we're not connected and the controller is
    // not actually advertising, re-arm the restart (covers a lost flag or
    // an onDisconnect that never fired).
    if (state != BLE_STATE_CONNECTED && !need_advertise &&
        (now - last_adv_ms) >= ADV_RETRY_MS &&
        !NimBLEDevice::getAdvertising()->isAdvertising()) {
        need_advertise = true;
    }

    if (need_advertise && (now - last_adv_ms) >= ADV_RETRY_MS) {
        last_adv_ms = now;
        // Keep the flag set until start() actually succeeds so a failed
        // attempt is retried on the next tick instead of going dark until
        // a power cycle.
        if (start_advertising()) need_advertise = false;
    }

    // Periodic [HEAP] heartbeat over the TX notify channel — fires every
    // 60 s while connected so the daemon log gets a trail of heap stats
    // alongside the boot diag. Watching `min` drift across heartbeats is
    // the cheapest leak detector we have on this device. Disconnects show
    // up against this trail too — last seen heap before the drop pins
    // down whether it was OOM-driven.
    static uint32_t heap_tx_last_ms = 0;
    if (state == BLE_STATE_CONNECTED && tx_char &&
        (now - heap_tx_last_ms) >= 60000UL) {
        heap_tx_last_ms = now;
        char hbuf[80];
        int n = snprintf(hbuf, sizeof(hbuf),
            "[HEAP] free=%u min=%u up=%lu",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)(now / 1000));
        if (n > 0) {
            tx_char->setValue((uint8_t*)hbuf, (size_t)n);
            tx_char->notify();
        }
    }

    // Diagnostic bridge — fire one "[BOOT] …" notify per connection.
    // Format chosen so the daemon's TX-notify handler can `if startswith
    // b"[BOOT]"` + log directly to daemon.log. Carries:
    //   reason=<label> (<code>)  — esp_reset_reason()
    //   free=<bytes>             — current free heap
    //   min=<bytes>              — minimum free heap since boot
    //   up=<seconds>             — process uptime (millis() / 1000)
    // Uses snprintf into a stack buffer; fits inside the negotiated MTU
    // by a wide margin (~80 bytes typical).
    if (diag_pending && state == BLE_STATE_CONNECTED && tx_char &&
        (int32_t)(now - diag_send_at_ms) >= 0) {
        char buf[100];
        size_t free_now  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        int n = snprintf(buf, sizeof(buf),
            "[BOOT] reason=%s(%d) free=%u min=%u up=%lu",
            reset_reason_label(diag_reset_reason), diag_reset_reason,
            (unsigned)free_now, (unsigned)free_min,
            (unsigned long)(now / 1000));
        if (n > 0) {
            tx_char->setValue((uint8_t*)buf, (size_t)n);
            tx_char->notify();
            Serial.printf("BLE: sent diag → %s\n", buf);
        }
        diag_pending = false;
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    NimBLEDevice::deleteAllBonds();
    Serial.println("BLE: bonds cleared");
    if (state == BLE_STATE_CONNECTED) {
        server->disconnect(server->getPeerInfo(0).getConnHandle());
    }
    need_advertise = true;
}

bool ble_has_data(void) {
    return data_ready;
}

const char* ble_get_data(void) {
    data_ready = false;
    return rx_buf;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

bool ble_has_ctrl_cmd(void) {
    return ctrl_head != ctrl_tail;
}

uint8_t ble_get_ctrl_cmd(void) {
    if (ctrl_head == ctrl_tail) return 0;
    uint8_t cmd = ctrl_queue[ctrl_tail];
    ctrl_tail = (ctrl_tail + 1) % CTRL_QUEUE_SIZE;
    return cmd;
}

void ble_keyboard_press(uint8_t key, uint8_t modifier) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    // HID report: [modifier, reserved, key1, key2, key3, key4, key5, key6]
    uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}

void ble_keyboard_release(void) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}
