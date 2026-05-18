#include "ui.h"
#include "splash.h"
#include "emo.h"
#include "i18n.h"
#include <cstring>
#include <lvgl.h>
#include "display_cfg.h"
#include "theme.h"

// ─── Cyrillic-enabled custom fonts (generated with lv_font_conv) ──────────
// Font files in fonts_cyr/ define the standard symbol names directly
// (lv_font_montserrat_12, etc.) so no aliases or shim are needed.
// Built-in Montserrat fonts are disabled via platformio.ini build flags.

// ─── Fonts ─────────────────────────────────────────────────────────────────
#define FONT_TITLE      lv_font_montserrat_20
#define FONT_PCT        lv_font_montserrat_28      // built-in, numbers only
#define FONT_PCT_SMALL  lv_font_montserrat_14
#define FONT_INFO       lv_font_montserrat_12
#define FONT_ANIM       lv_font_montserrat_16
#define FONT_BT_TITLE   lv_font_montserrat_28      // built-in, "Bluetooth" Latin only
#define FONT_BT_STATUS  lv_font_montserrat_20
#define FONT_BT_BODY    lv_font_montserrat_14
#define FONT_BT_SMALL   lv_font_montserrat_12

// ─── Color tokens ─────────────────────────────────────────────────────────
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ─── Layout: 240×280 portrait, concentric arc gauges ──────────────────────
#define SCR_W         240
#define SCR_H         280
#define MARGIN        8
#define TITLE_Y       8
#define ARC_Y         32
#define ARC_S_SIZE    170
#define ARC_W_SIZE    130
#define ARC_S_WIDTH   12
#define ARC_W_WIDTH   8

// ─── Usage screen widgets ─────────────────────────────────────────────────
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* arc_session;
static lv_obj_t* arc_weekly;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;

// ─── Bluetooth screen widgets ─────────────────────────────────────────────
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ─── Shared state ───────────────────────────────────────────────────────────
static screen_t current_screen = SCREEN_USAGE;
static screen_t prev_non_splash_screen = SCREEN_USAGE;

// Animation: previous percentage values for smooth counting
static int prev_s_pct = -1;
static int prev_w_pct = -1;

// Last pushed data / BLE status — kept so ui_relang() can re-render
// localized labels immediately on a language switch.
static UsageData last_usage = {};
static bool have_usage = false;
static ble_state_t last_ble_state = BLE_STATE_DISCONNECTED;
static char last_ble_name[48] = "";
static char last_ble_mac[48] = "";

// ─── Animation state (spinner + messages) ──────────────────────────────────
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

// Font-safe pulsing spinner (· is in fonts_cyr after the U+00B7 regen; the
// rest are ASCII). Avoids dingbat glyphs Montserrat doesn't ship.
static const char* const spinner_frames[] = {
    "\xC2\xB7", "*", "o", "O", "o", "*",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

// Spinner messages now come from i18n (i18n_spinner); count is per-language.
static int anim_msg_count = 0;

// ─── Helpers ────────────────────────────────────────────────────────────────

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "%s %d%s", TR(STR_RESET_IN), mins, TR(STR_U_MIN));
    } else if (mins < 1440) {
        snprintf(buf, len, "%s %d%s %d%s", TR(STR_RESET_IN),
                 mins / 60, TR(STR_U_HOUR), mins % 60, TR(STR_U_MIN));
    } else {
        snprintf(buf, len, "%s %d%s %d%s", TR(STR_RESET_IN),
                 mins / 1440, TR(STR_U_DAY), (mins % 1440) / 60, TR(STR_U_HOUR));
    }
}

// ─── Arc gauge animation ───────────────────────────────────────────────────

typedef void (*pct_anim_cb_t)(void*, int32_t);

static void arc_anim_cb(void* obj, int32_t v) {
    lv_arc_set_value((lv_obj_t*)obj, (int)v);
}

static void session_pct_cb(void* obj, int32_t v) {
    lv_label_set_text_fmt((lv_obj_t*)obj, "%d%%", (int)v);
}

static void weekly_pct_cb(void* obj, int32_t v) {
    lv_label_set_text_fmt((lv_obj_t*)obj, "%s%d%%", TR(STR_WEEK_PCT_PREFIX), (int)v);
}

static void animate_gauge(lv_obj_t* arc, lv_obj_t* label,
                          int from, int to,
                          pct_anim_cb_t label_cb) {
    if (from < 0) from = 0;
    if (from == to) {
        lv_arc_set_value(arc, to);
        label_cb(label, to);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)arc_anim_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, 800);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, label);
    lv_anim_set_exec_cb(&b, (lv_anim_exec_xcb_t)label_cb);
    lv_anim_set_values(&b, from, to);
    lv_anim_set_time(&b, 800);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_start(&b);
}

// ─── Arc creation helper ────────────────────────────────────────────────────

