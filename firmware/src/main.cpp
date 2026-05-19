#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "i18n.h"
#include "emo.h"
#include <Preferences.h>
#include <esp_system.h>

// ─── Hardware objects (allocated in setup() — C6 crashes on global new) ────
Arduino_DataBus *bus = nullptr;
Arduino_ST7789 *gfx = nullptr;
SPIClass *vspi = nullptr;

static UsageData usage = {};

// Language persistence (NVS). The daemon's --lang only seeds the language
// until the user picks one on-device; after that the device choice wins.
static Preferences g_prefs;
static bool g_lang_user_set = false;

// ─── Rotation state ─────────────────────────────────────────────────────────
uint8_t current_rotation = 0;  // 0..3

// When ST7789 rotates 90°/270°, the logical width/height swap.
static int16_t logical_w() { return (current_rotation % 2 == 0) ? LCD_WIDTH : LCD_HEIGHT; }
static int16_t logical_h() { return (current_rotation % 2 == 0) ? LCD_HEIGHT : LCD_WIDTH; }

// ─── LVGL draw buffers (internal SRAM, partial render) ─────────────────────
#define BUF_LINES 10
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// LVGL flush callback — ST7789 does rotation natively via MADCTL
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
    lv_display_flush_ready(disp);
}

// ─── Parse JSON ─────────────────────────────────────────────────────────────
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->epoch = doc["ts"] | 0u;
    out->tz_off = doc["tz"] | 0;
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

// ─── Generic button handler ─────────────────────────────────────────────────
struct btn_config_t {
    uint8_t gpio;
    bool     active;       // false = skip (no button connected to this GPIO)
    void (*on_short)(void);
    void (*on_double)(void);
    void (*on_long_start)(void);
    void (*on_long_end)(void);
};

struct btn_state_t {
    bool     pressed;
    uint32_t press_start;
    uint32_t last_tap;
    uint8_t  tap_count;
    bool     long_sent;
};

static btn_state_t btn_states[3] = {};

static void btn_poll_one(const btn_config_t& cfg, btn_state_t& st) {
    if (!cfg.active) return;
    bool now = (digitalRead(cfg.gpio) == LOW);

    if (now && !st.pressed) {
        st.pressed = true;
        st.press_start = millis();
        st.long_sent = false;
    }

    if (!now && st.pressed) {
        st.pressed = false;
        uint32_t dur = millis() - st.press_start;
        if (dur >= BTN_LONG_MS) {
            if (cfg.on_long_end) cfg.on_long_end();
        } else {
            st.tap_count++;
            st.last_tap = millis();
        }
    }

    if (st.pressed && !st.long_sent && (millis() - st.press_start >= BTN_LONG_MS)) {
        st.long_sent = true;
        if (cfg.on_long_start) cfg.on_long_start();
    }

    if (st.tap_count > 0 && !st.pressed && (millis() - st.last_tap >= BTN_DOUBLE_MS)) {
        if (st.tap_count >= 2 && cfg.on_double) {
            cfg.on_double();
        } else if (cfg.on_short) {
            cfg.on_short();
        }
        st.tap_count = 0;
    }
}

// ─── Button actions ─────────────────────────────────────────────────────────
static void action_trigger_animation(void) {
    ui_trigger_animation();
    // Persist the chosen EMO view mode so it survives a reboot.
    if (ui_get_current_screen() == SCREEN_EMO) {
        g_prefs.putInt("emo_view", (int)emo_get_view());
    }
}

static void action_cycle_screen(void) {
    ui_cycle_screen();
}

static void action_rotate(void) {
    current_rotation = (current_rotation + 1) % 4;
    gfx->setRotation(current_rotation);
    lv_display_t* disp = lv_display_get_default();
    if (disp) lv_display_set_resolution(disp, logical_w(), logical_h());
    lv_obj_invalidate(lv_screen_active());
    ui_relayout();
    splash_relayout();
    Serial.printf("Rotation: %d (%dx%d)\n", current_rotation, logical_w(), logical_h());
}

static void action_space_down(void) {
    ble_keyboard_press(0x2C, 0);
}

static void action_space_up(void) {
    ble_keyboard_release();
}

static void action_noop(void) {}

// Apply a language as an explicit user choice and persist it to NVS
// (device/web choice wins over the daemon's connect-time --lang seed).
static void set_lang_persist(lang_t l) {
    i18n_set(l);
    ui_relang();
    g_lang_user_set = true;
    g_prefs.putInt("lang", (int)l);
    g_prefs.putBool("lang_set", true);
}

// Long-press BOOT: toggle UI language.
static void action_toggle_lang(void) {
    set_lang_persist(g_lang == LANG_EN ? LANG_RU : LANG_EN);
}

