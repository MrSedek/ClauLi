#include "emo.h"
#include "emo_eyes.h"
#include "theme.h"
#include "i18n.h"
#include "display_cfg.h"
#include <Arduino.h>

// ─── Layout (240×280 portrait) ─────────────────────────────────────────────
#define SCR_W        240
#define SCR_H        280
#define EYE_SRC      32                       // source bitmap is 32×32
#define EYE_PX       86                       // rendered eye size (px)
#define EYE_SCALE    (256 * EYE_PX / EYE_SRC) // LVGL scale factor (256 = 1.0×)
#define EYE_GAP      14
#define EYE_CY       96                       // eye center Y
#define EYE_L_CX     ((SCR_W - (2 * EYE_PX + EYE_GAP)) / 2 + EYE_PX / 2)
#define EYE_R_CX     (EYE_L_CX + EYE_PX + EYE_GAP)
#define INFO_S_Y     162   // text-mode session line Y
#define INFO_W_Y     186   // text-mode weekly line Y
#define BAR_W        184
#define BAR_H        12
#define BAR_X        ((SCR_W - BAR_W) / 2)
#define BAR_S_Y      216   // text+grad session bar Y
#define BAR_W_Y      240   // text+grad weekly bar Y
// Compact mode: bars + short caption directly above each bar.
#define CMP_CAP_S_Y  186
#define CMP_BAR_S_Y  204
#define CMP_CAP_W_Y  226
#define CMP_BAR_W_Y  244

// ─── Bright reactive palette (vivid, not the muted theme tones) ────────────
#define EMO_GREEN  lv_color_hex(0x3DE07A)
#define EMO_AMBER  lv_color_hex(0xFFB02E)
#define EMO_RED    lv_color_hex(0xFF4D4D)
#define EMO_TRACK  lv_color_hex(0x2A2A28)

// ─── Moods (order matches emo_eye_frames in emo_eyes.h) ────────────────────
enum emo_mood_t : uint8_t {
    EMO_HAPPY,
    EMO_NEUTRAL,
    EMO_SLEEP,
    EMO_ANGRY,
    EMO_UPSET,
    EMO_SAD,
    EMO_LOVE,
    EMO_MOOD_COUNT
};

// View modes cycled by button-1:
//   1 eyes  2 eyes+text  3 eyes+grad  4 eyes+text+grad  5 eyes+compact
enum emo_view_t : uint8_t {
    VIEW_EYES,
    VIEW_TEXT,
    VIEW_GRAD,
    VIEW_TEXT_GRAD,
    VIEW_COMPACT,
    EMO_CONTENT_COUNT
};
// 10 cycled modes: content 0..4 without clock, then the same 5 with a
// top clock. view_idx 0..9 → content = idx % 5, show_clock = idx >= 5.
#define EMO_VIEW_COUNT 10

// ─── State ─────────────────────────────────────────────────────────────────
static lv_obj_t* emo_container = nullptr;
static lv_obj_t* obj_eye_l     = nullptr;
static lv_obj_t* obj_eye_r     = nullptr;
static lv_obj_t* obj_info_s    = nullptr;  // session line
static lv_obj_t* obj_info_w    = nullptr;  // weekly line
static lv_obj_t* obj_bar_s     = nullptr;  // session gradient bar
static lv_obj_t* obj_bar_w     = nullptr;  // weekly gradient bar
static lv_obj_t* obj_clock     = nullptr;  // top clock (HH:MM)
static lv_obj_t* obj_zzz       = nullptr;  // "z z z" sleep overlay

static bool      active        = false;
static bool      sleeping      = false;    // limit exhausted → sleep mode
static bool      connected     = false;    // BLE link up (false → "Reconnecting…")
static uint8_t   recon_dots    = 0;        // animated trailing-dot phase
static uint32_t  recon_last_ms = 0;
static uint8_t   cur_mood      = EMO_NEUTRAL;
static uint8_t   view_idx      = 0;        // 0..9 (see EMO_VIEW_COUNT)
static UsageData last_data     = {};