static lv_obj_t* make_arc(lv_obj_t* parent, int size, int width,
                           lv_color_t indicator_color) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(arc, 135, 45);

    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, indicator_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_set_style_size(arc, 0, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);

    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    return arc;
}

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 8, 0);
    lv_obj_set_style_pad_right(panel, 8, 0);
    lv_obj_set_style_pad_top(panel, 6, 0);
    lv_obj_set_style_pad_bottom(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

// ─── Usage Screen — concentric arc gauges (RUSSIAN) ─────────────────────────

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, SCR_W, SCR_H);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_CLICKABLE);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, TR(STR_USAGE_TITLE));
    lv_obj_set_style_text_font(lbl_title, &FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    arc_session = make_arc(usage_container, ARC_S_SIZE, ARC_S_WIDTH, COL_GREEN);
    lv_obj_align(arc_session, LV_ALIGN_TOP_MID, 0, ARC_Y);

    int offset = (ARC_S_SIZE - ARC_W_SIZE) / 2;
    arc_weekly = make_arc(usage_container, ARC_W_SIZE, ARC_W_WIDTH, COL_GREEN);
    lv_obj_align(arc_weekly, LV_ALIGN_TOP_MID, 0, ARC_Y + offset);

    int center_y = ARC_Y + ARC_S_SIZE / 2;

    lbl_session_pct = lv_label_create(usage_container);
    lv_label_set_text(lbl_session_pct, "--%");
    lv_obj_set_style_text_font(lbl_session_pct, &FONT_PCT, 0);
    lv_obj_set_style_text_color(lbl_session_pct, COL_TEXT, 0);
    lv_obj_align(lbl_session_pct, LV_ALIGN_TOP_MID, 0, center_y - 30);

    lbl_weekly_pct = lv_label_create(usage_container);
    lv_label_set_text_fmt(lbl_weekly_pct, "%s--%%", TR(STR_WEEK_PCT_PREFIX));
    lv_obj_set_style_text_font(lbl_weekly_pct, &FONT_ANIM, 0);
    lv_obj_set_style_text_color(lbl_weekly_pct, COL_DIM, 0);
    lv_obj_align(lbl_weekly_pct, LV_ALIGN_TOP_MID, 0, center_y + 14);

    lbl_session_reset = lv_label_create(usage_container);
    lv_label_set_text_fmt(lbl_session_reset, "%s \xC2\xB7 ---", TR(STR_SESSION));
    lv_obj_set_style_text_font(lbl_session_reset, &FONT_INFO, 0);
    lv_obj_set_style_text_color(lbl_session_reset, COL_DIM, 0);
    lv_obj_align(lbl_session_reset, LV_ALIGN_TOP_MID, 0, ARC_Y + ARC_S_SIZE + 4);

    lbl_weekly_reset = lv_label_create(usage_container);
    lv_label_set_text_fmt(lbl_weekly_reset, "%s \xC2\xB7 ---", TR(STR_WEEK));
    lv_obj_set_style_text_font(lbl_weekly_reset, &FONT_INFO, 0);
    lv_obj_set_style_text_color(lbl_weekly_reset, COL_DIM, 0);
    lv_obj_align(lbl_weekly_reset, LV_ALIGN_TOP_MID, 0, ARC_Y + ARC_S_SIZE + 20);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &FONT_ANIM, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -6);
}