// Button configurations — 1 click=animation, 2 clicks=screen cycle.
// BOOT long-press toggles language (Space/PTT lives on BTN_A/B only).
static const btn_config_t btn_configs[] = {
    // BTN_BOOT (GPIO 9): short=animation, double=cycle screen, long=language
    { BTN_BOOT, true,  action_trigger_animation, action_cycle_screen, action_toggle_lang, action_noop },
    // BTN_A (GPIO 7): inactive — shared with LCD_BL until external button is wired
    { BTN_A,    false, action_trigger_animation, action_cycle_screen, action_space_down, action_space_up },
    // BTN_B (GPIO 6): inactive — shared with LCD_RESET until external button is wired
    { BTN_B,    false, action_cycle_screen, action_rotate, action_space_down, action_space_up },
};
#define BTN_COUNT 3

static void btn_poll(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        btn_poll_one(btn_configs[i], btn_states[i]);
    }
}

// ─── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // Init display — Arduino_HWSPI (SPIClass) on ESP32-C6
    vspi = new SPIClass(SPI);
    vspi->begin(LCD_SCLK, -1 /* MISO */, LCD_MOSI, LCD_CS);
    bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, -1, vspi);
    gfx = new Arduino_ST7789(
        bus, LCD_RESET, 0 /* rotation */,
        true /* ips — ST7789V2 1.69" panel needs color inversion */, LCD_WIDTH, LCD_HEIGHT,
        LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);

    if (!gfx->begin()) {
        Serial.println("GFX begin FAILED!");
        while (1) delay(1000);
    }
    gfx->fillScreen(0x0000);

    // Backlight on
    pinMode(LCD_BL, OUTPUT);
    analogWrite(LCD_BL, 255);

    // Buttons
    pinMode(BTN_BOOT, INPUT_PULLUP);
    // BTN_A/B not initialized — GPIO 6/7 used by LCD RST/BL

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    int16_t w = logical_w();
    buf1 = (uint16_t*)malloc(w * BUF_LINES * 2);
    buf2 = (uint16_t*)malloc(w * BUF_LINES * 2);

    lv_display_t* disp = lv_display_create(w, logical_h());
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, w * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ble_init();

    // Restore on-device language choice (NVS). Default EN until set.
    g_prefs.begin("clawd", false);
    if (g_prefs.getBool("lang_set", false)) {
        g_lang_user_set = true;
        i18n_set((lang_t)g_prefs.getInt("lang", (int)LANG_EN));
    }

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
#ifdef EMO_SELFTEST
    ui_show_screen(SCREEN_EMO);
    emo_set_view(9);  // compact + clock
#else
    emo_set_view((uint8_t)g_prefs.getInt("emo_view", 0));  // restore saved mode
    ui_show_screen(SCREEN_EMO);
#endif

    Serial.println("Dashboard ready");
}

// ─── Loop ───────────────────────────────────────────────────────────────────
static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    splash_tick();

    btn_poll();

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
        emo_set_connected(bs == BLE_STATE_CONNECTED);
    }

#ifdef EMO_SELFTEST
    // Synthetic 0→100% sweep to eyeball the smooth color + ticking clock.
    static uint32_t st_last = 0;
    static float st_pct = 0.0f;
    if (millis() - st_last >= 40) {
        st_last = millis();
        st_pct += 0.5f;
        if (st_pct > 100.0f) st_pct = 0.0f;
        usage.session_pct = st_pct;
        usage.weekly_pct = 100.0f - st_pct;
        usage.session_reset_mins = 187;
        usage.weekly_reset_mins = 4320;
        usage.epoch = 1700000000u + (millis() / 1000u);
        usage.tz_off = 0;
        usage.ok = true;
        usage.valid = true;
        ui_update(&usage);
    }
    // Also exercise the emo emotion/animation change.
    static uint32_t st_emo = 0;
    if (millis() - st_emo >= 2500) {
        st_emo = millis();
        emo_next_emotion();
    }
#else
    // Process incoming BLE data
    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before && splash_is_active()) {
                splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }
#endif

    // Process BLE control commands
    if (ble_has_ctrl_cmd()) {
        uint8_t cmd = ble_get_ctrl_cmd();
        switch (cmd) {
        case 0x01: ui_show_screen(SCREEN_USAGE);      break;
        case 0x02: ui_show_screen(SCREEN_BLUETOOTH);  break;
        case 0x03: ui_show_screen(SCREEN_SPLASH);     break;
        case 0x04: ui_cycle_screen();                 break;
        case 0x06: ui_show_screen(SCREEN_EMO);        break;
        case 0x05: esp_restart();                     break;  // device reboot
        case 0x07: action_trigger_animation();        break;  // BOOT-click analog (cycle view)
        case 0x08: emo_next_emotion();                break;  // next emotion/animation now
        case 0x10: ble_request_refresh();             break;
        // 0x40/0x41: connect-time seed — ignored once the user has chosen.
        case 0x40: if (!g_lang_user_set) { i18n_set(LANG_EN); ui_relang(); } break;
        case 0x41: if (!g_lang_user_set) { i18n_set(LANG_RU); ui_relang(); } break;
        // 0x42/0x43: explicit choice from the web UI — apply + persist.
        case 0x42: set_lang_persist(LANG_EN); break;
        case 0x43: set_lang_persist(LANG_RU); break;
        default: break;
        }
    }

    delay(5);
}