// view_idx → content layer + clock flag
#define VIEW_CONTENT  (view_idx % EMO_CONTENT_COUNT)
#define VIEW_CLOCK    (view_idx >= EMO_CONTENT_COUNT)

// Live clock: base epoch (local) captured at last sync + millis snapshot.
static uint32_t clock_base    = 0;   // local epoch secs at last sync (0=none)
static uint32_t clock_base_ms = 0;   // millis() at last sync
static int      clock_last_min = -1; // last rendered minute-of-day

// Blink (natural random)
static uint32_t blink_next_ms  = 0;
static bool     blink_closed   = false;
static uint32_t blink_close_ms = 0;

// Random auto mood change (20 s … 10 min)
static uint32_t mood_next_ms   = 0;
#define MOOD_MIN_MS  20000UL
#define MOOD_MAX_MS  600000UL

// ─── Color (usage-reactive, smooth 0→100% gradient) ────────────────────────
static lv_color_t lerp_color(lv_color_t a, lv_color_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint32_t ca = lv_color_to_u32(a), cb = lv_color_to_u32(b);
    uint8_t ar = (ca >> 16) & 0xFF, ag = (ca >> 8) & 0xFF, ab = ca & 0xFF;
    uint8_t br = (cb >> 16) & 0xFF, bg = (cb >> 8) & 0xFF, bb = cb & 0xFF;
    return lv_color_make((uint8_t)(ar + (br - ar) * t),
                         (uint8_t)(ag + (bg - ag) * t),
                         (uint8_t)(ab + (bb - ab) * t));
}

// Continuous green → amber → red across 0..100%.
static lv_color_t pct_color(float pct) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    if (pct < 50.0f) return lerp_color(EMO_GREEN, EMO_AMBER, pct / 50.0f);
    return lerp_color(EMO_AMBER, EMO_RED, (pct - 50.0f) / 50.0f);
}

