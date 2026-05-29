#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "ota.h"
#include "splash.h"
#include "usage_rate.h"
#include "i18n.h"
#include "emo.h"
#include "emo2.h"
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

// QA screenshot: when set, my_flush_cb streams each rendered strip over Serial
// (rgb565le). A full-screen invalidate flushes strips top-to-bottom, so the
// concatenated byte stream equals the framebuffer in row-major order.
static volatile bool g_screenshot = false;

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
    if (g_screenshot) Serial.write((const uint8_t*)src, (size_t)w * h * 2);
    lv_display_flush_ready(disp);
}

// Dump the active screen over Serial in the protocol screenshot.sh expects.
static void do_screenshot() {
    int32_t w = logical_w(), h = logical_h();
    Serial.printf("SCREENSHOT_START %d %d %d\n", (int)w, (int)h, (int)(w * h * 2));
    Serial.flush();
    g_screenshot = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(lv_display_get_default());
    g_screenshot = false;
    Serial.println("SCREENSHOT_END");
}

// ─── Parse JSON ─────────────────────────────────────────────────────────────
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    // `s` is mandatory for a usage frame — if missing, this payload is
    // something else (e.g. {cfg:...} config blob) and we leave `valid=false`
    // so the caller doesn't update the UI with zeros.
    if (!doc["s"].is<float>() && !doc["s"].is<int>()) {
        out->valid = false;
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
    // Persist the chosen view mode so it survives a reboot.
    switch (ui_get_current_screen()) {
    case SCREEN_EMO:  g_prefs.putInt("emo_view",  (int)emo_get_view());  break;
    case SCREEN_EMO2: g_prefs.putInt("emo2_view", (int)emo2_get_view()); break;
    default: break;
    }
}

static void action_cycle_screen(void) {
    ui_cycle_screen();
}

// Apply a specific rotation + persist to NVS. Both the BTN_B-driven cycle
// and the web CTRL bytes (0x30/0x31) call this so the value survives reboot.
static void action_set_rotation_persisted(uint8_t rot) {
    rot &= 0x03;
    if (rot == current_rotation) return;
    current_rotation = rot;
    gfx->setRotation(current_rotation);
    lv_display_t* disp = lv_display_get_default();
    if (disp) lv_display_set_resolution(disp, logical_w(), logical_h());
    lv_obj_invalidate(lv_screen_active());
    ui_relayout();
    splash_relayout();
    emo2_relayout();   // walks every absolute-positioned emo2 layout object
    g_prefs.putUInt("rotation", current_rotation);
    Serial.printf("Rotation: %d (%dx%d) [persisted]\n",
                  current_rotation, logical_w(), logical_h());
}
static void action_rotate(void) {
    action_set_rotation_persisted((current_rotation + 1) % 4);
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
    // Boot reason + heap snapshot — first line on every restart so user can
    // grep the serial log when investigating "device keeps reconnecting" bug
    // reports without needing to attach an extra debug tool.
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* rname = "UNKNOWN";
        switch (rr) {
        case ESP_RST_POWERON:   rname = "POWERON";  break;
        case ESP_RST_EXT:       rname = "EXT_PIN";  break;
        case ESP_RST_SW:        rname = "SOFTWARE"; break;
        case ESP_RST_PANIC:     rname = "PANIC";    break;
        case ESP_RST_INT_WDT:   rname = "INT_WDT";  break;
        case ESP_RST_TASK_WDT:  rname = "TASK_WDT"; break;
        case ESP_RST_WDT:       rname = "OTHER_WDT";break;
        case ESP_RST_DEEPSLEEP: rname = "DEEPSLEEP";break;
        case ESP_RST_BROWNOUT:  rname = "BROWNOUT"; break;
        case ESP_RST_SDIO:      rname = "SDIO";     break;
        default:                rname = "UNKNOWN";  break;
        }
        uint32_t free_heap  = ESP.getFreeHeap();
        uint32_t min_heap   = ESP.getMinFreeHeap();
        Serial.printf("[BOOT] reason=%s (%d) free_heap=%u min_heap=%u\n",
                      rname, (int)rr, (unsigned)free_heap, (unsigned)min_heap);
    }
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
    // Restore display orientation persisted across reboots (BTN_B or web).
    uint8_t saved_rot = (uint8_t)(g_prefs.getUInt("rotation", 0) & 0x03);
    if (saved_rot != current_rotation) {
        current_rotation = saved_rot;
        gfx->setRotation(current_rotation);
        lv_display_t* d = lv_display_get_default();
        if (d) lv_display_set_resolution(d, logical_w(), logical_h());
    }

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
#ifdef EMO_SELFTEST
    ui_show_screen(SCREEN_EMO2);
    emo_set_view(9);  // compact + clock (only affects legacy emo)
#else
    emo_set_view((uint8_t)g_prefs.getInt("emo_view", 0));   // legacy emo view
    emo2_set_view((uint8_t)g_prefs.getInt("emo2_view", 0)); // emo 2.0 view
    ui_show_screen(SCREEN_EMO2);   // emo 2.0 is the new default screen