// ─── Bluetooth Screen (RUSSIAN) ─────────────────────────────────────────────

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, SCR_W, SCR_H);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, &FONT_BT_TITLE, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    int content_y = 36;
    int content_w = SCR_W - 2 * MARGIN;
    lv_obj_t* p_info = make_panel(ble_container, MARGIN, content_y, content_w, 100);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, TR(STR_BT_LOADING));
    lv_obj_set_style_text_font(lbl_ble_status, &FONT_BT_STATUS, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 0, 0);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text_fmt(lbl_ble_device, "%s ---", TR(STR_BT_DEVICE));
    lv_obj_set_style_text_font(lbl_ble_device, &FONT_BT_BODY, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 28);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text_fmt(lbl_ble_mac, "%s ---", TR(STR_BT_ADDR));
    lv_obj_set_style_text_font(lbl_ble_mac, &FONT_BT_BODY, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 48);

    int reset_y = content_y + 100 + 6;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, MARGIN, reset_y);
    lv_obj_set_size(reset_zone, content_w, 40);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 6, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 8, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, [](lv_event_t* e) { ble_clear_bonds(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, TR(STR_BT_RESET));
    lv_obj_set_style_text_font(reset_lbl, &FONT_BT_BODY, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    lv_obj_t* lbl_credit = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit, "Build by Sedek");
    lv_obj_set_style_text_font(lbl_credit, &FONT_BT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -22);

    lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit2, "github.com/MrSedek \xC2\xB7 t.me/sedek");
    lv_obj_set_style_text_font(lbl_credit2, &FONT_BT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ─── Public API ─────────────────────────────────────────────────────────────

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_usage_screen(scr);
    init_bluetooth_screen(scr);
    emo_init();
    splash_init();
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    int s_pct = (int)(data->session_pct + 0.5f);
    int w_pct = (int)(data->weekly_pct + 0.5f);

    animate_gauge(arc_session, lbl_session_pct,
                  prev_s_pct, s_pct, session_pct_cb);
    animate_gauge(arc_weekly, lbl_weekly_pct,
                  prev_w_pct, w_pct, weekly_pct_cb);

    lv_obj_set_style_arc_color(arc_session, pct_color(data->session_pct), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text_fmt(lbl_session_reset, "%s \xC2\xB7 %s", TR(STR_SESSION), buf);
    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text_fmt(lbl_weekly_reset, "%s \xC2\xB7 %s", TR(STR_WEEK), buf);

    emo_set_usage(data);

    last_usage = *data;
    have_usage = true;
    prev_s_pct = s_pct;
    prev_w_pct = w_pct;
}

void ui_tick_anim(void) {
    emo_tick();

    // Usage screen spinner (only when usage is visible)
    if (current_screen != SCREEN_USAGE) return;

    const char* const* msgs = i18n_spinner(&anim_msg_count);
    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % anim_msg_count;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                         : (SPINNER_PHASES - anim_phase);

        if (anim_msg_idx >= anim_msg_count) anim_msg_idx = 0;
        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s...",
                 spinner_frames[anim_spinner_idx],
                 msgs[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    emo_hide();
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_EMO:        emo_show(); break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
}

void ui_cycle_screen(void) {
    // Cycle: Usage → Emo → Splash → Usage (Bluetooth NOT in cycle)
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:      next = SCREEN_EMO; break;
    case SCREEN_EMO:        next = SCREEN_SPLASH; break;
    case SCREEN_SPLASH:     next = SCREEN_USAGE; break;
    case SCREEN_BLUETOOTH:  next = SCREEN_USAGE; break;
    default:                next = SCREEN_USAGE; break;
    }
    ui_show_screen(next);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_trigger_animation(void) {
    switch (current_screen) {
    case SCREEN_SPLASH:
        splash_next();
        break;
    case SCREEN_EMO:
        emo_next_mood();
        break;
    case SCREEN_USAGE:
        // Cycle the spinner message immediately
        i18n_spinner(&anim_msg_count);
        if (anim_msg_count > 0)
            anim_msg_idx = (anim_msg_idx + 1) % anim_msg_count;
        anim_msg_start = lv_tick_get();
        break;
    default:
        break;
    }
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    last_ble_state = state;
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, TR(STR_BT_CONNECTED));
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, TR(STR_BT_WAITING));
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, TR(STR_BT_DISCONNECTED));
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, TR(STR_BT_LOADING));
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        strncpy(last_ble_name, name, sizeof(last_ble_name) - 1);
        last_ble_name[sizeof(last_ble_name) - 1] = '\0';
        lv_label_set_text_fmt(lbl_ble_device, "%s %s", TR(STR_BT_DEVICE), name);
    }
    if (mac) {
        strncpy(last_ble_mac, mac, sizeof(last_ble_mac) - 1);
        last_ble_mac[sizeof(last_ble_mac) - 1] = '\0';
        lv_label_set_text_fmt(lbl_ble_mac, "%s %s", TR(STR_BT_ADDR), mac);
    }
}

// Re-apply every localized label for the current language (called after a
// BLE language-switch CTRL byte). Data-driven labels are re-rendered from the
// last cached values; the rest fall back to placeholders.
void ui_relang(void) {
    lv_label_set_text(lbl_title, TR(STR_USAGE_TITLE));

    if (have_usage) {
        char buf[48];
        format_reset_time(last_usage.session_reset_mins, buf, sizeof(buf));
        lv_label_set_text_fmt(lbl_session_reset, "%s \xC2\xB7 %s", TR(STR_SESSION), buf);
        format_reset_time(last_usage.weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text_fmt(lbl_weekly_reset, "%s \xC2\xB7 %s", TR(STR_WEEK), buf);
        weekly_pct_cb(lbl_weekly_pct, (int)(last_usage.weekly_pct + 0.5f));
    } else {
        lv_label_set_text_fmt(lbl_session_reset, "%s \xC2\xB7 ---", TR(STR_SESSION));
        lv_label_set_text_fmt(lbl_weekly_reset, "%s \xC2\xB7 ---", TR(STR_WEEK));
        lv_label_set_text_fmt(lbl_weekly_pct, "%s--%%", TR(STR_WEEK_PCT_PREFIX));
    }

    ui_update_ble_status(last_ble_state,
                         last_ble_name[0] ? last_ble_name : nullptr,
                         last_ble_mac[0] ? last_ble_mac : nullptr);

    emo_relang();
}

void ui_relayout(void) {
    lv_obj_invalidate(lv_screen_active());
}