static void apply_tint(void) {
    lv_color_t c = pct_color(last_data.valid ? last_data.session_pct : 0.0f);
    lv_obj_set_style_image_recolor(obj_eye_l, c, 0);
    lv_obj_set_style_image_recolor(obj_eye_r, c, 0);
    lv_obj_set_style_image_recolor_opa(obj_eye_l, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor_opa(obj_eye_r, LV_OPA_COVER, 0);
}

// ─── Reset-time text helpers ───────────────────────────────────────────────
// Session: hours + minutes.  Weekly: days or hours.
static void fmt_session_left(int mins, char* buf, size_t len) {
    if (mins < 0)        snprintf(buf, len, "--");
    else if (mins < 60)  snprintf(buf, len, "%d%s", mins, TR(STR_U_MIN));
    else                 snprintf(buf, len, "%d%s %02d%s",
                                  mins / 60, TR(STR_U_HOUR), mins % 60, TR(STR_U_MIN));
}

static void fmt_weekly_left(int mins, char* buf, size_t len) {
    if (mins < 0)         snprintf(buf, len, "--");
    else if (mins < 60)   snprintf(buf, len, "%d%s", mins, TR(STR_U_MIN));
    else if (mins < 1440) snprintf(buf, len, "%d%s", mins / 60, TR(STR_U_HOUR));
    else                  snprintf(buf, len, "%d%s %d%s",
                                  mins / 1440, TR(STR_U_DAY), (mins % 1440) / 60, TR(STR_U_HOUR));
}

// ─── Timer helpers ─────────────────────────────────────────────────────────
static void schedule_next_mood(void) {
    mood_next_ms = millis() + MOOD_MIN_MS +
                   (esp_random() % (MOOD_MAX_MS - MOOD_MIN_MS + 1));
}

static uint8_t pick_random_mood(void) {
    return (cur_mood + 1 + (esp_random() % (EMO_MOOD_COUNT - 1))) % EMO_MOOD_COUNT;
}

// ─── Live clock (HH:MM, 24h) ───────────────────────────────────────────────
static void update_clock(bool force) {
    if (!obj_clock) return;
    if (clock_base == 0) {
        if (force) lv_label_set_text(obj_clock, "--:--");
        return;
    }
    uint32_t local = clock_base + (millis() - clock_base_ms) / 1000u;
    int mod = (int)(local % 86400u);
    int mins = mod / 60;
    if (!force && mins == clock_last_min) return;
    clock_last_min = mins;
    lv_label_set_text_fmt(obj_clock, "%02d:%02d", mins / 60, mins % 60);
}

// ─── BLE-down message: "Reconnecting" + animated trailing dots ─────────────
static void render_reconnecting(void) {
    if (!obj_info_s || !obj_info_w) return;
    char buf[40];
    int n = recon_dots & 3;  // 0..3 dots
    snprintf(buf, sizeof(buf), "%s%.*s", TR(STR_RECONNECT), n, "...");
    lv_label_set_text(obj_info_s, buf);
    lv_label_set_text(obj_info_w, "");
}

// ─── Refresh usage text + bars from last_data ──────────────────────────────
static void refresh_usage(void) {
    if (!connected) {
        render_reconnecting();
        lv_bar_set_value(obj_bar_s, 0, LV_ANIM_OFF);
        lv_bar_set_value(obj_bar_w, 0, LV_ANIM_OFF);
        return;
    }

    bool compact = (VIEW_CONTENT == VIEW_COMPACT);
    const char* s_lbl = compact ? TR(STR_SESSION_SHORT) : TR(STR_SESSION);
    const char* w_lbl = compact ? TR(STR_WEEK_SHORT)    : TR(STR_WEEK);

    if (!last_data.valid) {
        lv_label_set_text_fmt(obj_info_s, "%s --", s_lbl);
        lv_label_set_text_fmt(obj_info_w, "%s --", w_lbl);
        lv_bar_set_value(obj_bar_s, 0, LV_ANIM_OFF);
        lv_bar_set_value(obj_bar_w, 0, LV_ANIM_OFF);
        return;
    }
    char left[24];
    int s = (int)(last_data.session_pct + 0.5f);
    int w = (int)(last_data.weekly_pct + 0.5f);

    fmt_session_left(last_data.session_reset_mins, left, sizeof(left));
    if (compact) lv_label_set_text_fmt(obj_info_s, "%s %d%% %s", s_lbl, s, left);
    else         lv_label_set_text_fmt(obj_info_s, "%s %d%%  \xC2\xB7  %s", s_lbl, s, left);
    fmt_weekly_left(last_data.weekly_reset_mins, left, sizeof(left));
    if (compact) lv_label_set_text_fmt(obj_info_w, "%s %d%% %s", w_lbl, w, left);
    else         lv_label_set_text_fmt(obj_info_w, "%s %d%%  \xC2\xB7  %s", w_lbl, w, left);

    lv_bar_set_value(obj_bar_s, s, LV_ANIM_ON);
    lv_bar_set_value(obj_bar_w, w, LV_ANIM_ON);
    lv_obj_set_style_bg_color(obj_bar_s, pct_color(last_data.session_pct),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj_bar_w, pct_color(last_data.weekly_pct),
                              LV_PART_INDICATOR);
}

// ─── Position/show widgets for the active view mode ────────────────────────
static void apply_view(void) {
    uint8_t content = VIEW_CONTENT;
    bool text = (content == VIEW_TEXT || content == VIEW_TEXT_GRAD ||
                 content == VIEW_COMPACT);
    bool bars = (content == VIEW_GRAD || content == VIEW_TEXT_GRAD ||
                 content == VIEW_COMPACT);
    bool compact = (content == VIEW_COMPACT);

    if (compact) {
        // Short caption left-aligned just above each bar.
        lv_obj_set_style_text_font(obj_info_s, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_font(obj_info_w, &lv_font_montserrat_12, 0);
        lv_obj_align(obj_info_s, LV_ALIGN_TOP_LEFT, BAR_X, CMP_CAP_S_Y);
        lv_obj_align(obj_info_w, LV_ALIGN_TOP_LEFT, BAR_X, CMP_CAP_W_Y);
        lv_obj_align(obj_bar_s,  LV_ALIGN_TOP_MID, 0, CMP_BAR_S_Y);
        lv_obj_align(obj_bar_w,  LV_ALIGN_TOP_MID, 0, CMP_BAR_W_Y);
    } else {
        lv_obj_set_style_text_font(obj_info_s, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_font(obj_info_w, &lv_font_montserrat_14, 0);
        lv_obj_align(obj_info_s, LV_ALIGN_TOP_MID, 0, INFO_S_Y);
        lv_obj_align(obj_info_w, LV_ALIGN_TOP_MID, 0, INFO_W_Y);
        lv_obj_align(obj_bar_s,  LV_ALIGN_TOP_MID, 0, BAR_S_Y);
        lv_obj_align(obj_bar_w,  LV_ALIGN_TOP_MID, 0, BAR_W_Y);
    }

    lv_obj_t* txt[] = { obj_info_s, obj_info_w };
    lv_obj_t* bar[] = { obj_bar_s, obj_bar_w };
    for (auto o : txt) text ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN)
                            : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    for (auto o : bar) bars ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN)
                            : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);

    if (VIEW_CLOCK) {
        lv_obj_clear_flag(obj_clock, LV_OBJ_FLAG_HIDDEN);
        update_clock(true);
    } else {
        lv_obj_add_flag(obj_clock, LV_OBJ_FLAG_HIDDEN);
    }

    refresh_usage();  // text format depends on compact vs full
}