#endif

    Serial.println("Dashboard ready");
}

// ─── Loop ───────────────────────────────────────────────────────────────────
static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    ota_tick();
    splash_tick();

    btn_poll();

    // Serial command handler (QA screenshot). Line-buffered; "screenshot\n"
    // dumps the active LVGL screen over Serial via do_screenshot().
    {
        static char scmd[16];
        static uint8_t slen = 0;
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                scmd[slen] = 0;
                if (slen && strcmp(scmd, "screenshot") == 0) do_screenshot();
                slen = 0;
            } else if (slen < sizeof(scmd) - 1) {
                scmd[slen++] = c;
            }
        }
    }

    // Heap watch — log free + min-since-boot every 60 s. A monotonic decline
    // of `min_heap` signals a memory leak; a sudden drop predicts a future
    // OOM-watchdog reset. Cheap (one printf/min) and grep-able from serial.
    static uint32_t heap_log_last_ms = 0;
    uint32_t _now_ms = millis();
    if (_now_ms - heap_log_last_ms >= 60000) {
        heap_log_last_ms = _now_ms;
        Serial.printf("[HEAP] free=%u min=%u largest=%u uptime=%us\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)ESP.getMaxAllocHeap(),
                      (unsigned)(_now_ms / 1000));
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        // BLE state transition trace — pairs with daemon-side BLE: CONNECT /
        // DISCONNECT log lines for round-trip diagnostics.
        const char* sname = (bs == BLE_STATE_CONNECTED)    ? "CONNECTED"
                          : (bs == BLE_STATE_ADVERTISING)  ? "ADVERTISING"
                          : (bs == BLE_STATE_DISCONNECTED) ? "DISCONNECTED"
                                                            : "UNKNOWN";
        Serial.printf("[BLE] state -> %s (uptime=%us free_heap=%u)\n",
                      sname, (unsigned)(_now_ms / 1000),
                      (unsigned)ESP.getFreeHeap());
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
        emo_set_connected(bs == BLE_STATE_CONNECTED);
        emo2_set_connected(bs == BLE_STATE_CONNECTED);
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
    // Process incoming BLE data — payload may be a usage frame ({s:...}),
    // a config blob ({cfg:...}) or both. Route by JSON keys.
    if (ble_has_data()) {
        const char* json = ble_get_data();
        bool ack = false;
        // Try cfg first — won't touch usage if it's a pure config payload.
        if (emo2_apply_cfg_json(json)) ack = true;
        // Then try usage. parse_json sets `valid=true` only when `s` exists.
        if (parse_json(json, &usage) && usage.valid) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before && splash_is_active()) {
                splash_pick_for_current_rate();
            }
            ui_update(&usage);
            emo2_set_data_received();   // anchor for state-machine staleness
            ack = true;
        }
        if (ack) ble_send_ack();
        else     ble_send_nack();
    }
