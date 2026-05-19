#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

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

#define BLE_BUF_SIZE 512

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
static volatile bool has_received_data = false;
static char mac_str[18];

// Control command state
static volatile bool ctrl_ready = false;
static volatile uint8_t ctrl_cmd = 0;

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
        s->updateConnParams(conn_handle, 24, 48, 0, 400);
        Serial.printf("BLE: connected from %s\n", info.getAddress().toString().c_str());
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
        if (len >= BLE_BUF_SIZE) len = BLE_BUF_SIZE - 1;
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
        last_activity_ms = millis();
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
            ctrl_cmd = (uint8_t)val[0];
            ctrl_ready = true;
            last_activity_ms = millis();
        }
    }
};

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
    return ctrl_ready;
}

uint8_t ble_get_ctrl_cmd(void) {
    ctrl_ready = false;
    return ctrl_cmd;
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