// ─── Apply a mood (swap eye bitmaps) ───────────────────────────────────────
static void apply_mood(uint8_t mood) {
    cur_mood = mood;
    lv_image_set_src(obj_eye_l, emo_eye_frames[mood][0]);
    lv_image_set_src(obj_eye_r, emo_eye_frames[mood][1]);
    apply_tint();
}

// ─── Sleep mode (limit exhausted): Zzz float + eye "breathing" ─────────────
static void zzz_anim_cb(void* var, int32_t v) {
    lv_obj_t* o = (lv_obj_t*)var;
    lv_obj_set_style_translate_y(o, -(v * 38 / 100), 0);
    lv_opa_t op = (v < 70) ? LV_OPA_COVER
                           : (lv_opa_t)(LV_OPA_COVER * (100 - v) / 30);
    lv_obj_set_style_opa(o, op, 0);
}

static void breath_anim_cb(void* var, int32_t v) {
    (void)var;  // used only as the animation key
    if (obj_eye_l) lv_image_set_scale(obj_eye_l, v);
    if (obj_eye_r) lv_image_set_scale(obj_eye_r, v);
}

static void start_sleep_anim(void) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj_zzz);
    lv_anim_set_exec_cb(&a, zzz_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 1800);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, 300);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    // obj_eye_l is just the anim key here (cb scales both eyes). Safe even
    // if obj_eye_l has other anims: lv_anim_del() below matches on the
    // (var, exec_cb) pair, so only this breathing anim is removed.
    lv_anim_set_var(&b, obj_eye_l);
    lv_anim_set_exec_cb(&b, breath_anim_cb);
    lv_anim_set_values(&b, EYE_SCALE, EYE_SCALE + EYE_SCALE * 6 / 100);
    lv_anim_set_time(&b, 2200);
    lv_anim_set_playback_time(&b, 2200);
    lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&b);
}