#endif

    // Process BLE control commands — drain the queue per tick so a rapid
    // host-side burst (e.g. form+op+color from a state-config edit) isn't
    // throttled to one byte per 16ms loop.
    while (ble_has_ctrl_cmd()) {
        uint8_t cmd = ble_get_ctrl_cmd();
        switch (cmd) {
        case 0x01: ui_show_screen(SCREEN_USAGE);      break;
        case 0x02: ui_show_screen(SCREEN_BLUETOOTH);  break;
        case 0x03: ui_show_screen(SCREEN_SPLASH);     break;
        case 0x04: ui_cycle_screen();                 break;
        case 0x06: ui_show_screen(SCREEN_EMO);        break;
        case 0x09: ui_show_screen(SCREEN_EMO2);       break;  // ClauLi (HD)
        case 0x0A: ui_show_screen(SCREEN_EMO2);
                   emo2_run_diagnostics();            break;  // 8s diag sequence
        case 0x05: esp_restart();                     break;  // device reboot
        case 0x07: action_trigger_animation();        break;  // BOOT-click analog (cycle view)
        case 0x08:                                            // next animation now (screen-aware)
            switch (ui_get_current_screen()) {
            case SCREEN_SPLASH:   splash_next();           break;
            case SCREEN_EMO2:     emo2_next_emotion();     break;
            default:              emo_next_emotion();      break;
            }
            break;
        case 0x10: ble_request_refresh();             break;
        // 0x18–0x1B: daemon-driven state signals for emo2.
        case 0x18: emo2_set_token_expired(true);      break;
        case 0x19: emo2_set_token_expired(false);     break;
        case 0x1A: emo2_set_manual_mode(true);        break;
        case 0x1B: emo2_set_manual_mode(false);       break;
        // 0x1C–0x1F: halo colour override (cyan/amber/red/auto).
        case 0x1C: emo2_set_color_override(0);        break;
        case 0x1D: emo2_set_color_override(1);        break;
        case 0x1E: emo2_set_color_override(2);        break;
        case 0x1F: emo2_set_color_override(0xFF);     break;
        // % stats layout pick (matches daemon CTRL_STATS_*)
        case 0x20: emo2_set_stats_layout(0); break;   // off
        case 0x21: emo2_set_stats_layout(1); break;   // bezel_orbit
        case 0x22: emo2_set_stats_layout(2); break;   // twin_columns
        case 0x23: emo2_set_stats_layout(3); break;   // hud_ribbon
        case 0x24: emo2_set_stats_layout(4); break;   // brows (deprecated)
        case 0x25: emo2_set_stats_layout(5); break;   // tear_pearls
        case 0x26: emo2_set_stats_layout(6); break;   // corner_chip
        case 0x27: emo2_set_stats_layout(7); break;   // ecg_monitor
        case 0x28: emo2_set_stats_layout(8); break;   // classic (text + bars)
        // Orientation (web toggle persists to NVS — same as BTN_B but explicit)
        case 0x30: action_set_rotation_persisted(0); break;   // portrait
        case 0x31: action_set_rotation_persisted(1); break;   // landscape (90° CW)
        // Usage text mode (matches daemon CTRL_TEXT_*)
        case 0x44: emo2_set_text_mode(0); break;      // none
        case 0x45: emo2_set_text_mode(1); break;      // pct only
        case 0x46: emo2_set_text_mode(2); break;      // reset only
        case 0x47: emo2_set_text_mode(3); break;      // both
        // 0x51-0x54: text SOURCE (off/session/weekly/both)
        case 0x51: emo2_set_text_source(0); break;
        case 0x52: emo2_set_text_source(1); break;
        case 0x53: emo2_set_text_source(2); break;
        case 0x54: emo2_set_text_source(3); break;
        // 0x55-0x57: text FORMAT (pct / pct+reset / reset)
        case 0x55: emo2_set_text_format(0); break;
        case 0x56: emo2_set_text_format(1); break;
        case 0x57: emo2_set_text_format(2); break;
        // 0x5C-0x5E: text PLACEMENT (top / middle / bottom — item 2/3).
        case 0x5C: emo2_set_text_placement(0); break;
        case 0x5D: emo2_set_text_placement(1); break;
        case 0x5E: emo2_set_text_placement(2); break;
        // Clock style — 7 picked candidates + off. 0x48-0x4C use the
        // legacy byte range; 0x58 + 0x5A-0x5B continue past the
        // text_format CTRLs. 0x59 reserved (was badge — dropped).
        case 0x48: emo2_set_clock_style(0); break;   // off
        case 0x49: emo2_set_clock_style(1); break;   // mono (c01)
        case 0x4A: emo2_set_clock_style(2); break;   // major_mono (c05)
        case 0x4B: emo2_set_clock_style(3); break;   // orbitron (c07)
        case 0x4C: emo2_set_clock_style(4); break;   // outline (c08)
        case 0x58: emo2_set_clock_style(5); break;   // neon (c10)
        // case 0x59: badge — DROPPED.
        case 0x5A: emo2_set_clock_style(6); break;   // seconds (c13)
        case 0x5B: emo2_set_clock_style(7); break;   // bracket (c15)
        // 0x4D-0x50: LAYOUT-fill colour override (separate from halo).
        case 0x4D: emo2_set_layout_color_override(0);    break;  // cyan
        case 0x4E: emo2_set_layout_color_override(1);    break;  // amber
        case 0x4F: emo2_set_layout_color_override(2);    break;  // red
        case 0x50: emo2_set_layout_color_override(0xFF); break;  // auto
        // 0x40/0x41: connect-time seed — ignored once the user has chosen.
        case 0x40: if (!g_lang_user_set) { i18n_set(LANG_EN); ui_relang(); } break;
        case 0x41: if (!g_lang_user_set) { i18n_set(LANG_RU); ui_relang(); } break;
        // 0x42/0x43: explicit choice from the web UI — apply + persist.
        case 0x42: set_lang_persist(LANG_EN); break;
        case 0x43: set_lang_persist(LANG_RU); break;
        // 0x80–0x9F: explicit form select on emo2 (up to 32 moods).
        // 0xA0–0xBF: trigger one of up to 32 motion ops on emo2.
        // Moved here from 0x50/0x70 which collided with the layout-colour
        // (0x4D-0x50), text-source (0x51-0x54), text-format (0x55-0x57),
        // and clock-style (0x58-0x5B) CTRLs added in later iterations.
        default:
            if (cmd >= 0x80 && cmd <= 0x9F) {
                // Form picker — switch to ClauLi if not already showing.
                if (ui_get_current_screen() != SCREEN_EMO2) ui_show_screen(SCREEN_EMO2);
                emo2_set_mood_idx(cmd - 0x80);
            } else if (cmd >= 0xA0 && cmd <= 0xBF) {
                // Motion op — same routing as forms.
                if (ui_get_current_screen() != SCREEN_EMO2) ui_show_screen(SCREEN_EMO2);
                emo2_trigger_op(cmd - 0xA0);
            }
            break;
        }
    }

    delay(5);
}