static void stop_sleep_anim(void) {
    lv_anim_del(obj_zzz, zzz_anim_cb);
    lv_anim_del(obj_eye_l, breath_anim_cb);
    if (obj_eye_l) lv_image_set_scale(obj_eye_l, EYE_SCALE);
    if (obj_eye_r) lv_image_set_scale(obj_eye_r, EYE_SCALE);
    if (obj_zzz) {
        lv_obj_set_style_translate_y(obj_zzz, 0, 0);
        lv_obj_set_style_opa(obj_zzz, LV_OPA_COVER, 0);
    }
}

// Enter/leave sleep when the session or weekly limit is exhausted.
static void update_sleep_state(void) {
    if (!connected) return;  // stale data while BLE is down — no sleep
    bool ex = last_data.valid && (last_data.session_pct >= 100.0f ||
                                  last_data.weekly_pct >= 100.0f);
    if (ex && !sleeping) {
        sleeping = true;
        apply_mood(EMO_SLEEP);
        if (obj_zzz) lv_obj_clear_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
        start_sleep_anim();
    } else if (!ex && sleeping) {
        sleeping = false;
        stop_sleep_anim();
        if (obj_zzz) lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
        apply_mood(EMO_NEUTRAL);
        blink_closed = false;
        schedule_next_mood();
    }
}

static lv_obj_t* make_eye(lv_obj_t* parent, int cx, int cy) {
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, emo_eye_frames[EMO_NEUTRAL][0]);
    lv_image_set_antialias(img, true);
    lv_image_set_pivot(img, EYE_SRC / 2, EYE_SRC / 2);
    lv_image_set_scale(img, EYE_SCALE);   // scales around image center (pivot)
    lv_obj_set_size(img, EYE_SRC, EYE_SRC);
    lv_obj_set_pos(img, cx - EYE_SRC / 2, cy - EYE_SRC / 2);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static lv_obj_t* make_info(lv_obj_t* parent, int y) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, THEME_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int y) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_size(b, BAR_W, BAR_H);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(b, BAR_H / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, EMO_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(b, BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, EMO_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

// ─── Create LVGL objects ───────────────────────────────────────────────────
void emo_init(void) {
    emo_container = lv_obj_create(lv_screen_active());
    lv_obj_set_size(emo_container, SCR_W, SCR_H);
    lv_obj_set_pos(emo_container, 0, 0);
    lv_obj_set_style_bg_opa(emo_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(emo_container, 0, 0);
    lv_obj_set_style_pad_all(emo_container, 0, 0);
    lv_obj_clear_flag(emo_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(emo_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(emo_container, LV_OBJ_FLAG_HIDDEN);

    obj_eye_l = make_eye(emo_container, EYE_L_CX, EYE_CY);
    obj_eye_r = make_eye(emo_container, EYE_R_CX, EYE_CY);

    obj_info_s = make_info(emo_container, INFO_S_Y);
    obj_info_w = make_info(emo_container, INFO_W_Y);
    obj_bar_s  = make_bar(emo_container, BAR_S_Y);
    obj_bar_w  = make_bar(emo_container, BAR_W_Y);

    obj_clock = lv_label_create(emo_container);
    lv_label_set_text(obj_clock, "--:--");
    lv_obj_set_style_text_font(obj_clock, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(obj_clock, THEME_TEXT, 0);
    lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_clear_flag(obj_clock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj_clock, LV_OBJ_FLAG_HIDDEN);

    obj_zzz = lv_label_create(emo_container);
    lv_label_set_text(obj_zzz, "z z z");
    lv_obj_set_style_text_font(obj_zzz, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj_zzz, THEME_TEXT, 0);
    lv_obj_align(obj_zzz, LV_ALIGN_TOP_LEFT, 188, 34);
    lv_obj_clear_flag(obj_zzz, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj_zzz, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);

    apply_mood(cur_mood);
    refresh_usage();
    apply_view();

    blink_next_ms = millis() + 2000 + (esp_random() % 4000);
    schedule_next_mood();
}

// ─── Public API ────────────────────────────────────────────────────────────
void emo_show(void) {
    active = true;
    lv_obj_clear_flag(emo_container, LV_OBJ_FLAG_HIDDEN);
    blink_closed = false;
    apply_mood(cur_mood);
    refresh_usage();
    apply_view();
    blink_next_ms = millis() + 2000 + (esp_random() % 4000);
    schedule_next_mood();
    if (sleeping) {              // re-shown while limit still exhausted
        apply_mood(EMO_SLEEP);
        if (obj_zzz) lv_obj_clear_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
        start_sleep_anim();
    } else {
        update_sleep_state();    // may enter sleep if data arrived hidden
    }
}

void emo_hide(void) {
    active = false;
    lv_obj_add_flag(emo_container, LV_OBJ_FLAG_HIDDEN);
}

bool emo_is_active(void) {
    return active;
}

// Button short-press: cycle the 10 view modes (5 layers ×, then with a
// top clock). Emotions change on their own random 20 s–10 min timer.
void emo_next_mood(void) {
    view_idx = (view_idx + 1) % EMO_VIEW_COUNT;
    apply_view();
}

void emo_set_view(uint8_t idx) {
    view_idx = idx % EMO_VIEW_COUNT;
    apply_view();
}

uint8_t emo_get_view(void) {
    return view_idx;
}

void emo_next_emotion(void) {
    if (sleeping) return;  // don't override sleep via button
    apply_mood(pick_random_mood());
    blink_closed = false;
    schedule_next_mood();
}

void emo_set_usage(const UsageData* data) {
    if (data) last_data = *data;
    connected = true;  // fresh data implies the BLE link is up
    if (data && data->epoch) {
        clock_base = data->epoch + (uint32_t)data->tz_off;
        clock_base_ms = millis();
        update_clock(true);
    }
    update_sleep_state();
    if (!active) return;
    apply_tint();
    refresh_usage();
}

void emo_set_connected(bool c) {
    if (c == connected) return;
    connected = c;
    if (!c) {
        recon_dots = 0;
        recon_last_ms = millis();
        if (sleeping) {  // stale usage — drop the sleep visuals
            sleeping = false;
            stop_sleep_anim();
            if (obj_zzz) lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (active) refresh_usage();
}

void emo_relang(void) {
    apply_view();  // re-applies localized text + fonts + layout
}

void emo_relayout(void) {
    if (!active) return;
    lv_obj_invalidate(emo_container);
}

// ─── Tick — blink + random auto mood change ────────────────────────────────
void emo_tick(void) {
    if (!active) return;
    uint32_t now = millis();

    if (VIEW_CLOCK) update_clock(false);

    if (!connected) {  // BLE down: animate the "Reconnecting…" dots only
        if (now - recon_last_ms >= 450) {
            recon_last_ms = now;
            recon_dots++;
            render_reconnecting();
        }
        return;
    }

    if (sleeping) return;  // limit exhausted: no mood change / blink

    // Random auto mood change (20 s … 10 min).
    if (now >= mood_next_ms) {
        apply_mood(pick_random_mood());
        blink_closed = false;
        schedule_next_mood();
    }

    // Natural random blink. Sleep mood is already a flat bar — skip.
    bool can_blink = (cur_mood != EMO_SLEEP);

    if (blink_closed) {
        if (now >= blink_close_ms) {
            blink_closed = false;
            lv_image_set_src(obj_eye_l, emo_eye_frames[cur_mood][0]);
            lv_image_set_src(obj_eye_r, emo_eye_frames[cur_mood][1]);
            blink_next_ms = now + 2000 + (esp_random() % 4000);
        }
    } else if (can_blink && now >= blink_next_ms) {
        blink_closed = true;
        lv_image_set_src(obj_eye_l, emo_eye_blink);
        lv_image_set_src(obj_eye_r, emo_eye_blink);
        blink_close_ms = now + 90;
    }
}
