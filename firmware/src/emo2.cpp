#include "emo2.h"
#include "emo_eyes_hd.h"
#include "i18n.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include <tuple>

// ─── Layout — portrait 240×280 baseline, landscape adapts via scr_w()/scr_h()
// Native panel is 240×280 portrait. Rotation 1 swaps to logical 280×240. All
// position calculations route through scr_w()/scr_h() so a single
// emo2_relayout() call after lv_display_set_resolution rerenders the entire
// scene at the new aspect ratio without having to recreate any objects.
extern uint8_t current_rotation;        // owned by main.cpp
static inline int scr_w(void) { return (current_rotation % 2 == 0) ? 240 : 280; }
static inline int scr_h(void) { return (current_rotation % 2 == 0) ? 280 : 240; }
// Portrait constants — used by the create-time setup. Run-time positioning
// (relayout, refresh_usage_info) uses scr_w()/scr_h() so it tracks rotation.
#define SCR_W       240
#define SCR_H       280
#define BASE_PX     80          // base eye on-screen size
// "Halo" is now a SHADOW layer underneath the eye_base — same silhouette, same
// scale (no outer glow), tinted darker, offset down by HALO_SHADOW_DY pixels.
// The outer-glow version (scale +8, OPA 30) looked fuzzy and stairstepped on
// the AMOLED panel; this drop-shadow gives a clean 3D-emboss feel that lifts
// the silhouette off the black background without the ugly halo ring.
#define HALO_SRC        64          // same source as base eye
#define HALO_PX         BASE_PX     // no scale-up — 1:1 with base
#define HALO_SHADOW_DY  3           // pixels: shadow sits below base
#define SPEC_W      22          // specular displayed size
#define SPEC_H      16
#define EYE_GAP     20
#define EYE_CY      110
#define EYE_L_CX    ((SCR_W - (2 * BASE_PX + EYE_GAP)) / 2 + BASE_PX / 2)
#define EYE_R_CX    (EYE_L_CX + BASE_PX + EYE_GAP)

// LVGL scale factors (256 = 1.0×). Source sizes from emo_eyes_hd.h.
#define BASE_SCALE  (256 * BASE_PX  / 64)
#define HALO_SCALE  (256 * HALO_PX  / HALO_SRC)
#define SPEC_SCALE  (256 * SPEC_W   / 14)

// ─── EMO-look palette ──────────────────────────────────────────────────────
// Teal-cyan brand colour, close to the Living.ai EMO robot's eye glow.
#define EMO2_BASE   lv_color_hex(0x3DE0E0)
#define EMO2_WARN   lv_color_hex(0xFFA000)   // deeper amber (was 0xFFB02E)
#define EMO2_DANGER lv_color_hex(0xFF1818)   // saturated red — was 0xFF4D4D
                                              // which read pink (G/B=77 instead of 24)
#define EMO2_SPEC   lv_color_hex(0xFFFFFF)

// "Halo" repurposed as drop-shadow under the silhouette — 50% opacity reads
// as clear depth without dominating. Combined with HALO_SHADOW_DY=3 px y-
// offset and a darker tint (shadow_color() in apply_colours) this gives a
// 3D-emboss look that lifts the eye off the black background.
#define HALO_OPA    LV_OPA_50
#define SPEC_OPA    LV_OPA_90

// ─── Moods (order must match emo2_*_frames in emo_eyes_hd.h) ───────────────
enum mood_t : uint8_t {
    M_HAPPY, M_NEUTRAL, M_SLEEP, M_ANGRY, M_UPSET, M_SAD, M_LOVE,
    M_CIRCLE, M_PUPIL_LEFT, M_PUPIL_SLIT, M_CROSS,
    M_OVAL_TALL, M_DIAMOND,
    M_SQUIRCLE_THICK, M_RECT_TV, M_CAPSULE_H, M_CRESCENT,
    M_BRACKETS, M_PIXEL_CLUSTER, M_Q_EYE, M_EXCLAIM,
    M_COUNT
};

// ─── View modes (mirrors emo.cpp): 5 content × 2 clock = 10 ────────────────
enum view_t : uint8_t {
    V_EYES, V_TEXT, V_GRAD, V_TEXT_GRAD, V_COMPACT, V_CONTENT_COUNT
};
#define EMO2_VIEW_COUNT 10
#define V_BODY(v)  ((v) % V_CONTENT_COUNT)
#define V_CLOCK(v) ((v) >= V_CONTENT_COUNT)

// Info-area layout. Classic / text-only views stack text-S / bar-S /
// text-W / bar-W vertically (was text-S / text-W / bar-S / bar-W which
// didn't match the emo2-stats.html Classic reference). Spacing tuned for
// the 280-px portrait — eyes end at ~y170, then 14 px to first text.
//
// Three placement variants (item 2/3): top / middle / bottom of the lower
// screen block. Index 0/1/2 keyed by `text_placement` (default 1 = middle =
// legacy values). The bar Y values trail their text Y by +16 / +16 / +16 so
// the bar lives directly below its label across all three placements.
#define TEXT_PLACE_TOP    0
#define TEXT_PLACE_MIDDLE 1
#define TEXT_PLACE_BOTTOM 2
// Top variant shifted +12 px (user-feedback: "сверху" was too close to the
// eyes; bumped down by ~2 line-heights to clear the silhouette). Middle and
// bottom unchanged.
// Bottom values now visibly differ from MIDDLE — the previous tables had
// INFO_W_Y_TBL[BOTTOM] == INFO_W_Y_TBL[MIDDLE] (both 220), so "bottom"
// only moved the session row by 16 px (184→200) and weekly didn't move
// at all. User read that as "bottom не применяется". The new BOTTOM
// Ys drop both rows ~16-20 px below their MIDDLE positions while
// staying above the H=280 / H=240 hardware edges.
static const int16_t INFO_S_Y_TBL[3] = {172, 184, 216};
static const int16_t INFO_W_Y_TBL[3] = {192, 220, 240};
static const int16_t BAR_S_Y_TBL[3]  = {188, 200, 232};
static const int16_t BAR_W_Y_TBL[3]  = {208, 236, 256};
// info_s_y / info_w_y / bar_s_y / bar_w_y — defined further down (after
// stats_layout + LAYOUT_RIBBON come into scope). They map ribbon's
// "bottom" pick onto middle's Y so the overlay text stays above the
// ribbon chrome.
#define BAR_W        184
#define BAR_H        10
#define BAR_X        ((SCR_W - BAR_W) / 2)
#define CLOCK_Y      18

// ─── State ─────────────────────────────────────────────────────────────────
static lv_obj_t* container     = nullptr;
static lv_obj_t* eye_halo[2]   = {nullptr, nullptr};
static lv_obj_t* eye_base[2]   = {nullptr, nullptr};
static lv_obj_t* eye_spec[2]   = {nullptr, nullptr};
static lv_obj_t* obj_zzz       = nullptr;
static lv_obj_t* obj_status    = nullptr;  // reconnecting / info text
static lv_obj_t* obj_info_s    = nullptr;  // session % text
static lv_obj_t* obj_info_w    = nullptr;  // weekly % text
static lv_obj_t* obj_bar_s     = nullptr;  // session bar
static lv_obj_t* obj_bar_w     = nullptr;  // weekly bar
static lv_obj_t* obj_clock     = nullptr;  // top HH:MM (all styles)
static lv_obj_t* obj_clock_secs = nullptr; // SS suffix — only used by CS_SECONDS

// Bezel-orbit % layout. Rounded-rect outline as dim track. SESSION traces
// the RIGHT half perimeter (top-right + right edge + bottom-right) starting
// CW from a small gap at the top-centre. WEEKLY traces the LEFT half mirrored
// (CCW). Two gaps (top-centre, bottom-centre) visually separate the two
// values — at 100 % each fill wraps from one gap to the other, drawing the
// whole right (or left) half of the bezel.
//
// 6 sub-rects total (3 per half: top stub, side, bottom stub). They grow
// independently as cumulative `fill` walks the path.
static lv_obj_t* bezel_outline = nullptr;
static lv_obj_t* bez_s_top     = nullptr;   // session: top-right stub
static lv_obj_t* bez_s_arc_top = nullptr;   // session: top-right CORNER (arc)
static lv_obj_t* bez_s_right   = nullptr;   // session: right edge
static lv_obj_t* bez_s_arc_bot = nullptr;   // session: bottom-right CORNER
static lv_obj_t* bez_s_bot     = nullptr;   // session: bottom-right stub
static lv_obj_t* bez_w_top     = nullptr;   // weekly:  top-left stub
static lv_obj_t* bez_w_arc_top = nullptr;   // weekly:  top-left CORNER
static lv_obj_t* bez_w_left    = nullptr;   // weekly:  left edge
static lv_obj_t* bez_w_arc_bot = nullptr;   // weekly:  bottom-left CORNER
static lv_obj_t* bez_w_bot     = nullptr;   // weekly:  bottom-left stub
#define BEZEL_INSET    6
#define BEZEL_THICK    6      // thinner outline + fills (was 9)
#define BEZEL_RADIUS   30
#define BEZEL_GAP      16     // total px gap at top-centre and bottom-centre
                              // between session and weekly fills
#define BEZ_HALF_GAP   (BEZEL_GAP / 2)
// Path segment lengths for ONE half of the perimeter (session OR weekly).
// horiz stub = from centre-gap to corner-start; vert = full straight side;
// then a mirrored horiz stub on the bottom.
#define BEZ_HORIZ_LEN  ((SCR_W - 2 * BEZEL_INSET) / 2 - BEZ_HALF_GAP - BEZEL_RADIUS)
#define BEZ_VERT_LEN   (SCR_H - 2 * BEZEL_INSET - 2 * BEZEL_RADIUS)
// Per-corner arc length (quarter circumference) — integer-truncated since
// the fill walks in pixel units. With BEZEL_RADIUS=30: π·30/2 ≈ 47.
#define BEZ_ARC_LEN    ((int)(3.14159265f * BEZEL_RADIUS / 2.0f))
// Half-path = horiz stub + corner arc + side edge + corner arc + horiz stub.
#define BEZ_PATH_LEN   (2 * BEZ_HORIZ_LEN + BEZ_VERT_LEN + 2 * BEZ_ARC_LEN)

// Twin-columns layout: two vertical bars on the screen edges. Left = session,
// right = weekly. Fill grows from the bottom up.
static lv_obj_t* col_left_track   = nullptr;
static lv_obj_t* col_left_fill    = nullptr;
static lv_obj_t* col_right_track  = nullptr;
static lv_obj_t* col_right_fill   = nullptr;
#define COL_THICK     7       // (was 4) wider so 20% reads as a chunky bar
#define COL_INSET     6
#define COL_HEIGHT    (SCR_H - 2 * COL_INSET)

// HUD-ribbon layout: two thin horizontal bars + tiny S/W labels stacked at
// the bottom of the screen, in a "control panel" / monitor strip aesthetic.
static lv_obj_t* ribbon_lbl_s   = nullptr;
static lv_obj_t* ribbon_lbl_w   = nullptr;
static lv_obj_t* ribbon_pct_s   = nullptr;
static lv_obj_t* ribbon_pct_w   = nullptr;
// Ribbon now uses discrete segment blocks instead of a smooth bar so it
// reads like a HUD / mixer-channel meter (matching the web emo2-stats.html
// preview). 10 segments per row, lit count = pct/10.
#define RIBBON_Y_S         240     // session row
#define RIBBON_Y_W         258     // weekly row (tighter spacing now that
                                    // segments are taller than the old 4-px bar)
#define RIBBON_SEG_COUNT   10
#define RIBBON_SEG_H       14      // (was 10) taller segments
#define RIBBON_SEG_W       14      // (was 12) wider — but capped at 14 so 10
                                    // segments + 9 gaps still fit RIBBON_BAR_W
                                    // budget (158 px of 164 available)
#define RIBBON_SEG_GAP     2
#define RIBBON_BAR_X       38      // after the "S "/"W " label
#define RIBBON_BAR_W       (RIBBON_SEG_COUNT * RIBBON_SEG_W + (RIBBON_SEG_COUNT - 1) * RIBBON_SEG_GAP)
static lv_obj_t* ribbon_seg_s[RIBBON_SEG_COUNT] = {nullptr};
static lv_obj_t* ribbon_seg_w[RIBBON_SEG_COUNT] = {nullptr};

// Tear-pearls layout: chain of small dots under each eye. Count = pct/20.
#define PEARLS_PER_EYE 5
static lv_obj_t* pearl_l[PEARLS_PER_EYE] = {nullptr, nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* pearl_r[PEARLS_PER_EYE] = {nullptr, nullptr, nullptr, nullptr, nullptr};

// Corner-chip layout: TWO framed badges, one in each bottom corner. LEFT
// chip = SESSION % (swapped from previous version), RIGHT chip = WEEKLY %.
// Each chip shows the % value + a full localised label ("Сессия" / "Неделя").
// text_format controls whether the chip body shows %, reset-time, or both
// stacked. Visibility driven by text_source:
//   - SESSION → only LEFT (session) visible
//   - WEEKLY  → only RIGHT (weekly) visible
//   - BOTH    → both visible
//   - OFF     → neither (eyes-only)
static lv_obj_t* chip_frame_l = nullptr;     // LEFT  — session
static lv_obj_t* chip_big_l   = nullptr;
static lv_obj_t* chip_tag_l   = nullptr;     // localised "Сессия"
static lv_obj_t* chip_fill_l  = nullptr;     // bottom-anchored coloured rim showing % for PCT_RESET
static lv_obj_t* chip_frame_r = nullptr;     // RIGHT — weekly
static lv_obj_t* chip_big_r   = nullptr;
static lv_obj_t* chip_tag_r   = nullptr;     // localised "Неделя"
static lv_obj_t* chip_fill_r  = nullptr;
// CHIP_W bumped 64→86 so the full Russian "Сессия" / "Неделя" labels fit
// at montserrat_14. CHIP_PAD_X/B bumped 8→18 to keep the chip away from
// the AMOLED's rounded display corners which were clipping the frame.
#define CHIP_W     86
#define CHIP_H     56
#define CHIP_PAD_X 18
#define CHIP_PAD_B 18

// ECG monitor layout: scrolling polyline near the bottom of the screen.
#define ECG_POINTS  60
#define ECG_Y_MID   200
#define ECG_X0      12
#define ECG_X1      (SCR_W - 12)
static lv_obj_t* ecg_line  = nullptr;
// ECG layout text overlay now reuses obj_info_s/_w like BEZEL/COLUMNS/RIBBON
// (text_source + text_format apply uniformly). Old built-in readout removed.
static lv_point_precise_t ecg_pts[ECG_POINTS];
static uint32_t ecg_phase_ms   = 0;
static uint32_t ecg_last_tick  = 0;

static bool      active        = false;
static bool      sleeping      = false;
static bool      connected     = false;
static bool      token_expired = false;     // CTRL 0x18 / 0x19 signal from daemon
static bool      manual_mode   = false;     // CTRL 0x1A / 0x1B — disables auto schedulers
static bool      ota_active    = false;     // OTA upload in progress (set by ota.cpp via emo2_set_ota_progress)
// % stats layout: 0=off, 1=bezel_orbit, 2=twin_columns, 3=hud_ribbon.
// User picks from the gallery in the web UI; daemon pushes the active one on
// connect via CTRL 0x20–0x23.
#define LAYOUT_OFF          0
#define LAYOUT_BEZEL        1
#define LAYOUT_COLUMNS      2
#define LAYOUT_RIBBON       3
#define LAYOUT_BROWS        4
#define LAYOUT_TEAR_PEARLS  5
#define LAYOUT_CORNER_CHIP  6
#define LAYOUT_ECG          7
#define LAYOUT_CLASSIC      8     // legacy text-under-eyes + thin horizontal bars
#define LAYOUT_MAX          8
static uint8_t   stats_layout  = LAYOUT_BEZEL;

// Per-layout Y-placement override. Ribbon's chrome sits near Y=240/258
// (RIBBON_Y_S/W); user spec for ribbon+bottom (item 4.1 refined):
//   • bottom row (weekly text+bar) STAYS at global MIDDLE Y (so it sits
//     comfortably above the ribbon, where it always lived in middle pick)
//   • top row (session text+bar) SHIFTS DOWN to global BOTTOM Y, i.e.
//     ~16 px lower than its middle position, tightening the two-row stack
// All other layouts use INFO_*_Y_TBL[placement] directly.
// Ribbon-specific BOTTOM override: ribbon chrome lives at Y=H-40 (session)
// and Y=H-22 (weekly), i.e. 240 / 258 on portrait. Using the global
// BOTTOM Y table (216 / 240 / 232 / 256) puts the overlay text right
// on top of the ribbon labels — user reported "ничего не двигается"
// because the rows collided with ribbon chrome. Step the BOTTOM picks
// up one notch in ribbon mode so text sits cleanly above the chrome.
// Ribbon-bottom inter-row gap matches the TOP-placement gap (20 px text,
// 20 px bar), so the two rows read as a compact pair like at the top.
// TOP : session=172, weekly=192 (gap 20); BOTTOM was session=200/weekly=224
// (gap 24) — too loose. Now session=200/weekly=220 (gap 20).
static inline int16_t info_s_y(uint8_t placement) {
    if (stats_layout == LAYOUT_RIBBON && placement == TEXT_PLACE_BOTTOM) return 200;
    return INFO_S_Y_TBL[placement <= TEXT_PLACE_BOTTOM ? placement : TEXT_PLACE_MIDDLE];
}
static inline int16_t info_w_y(uint8_t placement) {
    if (stats_layout == LAYOUT_RIBBON && placement == TEXT_PLACE_BOTTOM) return 220;
    return INFO_W_Y_TBL[placement <= TEXT_PLACE_BOTTOM ? placement : TEXT_PLACE_MIDDLE];
}
static inline int16_t bar_s_y(uint8_t placement) {
    if (stats_layout == LAYOUT_RIBBON && placement == TEXT_PLACE_BOTTOM) return 216;
    return BAR_S_Y_TBL[placement <= TEXT_PLACE_BOTTOM ? placement : TEXT_PLACE_MIDDLE];
}
static inline int16_t bar_w_y(uint8_t placement) {
    if (stats_layout == LAYOUT_RIBBON && placement == TEXT_PLACE_BOTTOM) return 236;
    return BAR_W_Y_TBL[placement <= TEXT_PLACE_BOTTOM ? placement : TEXT_PLACE_MIDDLE];
}

// Single source of truth for the status / reconnect label position. Placed in
// the safe belt just below the eyes — above every layout's bottom furniture
// (corner chips at H-74, ribbon at H-40, ecg polyline). Orient-aware via
// scr_h()/current_rotation so it never collides with the chip in landscape
// (where H shrinks to 240 and the chip top rises to 166). Every path that
// writes obj_status text (token_expired, OTA, relayout) routes through this so
// the label has one stable Y and never jumps between states.
static void position_status_label(void) {
    if (!obj_status) return;
    const int H = scr_h();
    const int eye_cy = (current_rotation % 2 == 0) ? EYE_CY : (H * 11 / 28);
    lv_obj_align(obj_status, LV_ALIGN_TOP_MID, 0, eye_cy + 40);
}
// Usage text content mode (CTRL 0x44–0x47). Default: show both pct + reset.
// Legacy enum kept for back-compat — text rendering now uses the
// (text_source, text_format) pair below. emo2_set_text_mode() decomposes a
// legacy value into the pair.
#define TEXT_MODE_NONE       0
#define TEXT_MODE_PCT_ONLY   1
#define TEXT_MODE_RESET_ONLY 2
#define TEXT_MODE_BOTH       3
static uint8_t   text_mode    = TEXT_MODE_BOTH;
// Text source: WHICH value(s) to display (CTRL 0x51-0x54).
#define TEXT_SRC_OFF      0
#define TEXT_SRC_SESSION  1
#define TEXT_SRC_WEEKLY   2
#define TEXT_SRC_BOTH     3
static uint8_t   text_source  = TEXT_SRC_BOTH;
// Text format: HOW the visible value is formatted (CTRL 0x55-0x57).
#define TEXT_FMT_PCT        0     // "S 42%"
#define TEXT_FMT_PCT_RESET  1     // "S 42% · 3h 20m"
#define TEXT_FMT_RESET      2     // "S 3h 20m"
static uint8_t   text_format  = TEXT_FMT_PCT_RESET;
// Text placement: WHERE the overlay text(s) sit in the lower screen block
// (CTRL 0x5C-0x5E). 0=top, 1=middle (legacy default), 2=bottom. Indexes
// INFO_*_Y_TBL / BAR_*_Y_TBL arrays. Persisted in NVS as `e2tp`.
static uint8_t   text_placement = TEXT_PLACE_MIDDLE;
// Orient-aware placement clamp (items 7/8). Landscape (H=240) has almost no
// vertical room below the eyes once the ribbon/ecg chrome claims the bottom,
// so the overlay text is forced to the TOP belt regardless of the stored
// pick. Mirrors the web's placementAllowed(layout,'horizontal') coercion, and
// keeps the device correct even if the daemon pushes middle/bottom. Portrait
// and every other layout use the stored value unchanged.
static inline uint8_t effective_text_placement(void) {
    if ((current_rotation % 2 != 0) &&
        (stats_layout == LAYOUT_RIBBON || stats_layout == LAYOUT_ECG))
        return TEXT_PLACE_TOP;
    return text_placement;
}
// 0xFF = auto by % (use gradient).
// 0/1/2 = fixed palette index (EMO2_BASE / EMO2_WARN / EMO2_DANGER).
// 0x80 = custom RGB (use custom_halo_r/g/b below).
static uint8_t   color_override = 0xFF;
static uint8_t   custom_halo_r  = 0xFF;
static uint8_t   custom_halo_g  = 0xFF;
static uint8_t   custom_halo_b  = 0xFF;
// Independent palette override for the % LAYOUT fills (bezel / columns /
// ribbon / pearls / chip / classic / ECG). Same encoding as `color_override`
// — 0xFF = lerp by pct (current behaviour), 0..2 = locked EMO2_BASE/WARN/DANGER.
// Persisted in NVS under key `e2lc`; CTRL bytes 0x4D-0x50 set it.
// Same encoding as color_override: 0xFF=auto, 0/1/2=palette, 0x80=custom.
static uint8_t   layout_color_override = 0xFF;
static uint8_t   custom_layout_r       = 0xFF;
static uint8_t   custom_layout_g       = 0xFF;
static uint8_t   custom_layout_b       = 0xFF;
// Clock colour override (separate from layout/halo). Three modes:
//   0xFF = DEFAULT — use the per-style hardcoded colour from
//          apply_clock_style() (white / cyan / amber as each style defines).
//   0xA0 = AUTO    — follow the live %-by-pct gradient (pct_color()), so the
//          clock tints with usage just like the halo / layout fills.
//   0x80 = CUSTOM  — paint with custom_clock_{r,g,b}.
static uint8_t   clock_color_override  = 0xFF;
static uint8_t   custom_clock_r        = 0xFF;
static uint8_t   custom_clock_g        = 0xFF;
static uint8_t   custom_clock_b        = 0xFF;
// Clock style — 0=off, 1=minimal, 2=big, 3=mono, 4=dot. Persisted in NVS as
// key `e2cs`. Together with V_CLOCK(view_idx) controls clock visibility:
// off ⇒ always hidden; else V_CLOCK still gates whether the current view
// shows the clock.
// 8 user-picked clock styles (from emo2-clock-candidates.html gallery) +
// off. The previous {minimal, big, mono, dot} placeholders are retired;
// load_emo2_cfg_from_nvs migrates the old NVS byte to the closest new
// pick so existing users don't lose their setting after OTA.
#define CS_OFF         0
#define CS_MONO        1   // c01 — Share Tech Mono · white · big
#define CS_MAJOR_MONO  2   // c05 — Major Mono Display feel · cyan · spaced
#define CS_ORBITRON    3   // c07 — Orbitron 700 · cyan · widely spaced
#define CS_OUTLINE     4   // c08 — Outline-only (LVGL approximation)
#define CS_NEON        5   // c10 — Neon glow halo · cyan
// (Badge style dropped — was c12, didn't match brand visually.)
#define CS_SECONDS     6   // c13 — HH:MM:SS · seconds tick visible
#define CS_BRACKET     7   // c15 — [HH:MM] · amber inside dim brackets
#define CS_MAX         7
static uint8_t   clock_style = CS_MONO;
static void apply_clock_style(void);   // fwd-decl — defined further down
static void apply_clock_color(float pct);   // fwd-decl — defined near apply_clock_style

// Pace multipliers (×10). 10 = 1.0× (legacy default). User-configurable in
// the web ⏱ Timing block, NVS-persisted as `e2ap` / `e2fp`.
// Defaults 20 (= 2.0×) — animations + form rotations are noticeably less
// frenetic than the original cadence which felt overstimulating.
static uint8_t   anim_pace_x10 = 20;   // blink/heartbeat/saccade/wink/curious/warning
static uint8_t   form_pace_x10 = 20;   // mood rotation
// Apply pace to a base milliseconds value. Clamps the lower bound so a
// 0.5× multiplier never collapses a 1.5-sec interval into < 100 ms.
#define ANIM_PACE(ms) ((uint32_t)(((uint32_t)(ms) * anim_pace_x10 + 5) / 10))
#define FORM_PACE(ms) ((uint32_t)(((uint32_t)(ms) * form_pace_x10 + 5) / 10))
static uint8_t   cur_mood      = M_NEUTRAL;
static UsageData last_data     = {};
static uint32_t  last_data_received_ms = 0;  // for data_stale() — set by emo2_set_data_received()

// `cfg_apply_in_progress` is set while `emo2_apply_cfg_json` parses a JSON
// blob and calls multiple setters in sequence (text_mode + text_source +
// text_format + text_placement + layout_color + clock_style + …). Each
// setter would otherwise trigger its own `apply_view()` + `refresh_usage_info()`,
// causing 3-4 full repaints per BLE push. With the flag, setters skip the
// UI refresh; we do ONE combined refresh at the end of `emo2_apply_cfg_json`.
static bool cfg_apply_in_progress = false;

// Test-pct override (web "TEST → ESP" feature). Daemon pushes `{cfg:{test_pct:N}}`
// when the user drags the gradient-editor TEST slider. While the override is
// active the colour pipeline uses this value INSTEAD of real session_pct so
// the user can SEE every threshold crossing on the device without waiting for
// real usage to climb. Auto-clears after TEST_PCT_TTL_MS without a new push.
static float    test_pct_override   = -1.0f;   // -1 = inactive
static uint32_t test_pct_expires_ms = 0;
#define TEST_PCT_TTL_MS  6000   // 6 s — long enough to spot the colour, short
                                // enough that real data takes over quickly

// Returns the % to feed the colour pipeline. Test override wins; otherwise
// real session_pct. Both BAR fill amounts and TEXT labels still use the real
// pct — only the COLOURS are overridden.
static inline float eff_session_pct(void) {
    if (test_pct_override >= 0.0f && (int32_t)(test_pct_expires_ms - millis()) > 0) {
        return test_pct_override;
    }
    return last_data.valid ? last_data.session_pct : 0.0f;
}

// ─── Per-state config + state machine (Phase A) ────────────────────────────
// Mirrors daemon's emo2_states.json. Daemon ships the whole blob over the
// RX char as JSON; ESP persists to NVS + drives its own rotation/state.
// Indices match daemon FORM_NAMES / OP_NAMES (op index is name-list, NOT
// the DM_* enum — translated +1 when calling emo2_trigger_op).
#define EMO2_MAX_FORMS_PER_STATE 8
#define EMO2_MAX_OPS_PER_STATE   8
#define EMO2_NUM_STATES          4   // CONNECTED / CONNECTING / BLE_OFF / TOKEN_EXPIRED
#define EMO2_LAYOUT_SLOTS        8   // text_mode[] indexed by LAYOUT_*

typedef enum {
    EST_CONNECTED = 0, EST_CONNECTING, EST_BLE_OFF, EST_TOKEN_EXPIRED
} emo2_dstate_t;

struct emo2_state_cfg_t {
    uint8_t forms[EMO2_MAX_FORMS_PER_STATE];
    uint8_t n_forms;
    uint8_t ops[EMO2_MAX_OPS_PER_STATE];
    uint8_t n_ops;
    uint8_t color;          // 0=auto, 1=cyan, 2=amber, 3=red, 4=custom
    uint8_t custom_r;
    uint8_t custom_g;
    uint8_t custom_b;
    // Phase D — per-state visual stack lifted from global emo2_stats.
    // 0xFF in any field = "field not set, fall through to global default".
    uint8_t layout;         // LAYOUT_* enum (0..7); 0xFF = unset
    uint8_t text_source;    // TEXT_SRC_*  (0..3); 0xFF = unset
    uint8_t text_format;    // TEXT_FMT_*  (0..2); 0xFF = unset
    uint8_t text_placement; // TEXT_PLACE_* (0..2); 0xFF = unset
    uint8_t layout_color;   // 0=auto, 1=cyan, 2=amber, 3=red, 4=custom; 0xFF = unset
    uint8_t lc_r;           // custom layout RGB (when layout_color == 4)
    uint8_t lc_g;
    uint8_t lc_b;
};
struct emo2_full_cfg_t {
    uint32_t magic;         // 0xE2C0FFEE — for forward-compat detection
    emo2_state_cfg_t per_state[EMO2_NUM_STATES];
    uint8_t  active_layout;
    uint8_t  layout_text_mode[EMO2_LAYOUT_SLOTS];
    uint8_t  _pad[3];
};
// Magic bumped 0xE2C0FFEE → 0xE2C0FFEF when Phase D added per-state
// layout / text / layout_color fields to emo2_state_cfg_t. struct sizeof
// changed too, so the load-time length check independently rejects
// records from the previous schema. Users see factory defaults on first
// boot after this firmware and re-pick their per-state visuals.
#define EMO2_CFG_MAGIC 0xE2C0FFEF

// Sane fallback if NVS is empty and daemon hasn't pushed anything yet.
static emo2_full_cfg_t emo2_cfg = {
    .magic = EMO2_CFG_MAGIC,
    .per_state = {
        // Each state initialiser: forms[], n_forms, ops[], n_ops, color,
        // custom_r/g/b, layout, text_source, text_format, text_placement,
        // layout_color, lc_r/g/b. 0xFF in any Phase-D field = "fall back to
        // global" (state machine resolves at apply time).
        // CONNECTED: neutral/happy/circle + blink/wave, halo=auto, layout=ribbon
        { {M_NEUTRAL, M_HAPPY, M_CIRCLE}, 3, {0/*blink*/, 9/*wave*/}, 2, 0, 0, 0, 0,
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0 },
        // CONNECTING: circle + pulse_alt, halo=cyan
        { {M_CIRCLE}, 1, {10/*pulse_alt*/}, 1, 1, 0, 0, 0,
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0 },
        // BLE_OFF: cross + upset + shake, halo=red
        { {M_CROSS, M_UPSET}, 2, {4/*shake*/}, 1, 3, 0, 0, 0,
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0 },
        // TOKEN_EXPIRED: cross + angry + warning, halo=red
        { {M_CROSS, M_ANGRY}, 2, {12/*warning*/}, 1, 3, 0, 0, 0,
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0 },
    },
    .active_layout = LAYOUT_BEZEL,
    // index by LAYOUT_*: OFF, BEZEL, COLUMNS, RIBBON, BROWS, PEARLS, CHIP, ECG
    .layout_text_mode = { TEXT_MODE_BOTH, TEXT_MODE_BOTH, TEXT_MODE_PCT_ONLY,
                          TEXT_MODE_NONE, TEXT_MODE_BOTH, TEXT_MODE_BOTH,
                          TEXT_MODE_NONE, TEXT_MODE_PCT_ONLY },
    ._pad = {0,0,0},
};
static bool emo2_cfg_loaded = false;       // true once NVS or daemon populated

// Rotation scheduler state.
#define EMO2_FORM_ROT_MS  20000
#define EMO2_OP_ROT_MS     8000
#define EMO2_DATA_STALE_MS 150000UL   // 2.5× POLL_INTERVAL (60s)
static emo2_dstate_t prev_dstate = EST_BLE_OFF;
static uint32_t      form_next_ms = 0, op_next_ms = 0;
static uint8_t       rot_form_i   = 0, rot_op_i   = 0;

// Forward decls — implementations live below the public API block.
static void emo2_load_cfg_from_nvs(void);
static void emo2_save_cfg_to_nvs(void);
static void emo2_tick_state_machine(void);
// Forward-declared so refresh_usage_info + emo2_tick can call it before
// the body at ~L2680. "Loading" includes stale data (data_stale==true),
// so the layout chrome falls back to the loading animation when the
// daemon connection drops mid-session.
static bool data_stale(void);
static uint8_t   recon_dots    = 0;
static uint32_t  recon_last_ms = 0;
static uint8_t   view_idx      = 0;        // 0..9 (see EMO2_VIEW_COUNT)

// Live clock (HH:MM): base epoch captured at the last sync + millis snapshot.
static uint32_t  clock_base    = 0;
static uint32_t  clock_base_ms = 0;
static int       clock_last_min = -1;

// Animation schedulers — each driven from emo2_tick().
static uint32_t  blink_next_ms     = 0;
static uint32_t  mood_next_ms      = 0;
static uint32_t  saccade_next_ms   = 0;
static uint32_t  curious_next_ms   = 0;
static uint32_t  heartbeat_next_ms = 0;
static uint32_t  warning_next_ms   = 0;
static uint32_t  wink_next_ms      = 0;     // single-eye blink
static uint32_t  eyeroll_start_ms  = 0;     // 0 = idle; else timestamp of start
static uint32_t  eyeroll_next_ms   = 0;
static uint32_t  lookaround_start_ms = 0;   // 0 = idle
static uint32_t  lookaround_next_ms  = 0;
static uint32_t  shake_start_ms    = 0;     // 0 = idle
static uint32_t  bounce_start_ms   = 0;     // 0 = idle (happy reaction)
static uint32_t  nod_start_ms      = 0;     // 0 = idle (slow agreement)
static uint32_t  nod_next_ms       = 0;
static uint32_t  confused_next_ms  = 0;
static uint32_t  wave_start_ms     = 0;     // 0 = idle
static uint32_t  wave_next_ms      = 0;
static uint32_t  pulse_alt_start_ms= 0;     // 0 = idle
static uint32_t  pulse_alt_next_ms = 0;
static float     last_session_pct  = -1.0f; // tracks deltas for surprise

#define EYEROLL_DUR_MS    900
#define LOOKAROUND_DUR_MS 1600
#define SHAKE_DUR_MS      500     // was 400 — slightly longer for visibility
#define BOUNCE_DUR_MS     900     // was 700
#define NOD_DUR_MS        1800    // was 1500
#define WAVE_DUR_MS       4000     // ~2.5 cycles of horizontal sine drift
#define PULSE_ALT_DUR_MS  3500     // ~3 cycles of out-of-phase scale
#define DIAG_DUR_MS       16000     // 16 × 1-second steps

static bool      diag_active   = false;
static uint32_t  diag_start_ms = 0;
static int8_t    diag_step     = -1;

// Operations the diag step can demonstrate. DM_NONE = just show form+color.
enum DiagMotion : uint8_t {
    DM_NONE = 0, DM_BLINK, DM_WINK, DM_SACCADE, DM_EYEROLL, DM_SHAKE,
    DM_CONFUSED, DM_SURPRISE, DM_BOUNCE, DM_NOD, DM_WAVE, DM_PULSE_ALT,
    DM_YAWN, DM_WARNING,
};
struct DiagStep { uint8_t mood; uint8_t pct; uint8_t motion; };

// 16-step composite diagnostic: every form + every motion operation,
// 1 second per step. Position chain handlers continue running during diag,
// only the random schedulers are paused.
static const DiagStep DIAG_SEQ[] = {
    // pct values are deliberately spread to walk through the full halo
    // colour range (cyan ≤70 → amber 70-90 → red ≥90 → back).
    {M_NEUTRAL,    25, DM_NONE},      //  0s cyan baseline
    {M_HAPPY,      40, DM_BLINK},     //  1s cyan
    {M_CIRCLE,     55, DM_WINK},      //  2s cyan
    {M_OVAL_TALL,  68, DM_SACCADE},   //  3s warm-cyan
    {M_DIAMOND,    75, DM_EYEROLL},   //  4s entering amber
    {M_PUPIL_LEFT, 82, DM_NOD},       //  5s amber
    {M_PUPIL_SLIT, 88, DM_CONFUSED},  //  6s warm amber
    {M_ANGRY,      92, DM_SHAKE},     //  7s amber→red
    {M_NEUTRAL,    95, DM_SURPRISE},  //  8s red
    {M_UPSET,      98, DM_BOUNCE},    //  9s near peak
    {M_SAD,        96, DM_NONE},      // 10s red
    {M_CROSS,      99, DM_WARNING},   // 11s peak
    {M_NEUTRAL,    70, DM_WAVE},      // 12s amber (cooling)
    {M_NEUTRAL,    50, DM_PULSE_ALT}, // 13s cyan
    {M_NEUTRAL,    35, DM_YAWN},      // 14s cyan
    {M_NEUTRAL,    20, DM_NONE},      // 15s back to baseline
};
#define DIAG_STEPS ((int)(sizeof(DIAG_SEQ) / sizeof(DIAG_SEQ[0])))

// ─── Colour helpers ────────────────────────────────────────────────────────
static lv_color_t lerp_color(lv_color_t a, lv_color_t b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    uint8_t r = a.red   + (int)((b.red   - a.red)   * t);
    uint8_t g = a.green + (int)((b.green - a.green) * t);
    uint8_t bl= a.blue  + (int)((b.blue  - a.blue)  * t);
    return lv_color_make(r, g, bl);
}

// halo colour: stays brand-cyan up to 70 %, then warms to amber/red.
// If a fixed override is set (cyan/amber/red), bypass the % lerp.
// User-configurable colour-by-% stops. Default mirrors the old hardcoded
// cyan→amber→red breakpoints, extended with a green plateau between 50-70 %.
// Daemon ships an array via the cfg JSON blob (emo2_apply_cfg_json) and we
// persist it to NVS so it survives a reboot. Min 2 stops, max 4.
#define EMO2_MAX_COLOR_STOPS 4
struct e2_color_stop_t { uint8_t pct; uint8_t r, g, b; };
// Thresholds tuned for the typical session_pct distribution (5-hour window):
// most days the value sits in 20-60%, so the old 0/50/70/90 set locked the
// whole layout to first-stop cyan unless usage spiked. The new 0/20/50/80
// fires at least one threshold during a normal session, making "color-by-%"
// actually visible to the user.
static e2_color_stop_t color_stops[EMO2_MAX_COLOR_STOPS] = {
    { 0,  0x00, 0xE5, 0xE5 },   // bright cyan   (calm — first 0..20)
    { 20, 0x00, 0xFF, 0x66 },   // vivid green   (productive — 20..50)
    { 50, 0xFF, 0xD2, 0x00 },   // saturated yel (warning — 50..80)
    { 80, 0xFF, 0x18, 0x18 },   // pure red      (alert — 80..100)
};
static uint8_t n_color_stops = 4;

// Interpolation mode for the colour-by-pct pipeline.
//   0 = STEP   — pick the highest stop ≤ pct (default, abrupt thresholds).
//   1 = SMOOTH — linear lerp between adjacent stops (continuous gradient).
// Set via cfg JSON `gradient_mode`. Persisted to NVS alongside the stops.
static uint8_t gradient_mode = 0;

// Pure pct → step-colour lookup. Ignores BOTH halo and layout overrides
// (the caller decides whether to apply an override). Used as the building
// block for halo_color() and pct_color_layout().
static lv_color_t pct_color_stepped(float pct) {
    if (n_color_stops < 1) return EMO2_BASE;
    auto stop_color = [](const e2_color_stop_t& s) {
        return lv_color_make(s.r, s.g, s.b);
    };
    uint8_t pick = 0;
    for (uint8_t i = 0; i < n_color_stops; i++) {
        if (pct >= color_stops[i].pct) pick = i;
        else break;
    }
    return stop_color(color_stops[pick]);
}

// Linear lerp between the two stops bracketing `pct`. Below the first
// stop → first colour (clamp-low); above the last → last colour
// (clamp-high). Lerp is per-channel in 8-bit RGB — fast enough on C6
// and visually indistinguishable from gamma-aware lerp at our palette
// saturation levels.
static lv_color_t pct_color_smooth(float pct) {
    if (n_color_stops < 1) return EMO2_BASE;
    if (n_color_stops == 1)
        return lv_color_make(color_stops[0].r, color_stops[0].g, color_stops[0].b);
    if (pct <= color_stops[0].pct)
        return lv_color_make(color_stops[0].r, color_stops[0].g, color_stops[0].b);
    const e2_color_stop_t& last = color_stops[n_color_stops - 1];
    if (pct >= last.pct)
        return lv_color_make(last.r, last.g, last.b);
    // Find bracketing pair [i, i+1].
    for (uint8_t i = 0; i + 1 < n_color_stops; i++) {
        const e2_color_stop_t& a = color_stops[i];
        const e2_color_stop_t& b = color_stops[i + 1];
        if (pct >= a.pct && pct <= b.pct) {
            float span = (float)(b.pct - a.pct);
            float t = (span > 0.0f) ? ((pct - (float)a.pct) / span) : 0.0f;
            uint8_t r = (uint8_t)(a.r + (b.r - a.r) * t);
            uint8_t g = (uint8_t)(a.g + (b.g - a.g) * t);
            uint8_t bb= (uint8_t)(a.b + (b.b - a.b) * t);
            return lv_color_make(r, g, bb);
        }
    }
    return lv_color_make(last.r, last.g, last.b);   // unreachable
}

// Dispatcher — picks step or smooth based on `gradient_mode`. All call
// sites use this; the two implementations stay isolated so future
// modes (perceptual lerp, eased curves) can plug in here.
static inline lv_color_t pct_color(float pct) {
    return (gradient_mode == 1) ? pct_color_smooth(pct) : pct_color_stepped(pct);
}

// HALO colour — respects the user's halo-only override (CTRL 0x1C-0x1F)
// AND the per-state custom-hex picker (color_override == 0x80).
static lv_color_t halo_color(float pct) {
    if (color_override == 0x80) return lv_color_make(custom_halo_r, custom_halo_g, custom_halo_b);
    if (color_override == 0)    return EMO2_BASE;
    if (color_override == 1)    return EMO2_WARN;
    if (color_override == 2)    return EMO2_DANGER;
    return pct_color(pct);
}

// LAYOUT-fill colour — independent of the halo override. Only the layout-
// own override (CTRL 0x4D-0x50) shortcuts to a fixed palette entry; "auto"
// always returns the pct-driven colour so colour-by-% works even when
// the user has parked the halo on a fixed hue. New: 0x80 = custom RGB.
static lv_color_t layout_color(float pct) {
    static const lv_color_t PALETTE[3] = { EMO2_BASE, EMO2_WARN, EMO2_DANGER };
    if (layout_color_override == 0x80)
        return lv_color_make(custom_layout_r, custom_layout_g, custom_layout_b);
    if (layout_color_override <= 2) return PALETTE[layout_color_override];
    return pct_color(pct);
}

// Compute a darker "shadow" copy of the eye tint — ~35% brightness of each
// channel. Used for the halo layer which now acts as a drop-shadow under
// the silhouette to give the eye visible depth/volume.
static lv_color_t shadow_color(lv_color_t base) {
    uint32_t u32 = lv_color_to_u32(base);
    uint8_t r = (uint8_t)((u32 >> 16) & 0xFF);
    uint8_t g = (uint8_t)((u32 >>  8) & 0xFF);
    uint8_t b = (uint8_t)( u32        & 0xFF);
    return lv_color_make((uint8_t)(r * 35 / 100),
                         (uint8_t)(g * 35 / 100),
                         (uint8_t)(b * 35 / 100));
}

static void apply_colours(void) {
    // Both the silhouette AND the halo lerp with session_pct (cyan → amber →
    // red). The halo is now a DARKER variant of the same tint, used as a
    // drop-shadow beneath the silhouette (see HALO_SHADOW_DY in apply_eye_
    // offset + emo2_show). Specular stays white to preserve the 3-D bulge.
    lv_color_t tint = halo_color(eff_session_pct());
    lv_color_t shadow = shadow_color(tint);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_image_recolor    (eye_base[i], tint,      0);
        lv_obj_set_style_image_recolor_opa(eye_base[i], LV_OPA_COVER, 0);
        lv_obj_set_style_image_recolor    (eye_halo[i], shadow,    0);
        lv_obj_set_style_image_recolor_opa(eye_halo[i], LV_OPA_COVER, 0);
        lv_obj_set_style_image_recolor    (eye_spec[i], EMO2_SPEC, 0);
        lv_obj_set_style_image_recolor_opa(eye_spec[i], LV_OPA_COVER, 0);
    }
}

// ─── Mood ──────────────────────────────────────────────────────────────────
// The specular highlight is a fixed-position upper-left dot sized for the
// full eye. Thin / off-center mood shapes (HAPPY arc ⌒, SAD arc ⌣, SLEEP
// slit, UPSET tiny dot) would render the spec outside their silhouette, so
// we hide it for those moods. Full-bodied shapes keep the lens shine.
static bool mood_has_spec(uint8_t m) {
    // Spec dot sits at upper-left of the full eye; only full-canvas shapes
    // host it correctly (clipped/thin/X shapes float it outside).
    switch (m) {
    case M_NEUTRAL:
    case M_LOVE:
    case M_CIRCLE:
    case M_PUPIL_LEFT:
    case M_PUPIL_SLIT:
    case M_OVAL_TALL:
    case M_DIAMOND:
    case M_SQUIRCLE_THICK:
    case M_RECT_TV:
        return true;
    // capsule/crescent/brackets/pixel/glyph eyes have no central "lens"
    // anchor for the specular dot.
    default:
        return false;
    }
}

static void set_mood(uint8_t m) {
    cur_mood = m;
    for (int i = 0; i < 2; i++) {
        lv_image_set_src(eye_base[i], emo2_base_frames[m][i]);
        // Halo reuses the SAME silhouette as the base layer so its glow
        // physically follows the mood contour (CROSS, SLIT, BRACKETS, etc.).
        // Was: emo2_halo_frames[m][i] — pre-blurred bloom that read circular
        // for every mood.
        lv_image_set_src(eye_halo[i], emo2_base_frames[m][i]);
        if (mood_has_spec(m)) lv_obj_clear_flag(eye_spec[i], LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag  (eye_spec[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── Animation callbacks ──────────────────────────────────────────────────
static void scale_y_cb(void* obj, int32_t v) {
    lv_image_set_scale_y((lv_obj_t*)obj, v);
}
static void scale_uniform_cb(void* obj, int32_t v) {
    lv_image_set_scale((lv_obj_t*)obj, v);
}
static void scale_x_cb(void* obj, int32_t v) {
    lv_image_set_scale_x((lv_obj_t*)obj, v);
}

// Apply a (dx, dy) offset to all three layers of both eyes. Centralised so
// saccade / eye-roll / look-around / shake never fight over coordinates.
static void apply_eye_offset(int dx, int dy) {
    for (int i = 0; i < 2; i++) {
        int cx = (i == 0) ? EYE_L_CX : EYE_R_CX;
        lv_obj_set_pos(eye_base[i], cx - 32 + dx,                  EYE_CY - 32 + dy);
        // Shadow layer rides HALO_SHADOW_DY pixels lower than the base for
        // a 3D-emboss feel.
        lv_obj_set_pos(eye_halo[i], cx - HALO_SRC / 2 + dx,
                                    EYE_CY - HALO_SRC / 2 + dy + HALO_SHADOW_DY);
        lv_obj_set_pos(eye_spec[i], cx - BASE_PX / 4 - 4 + dx,     EYE_CY - BASE_PX / 3 + dy);
    }
}
static void zzz_anim_cb(void* obj, int32_t v) {
    if (!obj) return;
    lv_obj_set_style_translate_y((lv_obj_t*)obj, -v / 3, 0);
    lv_obj_set_style_opa((lv_obj_t*)obj, 255 - (v * 255 / 100), 0);
}

// Smooth blink: scale_y 1.0 → 0.1 → 1.0 over 180 ms on all three layers.
static void do_blink(void) {
    for (int i = 0; i < 2; i++) {
        lv_obj_t* targets[3] = {eye_base[i], eye_halo[i], eye_spec[i]};
        for (int k = 0; k < 3; k++) {
            lv_anim_t a; lv_anim_init(&a);
            lv_anim_set_var(&a, targets[k]);
            lv_anim_set_exec_cb(&a, scale_y_cb);
            lv_anim_set_values(&a, 256, 32);
            lv_anim_set_time(&a, 90);
            lv_anim_set_playback_time(&a, 90);
            lv_anim_start(&a);
        }
    }
}

// Heartbeat: subtle halo scale pulse (1.0× → 1.06× → 1.0×) over 600 ms.
static void do_heartbeat(void) {
    int32_t lo = HALO_SCALE;
    int32_t hi = HALO_SCALE * 106 / 100;
    for (int i = 0; i < 2; i++) {
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, eye_halo[i]);
        lv_anim_set_exec_cb(&a, scale_uniform_cb);
        lv_anim_set_values(&a, lo, hi);
        lv_anim_set_time(&a, 280);
        lv_anim_set_playback_time(&a, 280);
        lv_anim_start(&a);
    }
}

// Warning flicker — bumped to ±30 % scale + slower playback so it's
// unambiguously visible at any halo opacity. Previous ±15 % over 440 ms
// was too subtle and the user thought WRN motion didn't fire at all.
static void do_warning_flicker(void) {
    int32_t bh_lo = BASE_SCALE, bh_hi = BASE_SCALE * 130 / 100;   // +30%
    int32_t hh_lo = HALO_SCALE, hh_hi = HALO_SCALE * 170 / 100;   // +70%
    for (int i = 0; i < 2; i++) {
        lv_anim_delete(eye_halo[i], scale_uniform_cb);
        lv_anim_delete(eye_base[i], scale_uniform_cb);
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, eye_halo[i]);
        lv_anim_set_exec_cb(&a, scale_uniform_cb);
        lv_anim_set_values(&a, hh_lo, hh_hi);
        lv_anim_set_time(&a, 220);
        lv_anim_set_playback_time(&a, 420);
        lv_anim_start(&a);
        lv_anim_t b; lv_anim_init(&b);
        lv_anim_set_var(&b, eye_base[i]);
        lv_anim_set_exec_cb(&b, scale_uniform_cb);
        lv_anim_set_values(&b, bh_lo, bh_hi);
        lv_anim_set_time(&b, 220);
        lv_anim_set_playback_time(&b, 420);
        lv_anim_start(&b);
    }
}

// Wink: asymmetric blink on one eye only. ~100 ms close + 100 ms open.
static void do_wink(int idx) {
    if (idx < 0 || idx > 1) return;
    lv_obj_t* targets[3] = {eye_base[idx], eye_halo[idx], eye_spec[idx]};
    for (int k = 0; k < 3; k++) {
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, targets[k]);
        lv_anim_set_exec_cb(&a, scale_y_cb);
        lv_anim_set_values(&a, 256, 32);
        lv_anim_set_time(&a, 100);
        lv_anim_set_playback_time(&a, 100);
        lv_anim_start(&a);
    }
}

// Surprised pop: both eyes briefly scale up uniformly, like a sharp inhale.
// 1.0× → 1.15× → 1.0× over ~400 ms.
static void do_surprised(void) {
    int32_t b_lo = BASE_SCALE, b_hi = BASE_SCALE * 115 / 100;
    int32_t h_lo = HALO_SCALE, h_hi = HALO_SCALE * 115 / 100;
    for (int i = 0; i < 2; i++) {
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, eye_base[i]);
        lv_anim_set_exec_cb(&a, scale_uniform_cb);
        lv_anim_set_values(&a, b_lo, b_hi);
        lv_anim_set_time(&a, 180);
        lv_anim_set_playback_time(&a, 220);
        lv_anim_start(&a);

        lv_anim_t h; lv_anim_init(&h);
        lv_anim_set_var(&h, eye_halo[i]);
        lv_anim_set_exec_cb(&h, scale_uniform_cb);
        lv_anim_set_values(&h, h_lo, h_hi);
        lv_anim_set_time(&h, 180);
        lv_anim_set_playback_time(&h, 220);
        lv_anim_start(&h);
    }
}

// Confused: one eye scales up to 1.10× and back, while the other holds
// steady — the classic "wait, what?" asymmetric stare.
static void do_confused(int idx) {
    if (idx < 0 || idx > 1) return;
    // Bigger asymmetric pulse — was 110/108 too subtle to read as confusion.
    int32_t b_lo = BASE_SCALE, b_hi = BASE_SCALE * 118 / 100;
    int32_t h_lo = HALO_SCALE, h_hi = HALO_SCALE * 115 / 100;

    // Cancel any in-flight confused anims on this eye so a fresh trigger
    // doesn't get cut short by a previous run's playback.
    lv_anim_delete(eye_base[idx], scale_uniform_cb);
    lv_anim_delete(eye_halo[idx], scale_uniform_cb);

    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, eye_base[idx]);
    lv_anim_set_exec_cb(&a, scale_uniform_cb);
    lv_anim_set_values(&a, b_lo, b_hi);
    lv_anim_set_time(&a, 320);            // was 220 — longer forward
    lv_anim_set_playback_time(&a, 600);   // was 380 — longer recovery
    lv_anim_start(&a);

    lv_anim_t h; lv_anim_init(&h);
    lv_anim_set_var(&h, eye_halo[idx]);
    lv_anim_set_exec_cb(&h, scale_uniform_cb);
    lv_anim_set_values(&h, h_lo, h_hi);
    lv_anim_set_time(&h, 320);
    lv_anim_set_playback_time(&h, 600);
    lv_anim_start(&h);
}

// Yawn: horizontal stretch (×1.25) then collapse — runs once when entering
// sleep, before the slit-shaped SLEEP sprite takes over.
static void do_yawn(void) {
    int32_t lo = BASE_SCALE, hi = BASE_SCALE * 125 / 100;
    for (int i = 0; i < 2; i++) {
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, eye_base[i]);
        lv_anim_set_exec_cb(&a, scale_x_cb);
        lv_anim_set_values(&a, lo, hi);
        lv_anim_set_time(&a, 280);
        lv_anim_set_playback_time(&a, 320);
        lv_anim_start(&a);
    }
}

// Boot-in: eyes "wake up" from a thin horizontal line to full height.
static void do_boot_in(void) {
    for (int i = 0; i < 2; i++) {
        lv_obj_t* targets[3] = {eye_base[i], eye_halo[i], eye_spec[i]};
        for (int k = 0; k < 3; k++) {
            lv_anim_t a; lv_anim_init(&a);
            lv_anim_set_var(&a, targets[k]);
            lv_anim_set_exec_cb(&a, scale_y_cb);
            lv_anim_set_values(&a, 16, 256);
            lv_anim_set_time(&a, 380);
            lv_anim_start(&a);
        }
    }
}

// Sleep breathing: slow halo pulse + Z's drifting up.
static void start_sleep_anim(void) {
    int32_t lo = HALO_SCALE;
    int32_t hi = HALO_SCALE * 110 / 100;
    for (int i = 0; i < 2; i++) {
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, eye_halo[i]);
        lv_anim_set_exec_cb(&a, scale_uniform_cb);
        lv_anim_set_values(&a, lo, hi);
        lv_anim_set_time(&a, 2400);
        lv_anim_set_playback_time(&a, 2400);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    lv_anim_t z; lv_anim_init(&z);
    lv_anim_set_var(&z, obj_zzz);
    lv_anim_set_exec_cb(&z, zzz_anim_cb);
    lv_anim_set_values(&z, 0, 100);
    lv_anim_set_time(&z, 1800);
    lv_anim_set_repeat_count(&z, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&z, 300);
    lv_anim_start(&z);
}
static void stop_sleep_anim(void) {
    for (int i = 0; i < 2; i++) {
        lv_anim_del(eye_halo[i], scale_uniform_cb);
        lv_image_set_scale(eye_halo[i], HALO_SCALE);
    }
    lv_anim_del(obj_zzz, zzz_anim_cb);
    if (obj_zzz) {
        lv_obj_set_style_translate_y(obj_zzz, 0, 0);
        lv_obj_set_style_opa(obj_zzz, LV_OPA_COVER, 0);
    }
}

// ─── Reset-time formatting + live clock ────────────────────────────────────
// Output is just the duration (e.g. "3ч 20м") — the surrounding label
// (refresh_usage_info::fmt_into) supplies the "Сессия: …" prefix. Earlier
// versions injected STR_RESET_IN ("до сброса") here, but the user wanted
// shorter labels with `:` after the source name.
static void fmt_left(int mins, char* out, size_t cap) {
    if (mins <= 0) { snprintf(out, cap, "%s", TR(STR_DASH)); return; }
    if (mins < 60)
        snprintf(out, cap, "%d%s", mins, TR(STR_U_MIN));
    else if (mins < 1440)
        snprintf(out, cap, "%d%s %d%s",
                 mins / 60, TR(STR_U_HOUR), mins % 60, TR(STR_U_MIN));
    else
        snprintf(out, cap, "%d%s %d%s",
                 mins / 1440, TR(STR_U_DAY), (mins % 1440) / 60, TR(STR_U_HOUR));
}

static void update_clock(bool force) {
    if (!obj_clock) return;
    // No time received yet → set an empty label (instead of "--:--"). The
    // bitmap digit-only fonts don't carry '-', so the old placeholder
    // rendered as tofu squares. apply_view() also hides the label while
    // clock_base==0, but an empty string is defensive in case any other
    // path re-shows the label before time arrives.
    if (clock_base == 0) { if (force) lv_label_set_text(obj_clock, ""); return; }
    uint32_t local = clock_base + (millis() - clock_base_ms) / 1000u;
    int mod  = (int)(local % 86400u);
    int mins = mod / 60;
    int secs = mod % 60;
    // Seconds-style ticks every second; everything else only when minute
    // rolls over. Tracked via two static counters so style switches at
    // boundaries don't flicker.
    static int clock_last_sec = -1;
    bool needs_redraw = force;
    if (clock_style == CS_SECONDS) {
        if (secs != clock_last_sec) needs_redraw = true;
    } else if (mins != clock_last_min) {
        needs_redraw = true;
    }
    if (!needs_redraw) return;
    clock_last_min = mins;
    clock_last_sec = secs;
    switch (clock_style) {
    case CS_BRACKET:
        lv_label_set_text_fmt(obj_clock, "[%02d:%02d]", mins / 60, mins % 60);
        break;
    case CS_SECONDS:
        // HH:MM in the main label (large white sharetech_56), SS in the
        // separate small cyan suffix label (sharetech_36). Matches web c13.
        lv_label_set_text_fmt(obj_clock, "%02d:%02d", mins / 60, mins % 60);
        if (obj_clock_secs) lv_label_set_text_fmt(obj_clock_secs, "%02d", secs);
        break;
    default:
        lv_label_set_text_fmt(obj_clock, "%02d:%02d", mins / 60, mins % 60);
        break;
    }
}

// Push fresh label text + bar values from last_data. Visibility is handled
// separately by apply_view().
static void refresh_usage_info(void) {
    if (!obj_info_s || !obj_info_w || !obj_bar_s || !obj_bar_w) return;
    // OTA owns the screen entirely — bail before doing anything else.
    if (ota_active) {
        lv_label_set_text(obj_info_s, "");
        lv_label_set_text(obj_info_w, "");
        lv_bar_set_value(obj_bar_s, 0, LV_ANIM_OFF);
        lv_bar_set_value(obj_bar_w, 0, LV_ANIM_OFF);
        return;
    }
    // "Loading" — any state where there's no usable Claude data yet:
    // BLE off, daemon still connecting, or token expired. We don't bail
    // any more; instead we feed phase-shifted synthetic percentages into
    // the layout chrome so the bezel/columns/ribbon/pearls/chip/ecg all
    // animate as looping LOADING indicators. The numeric text overlays
    // are blanked (no real number to show) and the chip / ribbon pct
    // labels show an ellipsis instead.
    bool loading = (!connected) || token_expired || (!last_data.valid) || data_stale();
    int s_pct, w_pct;
    char left_s[28], left_w[28];
    if (loading) {
        // Triangle wave on session (2.4s round trip) + a wider, offset
        // triangle on weekly (3.0s round trip, +1.2s offset) — out-of-
        // phase so the chrome doesn't move in lock-step.
        uint32_t now = millis();
        uint32_t ps  = now % 2400;
        uint32_t pw  = (now + 1200) % 3000;
        s_pct = (ps < 1200) ? (int)(ps * 100 / 1200)
                            : (int)((2400 - ps) * 100 / 1200);
        w_pct = (pw < 1500) ? (int)(pw * 100 / 1500)
                            : (int)((3000 - pw) * 100 / 1500);
        lv_label_set_text(obj_info_s, "");
        lv_label_set_text(obj_info_w, "");
        lv_bar_set_value(obj_bar_s, s_pct, LV_ANIM_OFF);
        lv_bar_set_value(obj_bar_w, w_pct, LV_ANIM_OFF);
        // Ellipsis stub for chip / ribbon consumers below.
        snprintf(left_s, sizeof(left_s), "...");
        snprintf(left_w, sizeof(left_w), "...");
    } else {
        bool compact = (V_BODY(view_idx) == V_COMPACT);
        const char* s_lbl = compact ? TR(STR_SESSION_SHORT) : TR(STR_SESSION);
        const char* w_lbl = compact ? TR(STR_WEEK_SHORT)    : TR(STR_WEEK);
        s_pct = (int)(last_data.session_pct + 0.5f);
        w_pct = (int)(last_data.weekly_pct + 0.5f);
        fmt_left(last_data.session_reset_mins, left_s, sizeof(left_s));
        fmt_left(last_data.weekly_reset_mins,  left_w, sizeof(left_w));
        // Format per (text_source, text_format) — shortened layout:
        //   PCT       → "Сессия: 42%"
        //   RESET     → "Сессия: 3ч 20м"   (fmt_left no longer prepends
        //                                    "до сброса" — see fmt_left())
        //   PCT_RESET → "Сессия: 42% / 3ч 20м"
        auto fmt_into = [&](lv_obj_t* lbl, const char* prefix, int pct, const char* left) {
            switch (text_format) {
            case TEXT_FMT_PCT:
                lv_label_set_text_fmt(lbl, "%s: %d%%", prefix, pct); break;
            case TEXT_FMT_RESET:
                lv_label_set_text_fmt(lbl, "%s: %s",   prefix, left); break;
            case TEXT_FMT_PCT_RESET:
            default:
                lv_label_set_text_fmt(lbl, "%s: %d%% / %s", prefix, pct, left); break;
            }
        };
        bool show_s = (text_source == TEXT_SRC_SESSION) || (text_source == TEXT_SRC_BOTH);
        bool show_w = (text_source == TEXT_SRC_WEEKLY)  || (text_source == TEXT_SRC_BOTH);
        if (show_s) fmt_into(obj_info_s, s_lbl, s_pct, left_s);
        else        lv_label_set_text(obj_info_s, "");
        if (show_w) fmt_into(obj_info_w, w_lbl, w_pct, left_w);
        else        lv_label_set_text(obj_info_w, "");
        lv_bar_set_value(obj_bar_s, s_pct, LV_ANIM_ON);
        lv_bar_set_value(obj_bar_w, w_pct, LV_ANIM_ON);
    }
    // Bar accent follows the same colour as the halo.
    // Layout fill colour: by default lerps cyan→amber→red with pct.
    // If user picked a fixed layout colour (`layout_color_override`), all
    // layout fills lock to that palette entry regardless of pct.
    // Colour for the wrap-around layout (bezel / columns / ribbon / chip /
    // pearls / ecg) — UNIFIED, driven by session_pct (or the test override
    // when active). Uses layout_color() which is INDEPENDENT of the halo
    // override: even if the user parked the eye halo on a fixed colour, the
    // layout still reacts to %.
    // When loading: drive layout colour from the synthetic s_pct so the
    // chrome pulses cyan→amber→red as the loading bar sweeps. Otherwise
    // use the eff_session_pct() (real session % or test override).
    float colour_pct = loading ? (float)s_pct : eff_session_pct();
    // Clock AUTO mode (0xA0) tracks the same pct as the layout fills, so the
    // clock tints with usage in lock-step. No-op for DEFAULT/CUSTOM modes.
    apply_clock_color(colour_pct);
    lv_color_t c_layout = layout_color(colour_pct);
    // Classic-layout bars stay per-percent (two independent indicators).
    // Bars use pct_color() (mode-aware) directly so a halo override on the
    // eye colour doesn't lock the bars either.
    // A fixed layout pick (palette 0/1/2 OR custom 0x80) locks BOTH bars to
    // the layout colour; only "auto" (0xFF) leaves them per-percent. Without
    // the 0x80 branch a custom hex coloured the chrome but NOT the classic
    // bars — the visible "custom layout colour ignored on the bars" bug.
    bool layout_fixed = (layout_color_override <= 2) || (layout_color_override == 0x80);
    lv_color_t c_s = layout_fixed ? c_layout : pct_color(colour_pct);
    lv_color_t c_w = layout_fixed ? c_layout : pct_color((float)w_pct);
    lv_obj_set_style_bg_color(obj_bar_s, c_s, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj_bar_w, c_w, LV_PART_INDICATOR);
    // Bezel-orbit fills — FULL perimeter including the rounded corners.
    // 5 sub-segments per half: top_stub → corner_arc → side_edge →
    // corner_arc → bot_stub. SESSION traces right half CW from a top-centre
    // gap; WEEKLY traces left half (CCW for the arcs so the fill grows
    // along the natural travel direction).
    if (bez_s_top && bez_w_top) {
        auto seg5 = [](int f) {
            int top = f > BEZ_HORIZ_LEN ? BEZ_HORIZ_LEN : (f < 0 ? 0 : f);
            int rem = f - top;
            int arc_t = rem > BEZ_ARC_LEN ? BEZ_ARC_LEN : (rem < 0 ? 0 : rem); rem -= arc_t;
            int side  = rem > BEZ_VERT_LEN ? BEZ_VERT_LEN : (rem < 0 ? 0 : rem); rem -= side;
            int arc_b = rem > BEZ_ARC_LEN ? BEZ_ARC_LEN : (rem < 0 ? 0 : rem); rem -= arc_b;
            int bot   = rem > BEZ_HORIZ_LEN ? BEZ_HORIZ_LEN : (rem < 0 ? 0 : rem);
            return std::tuple<int,int,int,int,int>{top, arc_t, side, arc_b, bot};
        };
        int s_fill = (BEZ_PATH_LEN * s_pct) / 100;
        int w_fill = (BEZ_PATH_LEN * w_pct) / 100;
        if (s_fill < 0) s_fill = 0; if (s_fill > BEZ_PATH_LEN) s_fill = BEZ_PATH_LEN;
        if (w_fill < 0) w_fill = 0; if (w_fill > BEZ_PATH_LEN) w_fill = BEZ_PATH_LEN;
        auto [s_top, s_arc_t, s_side, s_arc_b, s_bot] = seg5(s_fill);
        auto [w_top, w_arc_t, w_side, w_arc_b, w_bot] = seg5(w_fill);

        // Pixel→degrees for corner arc fill (0..90°). Avoid integer rounding
        // dropping the last degree at saturation.
        auto px2deg = [](int px) {
            int d = (px * 90) / BEZ_ARC_LEN;
            if (px > 0 && d == 0) d = 1;     // visible if we've started filling
            if (px >= BEZ_ARC_LEN) d = 90;
            return d;
        };

        // Geometry constants.
        const int top_y     = BEZEL_INSET;
        const int bot_y     = SCR_H - BEZEL_INSET - BEZEL_THICK;
        const int right_x   = SCR_W - BEZEL_INSET - BEZEL_THICK;
        const int left_x    = BEZEL_INSET;
        const int side_y    = BEZEL_INSET + BEZEL_RADIUS;
        const int top_gap_r = SCR_W / 2 + BEZ_HALF_GAP;
        const int top_gap_l = SCR_W / 2 - BEZ_HALF_GAP;
        const int bot_corn_r = SCR_W - BEZEL_INSET - BEZEL_RADIUS;
        const int bot_corn_l = BEZEL_INSET + BEZEL_RADIUS;

        // Bezel session ↔ weekly swap (item 1). LEFT-half widgets (bez_w_*)
        // now carry SESSION fills; RIGHT-half widgets (bez_s_*) carry
        // WEEKLY. Widget names stay (renaming all NVS/relayout refs would
        // be invasive) — only the data routed into them swaps. All fill
        // colours come from c_layout (session-pct driven, item 2).

        // RIGHT half = WEEKLY fill (was Session before the swap).
        lv_obj_set_pos (bez_s_top,   top_gap_r,           top_y);
        lv_obj_set_size(bez_s_top,   w_top,               BEZEL_THICK);
        lv_obj_set_pos (bez_s_right, right_x,             side_y);
        lv_obj_set_size(bez_s_right, BEZEL_THICK,         w_side);
        lv_obj_set_pos (bez_s_bot,   bot_corn_r - w_bot,  bot_y);
        lv_obj_set_size(bez_s_bot,   w_bot,               BEZEL_THICK);
        lv_obj_set_style_bg_color(bez_s_top,   c_layout, 0);
        lv_obj_set_style_bg_color(bez_s_right, c_layout, 0);
        lv_obj_set_style_bg_color(bez_s_bot,   c_layout, 0);
        // Corner arcs — TR fills CW 270°→360°; BR fills CW 0°→90°ent (weekly).
        if (bez_s_arc_top) {
            int d = px2deg(w_arc_t);
            lv_arc_set_angles(bez_s_arc_top, 270, 270 + d);
            lv_obj_set_style_arc_color(bez_s_arc_top, c_layout, LV_PART_INDICATOR);
        }
        if (bez_s_arc_bot) {
            int d = px2deg(w_arc_b);
            lv_arc_set_angles(bez_s_arc_bot, 0, d);
            lv_obj_set_style_arc_color(bez_s_arc_bot, c_layout, LV_PART_INDICATOR);
        }

        // LEFT half = SESSION fill (was Weekly before the swap).
        // CCW arcs preserved so fills grow along the natural travel direction.
        lv_obj_set_pos (bez_w_top,   top_gap_l - s_top,   top_y);
        lv_obj_set_size(bez_w_top,   s_top,               BEZEL_THICK);
        lv_obj_set_pos (bez_w_left,  left_x,              side_y);
        lv_obj_set_size(bez_w_left,  BEZEL_THICK,         s_side);
        lv_obj_set_pos (bez_w_bot,   bot_corn_l,          bot_y);
        lv_obj_set_size(bez_w_bot,   s_bot,               BEZEL_THICK);
        lv_obj_set_style_bg_color(bez_w_top,  c_layout, 0);
        lv_obj_set_style_bg_color(bez_w_left, c_layout, 0);
        lv_obj_set_style_bg_color(bez_w_bot,  c_layout, 0);
        // TL: fills CCW from 270°→180°. Indicator start moves left, end is fixed.
        if (bez_w_arc_top) {
            int d = px2deg(s_arc_t);
            lv_arc_set_angles(bez_w_arc_top, 270 - d, 270);
            lv_obj_set_style_arc_color(bez_w_arc_top, c_layout, LV_PART_INDICATOR);
        }
        // BL: fills CCW from 180°→90°. Indicator start moves left.
        if (bez_w_arc_bot) {
            int d = px2deg(s_arc_b);
            lv_arc_set_angles(bez_w_arc_bot, 180 - d, 180);
            lv_obj_set_style_arc_color(bez_w_arc_bot, c_layout, LV_PART_INDICATOR);
        }
    }
    // Twin columns — bottom-anchored vertical fill.
    if (col_left_fill && col_right_fill) {
        int sh = (COL_HEIGHT * s_pct) / 100;
        int wh = (COL_HEIGHT * w_pct) / 100;
        if (sh < 0) sh = 0; if (sh > COL_HEIGHT) sh = COL_HEIGHT;
        if (wh < 0) wh = 0; if (wh > COL_HEIGHT) wh = COL_HEIGHT;
        lv_obj_set_size(col_left_fill,  COL_THICK, sh);
        lv_obj_set_pos (col_left_fill,  COL_INSET, COL_INSET + (COL_HEIGHT - sh));
        lv_obj_set_size(col_right_fill, COL_THICK, wh);
        lv_obj_set_pos (col_right_fill, SCR_W - COL_INSET - COL_THICK, COL_INSET + (COL_HEIGHT - wh));
        // Layout fills (item 2): both columns share the session-pct colour
        // so the column on the WEEKLY side also responds to threshold
        // crossings. Quantity is still encoded via fill HEIGHT, only the
        // hue is unified.
        lv_obj_set_style_bg_color(col_left_fill,  c_layout, 0);
        lv_obj_set_style_bg_color(col_right_fill, c_layout, 0);
    }
    // HUD ribbon — horizontal bars + pct labels.
    if (ribbon_seg_s[0] && ribbon_seg_w[0]) {
        int lit_s = s_pct / 10;            // 0..10
        int lit_w = w_pct / 10;
        if (lit_s > RIBBON_SEG_COUNT) lit_s = RIBBON_SEG_COUNT;
        if (lit_w > RIBBON_SEG_COUNT) lit_w = RIBBON_SEG_COUNT;
        for (int i = 0; i < RIBBON_SEG_COUNT; i++) {
            bool on_s = i < lit_s;
            bool on_w = i < lit_w;
            lv_obj_set_style_bg_color(ribbon_seg_s[i],
                on_s ? c_layout : lv_color_hex(0x202020), 0);
            lv_obj_set_style_bg_color(ribbon_seg_w[i],
                on_w ? c_layout : lv_color_hex(0x202020), 0);
        }
        char buf[8];
        // Loading state: don't pretend we have a number. Show ellipsis.
        if (loading) snprintf(buf, sizeof(buf), "...");
        else         snprintf(buf, sizeof(buf), "%d%%", s_pct);
        lv_label_set_text(ribbon_pct_s, buf);
        if (loading) snprintf(buf, sizeof(buf), "...");
        else         snprintf(buf, sizeof(buf), "%d%%", w_pct);
        lv_label_set_text(ribbon_pct_w, buf);
        lv_obj_set_style_text_color(ribbon_pct_s, c_layout, 0);
        lv_obj_set_style_text_color(ribbon_pct_w, c_layout, 0);
    }
    // Tear-pearls — count steps unchanged (1 pearl visible at any pct>0,
    // each +20 pct lights another). Y-position now driven by a millis-based
    // phase so drops fall continuously; size bumped to 8×10 in mk_pearl.
    if (pearl_l[0] && pearl_r[0]) {
        int n_s = (s_pct + 19) / 20;
        int n_w = (w_pct + 19) / 20;
        if (n_s > PEARLS_PER_EYE) n_s = PEARLS_PER_EYE;
        if (n_w > PEARLS_PER_EYE) n_w = PEARLS_PER_EYE;
        const int base_y = EYE_CY + BASE_PX / 2 + 6;
        const int span_y = SCR_H - 14 - base_y;           // vertical track
        // Falling animation: each pearl gets a phase offset so drops are
        // staggered. Period ≈ 1500ms scaled inversely with pct (faster at
        // high %). Pearl at slot i visible only when i < n_s/w.
        uint32_t now = millis();
        uint32_t period = (uint32_t)(1500 - (s_pct * 8));  // 1500→700ms across 0..100
        if (period < 600) period = 600;
        for (int i = 0; i < PEARLS_PER_EYE; i++) {
            float ph = (float)((now + i * (period / PEARLS_PER_EYE)) % period) / (float)period;
            int dy = (int)(span_y * ph);
            int y_l = base_y + dy;
            // Right eye uses its own period (weekly_pct based) so the two
            // eyes drip at independent rates — emphasises that L=session,
            // R=weekly.
            uint32_t period_w = (uint32_t)(1500 - (w_pct * 8));
            if (period_w < 600) period_w = 600;
            float ph_w = (float)((now + i * (period_w / PEARLS_PER_EYE)) % period_w) / (float)period_w;
            int y_r = base_y + (int)(span_y * ph_w);
            lv_obj_set_pos(pearl_l[i], EYE_L_CX - 4, y_l);
            lv_obj_set_pos(pearl_r[i], EYE_R_CX - 4, y_r);
            // Both pearl streams share the session-pct colour (item 2);
            // weekly_pct rarely crosses the warning step on its own.
            lv_obj_set_style_bg_color(pearl_l[i], c_layout, 0);
            lv_obj_set_style_bg_color(pearl_r[i], c_layout, 0);
            // Fade at the tail of the fall so drops vanish near the bottom.
            uint8_t alpha_l = i < n_s ? (uint8_t)(255 * (1.0f - ph * 0.5f)) : 0;
            uint8_t alpha_r = i < n_w ? (uint8_t)(255 * (1.0f - ph_w * 0.5f)) : 0;
            lv_obj_set_style_bg_opa(pearl_l[i], alpha_l, 0);
            lv_obj_set_style_bg_opa(pearl_r[i], alpha_r, 0);
        }
    }
    // Corner chips — LEFT=session, RIGHT=weekly. text_format controls:
    //   PCT       → "42%"               (one line, no fill bar)
    //   RESET     → "3ч 20м"             (one line, no fill bar)
    //   PCT_RESET → "3ч 20м" + bottom-up coloured rim showing % via
    //               chip_fill_l/r border height (% NOT in the text).
    bool chip_pct_reset = (text_format == TEXT_FMT_PCT_RESET);
    auto fmt_chip = [&](char* buf, size_t cap, int pct, const char* left) {
        switch (text_format) {
        case TEXT_FMT_PCT:
            snprintf(buf, cap, "%d%%", pct); break;
        case TEXT_FMT_RESET:
            snprintf(buf, cap, "%s", left);  break;
        case TEXT_FMT_PCT_RESET:
        default:
            // pct is rendered as the partial border via chip_fill_l/r;
            // chip body shows just the reset time.
            snprintf(buf, cap, "%s", left); break;
        }
    };
    if (chip_big_l) {     // LEFT = session
        char buf[24];
        if (loading) snprintf(buf, sizeof(buf), "...");
        else         fmt_chip(buf, sizeof(buf), s_pct, left_s);
        lv_label_set_text(chip_big_l, buf);
        lv_obj_set_style_text_color(chip_big_l, c_layout, 0);
    }
    if (chip_big_r) {     // RIGHT = weekly
        char buf[24];
        if (loading) snprintf(buf, sizeof(buf), "...");
        else         fmt_chip(buf, sizeof(buf), w_pct, left_w);
        lv_label_set_text(chip_big_r, buf);
        lv_obj_set_style_text_color(chip_big_r, c_layout, 0);
    }
    // % fill bar (only for PCT_RESET format) — bottom-up coloured border
    // partially covering the chip's dim outline. Height = pct * CHIP_H / 100.
    if (chip_fill_l) {
        if (chip_pct_reset && stats_layout == LAYOUT_CORNER_CHIP) {
            int fh = (CHIP_H * s_pct) / 100;
            if (fh < 4) fh = 4;                  // tiny seed visible at low pct
            if (fh > CHIP_H) fh = CHIP_H;
            lv_obj_set_size(chip_fill_l, CHIP_W, fh);
            lv_obj_set_style_border_color(chip_fill_l, c_layout, 0);
            lv_obj_clear_flag(chip_fill_l, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(chip_fill_l, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (chip_fill_r) {
        if (chip_pct_reset && stats_layout == LAYOUT_CORNER_CHIP) {
            int fh = (CHIP_H * w_pct) / 100;
            if (fh < 4) fh = 4;
            if (fh > CHIP_H) fh = CHIP_H;
            lv_obj_set_size(chip_fill_r, CHIP_W, fh);
            lv_obj_set_style_border_color(chip_fill_r, c_layout, 0);
            lv_obj_clear_flag(chip_fill_r, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(chip_fill_r, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // ECG waveform line — same unified layout colour as the other wraps.
    if (ecg_line) {
        lv_obj_set_style_line_color(ecg_line, c_layout, 0);
    }
}

// Show/hide widgets for the active view mode.
static void apply_view(void) {
    if (!obj_info_s) return;
    uint8_t body = V_BODY(view_idx);
    bool text = (body == V_TEXT || body == V_TEXT_GRAD || body == V_COMPACT);
    bool bars = (body == V_GRAD || body == V_TEXT_GRAD || body == V_COMPACT);
    // Clock visibility: the picker is the SOLE source of truth. CS_OFF
    // hides; any other style shows it immediately — no need to cycle the
    // V_CLOCK view-mode via BOOT first.
    // Extra guard: hide while `clock_base == 0` (no time received from
    // daemon yet). The native bitmap clock fonts only carry digits + ':' +
    // '[' + ']' — they have NO '-' glyph, so the placeholder "--:--"
    // rendered as tofu squares during the connecting state. Hiding is
    // cleaner than rendering "00:00" (which would suggest a fake time).
    bool clk  = (clock_style != CS_OFF) && (clock_base != 0);
    // Status overlay (obj_status) sits at y≈212 and the weekly label at y=208.
    // When the overlay is showing meaningful text (reconnect / re-auth / OTA),
    // hide the info + bars to avoid an unreadable mash-up. OTA additionally
    // hides the clock — the screen becomes a dedicated progress display.
    bool status_overlay = (!connected) || token_expired || ota_active;
    if (status_overlay) { text = false; bars = false; }
    if (ota_active)     { clk  = false; }
    // Stats-layout choice trumps view_idx for text + bars (item: combine
    // text+bars and layout instead of having both fight for screen space).
    //   LAYOUT_CLASSIC → always show text + bars (regardless of view_idx)
    //   LAYOUT_OFF     → respect view_idx (legacy behaviour)
    //   BEZEL/COLUMNS/RIBBON/TEAR_PEARLS/ECG → respect text_source (the user
    //                                          picks per-layout overlay text)
    //   CHIP, BROWS → no obj_info_* overlay (chip has its own labels)
    bool layout_supports_overlay_text = (
        stats_layout == LAYOUT_BEZEL ||
        stats_layout == LAYOUT_COLUMNS ||
        stats_layout == LAYOUT_RIBBON ||
        stats_layout == LAYOUT_TEAR_PEARLS ||
        stats_layout == LAYOUT_ECG);
    if (!status_overlay) {
        if (stats_layout == LAYOUT_CLASSIC)         { text = true; bars = true; }
        else if (layout_supports_overlay_text)      { text = true; bars = false; }
        else if (stats_layout != LAYOUT_OFF)        { text = false; bars = false; }
    }
    // Legacy: TEXT_MODE_NONE force-hides. New: text_source==OFF too.
    if (text_mode == TEXT_MODE_NONE || text_source == TEXT_SRC_OFF) text = false;
    // Per-source label visibility: SESSION ⇒ obj_info_s only;
    //                              WEEKLY  ⇒ obj_info_w only;
    //                              BOTH    ⇒ both (default);
    //                              OFF     ⇒ neither (`text` already false).
    bool show_text_s = text && (text_source == TEXT_SRC_SESSION ||
                                text_source == TEXT_SRC_BOTH);
    bool show_text_w = text && (text_source == TEXT_SRC_WEEKLY  ||
                                text_source == TEXT_SRC_BOTH);

    auto setvis = [](lv_obj_t* o, bool show) {
        if (!o) return;
        if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    setvis(obj_info_s, show_text_s);
    setvis(obj_info_w, show_text_w);
    setvis(obj_bar_s,  bars);
    setvis(obj_bar_w,  bars);
    setvis(obj_clock,       clk);
    setvis(obj_clock_secs,  clk && clock_style == CS_SECONDS);
    // % stats layout — show one of {bezel, columns, ribbon} chrome based on
    // the daemon-pushed pick. Stays visible during connecting / ble_off /
    // token_expired so the chrome can animate as a LOOPING LOADING
    // INDICATOR (refresh_usage_info feeds synthetic phase-shifted % into
    // it). Only OTA hides everything — OTA owns the screen.
    bool show_bezel = (stats_layout == LAYOUT_BEZEL)       && !ota_active;
    bool show_cols  = (stats_layout == LAYOUT_COLUMNS)     && !ota_active;
    bool show_rib   = (stats_layout == LAYOUT_RIBBON)      && !ota_active;
    // LAYOUT_BROWS deprecated — kept in enum for CTRL-byte stability but no
    // longer renders. Selecting it is a visual no-op.
    bool show_pearl = (stats_layout == LAYOUT_TEAR_PEARLS) && !ota_active;
    bool show_chip  = (stats_layout == LAYOUT_CORNER_CHIP) && !ota_active;
    bool show_ecg   = (stats_layout == LAYOUT_ECG)         && !ota_active;
    setvis(bezel_outline, show_bezel);
    setvis(bez_s_top,   show_bezel);
    setvis(bez_s_right, show_bezel);
    setvis(bez_s_bot,   show_bezel);
    setvis(bez_s_arc_top, show_bezel);
    setvis(bez_s_arc_bot, show_bezel);
    setvis(bez_w_top,   show_bezel);
    setvis(bez_w_left,  show_bezel);
    setvis(bez_w_bot,   show_bezel);
    setvis(bez_w_arc_top, show_bezel);
    setvis(bez_w_arc_bot, show_bezel);
    setvis(col_left_track,  show_cols);
    setvis(col_left_fill,   show_cols);
    setvis(col_right_track, show_cols);
    setvis(col_right_fill,  show_cols);
    setvis(ribbon_lbl_s,    show_rib);
    setvis(ribbon_lbl_w,    show_rib);
    setvis(ribbon_pct_s,    show_rib);
    setvis(ribbon_pct_w,    show_rib);
    for (int i = 0; i < RIBBON_SEG_COUNT; i++) {
        setvis(ribbon_seg_s[i], show_rib);
        setvis(ribbon_seg_w[i], show_rib);
    }
    for (int i = 0; i < PEARLS_PER_EYE; i++) {
        setvis(pearl_l[i], show_pearl);
        setvis(pearl_r[i], show_pearl);
    }
    // Chip pair: per-corner visibility gated by both `show_chip` (active
    // layout is CORNER_CHIP) AND text_source (which source(s) to display).
    // LEFT chip = session; RIGHT chip = weekly (swapped from earlier
    // version per user request).
    bool chip_l_on = show_chip && (text_source == TEXT_SRC_SESSION ||
                                   text_source == TEXT_SRC_BOTH);
    bool chip_r_on = show_chip && (text_source == TEXT_SRC_WEEKLY  ||
                                   text_source == TEXT_SRC_BOTH);
    setvis(chip_frame_l, chip_l_on);
    setvis(chip_frame_r, chip_r_on);
    // chip_big_l/r and chip_tag_l/r are children → inherit visibility.
    setvis(ecg_line,        show_ecg);
    // Compact mode uses a smaller font so caption + bar pair fits.
    bool compact = (body == V_COMPACT);
    const lv_font_t* f = compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    lv_obj_set_style_text_font(obj_info_s, f, 0);
    lv_obj_set_style_text_font(obj_info_w, f, 0);
    refresh_usage_info();
    if (clk) update_clock(true);
}

// ─── Public API ───────────────────────────────────────────────────────────
void emo2_init(void) {
    // Try to restore per-state config from NVS before any state-machine
    // tick fires. If NVS empty / different size / bad magic — fall back
    // to baked-in defaults and wait for daemon to push a fresh blob.
    emo2_load_cfg_from_nvs();

    container = lv_obj_create(lv_screen_active());
    lv_obj_set_size(container, SCR_W, SCR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        int cx = (i == 0) ? EYE_L_CX : EYE_R_CX;

        eye_halo[i] = lv_image_create(container);
        // Halo repurposed as a drop-shadow under the silhouette. Same source
        // sprite (silhouette traces mood contour), SAME scale as base (no
        // outer glow that was reading as stairstepped fuzz on AMOLED), but
        // sits HALO_SHADOW_DY pixels lower with a darker tint (shadow_color
        // in apply_colours) and HALO_OPA transparency. Effect: subtle 3D
        // emboss / lift, no outer ring.
        lv_image_set_src(eye_halo[i], emo2_base_frames[M_NEUTRAL][i]);
        lv_image_set_pivot(eye_halo[i], HALO_SRC / 2, HALO_SRC / 2);
        lv_image_set_scale(eye_halo[i], HALO_SCALE);
        lv_obj_set_pos(eye_halo[i],
                       cx - HALO_SRC / 2,
                       EYE_CY - HALO_SRC / 2 + HALO_SHADOW_DY);
        lv_obj_set_style_opa(eye_halo[i], HALO_OPA, 0);

        eye_base[i] = lv_image_create(container);
        lv_image_set_src(eye_base[i], emo2_base_frames[M_NEUTRAL][i]);
        lv_image_set_pivot(eye_base[i], 32, 32);
        lv_image_set_scale(eye_base[i], BASE_SCALE);
        lv_obj_set_pos(eye_base[i], cx - 32, EYE_CY - 32);

        eye_spec[i] = lv_image_create(container);
        lv_image_set_src(eye_spec[i], &emo_img_spec);
        lv_image_set_pivot(eye_spec[i], 7, 5);
        lv_image_set_scale(eye_spec[i], SPEC_SCALE);
        // Place near upper-left of the base eye — classic lens shine.
        lv_obj_set_pos(eye_spec[i], cx - BASE_PX / 4 - 4, EYE_CY - BASE_PX / 3);
        lv_obj_set_style_opa(eye_spec[i], SPEC_OPA, 0);
    }

    obj_zzz = lv_label_create(container);
    lv_label_set_text(obj_zzz, "z  z  z");
    lv_obj_set_style_text_color(obj_zzz, EMO2_BASE, 0);
    lv_obj_set_style_text_font (obj_zzz, &lv_font_montserrat_16, 0);
    lv_obj_align(obj_zzz, LV_ALIGN_TOP_RIGHT, -28, 32);
    lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);

    obj_status = lv_label_create(container);
    lv_label_set_text(obj_status, "");
    lv_obj_set_style_text_color(obj_status, lv_color_hex(0xAAAAAA), 0);
    // Cyrillic-capable font — "Переподключение…" would tofu in built-in 10pt.
    lv_obj_set_style_text_font (obj_status, &lv_font_montserrat_16, 0);
    position_status_label();

    // Session / weekly info labels (text view modes).
    obj_info_s = lv_label_create(container);
    obj_info_w = lv_label_create(container);
    lv_obj_set_style_text_color(obj_info_s, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_color(obj_info_w, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font (obj_info_s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_font (obj_info_w, &lv_font_montserrat_14, 0);
    lv_obj_align(obj_info_s, LV_ALIGN_TOP_MID, 0, info_s_y(effective_text_placement()));
    lv_obj_align(obj_info_w, LV_ALIGN_TOP_MID, 0, info_w_y(effective_text_placement()));
    lv_obj_add_flag(obj_info_s, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(obj_info_w, LV_OBJ_FLAG_HIDDEN);

    // Gradient/bars (bar view modes).
    obj_bar_s = lv_bar_create(container);
    obj_bar_w = lv_bar_create(container);
    lv_obj_set_size(obj_bar_s, BAR_W, BAR_H);
    lv_obj_set_size(obj_bar_w, BAR_W, BAR_H);
    lv_obj_align(obj_bar_s, LV_ALIGN_TOP_MID, 0, bar_s_y(effective_text_placement()));
    lv_obj_align(obj_bar_w, LV_ALIGN_TOP_MID, 0, bar_w_y(effective_text_placement()));
    lv_bar_set_range(obj_bar_s, 0, 100);
    lv_bar_set_range(obj_bar_w, 0, 100);
    lv_obj_set_style_bg_color(obj_bar_s, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj_bar_w, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_radius(obj_bar_s, BAR_H/2, LV_PART_MAIN);
    lv_obj_set_style_radius(obj_bar_w, BAR_H/2, LV_PART_MAIN);
    lv_obj_set_style_radius(obj_bar_s, BAR_H/2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(obj_bar_w, BAR_H/2, LV_PART_INDICATOR);
    lv_obj_add_flag(obj_bar_s, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(obj_bar_w, LV_OBJ_FLAG_HIDDEN);

    // ─── Bezel-orbit % layout ──────────────────────────────────────────────
    // One rounded-rect outline as the dim track. Border-only styling so the
    // centre is transparent and the eyes show through.
    bezel_outline = lv_obj_create(container);
    lv_obj_remove_style_all(bezel_outline);
    lv_obj_set_size(bezel_outline, SCR_W - 2 * BEZEL_INSET, SCR_H - 2 * BEZEL_INSET);
    lv_obj_align(bezel_outline, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(bezel_outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bezel_outline, BEZEL_THICK, 0);
    // Brighter dim track so the unfilled portion reads clearly as "the rest
    // of the gauge" rather than fading into black (was 0x202020 — too dim).
    lv_obj_set_style_border_color(bezel_outline, lv_color_hex(0x404040), 0);
    lv_obj_set_style_border_opa(bezel_outline, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bezel_outline, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_radius(bezel_outline, BEZEL_RADIUS, 0);
    lv_obj_add_flag(bezel_outline, LV_OBJ_FLAG_HIDDEN);

    // 6 straight rect-fills (3 per half — top stub, side edge, bottom stub)
    // + 4 corner ARCS (one per rounded corner). Positioned + sized each
    // frame by refresh_usage_info(). The arcs round out the perimeter so
    // the fill traces the FULL outline shape — no gaps at the corners.
    auto mk_bez = [](lv_obj_t* parent) {
        lv_obj_t* o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        // Square ends (radius 0) so straight stubs butt flush against the
        // corner arcs — rounded ends pulled the fill inward and exposed the
        // dark track at each stub↔arc junction (the "line↔corner gap").
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_size(o, 0, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    };
    bez_s_top   = mk_bez(container);
    bez_s_right = mk_bez(container);
    bez_s_bot   = mk_bez(container);
    bez_w_top   = mk_bez(container);
    bez_w_left  = mk_bez(container);
    bez_w_bot   = mk_bez(container);

    // Corner arcs. Each lives in a (2R × 2R) bounding box positioned so
    // its centre sits on the bezel outline's corner curve centre. Width =
    // BEZEL_THICK matches the straight stubs.
    auto mk_arc = [](int corner_x, int corner_y, uint16_t bg_start, uint16_t bg_end) {
        lv_obj_t* a = lv_arc_create(container);
        lv_obj_remove_style_all(a);
        lv_obj_set_size(a, 2 * BEZEL_RADIUS, 2 * BEZEL_RADIUS);
        lv_obj_set_pos(a, corner_x - BEZEL_RADIUS, corner_y - BEZEL_RADIUS);
        lv_arc_set_bg_angles(a, bg_start, bg_end);
        lv_arc_set_angles   (a, bg_start, bg_start);   // empty initially
        lv_obj_set_style_arc_width(a, BEZEL_THICK, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, BEZEL_THICK, LV_PART_INDICATOR);
        // Hide the background track — the bezel_outline already provides it.
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        // Knob hidden — this is a static fill widget, not interactive.
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        return a;
    };
    const int TR_X = SCR_W - BEZEL_INSET - BEZEL_RADIUS;
    const int TL_X = BEZEL_INSET + BEZEL_RADIUS;
    const int T_Y  = BEZEL_INSET + BEZEL_RADIUS;
    const int B_Y  = SCR_H - BEZEL_INSET - BEZEL_RADIUS;
    bez_s_arc_top = mk_arc(TR_X, T_Y, 270, 360);   // TR — session CW
    bez_s_arc_bot = mk_arc(TR_X, B_Y, 0,   90);    // BR — session CW
    bez_w_arc_top = mk_arc(TL_X, T_Y, 180, 270);   // TL — weekly CCW
    bez_w_arc_bot = mk_arc(TL_X, B_Y, 90,  180);   // BL — weekly CCW

    // ─── Twin-columns layout ───────────────────────────────────────────────
    // Helper for dim track rectangles (columns + ribbon use this).
    auto mk_track = [](int x, int y, int w, int h) {
        lv_obj_t* o = lv_obj_create(container);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, w, h);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x202020), 0);
        lv_obj_set_style_radius(o, h < w ? h / 2 : w / 2, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    };
    col_left_track  = mk_track(COL_INSET, COL_INSET, COL_THICK, COL_HEIGHT);
    col_right_track = mk_track(SCR_W - COL_INSET - COL_THICK, COL_INSET, COL_THICK, COL_HEIGHT);
    auto mk_col_fill = []() {
        lv_obj_t* o = lv_obj_create(container);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, COL_THICK / 2, 0);
        lv_obj_set_size(o, COL_THICK, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    };
    col_left_fill  = mk_col_fill();
    col_right_fill = mk_col_fill();
    // Fills are positioned each frame from the bottom of the track upward.

    // ─── HUD-ribbon layout ─────────────────────────────────────────────────
    auto mk_ribbon_lbl = [](const char* txt, int x, int y, lv_color_t col) {
        lv_obj_t* l = lv_label_create(container);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, col, 0);
        lv_obj_set_pos(l, x, y);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return l;
    };
    ribbon_lbl_s = mk_ribbon_lbl("S",  COL_INSET + 6, RIBBON_Y_S - 2, lv_color_hex(0xCFCFCF));
    ribbon_lbl_w = mk_ribbon_lbl("W",  COL_INSET + 6, RIBBON_Y_W - 2, lv_color_hex(0x9F9F9F));
    ribbon_pct_s = mk_ribbon_lbl("0%", SCR_W - 32,    RIBBON_Y_S - 2, lv_color_hex(0xCFCFCF));
    ribbon_pct_w = mk_ribbon_lbl("0%", SCR_W - 32,    RIBBON_Y_W - 2, lv_color_hex(0x9F9F9F));
    // Ribbon segments — 10 small lv_obj per row, each 12×10. Lit segments
    // are coloured + fully opaque; dim segments use the same dim-grey track
    // pattern as the bezel outline so the meter feels analogue.
    auto mk_seg = [](int x, int y) {
        lv_obj_t* o = lv_obj_create(container);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, RIBBON_SEG_W, RIBBON_SEG_H);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x202020), 0);
        lv_obj_set_style_radius(o, 2, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    };
    for (int i = 0; i < RIBBON_SEG_COUNT; i++) {
        int sx = RIBBON_BAR_X + i * (RIBBON_SEG_W + RIBBON_SEG_GAP);
        ribbon_seg_s[i] = mk_seg(sx, RIBBON_Y_S);
        ribbon_seg_w[i] = mk_seg(sx, RIBBON_Y_W);
    }

    // ─── Tear-pearls layout ────────────────────────────────────────────────
    auto mk_pearl = [](int cx) {
        lv_obj_t* o = lv_obj_create(container);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, 8, 10);    // (was 5×7) bigger drops
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, 4, 0);
        // Position updated each frame by refresh_usage_info + emo2_tick.
        lv_obj_set_pos(o, cx - 4, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    };
    int lx = EYE_L_CX, rx = EYE_R_CX;
    for (int i = 0; i < PEARLS_PER_EYE; i++) {
        pearl_l[i] = mk_pearl(lx);
        pearl_r[i] = mk_pearl(rx);
    }

    // ─── Corner-chip layout ────────────────────────────────────────────────
    // Two framed badges in the bottom corners (left=weekly, right=session).
    // Each chip carries one big "% number" plus a tiny corner-tag glyph
    // (S or W) so the user can see which value at a glance.
    auto mk_chip_frame = [](bool right) {
        lv_obj_t* fr = lv_obj_create(container);
        lv_obj_remove_style_all(fr);
        lv_obj_set_size(fr, CHIP_W, CHIP_H);
        lv_obj_align(fr,
                     right ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT,
                     right ? -CHIP_PAD_X : CHIP_PAD_X,
                     -CHIP_PAD_B);
        lv_obj_set_style_bg_opa(fr, LV_OPA_30, 0);
        lv_obj_set_style_bg_color(fr, lv_color_hex(0x0A0A0A), 0);
        lv_obj_set_style_border_width(fr, 1, 0);
        lv_obj_set_style_border_opa(fr, LV_OPA_70, 0);
        lv_obj_set_style_border_color(fr, lv_color_hex(0x404040), 0);
        lv_obj_set_style_radius(fr, 10, 0);
        lv_obj_set_style_pad_all(fr, 0, 0);
        lv_obj_clear_flag(fr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(fr, LV_OBJ_FLAG_HIDDEN);
        return fr;
    };
    chip_frame_r = mk_chip_frame(true);
    chip_frame_l = mk_chip_frame(false);

    // % "fill" overlay — a bottom-anchored child of the chip frame that
    // shows the active threshold colour as a partial border from bottom
    // up to `pct * CHIP_H / 100`. Frame's own dim border still covers
    // the un-filled top portion. Hidden unless text_format == PCT_RESET.
    auto mk_chip_fill = [](lv_obj_t* parent) {
        lv_obj_t* f = lv_obj_create(parent);
        lv_obj_remove_style_all(f);
        lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(f, 2, 0);
        lv_obj_set_style_border_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(f, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_radius(f, 10, 0);
        lv_obj_set_style_pad_all(f, 0, 0);
        lv_obj_set_size(f, CHIP_W, 0);                      // height set per frame
        lv_obj_align(f, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(f, LV_OBJ_FLAG_HIDDEN);
        return f;
    };
    chip_fill_l = mk_chip_fill(chip_frame_l);
    chip_fill_r = mk_chip_fill(chip_frame_r);

    auto mk_chip_big = [](lv_obj_t* parent) {
        lv_obj_t* l = lv_label_create(parent);
        // montserrat_16 keeps the chip body compact enough to fit two lines
        // ("%\nreset-time") for the PCT_RESET text_format. Bumped a tick
        // down from the prior 20 px so the second line clears the tag.
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8E8E8), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, CHIP_W - 12);
        lv_label_set_text(l, "0%");
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -6);
        return l;
    };
    chip_big_l = mk_chip_big(chip_frame_l);
    chip_big_r = mk_chip_big(chip_frame_r);

    // chip_tag — full i18n label ("Сессия" / "Неделя"). Cyrillic fits in
    // the Cyrillic-built lv_font_montserrat_14 (~9px chars × 7 glyphs).
    auto mk_chip_tag = [](lv_obj_t* parent, const char* glyph) {
        lv_obj_t* l = lv_label_create(parent);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x8F8F8F), 0);
        lv_label_set_text(l, glyph);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 4);
        return l;
    };
    chip_tag_l = mk_chip_tag(chip_frame_l, TR(STR_SESSION));
    chip_tag_r = mk_chip_tag(chip_frame_r, TR(STR_WEEK));

    // ─── ECG monitor layout ────────────────────────────────────────────────
    for (int i = 0; i < ECG_POINTS; i++) {
        ecg_pts[i].x = ECG_X0 + ((ECG_X1 - ECG_X0) * i) / (ECG_POINTS - 1);
        ecg_pts[i].y = ECG_Y_MID;
    }
    ecg_line = lv_line_create(container);
    lv_obj_set_style_line_width(ecg_line, 2, 0);
    lv_obj_set_style_line_rounded(ecg_line, true, 0);
    lv_obj_set_style_line_color(ecg_line, lv_color_hex(0x3DE0E0), 0);
    lv_line_set_points(ecg_line, ecg_pts, ECG_POINTS);
    lv_obj_add_flag(ecg_line, LV_OBJ_FLAG_HIDDEN);

    // Top clock — primary HH:MM (every style except OFF uses this label).
    obj_clock = lv_label_create(container);
    lv_label_set_text(obj_clock, "--:--");

    // Seconds-suffix label — only shown when clock_style == CS_SECONDS.
    // Smaller font + cyan to match the c13 web reference (HH:MM white
    // followed by a smaller cyan SS suffix). For all other styles the
    // label stays hidden and adds zero visual cost.
    // Must be created BEFORE apply_clock_style(): that function only aligns
    // this label when it already exists, so if the NVS-restored style is
    // CS_SECONDS the suffix would otherwise stay at (0,0) / top-left.
    obj_clock_secs = lv_label_create(container);
    lv_label_set_text(obj_clock_secs, "00");
    lv_obj_set_style_text_font (obj_clock_secs, &lv_font_clock_sharetech_36, 0);
    lv_obj_set_style_text_color(obj_clock_secs, EMO2_BASE, 0);
    lv_obj_add_flag(obj_clock_secs, LV_OBJ_FLAG_HIDDEN);

    // Style applied from the user's clock_style picker (NVS-restored).
    apply_clock_style();
    lv_obj_add_flag(obj_clock, LV_OBJ_FLAG_HIDDEN);

    apply_colours();
    apply_view();   // initial visibility matches view_idx=0 (eyes-only)
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

void emo2_show(void) {
    if (!container) return;
    active = true;
    lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
    if (connected) {
        do_boot_in();
        uint32_t now = millis();
        blink_next_ms     = now + 1500;
        mood_next_ms      = now + 60000 + (esp_random() % 120000);
        saccade_next_ms   = now + 1800;
        curious_next_ms   = now + 30000 + (esp_random() % 30000);
        heartbeat_next_ms = now + 2500;
        warning_next_ms   = now + 6000;
        wink_next_ms      = now + 15000 + (esp_random() % 15000);
        eyeroll_next_ms   = now + 45000 + (esp_random() % 45000);
        lookaround_next_ms= now + 35000 + (esp_random() % 25000);
        nod_next_ms       = now + 90000 + (esp_random() % 60000);
        confused_next_ms  = now + 110000 + (esp_random() % 70000);
        wave_next_ms      = now + 75000 + (esp_random() % 60000);
        pulse_alt_next_ms = now + 95000 + (esp_random() % 70000);
        eyeroll_start_ms = lookaround_start_ms = shake_start_ms = 0;
        bounce_start_ms = nod_start_ms = 0;
        wave_start_ms = pulse_alt_start_ms = 0;
    }
}

void emo2_hide(void) {
    if (!container) return;
    active = false;
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    // Cancel pending animations so a re-show starts cleanly.
    for (int i = 0; i < 2; i++) {
        lv_anim_del(eye_base[i], scale_y_cb);
        lv_anim_del(eye_base[i], scale_x_cb);
        lv_anim_del(eye_base[i], scale_uniform_cb);
        lv_anim_del(eye_halo[i], scale_y_cb);
        lv_anim_del(eye_halo[i], scale_uniform_cb);
        lv_anim_del(eye_spec[i], scale_y_cb);
        // Restore default scales so a yawn/surprise mid-flight doesn't stick.
        lv_image_set_scale(eye_base[i], BASE_SCALE);
        lv_image_set_scale(eye_halo[i], HALO_SCALE);
        lv_image_set_scale(eye_spec[i], SPEC_SCALE);
    }
    lv_anim_del(obj_zzz, zzz_anim_cb);
    eyeroll_start_ms = lookaround_start_ms = shake_start_ms = 0;
    bounce_start_ms = nod_start_ms = 0;
    wave_start_ms = pulse_alt_start_ms = 0;
    diag_active = false; diag_step = -1;
}

bool emo2_is_active(void) { return active; }

// Fire the motion op for the current diag step. Each maps to either an
// LVGL animation trigger (blink/wink/confused/surprised/yawn/warning) or
// a position-chain state variable (eyeroll/shake/bounce/nod/wave/pulse_alt
// run by the normal motion handlers later in emo2_tick).
static void diag_apply_motion(uint8_t op, uint32_t now) {
    switch (op) {
    case DM_BLINK:     do_blink();                                break;
    case DM_WINK:      do_wink((int)(esp_random() & 1));          break;
    case DM_SACCADE:   saccade_next_ms = now;                     break;
    case DM_EYEROLL:   eyeroll_start_ms = now;                    break;
    case DM_SHAKE:     shake_start_ms = now;                      break;
    case DM_CONFUSED:  do_confused((int)(esp_random() & 1));      break;
    case DM_SURPRISE:  do_surprised();                            break;
    case DM_BOUNCE:    bounce_start_ms = now;                     break;
    case DM_NOD:       nod_start_ms = now;                        break;
    case DM_WAVE:      wave_start_ms = now;                       break;
    case DM_PULSE_ALT: pulse_alt_start_ms = now;                  break;
    case DM_YAWN:      do_yawn();                                 break;
    case DM_WARNING:   do_warning_flicker();                      break;
    default: break;
    }
}

void emo2_run_diagnostics(void) {
    if (!container) return;
    // Make sure emo2 is visible & not stuck in a special state.
    if (!active) {
        // Show emo2 indirectly — ui_show_screen() is the right path, but
        // we don't include ui.h here. Set our flags so emo2_show() works
        // even if active hadn't been re-set.
        // Caller is expected to ui_show_screen(SCREEN_EMO2) first.
    }
    if (sleeping) {
        sleeping = false;
        stop_sleep_anim();
        if (obj_zzz) lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
    }
    diag_active = true;
    diag_start_ms = millis();
    diag_step = -1;
}

uint8_t emo2_get_view(void) { return view_idx; }

void emo2_set_view(uint8_t idx) {
    view_idx = idx % EMO2_VIEW_COUNT;
    if (container) apply_view();
}

void emo2_next_view(void) {
    view_idx = (view_idx + 1) % EMO2_VIEW_COUNT;
    if (container) apply_view();
}

void emo2_set_mood_idx(uint8_t idx) {
    if (!container || idx >= M_COUNT) return;
    // Drop sleep visuals if needed — explicit override.
    if (sleeping) {
        sleeping = false;
        stop_sleep_anim();
        if (obj_zzz) lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
    }
    set_mood(idx);
    // Reset the auto-rotation clock so the choice sticks for a while.
    mood_next_ms = millis() + 60000 + (esp_random() % 120000);
}

void emo2_trigger_op(uint8_t op) {
    if (!container) return;
    uint32_t now = millis();
    switch (op) {
    case DM_BLINK:     do_blink();                              break;
    case DM_WINK:      do_wink((int)(esp_random() & 1));        break;
    case DM_SACCADE: {
        int dx = ((int)(esp_random() % 5)) - 2;
        int dy = ((int)(esp_random() % 3)) - 1;
        apply_eye_offset(dx, dy);
        saccade_next_ms = now + 800;
        break;
    }
    case DM_EYEROLL:   eyeroll_start_ms   = now; break;
    case DM_SHAKE:     shake_start_ms     = now; break;
    case DM_CONFUSED:  do_confused((int)(esp_random() & 1));    break;
    case DM_SURPRISE:  do_surprised();                          break;
    case DM_BOUNCE:    bounce_start_ms    = now; break;
    case DM_NOD:       nod_start_ms       = now; break;
    case DM_WAVE:      wave_start_ms      = now; break;
    case DM_PULSE_ALT: pulse_alt_start_ms = now; break;
    case DM_YAWN:      do_yawn();                               break;
    case DM_WARNING:
        // Manual warning trigger: flicker + shake together so it reads as
        // an actual "alert" not a barely-visible halo blip.
        do_warning_flicker();
        shake_start_ms = now;
        break;
    default: break;
    }
}

void emo2_next_emotion(void) {
    if (!active || sleeping || !connected) return;
    // Manual cycle (web "Next anim" / CTRL 0x08): include CROSS so the user
    // can reach the shocked face on demand. Auto-rotation in emo2_tick keeps
    // skipping it — CROSS is a punctuation, not a resting mood.
    uint8_t next = cur_mood;
    for (int tries = 0; tries < 16 &&
         (next == cur_mood || next == M_SLEEP || next == M_LOVE ||
          next == M_OVAL_TALL || next == M_DIAMOND ||
          next == M_PUPIL_LEFT); tries++) {
        next = esp_random() % M_COUNT;
    }
    set_mood(next);
    mood_next_ms = millis() + 60000 + (esp_random() % 120000);
}
void emo2_relang(void) {
    if (!container) return;
    // Re-localise chip tag labels (Сессия / Неделя ↔ Session / Week)
    if (chip_tag_l) lv_label_set_text(chip_tag_l, TR(STR_SESSION));
    if (chip_tag_r) lv_label_set_text(chip_tag_r, TR(STR_WEEK));
    apply_view();
}
// emo2_relayout — see below (full landscape-aware re-positioning of every
// absolute-placed layout object). The stub previously here was kept only
// for header symmetry with ui_relayout / splash_relayout.

void emo2_set_token_expired(bool expired) {
    if (expired == token_expired) return;
    token_expired = expired;
    if (active) {
        if (expired) {
            if (obj_status) lv_label_set_text(obj_status, TR(STR_REAUTH));
        } else {
            if (obj_status) lv_label_set_text(obj_status, "");
        }
        // Reapply view so info/bars are hidden under the status overlay
        // (or restored when the token recovers).
        apply_view();
    }
}

// OTA progress overlay. Hides eyes + info + bars + clock, shows a big
// "OTA NN%" label centred on screen. Called from ota.cpp.
void emo2_set_ota_progress(uint8_t state, uint8_t pct) {
    if (!active || !obj_status) {
        // Stash state so apply_view (next show) does the right thing.
        ota_active = (state == 0 || state == 1);
        return;
    }
    static bool prev_active = false;
    bool entering = (state == 0 || state == 1) && !prev_active;
    bool exiting  = (state == 2 || state == 3) && prev_active;

    if (entering) {
        // Bigger font so the percentage is unmistakable. Same Y as every other
        // status state (helper) — one stable position, no jumps between modes.
        lv_obj_set_style_text_font(obj_status, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(obj_status, lv_color_hex(0xE0E0E0), 0);
        position_status_label();
        ota_active = true;
        apply_view();          // hides eyes/info/bars/clock
        // Hide eyes explicitly (apply_view doesn't touch them).
        for (int i = 0; i < 2; i++) {
            if (eye_base[i]) lv_obj_add_flag(eye_base[i], LV_OBJ_FLAG_HIDDEN);
            if (eye_halo[i]) lv_obj_add_flag(eye_halo[i], LV_OBJ_FLAG_HIDDEN);
            if (eye_spec[i]) lv_obj_add_flag(eye_spec[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (obj_zzz) lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[24];
    if (state == 0) {
        lv_label_set_text(obj_status, "OTA 0%");
    } else if (state == 1) {
        if (pct > 100) pct = 100;
        snprintf(buf, sizeof(buf), "OTA %u%%", (unsigned)pct);
        lv_label_set_text(obj_status, buf);
    } else if (state == 2) {
        lv_label_set_text(obj_status, "OTA ✓");
        lv_obj_set_style_text_color(obj_status, lv_color_hex(0x8FA66E), 0);
    } else if (state == 3) {
        lv_label_set_text(obj_status, "OTA ✕");
        lv_obj_set_style_text_color(obj_status, lv_color_hex(0xFF4D4D), 0);
    }

    if (exiting) {
        // After DONE/ERROR we'd normally restore the UI, but on DONE the ESP
        // is about to reboot (~1.5s), so we just leave the message visible.
        // On ERROR (state==3), do restore so the user sees their dashboard.
        if (state == 3) {
            ota_active = false;
            lv_obj_set_style_text_font(obj_status, &lv_font_montserrat_16, 0);
            position_status_label();
            // Show eyes back.
            for (int i = 0; i < 2; i++) {
                if (eye_base[i]) lv_obj_clear_flag(eye_base[i], LV_OBJ_FLAG_HIDDEN);
                if (eye_halo[i]) lv_obj_clear_flag(eye_halo[i], LV_OBJ_FLAG_HIDDEN);
                if (eye_spec[i]) lv_obj_clear_flag(eye_spec[i], LV_OBJ_FLAG_HIDDEN);
            }
            apply_view();
        }
    }
    prev_active = ota_active;
}

void emo2_set_manual_mode(bool m) {
    if (m == manual_mode) return;
    manual_mode = m;
    // When entering manual: leave whatever is on screen as-is, just stop
    // scheduling new auto events. Leaving manual resets schedulers to
    // start fresh (avoid a burst of stale-overdue events).
    if (!m && active) {
        uint32_t now = millis();
        blink_next_ms     = now + 1500;
        mood_next_ms      = now + 60000;
        heartbeat_next_ms = now + 2500;
        saccade_next_ms   = now + 1800;
        wink_next_ms      = now + 15000;
    }
}

// Deferred NVS write — setters flip this flag and bump the timestamp; the
// main-loop tick (emo2_tick) flushes after `NVS_FLUSH_QUIET_MS` of quiet.
// Coalesces rapid bursts (e.g. gradient-slider drag → 5+ POSTs/s) into one
// flash write. Eliminates the watchdog-during-flash-erase reboot loop we
// saw when every BLE-callback path called Preferences directly.
static volatile bool nvs_dirty         = false;
static volatile uint32_t nvs_dirty_ms  = 0;
#define NVS_FLUSH_QUIET_MS 750

static inline void mark_nvs_dirty(void) {
    nvs_dirty    = true;
    nvs_dirty_ms = millis();
}

void emo2_set_color_override(uint8_t mode) {
    color_override = mode;
    if (active) apply_colours();
}

// Layout-fill palette override — independent from the eye halo colour.
// 0xFF = auto (lerp by pct); 0..2 = locked palette index.
void emo2_set_layout_color_override(uint8_t mode) {
    layout_color_override = mode;
    mark_nvs_dirty();
    if (active) refresh_usage_info();
}
uint8_t emo2_get_layout_color_override(void) { return layout_color_override; }

// Apply font + colour + position for the current clock_style. Each branch
// uses a NATIVE bitmap font generated from the exact web TTF (digit-only
// glyphs to keep flash cost low — see fonts_clock/*.c, ~14-25 KB each).
// No more `transform_scale` — the rasteriser draws at the right size from
// the start, so the result matches the web preview pixel-for-pixel
// (modulo subpixel antialiasing, which the ESP doesn't have).
static void apply_clock_style(void) {
    if (!obj_clock) return;
    lv_obj_remove_style_all(obj_clock);   // wipe prior style for clean swap
    lv_obj_set_style_text_letter_space(obj_clock, 0, 0);
    lv_obj_set_style_transform_scale(obj_clock, 256, 0);   // 1.0× reset
    // Sensible per-style defaults; each case overrides what it needs.
    lv_obj_set_style_bg_opa(obj_clock, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(obj_clock, 0, 0);
    lv_obj_set_style_radius(obj_clock, 0, 0);
    lv_obj_set_style_shadow_opa(obj_clock, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(obj_clock, 0, 0);
    lv_obj_set_style_shadow_spread(obj_clock, 0, 0);

    switch (clock_style) {
    case CS_MONO:
        // c01 · Share Tech Mono 56 px · white. Native rasterisation.
        lv_obj_set_style_text_font (obj_clock, &lv_font_clock_sharetech_56, 0);
        lv_obj_set_style_text_color(obj_clock, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, 0, CLOCK_Y);
        break;
    case CS_MAJOR_MONO:
        // c05 · Major Mono Display 44 px · cyan. The typeface's own glyph
        // shape carries the look — no letter-spacing hack needed.
        lv_obj_set_style_text_font (obj_clock, &lv_font_clock_major_44, 0);
        lv_obj_set_style_text_color(obj_clock, EMO2_BASE, 0);
        lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, 0, CLOCK_Y + 6);
        break;
    case CS_ORBITRON:
        // c07 · Orbitron-Bold 44 px · cyan + Orbitron's signature spacing.
        lv_obj_set_style_text_font (obj_clock, &lv_font_clock_orbitron_44, 0);
        lv_obj_set_style_text_color(obj_clock, EMO2_BASE, 0);
        lv_obj_set_style_text_letter_space(obj_clock, 4, 0);
        lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, 0, CLOCK_Y + 6);
        break;
    case CS_OUTLINE:
        // c08 · Onest-Bold 56 px sub-saturated cyan. Native stroke-only is
        // still impossible (LVGL bitmap fonts ARE filled glyphs); use very
        // low opa (~40%) on a heavy weight to read as a "ghost" / wireframe.
        lv_obj_set_style_text_font (obj_clock, &lv_font_clock_onest_56, 0);
        lv_obj_set_style_text_color(obj_clock, EMO2_BASE, 0);
        lv_obj_set_style_text_opa  (obj_clock, LV_OPA_40, 0);
        lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, 0, CLOCK_Y);
        break;
    case CS_NEON:
        // c10 · Orbitron-Bold 44 px white digits on a semi-opaque dark
        // rounded "neon plate". LVGL label shadow is bounding-box based
        // (not per-glyph), so the previous fill-less style drew a hollow
        // rounded frame the user read as a silly empty box. Giving the
        // label an actual translucent fill turns that same outline into
        // an intentional glowing panel, and the cyan shadow now reads as
        // the plate's outer glow.
        lv_obj_set_style_text_font (obj_clock, &lv_font_clock_orbitron_44, 0);
        lv_obj_set_style_text_color(obj_clock, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_letter_space(obj_clock, 4, 0);
        // Cyan-tinted dark fill bright enough to read as a panel over the
        // black screen (the old 0x041417 @ 50% was invisible), plus a bold
        // full-opacity cyan border so the plate has a defined neon frame.
        lv_obj_set_style_bg_color(obj_clock, lv_color_hex(0x09333D), 0);
        lv_obj_set_style_bg_opa  (obj_clock, LV_OPA_70, 0);
        lv_obj_set_style_radius  (obj_clock, 14, 0);
        lv_obj_set_style_pad_hor (obj_clock, 18, 0);
        lv_obj_set_style_pad_ver (obj_clock, 8, 0);
        lv_obj_set_style_border_color(obj_clock, EMO2_BASE, 0);
        lv_obj_set_style_border_opa  (obj_clock, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj_clock, 2, 0);
        lv_obj_set_style_shadow_color(obj_clock, EMO2_BASE, 0);
        lv_obj_set_style_shadow_opa  (obj_clock, LV_OPA_70, 0);
        lv_obj_set_style_shadow_width(obj_clock, 20, 0);
        lv_obj_set_style_shadow_spread(obj_clock, 1, 0);
        lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, 0, CLOCK_Y + 6);
        break;
    case CS_SECONDS:
        // c13 · HH:MM in Share Tech Mono 56 px white (primary), SS suffix
        // in 36 px cyan via obj_clock_secs — matches the web preview's
        // dual-font composition. obj_clock holds just "HH:MM"; the suffix
        // label aligns to the right of it (positioned at refresh time).
        lv_obj_set_style_text_font (obj_clock, &lv_font_clock_sharetech_56, 0);
        lv_obj_set_style_text_color(obj_clock, lv_color_hex(0xFFFFFF), 0);
        // Shift left so the combined HH:MM + SS fits centred.
        lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, -28, CLOCK_Y);
        break;
    case CS_BRACKET:
        // c15 · Azeret Mono 40 px · amber [ HH:MM ].
        lv_obj_set_style_text_font (obj_clock, &lv_font_clock_azeret_40, 0);
        lv_obj_set_style_text_color(obj_clock, EMO2_WARN, 0);
        lv_obj_set_style_text_letter_space(obj_clock, 2, 0);
        lv_obj_align(obj_clock, LV_ALIGN_TOP_MID, 0, CLOCK_Y + 8);
        break;
    case CS_OFF:
    default:
        lv_obj_set_style_text_font(obj_clock, &lv_font_montserrat_20, 0);
        break;
    }
    // Position the seconds-suffix label next to obj_clock when CS_SECONDS
    // is active. Aligned to obj_clock's right-edge baseline.
    if (obj_clock_secs) {
        if (clock_style == CS_SECONDS) {
            // Right-align to the screen with a CLOCK_Y offset; obj_clock
            // is shifted -28 so they meet near the centre.
            lv_obj_align(obj_clock_secs, LV_ALIGN_TOP_MID, 60, CLOCK_Y + 16);
        }
        // Visibility is gated separately in apply_view().
    }
    // Clock-colour override — paints over the per-style hardcoded colour.
    // Applied AFTER the switch so it wins regardless of which style ran.
    // For AUTO mode the gradient is recomputed every refresh (see
    // refresh_usage_info), so seed it here with the last-known pct.
    apply_clock_color(last_data.valid ? last_data.session_pct : 0.0f);
}

// Repaint the clock text per the colour-override mode. 0xFF leaves the
// per-style colour set by apply_clock_style(); 0xA0 follows the %-gradient;
// 0x80 uses the custom RGB. Called from apply_clock_style() (style/mode
// change) and from refresh_usage_info() each tick so AUTO tracks live %.
static void apply_clock_color(float pct) {
    if (!obj_clock) return;
    lv_color_t cc;
    if (clock_color_override == 0x80) {
        cc = lv_color_make(custom_clock_r, custom_clock_g, custom_clock_b);
    } else if (clock_color_override == 0xA0) {
        cc = pct_color(pct);
    } else {
        return;   // 0xFF — keep the per-style hardcoded colour.
    }
    lv_obj_set_style_text_color(obj_clock, cc, 0);
    if (obj_clock_secs)
        lv_obj_set_style_text_color(obj_clock_secs, cc, 0);
}

void emo2_set_clock_style(uint8_t style) {
    if (style > CS_MAX) return;
    if (style == clock_style) return;
    clock_style = style;
    mark_nvs_dirty();
    if (active) { apply_clock_style(); apply_view(); }
}
uint8_t emo2_get_clock_style(void) { return clock_style; }

// Recompute positions for every absolute-positioned layout object using
// scr_w()/scr_h(). LV_ALIGN_*-aligned objects auto-update when the parent
// resizes — those don't need touching. Called from main.cpp after a rotation
// flip (CTRL 0x30/0x31 or BTN_B cycle).
void emo2_relayout(void) {
    if (!container) return;
    const int W = scr_w(), H = scr_h();
    lv_obj_set_size(container, W, H);
    lv_obj_set_pos(container, 0, 0);

    // Eyes — centred horizontally with EYE_GAP between them, vertically at
    // ~40 % of H so there's room below for layout chrome.
    const int eye_cy   = H * 11 / 28;     // portrait: 110 of 280; landscape: ~94 of 240
    const int eye_l_cx = (W - (2 * BASE_PX + EYE_GAP)) / 2 + BASE_PX / 2;
    const int eye_r_cx = eye_l_cx + BASE_PX + EYE_GAP;
    for (int i = 0; i < 2; i++) {
        int cx = (i == 0) ? eye_l_cx : eye_r_cx;
        if (eye_halo[i]) lv_obj_set_pos(eye_halo[i],
                                        cx - HALO_SRC / 2,
                                        eye_cy - HALO_SRC / 2 + HALO_SHADOW_DY);
        if (eye_base[i]) lv_obj_set_pos(eye_base[i], cx - 32,           eye_cy - 32);
        if (eye_spec[i]) lv_obj_set_pos(eye_spec[i],
                                        cx - BASE_PX / 4 - 4,
                                        eye_cy - BASE_PX / 3);
    }

    // Classic info+bars — restack at proportional Y so landscape (H=240)
    // squeezes them above the bottom layout area. Use lv_obj_align with
    // LV_ALIGN_TOP_MID instead of set_pos(x=0) — that auto-centres the
    // object regardless of its own size. set_pos(0,y) + set_width(W) on a
    // label leaves text left-aligned because the default text-align is
    // LEFT (this was the portrait-centering regression).
    //
    // Placement-aware (item 2/3): scale the portrait Y values (designed for
    // H=280) by current H. The three placement variants reposition the entire
    // text-stack within the lower screen block.
    const int tp = effective_text_placement(); // 0=top, 1=middle, 2=bottom (landscape ribbon/ecg → top)
    // Use info_*_y() wrappers so ribbon's "bottom" pick maps to middle's Y.
    int y_text_s = (int)H * info_s_y(tp) / 280;
    int y_bar_s  = (int)H * bar_s_y(tp)  / 280;
    int y_text_w = (int)H * info_w_y(tp) / 280;
    int y_bar_w  = (int)H * bar_w_y(tp)  / 280;
    if (obj_info_s) lv_obj_align(obj_info_s, LV_ALIGN_TOP_MID, 0, y_text_s);
    if (obj_bar_s)  lv_obj_align(obj_bar_s,  LV_ALIGN_TOP_MID, 0, y_bar_s);
    if (obj_info_w) lv_obj_align(obj_info_w, LV_ALIGN_TOP_MID, 0, y_text_w);
    if (obj_bar_w)  lv_obj_align(obj_bar_w,  LV_ALIGN_TOP_MID, 0, y_bar_w);

    // Bezel outline — full screen inset. Recentre too (its anchor is set in
    // emo2_show() but a rotation flip occasionally leaves it off).
    if (bezel_outline) {
        lv_obj_set_size(bezel_outline, W - 2 * BEZEL_INSET, H - 2 * BEZEL_INSET);
        lv_obj_align(bezel_outline, LV_ALIGN_CENTER, 0, 0);
    }

    // Bezel corner arcs — absolute-positioned, must be moved on rotation.
    // Each arc lives in a 2R × 2R box positioned at corner-centre minus R.
    const int TR_X = W - BEZEL_INSET - BEZEL_RADIUS;
    const int TL_X = BEZEL_INSET + BEZEL_RADIUS;
    const int T_Y  = BEZEL_INSET + BEZEL_RADIUS;
    const int B_Y  = H - BEZEL_INSET - BEZEL_RADIUS;
    if (bez_s_arc_top) lv_obj_set_pos(bez_s_arc_top, TR_X - BEZEL_RADIUS, T_Y - BEZEL_RADIUS);
    if (bez_s_arc_bot) lv_obj_set_pos(bez_s_arc_bot, TR_X - BEZEL_RADIUS, B_Y - BEZEL_RADIUS);
    if (bez_w_arc_top) lv_obj_set_pos(bez_w_arc_top, TL_X - BEZEL_RADIUS, T_Y - BEZEL_RADIUS);
    if (bez_w_arc_bot) lv_obj_set_pos(bez_w_arc_bot, TL_X - BEZEL_RADIUS, B_Y - BEZEL_RADIUS);

    // Chip frames — LV_ALIGN_BOTTOM_LEFT/RIGHT children would auto-relocate
    // when the parent (container) resizes, BUT lv_obj_set_size doesn't
    // always trigger that — call align explicitly so the chips end up in
    // the new bottom corners.
    if (chip_frame_r) lv_obj_align(chip_frame_r, LV_ALIGN_BOTTOM_RIGHT, -CHIP_PAD_X, -CHIP_PAD_B);
    if (chip_frame_l) lv_obj_align(chip_frame_l, LV_ALIGN_BOTTOM_LEFT,   CHIP_PAD_X, -CHIP_PAD_B);

    // Columns — left edge + right edge, full height.
    if (col_left_track)  {
        lv_obj_set_size(col_left_track,  COL_THICK, H - 2 * COL_INSET);
        lv_obj_set_pos (col_left_track,  COL_INSET, COL_INSET);
    }
    if (col_right_track) {
        lv_obj_set_size(col_right_track, COL_THICK, H - 2 * COL_INSET);
        lv_obj_set_pos (col_right_track, W - COL_INSET - COL_THICK, COL_INSET);
    }
    // col_left_fill / col_right_fill are repositioned by refresh_usage_info()
    // every frame — no need to touch them here.

    // HUD ribbon — anchor near the bottom of the new screen height. Portrait
    // keeps the original fixed geometry (well-proportioned at W=240). Landscape
    // is wider (W=280): the fixed 10×14 segment row (x=38..196) used to bunch
    // up on the left with a big empty gap on the right. Stretch the segments to
    // span the full usable width between the S/W label gutter and the right %
    // readout so the meter reads full-width and balanced (items 5+6).
    {
        const bool landscape = (current_rotation % 2 != 0);
        int seg_w = RIBBON_SEG_W, seg_gap = RIBBON_SEG_GAP, bar_x = RIBBON_BAR_X;
        int pct_x = RIBBON_BAR_X + RIBBON_BAR_W + 6;
        if (landscape) {
            const int pct_reserve = 44;     // room for the right-edge "100%" readout
            int avail = W - bar_x - pct_reserve;
            seg_w = (avail - (RIBBON_SEG_COUNT - 1) * seg_gap) / RIBBON_SEG_COUNT;
            if (seg_w < RIBBON_SEG_W) seg_w = RIBBON_SEG_W;   // never shrink below portrait size
            int row_w = RIBBON_SEG_COUNT * seg_w + (RIBBON_SEG_COUNT - 1) * seg_gap;
            pct_x = bar_x + row_w + 6;
        }
        if (ribbon_lbl_s) lv_obj_set_pos(ribbon_lbl_s, 18,    H - 40);
        if (ribbon_lbl_w) lv_obj_set_pos(ribbon_lbl_w, 18,    H - 22);
        if (ribbon_pct_s) lv_obj_set_pos(ribbon_pct_s, pct_x, H - 40);
        if (ribbon_pct_w) lv_obj_set_pos(ribbon_pct_w, pct_x, H - 22);
        for (int i = 0; i < RIBBON_SEG_COUNT; i++) {
            int sx = bar_x + i * (seg_w + seg_gap);
            if (ribbon_seg_s[i]) { lv_obj_set_size(ribbon_seg_s[i], seg_w, RIBBON_SEG_H); lv_obj_set_pos(ribbon_seg_s[i], sx, H - 40); }
            if (ribbon_seg_w[i]) { lv_obj_set_size(ribbon_seg_w[i], seg_w, RIBBON_SEG_H); lv_obj_set_pos(ribbon_seg_w[i], sx, H - 22); }
        }
    }

    // ECG — rebuild the polyline X points across the new width.
    const int ecg_x1 = W - 12;
    const int ecg_y_mid = H - 80;
    for (int i = 0; i < ECG_POINTS; i++) {
        ecg_pts[i].x = ECG_X0 + ((ecg_x1 - ECG_X0) * i) / (ECG_POINTS - 1);
        ecg_pts[i].y = ecg_y_mid;
    }
    if (ecg_line) lv_line_set_points(ecg_line, ecg_pts, ECG_POINTS);

    // Status label and "zzz" sleep label — re-align via the shared helper so
    // the reconnect/OTA text keeps its single safe-belt Y after a rotation.
    position_status_label();
    if (obj_zzz)    lv_obj_align(obj_zzz, LV_ALIGN_TOP_RIGHT, -28, 32);

    // Force a redraw + repush colours/positions on next refresh.
    if (active) {
        apply_view();
        refresh_usage_info();
    }
}

void emo2_set_stats_layout(uint8_t layout) {
    if (layout > LAYOUT_MAX) return;
    if (layout == stats_layout) return;
    stats_layout = layout;
    // Persist via the emo2_cfg blob so the choice survives a reboot even
    // without daemon push. Setter dirty-flags the blob; the tick-loop
    // commits to NVS within NVS_FLUSH_QUIET_MS.
    emo2_cfg.active_layout = layout;
    mark_nvs_dirty();
    if (active) {
        // apply_view() already ends with refresh_usage_info() — used to
        // chain a second call here, doubling layout repaint cost.
        apply_view();
    }
}

uint8_t emo2_get_stats_layout(void) { return stats_layout; }

// Legacy 0x44-0x47 setter — decomposes the 4-value text_mode into the
// (text_source, text_format) pair. Daemon's push_emo2_full_config and the
// per-layout pickers in web ultimately route through 0x44-0x47 for
// back-compat, so this stays the canonical entry point.
void emo2_set_text_mode(uint8_t mode) {
    if (mode > TEXT_MODE_BOTH) return;
    uint8_t src = TEXT_SRC_BOTH;
    uint8_t fmt = TEXT_FMT_PCT_RESET;
    switch (mode) {
    case TEXT_MODE_NONE:       src = TEXT_SRC_OFF;  fmt = TEXT_FMT_PCT;       break;
    case TEXT_MODE_PCT_ONLY:   src = TEXT_SRC_BOTH; fmt = TEXT_FMT_PCT;       break;
    case TEXT_MODE_RESET_ONLY: src = TEXT_SRC_BOTH; fmt = TEXT_FMT_RESET;     break;
    case TEXT_MODE_BOTH:       src = TEXT_SRC_BOTH; fmt = TEXT_FMT_PCT_RESET; break;
    }
    text_mode   = mode;
    text_source = src;
    text_format = fmt;
    // Persist per-active-layout legacy text_mode in the cfg blob + the
    // decomposed src/fmt in their own NVS keys so reboot-without-daemon
    // restores all three coherently.
    if (stats_layout < EMO2_LAYOUT_SLOTS) {
        emo2_cfg.layout_text_mode[stats_layout] = mode;
        mark_nvs_dirty();
    }
    Preferences pp;
    if (pp.begin("clawd", false)) {
        pp.putUChar("e2ts", src);
        pp.putUChar("e2tf", fmt);
        pp.end();
    }
    if (active) { apply_view(); refresh_usage_info(); }
}
uint8_t emo2_get_text_mode(void) { return text_mode; }

void emo2_set_text_source(uint8_t src) {
    if (src > TEXT_SRC_BOTH) return;
    // No same-value early-exit: when daemon switches layouts, it sends
    // STATS_LAYOUT first (which already calls apply_view with the OLD
    // text_source) and then TEXT_SOURCE. If the new layout's stored
    // source matches the current value, the early-exit would skip the
    // post-layout-switch re-render. Always apply.
    text_source = src;
    // Defer NVS write — the tick-loop flush picks it up in ≤NVS_FLUSH_QUIET_MS.
    // Direct `Preferences::begin()` from this setter ran in BLE callback
    // context and stalled the NimBLE task during the 10-50 ms flash op.
    mark_nvs_dirty();
    if (!cfg_apply_in_progress && active) { apply_view(); refresh_usage_info(); }
}
uint8_t emo2_get_text_source(void) { return text_source; }

void emo2_set_text_format(uint8_t fmt) {
    if (fmt > TEXT_FMT_RESET) return;
    text_format = fmt;          // same reasoning as above — no early-exit
    mark_nvs_dirty();
    if (!cfg_apply_in_progress && active) refresh_usage_info();
}
uint8_t emo2_get_text_format(void) { return text_format; }

void emo2_set_text_placement(uint8_t p) {
    if (p > TEXT_PLACE_BOTTOM) return;
    if (text_placement == p) return;     // dead early-exit fixed — was missing return
    text_placement = p;
    mark_nvs_dirty();                    // deferred flash write (BLE callback safe)
    // Re-align the four text + bar widgets to the new Y band. Use relayout
    // because it handles BOTH portrait and landscape via the H-proportional
    // formula. apply_view() alone only shows/hides, doesn't move.
    if (!cfg_apply_in_progress && active) {
        emo2_relayout();
        apply_view();
        refresh_usage_info();
    }
}
uint8_t emo2_get_text_placement(void) { return text_placement; }

// ─── Phase A: config NVS + state machine + rotation ────────────────────────
static void emo2_load_cfg_from_nvs(void) {
    Preferences p;
    if (!p.begin("clawd", true)) return;
    if (p.isKey("e2cfg") && p.getBytesLength("e2cfg") == sizeof(emo2_cfg)) {
        emo2_full_cfg_t buf;
        size_t r = p.getBytes("e2cfg", &buf, sizeof(buf));
        if (r == sizeof(buf) && buf.magic == EMO2_CFG_MAGIC) {
            emo2_cfg = buf;
            emo2_cfg_loaded = true;
            // Sync the runtime layout selectors with the just-loaded blob so
            // the boot screen renders the user's last layout WITHOUT waiting
            // for a daemon push. Previously these RAM variables stayed at
            // their compile-time defaults until BLE re-connect, so the user
            // saw `bezel_orbit + text_mode=both` instead of e.g.
            // `corner_chip + text_mode=pct` they had picked.
            if (emo2_cfg.active_layout <= LAYOUT_MAX) {
                stats_layout = emo2_cfg.active_layout;
                uint8_t lv = emo2_cfg.active_layout;
                if (lv < EMO2_LAYOUT_SLOTS) {
                    text_mode = emo2_cfg.layout_text_mode[lv];
                }
            }
            Serial.println("emo2: NVS config loaded");
        }
    }
    // Layout colour override + clock style are small single-byte settings
    // — kept outside the bulk emo2_cfg blob so they survive a cfg schema bump.
    if (p.isKey("e2lc")) layout_color_override = p.getUChar("e2lc", 0xFF);
    if (p.isKey("e2cs")) {
        uint8_t cs = p.getUChar("e2cs", CS_MONO);
        // Schema version sentinel — `e2csv` was added with the 8-pick
        // gallery rollout. Missing/old → assume legacy v1 enum (off /
        // minimal / big / mono / dot) and migrate per visual closeness.
        uint8_t ver = p.getUChar("e2csv", 1);
        if (ver < 2) {
            //   v1.minimal(1) → v2.outline(4) — both small/thin
            //   v1.big(2)     → v2.mono(1)    — both large white
            //   v1.mono(3)    → v2.major_mono(2) — both cyan
            //   v1.dot(4)     → v2.orbitron(3) — both letter-spaced
            static const uint8_t MIGRATE_V1[] = {
                CS_OFF, CS_OUTLINE, CS_MONO, CS_MAJOR_MONO, CS_ORBITRON
            };
            if (cs < sizeof(MIGRATE_V1)) cs = MIGRATE_V1[cs];
            else                          cs = CS_MONO;
        }
        clock_style = (cs <= CS_MAX) ? cs : CS_MONO;
    }
    // Pace multipliers (×10), valid range 5..50 (0.5×..5×). Clamps anything
    // out-of-band to the legacy 10 (1×) default so a junk NVS read can't
    // freeze or starve the animation loop.
    if (p.isKey("e2ap")) {
        uint8_t v = p.getUChar("e2ap", 20);
        anim_pace_x10 = (v >= 5 && v <= 50) ? v : 20;
    }
    // Text-placement variant (0/1/2 = top/middle/bottom). Missing → middle.
    if (p.isKey("e2tp")) {
        uint8_t v = p.getUChar("e2tp", TEXT_PLACE_MIDDLE);
        text_placement = (v <= TEXT_PLACE_BOTTOM) ? v : TEXT_PLACE_MIDDLE;
    }
    // Text source / format — persisted natively so user picks survive a
    // reboot even if the daemon push is delayed. Defaults match the
    // legacy text_mode=BOTH (src=BOTH + fmt=PCT_RESET).
    if (p.isKey("e2ts")) {
        uint8_t v = p.getUChar("e2ts", TEXT_SRC_BOTH);
        text_source = (v <= TEXT_SRC_BOTH) ? v : TEXT_SRC_BOTH;
    }
    if (p.isKey("e2tf")) {
        uint8_t v = p.getUChar("e2tf", TEXT_FMT_PCT_RESET);
        text_format = (v <= TEXT_FMT_RESET) ? v : TEXT_FMT_PCT_RESET;
    }
    if (p.isKey("e2fp")) {
        uint8_t v = p.getUChar("e2fp", 20);
        form_pace_x10 = (v >= 5 && v <= 50) ? v : 20;
    }
    // Colour-by-% stops (user-configurable gradient).
    if (p.isKey("e2gn") && p.isKey("e2gs")) {
        uint8_t n = p.getUChar("e2gn", 0);
        if (n >= 2 && n <= EMO2_MAX_COLOR_STOPS) {
            size_t want = n * sizeof(e2_color_stop_t);
            if (p.getBytesLength("e2gs") == want) {
                e2_color_stop_t buf[EMO2_MAX_COLOR_STOPS];
                size_t got = p.getBytes("e2gs", buf, want);
                if (got == want) {
                    memcpy(color_stops, buf, want);
                    n_color_stops = n;
                }
            }
        }
    }
    // Gradient interpolation mode (0=step, 1=smooth). Defaults to step
    // for back-compat with pre-existing NVS records that don't have this key.
    if (p.isKey("e2gm")) {
        uint8_t v = p.getUChar("e2gm", 0);
        if (v <= 1) gradient_mode = v;
    }
    // Custom halo + layout RGB triplets. Optional — absent keys leave the
    // defaults (white 0xFFFFFF). Only takes effect when the matching
    // override is set to 0x80 (custom).
    if (p.isKey("e2hr")) custom_halo_r   = p.getUChar("e2hr", 0xFF);
    if (p.isKey("e2hg")) custom_halo_g   = p.getUChar("e2hg", 0xFF);
    if (p.isKey("e2hb")) custom_halo_b   = p.getUChar("e2hb", 0xFF);
    if (p.isKey("e2lr")) custom_layout_r = p.getUChar("e2lr", 0xFF);
    if (p.isKey("e2lg")) custom_layout_g = p.getUChar("e2lg", 0xFF);
    if (p.isKey("e2lb")) custom_layout_b = p.getUChar("e2lb", 0xFF);
    // Clock colour override mode + custom RGB.
    if (p.isKey("e2cc")) clock_color_override = p.getUChar("e2cc", 0xFF);
    if (p.isKey("e2cr")) custom_clock_r = p.getUChar("e2cr", 0xFF);
    if (p.isKey("e2cg")) custom_clock_g = p.getUChar("e2cg", 0xFF);
    if (p.isKey("e2cb")) custom_clock_b = p.getUChar("e2cb", 0xFF);
    p.end();
}
static void emo2_save_cfg_to_nvs(void) {
    Preferences p;
    if (!p.begin("clawd", false)) return;
    p.putBytes("e2cfg", &emo2_cfg, sizeof(emo2_cfg));
    p.putUChar("e2lc", layout_color_override);
    p.putUChar("e2cs",  clock_style);
    p.putUChar("e2csv", 2);              // schema version (see load_cfg)
    p.putUChar("e2ap", anim_pace_x10);
    p.putUChar("e2fp", form_pace_x10);
    // Text placement / source / format — single batched commit (used to
    // open Preferences from each setter individually = 4 flash transactions
    // per cfg push).
    p.putUChar("e2tp", text_placement);
    p.putUChar("e2ts", text_source);
    p.putUChar("e2tf", text_format);
    // Colour-by-% stops (gradient editor). Persisting here in the same
    // batch keeps rapid gradient edits → single NVS commit.
    if (n_color_stops >= 2 && n_color_stops <= EMO2_MAX_COLOR_STOPS) {
        p.putUChar("e2gn", n_color_stops);
        p.putBytes("e2gs", color_stops, n_color_stops * sizeof(e2_color_stop_t));
    }
    p.putUChar("e2gm", gradient_mode);   // 0=step, 1=smooth
    // Custom halo + layout RGB (6 byte keys, cheap NVS slot).
    p.putUChar("e2hr", custom_halo_r);
    p.putUChar("e2hg", custom_halo_g);
    p.putUChar("e2hb", custom_halo_b);
    p.putUChar("e2lr", custom_layout_r);
    p.putUChar("e2lg", custom_layout_g);
    p.putUChar("e2lb", custom_layout_b);
    // Clock-colour mode + custom RGB.
    p.putUChar("e2cc", clock_color_override);
    p.putUChar("e2cr", custom_clock_r);
    p.putUChar("e2cg", custom_clock_g);
    p.putUChar("e2cb", custom_clock_b);
    p.end();
}

// Daemon FORM_NAMES → mood_t (identical order, 0..M_COUNT-1). OP_NAMES is
// daemon's 0-based "blink, wink, saccade, ..." which maps to DiagMotion
// values DM_BLINK=1, DM_WINK=2, ... (off-by-one: DM_NONE=0 is reserved).
static inline void apply_cfg_form(emo2_dstate_t s, uint8_t idx) {
    const emo2_state_cfg_t& sc = emo2_cfg.per_state[s];
    if (idx >= sc.n_forms) return;
    uint8_t m = sc.forms[idx];
    if (m < M_COUNT) emo2_set_mood_idx(m);
}
static inline void apply_cfg_op(emo2_dstate_t s, uint8_t idx) {
    const emo2_state_cfg_t& sc = emo2_cfg.per_state[s];
    if (idx >= sc.n_ops) return;
    uint8_t op_idx = sc.ops[idx];     // daemon name-list index (0-based)
    emo2_trigger_op(op_idx + 1);       // → DiagMotion enum (DM_NONE=0 skipped)
}
static inline void apply_cfg_color(emo2_dstate_t s) {
    const emo2_state_cfg_t& sc = emo2_cfg.per_state[s];
    uint8_t c = sc.color;
    // Per-state color mode encoding:
    //   0 = auto (gradient)              → override 0xFF
    //   1 = cyan / 2 = amber / 3 = red   → override = c - 1
    //   4 = custom (use sc.custom_r/g/b) → override 0x80, copy RGB into globals
    if (c == 4) {
        custom_halo_r = sc.custom_r;
        custom_halo_g = sc.custom_g;
        custom_halo_b = sc.custom_b;
        emo2_set_color_override(0x80);
    } else if (c == 0) {
        emo2_set_color_override(0xFF);
    } else {
        emo2_set_color_override((uint8_t)(c - 1));
    }
}

// Phase D: apply the state's lifted visual stack (layout / text source /
// text format / text placement / layout colour). Each field is optional —
// 0xFF means "fall back to current global" so we skip the setter call.
// All setters short-circuit when the value is already current, so calling
// this on every state-transition is cheap even when nothing's per-state.
static inline void apply_cfg_state(emo2_dstate_t s) {
    const emo2_state_cfg_t& sc = emo2_cfg.per_state[s];
    // Layout is a SINGLE GLOBAL setting (emo2_cfg.active_layout), not per-state
    // — the daemon no longer sends a per-state `layout`, and we deliberately
    // ignore it if an old config still carries one. Every non-"connected"
    // state shows the loading animation over whatever the global layout is.
    if (sc.text_source != 0xFF && sc.text_source <= TEXT_SRC_BOTH)
        emo2_set_text_source(sc.text_source);
    if (sc.text_format != 0xFF && sc.text_format <= TEXT_FMT_RESET)
        emo2_set_text_format(sc.text_format);
    if (sc.text_placement != 0xFF && sc.text_placement <= TEXT_PLACE_BOTTOM)
        emo2_set_text_placement(sc.text_placement);
    if (sc.layout_color != 0xFF) {
        // Same encoding as halo color: 0=auto, 1=cyan, 2=amber, 3=red,
        // 4=custom (uses sc.lc_r/g/b → custom_layout_*).
        if (sc.layout_color == 4) {
            custom_layout_r = sc.lc_r;
            custom_layout_g = sc.lc_g;
            custom_layout_b = sc.lc_b;
            emo2_set_layout_color_override(0x80);
        } else if (sc.layout_color == 0) {
            emo2_set_layout_color_override(0xFF);
        } else if (sc.layout_color <= 3) {
            emo2_set_layout_color_override((uint8_t)(sc.layout_color - 1));
        }
    }
}

static bool data_stale(void) {
    if (!last_data.valid) return false;     // never received → handled separately
    if (last_data_received_ms == 0) return false;
    return (millis() - last_data_received_ms) > EMO2_DATA_STALE_MS;
}
static emo2_dstate_t derive_state_local(void) {
    if (token_expired)                       return EST_TOKEN_EXPIRED;
    if (!connected)                          return EST_BLE_OFF;
    if (!last_data.valid || data_stale())    return EST_CONNECTING;
    return EST_CONNECTED;
}

// Tick the state machine + rotation. Idempotent: only sends CTRL-equivalents
// when state changed or a rotation interval elapsed. No-op if manual mode is
// on, OTA in progress, diag running, or screen not active.
static void emo2_tick_state_machine(void) {
    if (manual_mode || ota_active || diag_active) return;
    if (!active) return;
    if (!emo2_cfg_loaded) return;            // wait for daemon's first push
    emo2_dstate_t s = derive_state_local();
    uint32_t now = millis();
    if (s != prev_dstate) {
        prev_dstate = s;
        rot_form_i = rot_op_i = 0;
        form_next_ms = now + EMO2_FORM_ROT_MS;
        op_next_ms   = now + EMO2_OP_ROT_MS;
        apply_cfg_form(s, 0);
        apply_cfg_op  (s, 0);
        apply_cfg_color(s);
        apply_cfg_state(s);   // Phase D: layout / text / layout_color
        return;
    }
    if ((int32_t)(now - form_next_ms) >= 0) {
        uint8_t n = emo2_cfg.per_state[s].n_forms;
        if (n > 0) { rot_form_i = (rot_form_i + 1) % n; apply_cfg_form(s, rot_form_i); }
        form_next_ms = now + EMO2_FORM_ROT_MS;
    }
    if ((int32_t)(now - op_next_ms) >= 0) {
        uint8_t n = emo2_cfg.per_state[s].n_ops;
        if (n > 0) { rot_op_i = (rot_op_i + 1) % n; apply_cfg_op(s, rot_op_i); }
        op_next_ms = now + EMO2_OP_ROT_MS;
    }
}

void emo2_set_data_received(void) { last_data_received_ms = millis(); }

// JSON apply. Schema: {"cfg":{"states":{"<id>":{"forms":[...],"ops":[...],
// "color":"auto|cyan|amber|red"}}, "layouts":{"active":"<id>","text":{...}}}}
// Returns true if a recognisable `cfg` envelope was found and applied.
bool emo2_apply_cfg_json(const char* json) {
    if (!json) return false;
    size_t jlen = strlen(json);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    // Unconditional [CFG] diagnostic — fires on EVERY cfg push, win or lose.
    // Tells us: was the JSON received whole (len), did it parse (err), and
    // which top-level cfg sections were present. If `cs=0` here while the
    // daemon-side log shows `color_stops` in the payload, the field was
    // truncated in transit (BLE MTU / chunk reassembly issue).
    Serial.printf("[CFG] len=%u err=%s ", (unsigned)jlen, err.c_str());
    if (err) { Serial.println(); Serial.printf("emo2 cfg JSON err: %s\n", err.c_str()); return false; }
    JsonObjectConst cfg = doc["cfg"].as<JsonObjectConst>();
    Serial.printf("cfg=%d st=%d ly=%d cs=%d lc=%d cl=%d\n",
                  (int)!cfg.isNull(),
                  (int)!cfg["states"].isNull(),
                  (int)!cfg["layouts"].isNull(),
                  (int)!cfg["color_stops"].isNull(),
                  (int)!cfg["layout_color"].isNull(),
                  (int)!cfg["clock_style"].isNull());
    if (cfg.isNull()) return false;
    // Suppress per-setter `apply_view()` + `refresh_usage_info()` so we don't
    // cascade 3-4 full repaints during a single BLE push. A combined refresh
    // runs at the end of this function.
    cfg_apply_in_progress = true;

    static const char* STATE_KEYS[EMO2_NUM_STATES] = {
        "connected", "connecting", "ble_off", "token_expired"
    };
    JsonObjectConst states = cfg["states"].as<JsonObjectConst>();
    if (!states.isNull()) {
        for (int si = 0; si < EMO2_NUM_STATES; si++) {
            JsonObjectConst st = states[STATE_KEYS[si]].as<JsonObjectConst>();
            if (st.isNull()) continue;
            emo2_state_cfg_t& dst = emo2_cfg.per_state[si];
            // forms
            JsonArrayConst forms = st["forms"].as<JsonArrayConst>();
            if (!forms.isNull()) {
                dst.n_forms = 0;
                for (JsonVariantConst v : forms) {
                    if (dst.n_forms >= EMO2_MAX_FORMS_PER_STATE) break;
                    int idx = v.as<int>();
                    if (idx >= 0 && idx < M_COUNT) dst.forms[dst.n_forms++] = (uint8_t)idx;
                }
            }
            // ops
            JsonArrayConst ops = st["ops"].as<JsonArrayConst>();
            if (!ops.isNull()) {
                dst.n_ops = 0;
                for (JsonVariantConst v : ops) {
                    if (dst.n_ops >= EMO2_MAX_OPS_PER_STATE) break;
                    int idx = v.as<int>();
                    if (idx >= 0 && idx < 16) dst.ops[dst.n_ops++] = (uint8_t)idx;
                }
            }
            // color — string. Built-ins "auto"/"cyan"/"amber"/"red" map to
            // mode 0/1/2/3. A "#RRGGBB" hex sets mode=4 (custom) and parses
            // the bytes into the per-state custom_r/g/b slot.
            const char* col = st["color"] | (const char*)nullptr;
            if (col) {
                if      (!strcmp(col, "auto"))  dst.color = 0;
                else if (!strcmp(col, "cyan"))  dst.color = 1;
                else if (!strcmp(col, "amber")) dst.color = 2;
                else if (!strcmp(col, "red"))   dst.color = 3;
                else if (col[0] == '#' && strlen(col) == 7) {
                    auto h = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                        return -1;
                    };
                    int r1=h(col[1]), r2=h(col[2]);
                    int g1=h(col[3]), g2=h(col[4]);
                    int b1=h(col[5]), b2=h(col[6]);
                    if (r1>=0 && r2>=0 && g1>=0 && g2>=0 && b1>=0 && b2>=0) {
                        dst.color    = 4;
                        dst.custom_r = (uint8_t)((r1 << 4) | r2);
                        dst.custom_g = (uint8_t)((g1 << 4) | g2);
                        dst.custom_b = (uint8_t)((b1 << 4) | b2);
                    }
                }
            }
            // Phase D: lifted per-state visual fields. Each is OPTIONAL —
            // absence = keep current value (likely the legacy global from
            // the previous push). Daemon always ships these once the web
            // UI moves to per-state config; until then daemon mirrors the
            // global values into all four states.
            // ── layout ──
            const char* slay = st["layout"] | (const char*)nullptr;
            if (slay) {
                static const struct { const char* id; uint8_t v; } SLMAP[] = {
                    {"none",         LAYOUT_OFF},
                    {"bezel_orbit",  LAYOUT_BEZEL},
                    {"twin_columns", LAYOUT_COLUMNS},
                    {"hud_ribbon",   LAYOUT_RIBBON},
                    {"brows",        LAYOUT_BROWS},
                    {"tear_pearls",  LAYOUT_TEAR_PEARLS},
                    {"corner_chip",  LAYOUT_CORNER_CHIP},
                    {"ecg_monitor",  LAYOUT_ECG},
                    {"classic",      LAYOUT_CLASSIC},
                };
                for (auto& e : SLMAP) if (!strcmp(slay, e.id)) { dst.layout = e.v; break; }
            }
            // ── text_source ──
            const char* sts = st["text_source"] | (const char*)nullptr;
            if (sts) {
                if      (!strcmp(sts, "off"))     dst.text_source = TEXT_SRC_OFF;
                else if (!strcmp(sts, "session")) dst.text_source = TEXT_SRC_SESSION;
                else if (!strcmp(sts, "weekly"))  dst.text_source = TEXT_SRC_WEEKLY;
                else if (!strcmp(sts, "both"))    dst.text_source = TEXT_SRC_BOTH;
            }
            // ── text_format ──
            const char* stf = st["text_format"] | (const char*)nullptr;
            if (stf) {
                if      (!strcmp(stf, "pct"))       dst.text_format = TEXT_FMT_PCT;
                else if (!strcmp(stf, "pct_reset")) dst.text_format = TEXT_FMT_PCT_RESET;
                else if (!strcmp(stf, "reset"))     dst.text_format = TEXT_FMT_RESET;
            }
            // ── text_placement ──
            const char* stp = st["text_placement"] | (const char*)nullptr;
            if (stp) {
                if      (!strcmp(stp, "top"))    dst.text_placement = TEXT_PLACE_TOP;
                else if (!strcmp(stp, "middle")) dst.text_placement = TEXT_PLACE_MIDDLE;
                else if (!strcmp(stp, "bottom")) dst.text_placement = TEXT_PLACE_BOTTOM;
            }
            // ── layout_color (same shape as halo `color` above) ──
            const char* slc = st["layout_color"] | (const char*)nullptr;
            if (slc) {
                if      (!strcmp(slc, "auto"))  dst.layout_color = 0;
                else if (!strcmp(slc, "cyan"))  dst.layout_color = 1;
                else if (!strcmp(slc, "amber")) dst.layout_color = 2;
                else if (!strcmp(slc, "red"))   dst.layout_color = 3;
                else if (slc[0] == '#' && strlen(slc) == 7) {
                    auto h = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                        return -1;
                    };
                    int r1=h(slc[1]),r2=h(slc[2]),g1=h(slc[3]),g2=h(slc[4]),b1=h(slc[5]),b2=h(slc[6]);
                    if (r1>=0 && r2>=0 && g1>=0 && g2>=0 && b1>=0 && b2>=0) {
                        dst.layout_color = 4;
                        dst.lc_r = (uint8_t)((r1<<4)|r2);
                        dst.lc_g = (uint8_t)((g1<<4)|g2);
                        dst.lc_b = (uint8_t)((b1<<4)|b2);
                    }
                }
            }
        }
        // Phase D: cfg change for ANY state — invalidate cached state so
        // the next state-machine tick re-applies the new per-state stack
        // (including the currently-active state).
        prev_dstate = (emo2_dstate_t)0xFF;
    }
    JsonObjectConst layouts = cfg["layouts"].as<JsonObjectConst>();
    if (!layouts.isNull()) {
        // map layout id string → LAYOUT_* enum (same order as STATS_LAYOUTS in web)
        static const struct { const char* id; uint8_t v; } LMAP[] = {
            {"none",         LAYOUT_OFF},
            {"bezel_orbit",  LAYOUT_BEZEL},
            {"twin_columns", LAYOUT_COLUMNS},
            {"hud_ribbon",   LAYOUT_RIBBON},
            {"brows",        LAYOUT_BROWS},
            {"tear_pearls",  LAYOUT_TEAR_PEARLS},
            {"corner_chip",  LAYOUT_CORNER_CHIP},
            {"ecg_monitor",  LAYOUT_ECG},
        };
        const char* act = layouts["active"] | (const char*)nullptr;
        if (act) {
            for (auto& e : LMAP) if (!strcmp(act, e.id)) {
                emo2_cfg.active_layout = e.v;
                emo2_set_stats_layout(e.v);
                break;
            }
        }
        JsonObjectConst tmap = layouts["text"].as<JsonObjectConst>();
        if (!tmap.isNull()) {
            for (JsonPairConst kv : tmap) {
                const char* lid = kv.key().c_str();
                const char* mode = kv.value().as<const char*>();
                if (!lid || !mode) continue;
                uint8_t v = TEXT_MODE_BOTH;
                if      (!strcmp(mode, "none"))  v = TEXT_MODE_NONE;
                else if (!strcmp(mode, "pct"))   v = TEXT_MODE_PCT_ONLY;
                else if (!strcmp(mode, "reset")) v = TEXT_MODE_RESET_ONLY;
                else if (!strcmp(mode, "both"))  v = TEXT_MODE_BOTH;
                for (auto& e : LMAP) {
                    if (!strcmp(lid, e.id) && e.v < EMO2_LAYOUT_SLOTS) {
                        emo2_cfg.layout_text_mode[e.v] = v;
                        break;
                    }
                }
            }
            // Apply the text mode for the now-active layout.
            uint8_t lv = emo2_cfg.active_layout;
            if (lv < EMO2_LAYOUT_SLOTS) emo2_set_text_mode(emo2_cfg.layout_text_mode[lv]);
        }
        // Text placement for the currently active layout (item 2/3).
        const char* tp = layouts["text_placement"] | (const char*)nullptr;
        if (tp) {
            if      (!strcmp(tp, "top"))    emo2_set_text_placement(TEXT_PLACE_TOP);
            else if (!strcmp(tp, "middle")) emo2_set_text_placement(TEXT_PLACE_MIDDLE);
            else if (!strcmp(tp, "bottom")) emo2_set_text_placement(TEXT_PLACE_BOTTOM);
        }
        // Native text_source / text_format for the active layout — survive
        // the lossy legacy text_mode collapse that previously dropped
        // src=SESSION/WEEKLY back to BOTH on every push.
        const char* ts = layouts["text_source"] | (const char*)nullptr;
        if (ts) {
            uint8_t v = text_source;
            if      (!strcmp(ts, "off"))     v = TEXT_SRC_OFF;
            else if (!strcmp(ts, "session")) v = TEXT_SRC_SESSION;
            else if (!strcmp(ts, "weekly"))  v = TEXT_SRC_WEEKLY;
            else if (!strcmp(ts, "both"))    v = TEXT_SRC_BOTH;
            if (v != text_source) emo2_set_text_source(v);
        }
        const char* tf = layouts["text_format"] | (const char*)nullptr;
        if (tf) {
            uint8_t v = text_format;
            if      (!strcmp(tf, "pct"))       v = TEXT_FMT_PCT;
            else if (!strcmp(tf, "pct_reset")) v = TEXT_FMT_PCT_RESET;
            else if (!strcmp(tf, "reset"))     v = TEXT_FMT_RESET;
            if (v != text_format) emo2_set_text_format(v);
        }
    }

    // Optional: colour-by-% stops. Each item = {"pct": 0..100, "color": "#RRGGBB"}.
    // 2..4 entries required to be honoured — any other count is rejected
    // (keeps the existing in-memory stops alive).
    JsonArrayConst stops = cfg["color_stops"].as<JsonArrayConst>();
    if (!stops.isNull()) {
        e2_color_stop_t tmp[EMO2_MAX_COLOR_STOPS];
        uint8_t n = 0;
        for (JsonVariantConst v : stops) {
            if (n >= EMO2_MAX_COLOR_STOPS) break;
            int pct = v["pct"] | -1;
            const char* hex = v["color"] | (const char*)nullptr;
            if (pct < 0 || pct > 100 || !hex || hex[0] != '#') continue;
            // Parse "#RRGGBB" manually — strtol fits but explicit is clearer.
            auto h = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int r1 = h(hex[1]), r2 = h(hex[2]);
            int g1 = h(hex[3]), g2 = h(hex[4]);
            int b1 = h(hex[5]), b2 = h(hex[6]);
            if (r1<0||r2<0||g1<0||g2<0||b1<0||b2<0) continue;
            tmp[n].pct = (uint8_t)pct;
            tmp[n].r   = (uint8_t)((r1 << 4) | r2);
            tmp[n].g   = (uint8_t)((g1 << 4) | g2);
            tmp[n].b   = (uint8_t)((b1 << 4) | b2);
            n++;
        }
        if (n >= 2 && n <= EMO2_MAX_COLOR_STOPS) {
            memcpy(color_stops, tmp, n * sizeof(e2_color_stop_t));
            n_color_stops = n;
            mark_nvs_dirty();   // batched flush in emo2_tick()
            // Diagnostic — confirms parse path ran and shows what the
            // colour pipeline will pick at the CURRENT pct. Watch the
            // device's Serial output (115200 baud) after pushing a
            // gradient change; this prints exactly once per cfg push.
            Serial.printf("[CSTOPS] n=%u override=%u eff_pct=%.1f active=%d\n",
                          n_color_stops, color_override,
                          (double)eff_session_pct(), (int)active);
            for (uint8_t i = 0; i < n_color_stops; i++) {
                Serial.printf("  stop[%u] pct=%u  #%02X%02X%02X\n",
                              i, color_stops[i].pct,
                              color_stops[i].r, color_stops[i].g, color_stops[i].b);
            }
            {
                lv_color_t pick = halo_color(eff_session_pct());
                uint32_t u = lv_color_to_u32(pick);
                Serial.printf("  → halo_color = #%06X\n",
                              (unsigned)(u & 0xFFFFFF));
            }
            if (active) {
                // halo_color() is used by BOTH the eye silhouette (via
                // apply_colours) AND every layout fill (via refresh_usage_
                // info). Refresh both so the new gradient lands instantly,
                // not just on the next data-poll.
                apply_colours();
                // Force-invalidate the eye sprites so LVGL paints the new
                // recolor immediately. lv_obj_set_style_image_recolor
                // marks the widget dirty in theory but the cached image
                // descriptor can lag a frame — explicit invalidate kills
                // the cache and guarantees the next frame uses the new
                // tint. (Belt-and-braces fix for the "stops change but
                // eye doesn't repaint" report.)
                for (int i = 0; i < 2; i++) {
                    if (eye_base[i]) lv_obj_invalidate(eye_base[i]);
                    if (eye_halo[i]) lv_obj_invalidate(eye_halo[i]);
                }
                refresh_usage_info();
            }
        } else {
            Serial.printf("[CSTOPS] REJECTED n=%u (need 2..%d)\n",
                          n, EMO2_MAX_COLOR_STOPS);
        }
    }

    // Pace multipliers: anim_pace_x10 / form_pace_x10 (×10 integer encoding).
    // Daemon ships as 5..50 → 0.5×..5×; out-of-band ignored.
    {
        int ap = cfg["anim_pace_x10"] | -1;
        int fp = cfg["form_pace_x10"] | -1;
        if (ap >= 5 && ap <= 50) anim_pace_x10 = (uint8_t)ap;
        if (fp >= 5 && fp <= 50) form_pace_x10 = (uint8_t)fp;
    }

    // Test-pct override (web "TEST → ESP" feature). When present, the colour
    // pipeline uses this value instead of real session_pct for TEST_PCT_TTL_MS
    // so the user can preview every threshold crossing on the device. Number
    // outside 0..100 disables the override (-1 = inactive).
    {
        JsonVariantConst tp = cfg["test_pct"];
        if (!tp.isNull()) {
            float v = tp.as<float>();
            if (v >= 0.0f && v <= 100.0f) {
                test_pct_override   = v;
                test_pct_expires_ms = millis() + TEST_PCT_TTL_MS;
            } else {
                test_pct_override   = -1.0f;
                test_pct_expires_ms = 0;
            }
            if (active) { apply_colours(); refresh_usage_info(); }
        }
    }

    // Layout-fill colour override — read from cfg blob so the initial-connect
    // push lines firmware up with daemon state (NVS-restored value can drift
    // if user edited via web while ESP was offline). Same idea for clock_style.
    {
        const char* lc = cfg["layout_color"] | (const char*)nullptr;
        if (lc) {
            uint8_t v = layout_color_override;   // keep on no-match
            bool changed_rgb = false;
            if      (!strcmp(lc, "cyan"))  v = 0;
            else if (!strcmp(lc, "amber")) v = 1;
            else if (!strcmp(lc, "red"))   v = 2;
            else if (!strcmp(lc, "auto"))  v = 0xFF;
            else if (lc[0] == '#' && strlen(lc) == 7) {
                // Custom hex — parse #RRGGBB into the layout RGB triplet.
                auto h = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return -1;
                };
                int r1=h(lc[1]),r2=h(lc[2]),g1=h(lc[3]),g2=h(lc[4]),b1=h(lc[5]),b2=h(lc[6]);
                if (r1>=0 && r2>=0 && g1>=0 && g2>=0 && b1>=0 && b2>=0) {
                    custom_layout_r = (uint8_t)((r1<<4)|r2);
                    custom_layout_g = (uint8_t)((g1<<4)|g2);
                    custom_layout_b = (uint8_t)((b1<<4)|b2);
                    v = 0x80;
                    changed_rgb = true;
                }
            }
            if (v != layout_color_override || changed_rgb) {
                emo2_set_layout_color_override(v);
            }
        }
        const char* cs = cfg["clock_style"] | (const char*)nullptr;
        if (cs) {
            uint8_t v = clock_style;
            if      (!strcmp(cs, "off"))        v = CS_OFF;
            else if (!strcmp(cs, "mono"))       v = CS_MONO;
            else if (!strcmp(cs, "major_mono")) v = CS_MAJOR_MONO;
            else if (!strcmp(cs, "orbitron"))   v = CS_ORBITRON;
            else if (!strcmp(cs, "outline"))    v = CS_OUTLINE;
            else if (!strcmp(cs, "neon"))       v = CS_NEON;
            else if (!strcmp(cs, "seconds"))    v = CS_SECONDS;
            else if (!strcmp(cs, "bracket"))    v = CS_BRACKET;
            if (v != clock_style) emo2_set_clock_style(v);
        }
        // Halo-live — TRANSIENT colour override (Preview "live" path while
        // manual_mode is on). Same string shape as per-state.color:
        //   "auto" / "cyan" / "amber" / "red" / "#RRGGBB".
        // Applies immediately, does NOT mark NVS dirty, does NOT touch
        // emo2_cfg.per_state — so the next full cfg push (e.g. on manual-
        // off or on reconnect) reverts the eye to the saved per-state
        // colour automatically.
        const char* hl = cfg["halo_live"] | (const char*)nullptr;
        if (hl) {
            if      (!strcmp(hl, "auto"))  emo2_set_color_override(0xFF);
            else if (!strcmp(hl, "cyan"))  emo2_set_color_override(0);
            else if (!strcmp(hl, "amber")) emo2_set_color_override(1);
            else if (!strcmp(hl, "red"))   emo2_set_color_override(2);
            else if (hl[0] == '#' && strlen(hl) == 7) {
                auto h = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return -1;
                };
                int r1=h(hl[1]),r2=h(hl[2]),g1=h(hl[3]),g2=h(hl[4]),b1=h(hl[5]),b2=h(hl[6]);
                if (r1>=0 && r2>=0 && g1>=0 && g2>=0 && b1>=0 && b2>=0) {
                    custom_halo_r = (uint8_t)((r1<<4)|r2);
                    custom_halo_g = (uint8_t)((g1<<4)|g2);
                    custom_halo_b = (uint8_t)((b1<<4)|b2);
                    emo2_set_color_override(0x80);
                }
            }
            // No mark_nvs_dirty() — transient by design.
        }
        // Clock colour — "default" / "auto" / "#RRGGBB".
        //   default → 0xFF (per-style hardcoded), auto → 0xA0 (%-gradient),
        //   #hex    → 0x80 (custom RGB). Mirrors layout_color parsing.
        const char* clc = cfg["clock_color"] | (const char*)nullptr;
        if (clc) {
            uint8_t v = clock_color_override;
            bool changed_rgb = false;
            if (!strcmp(clc, "default")) {
                v = 0xFF;
            } else if (!strcmp(clc, "auto")) {
                v = 0xA0;
            } else if (clc[0] == '#' && strlen(clc) == 7) {
                auto h = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return -1;
                };
                int r1=h(clc[1]),r2=h(clc[2]),g1=h(clc[3]),g2=h(clc[4]),b1=h(clc[5]),b2=h(clc[6]);
                if (r1>=0 && r2>=0 && g1>=0 && g2>=0 && b1>=0 && b2>=0) {
                    custom_clock_r = (uint8_t)((r1<<4)|r2);
                    custom_clock_g = (uint8_t)((g1<<4)|g2);
                    custom_clock_b = (uint8_t)((b1<<4)|b2);
                    v = 0x80;
                    changed_rgb = true;
                }
            }
            if (v != clock_color_override || changed_rgb) {
                clock_color_override = v;
                mark_nvs_dirty();
                if (active) apply_clock_style();
            }
        }
        // Gradient interpolation mode — companion to color_stops. Optional
        // field; missing → keep current (default step, NVS-restored).
        const char* gm = cfg["gradient_mode"] | (const char*)nullptr;
        if (gm) {
            uint8_t v = gradient_mode;
            if      (!strcmp(gm, "step"))   v = 0;
            else if (!strcmp(gm, "smooth")) v = 1;
            if (v != gradient_mode) {
                gradient_mode = v;
                mark_nvs_dirty();
                if (active) {
                    apply_colours();
                    for (int i = 0; i < 2; i++) {
                        if (eye_base[i]) lv_obj_invalidate(eye_base[i]);
                        if (eye_halo[i]) lv_obj_invalidate(eye_halo[i]);
                    }
                    refresh_usage_info();
                }
                Serial.printf("[GMODE] -> %s\n", (v == 1) ? "smooth" : "step");
            }
        }
    }

    emo2_cfg.magic = EMO2_CFG_MAGIC;
    emo2_cfg_loaded = true;
    // Defer the flash write — the tick-loop flush will pick it up in
    // ≤NVS_FLUSH_QUIET_MS. Eliminates the BLE-callback NVS stall that was
    // tripping the watchdog during rapid edits.
    mark_nvs_dirty();
    // Force a state-machine tick on next loop iteration by invalidating the
    // prev_dstate so the new config gets applied immediately.
    prev_dstate = (emo2_dstate_t)0xFF;
    // End of suppress window. ONE combined repaint — replaces the 3-4
    // cascading refreshes that ran inside each setter.
    cfg_apply_in_progress = false;
    if (active) {
        // emo2_relayout() — re-aligns the info/bar widgets to the new
        // text_placement Y. Previously only apply_view() + refresh_usage_
        // info() ran here, neither of which moves widgets — so a cfg push
        // that changed `text_placement` updated the variable but the
        // labels stayed at their previous Y. emo2_set_text_placement()
        // does call relayout, but only OUTSIDE cfg_apply_in_progress; the
        // batched final refresh has to do it explicitly.
        emo2_relayout();
        apply_view();
        refresh_usage_info();
    }
    Serial.println("emo2: cfg applied + saved");
    return true;
}

void emo2_set_connected(bool c) {
    if (c == connected) return;
    connected = c;
    if (!c) {
        recon_dots = 0; recon_last_ms = millis();
        if (sleeping) { sleeping = false; stop_sleep_anim(); }
        if (obj_zzz) lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
    }
    if (active) {
        for (int i = 0; i < 2; i++) {
            if (!c) {
                lv_obj_add_flag(eye_base[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(eye_halo[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(eye_spec[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(eye_base[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(eye_halo[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(eye_spec[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (c) {
            do_boot_in();
            uint32_t now = millis();
            blink_next_ms = now + 1500;
            mood_next_ms  = now + 60000;
            heartbeat_next_ms = now + 2500;
            // Only clear the status label if we're not in token-expired state
            // — re-auth overlay should outlive a BLE reconnect.
            if (!token_expired) lv_label_set_text(obj_status, "");
        }
        // Re-evaluate info/bars visibility now that connected has flipped.
        apply_view();
    }
}

void emo2_set_usage(const UsageData* data) {
    if (data) {
        last_data = *data;
        last_data_received_ms = millis();   // state-machine staleness anchor
    }
    connected = true;
    // Live-clock anchor
    if (data && data->epoch) {
        clock_base    = data->epoch + (uint32_t)data->tz_off;
        clock_base_ms = millis();
        if (clock_style != CS_OFF) update_clock(true);
    }
    if (!active) { apply_colours(); last_session_pct = last_data.session_pct; return; }
    // While diagnostics is running, the scripted halo colour must not be
    // overwritten by a fresh usage payload.
    if (!diag_active) apply_colours();
    refresh_usage_info();

    // Surprised pop on big jump UP (more usage); bounce on big drop DOWN
    // (window reset → fresh headroom, feels like good news).
    float new_pct = last_data.valid ? last_data.session_pct : 0.0f;
    if (last_session_pct >= 0.0f && !sleeping) {
        float d = new_pct - last_session_pct;
        if (d >= 15.0f) do_surprised();
        else if (d <= -15.0f && bounce_start_ms == 0) bounce_start_ms = millis();
    }
    last_session_pct = new_pct;

    // Enter / leave sleep when the session limit is exhausted.
    bool ex = last_data.valid && (last_data.session_pct >= 100.0f ||
                                  last_data.weekly_pct  >= 100.0f);
    if (ex && !sleeping) {
        sleeping = true;
        do_yawn();                       // brief horizontal stretch before slit
        set_mood(M_SLEEP);
        if (obj_zzz) lv_obj_clear_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
        start_sleep_anim();
    } else if (!ex && sleeping) {
        sleeping = false;
        stop_sleep_anim();
        if (obj_zzz) lv_obj_add_flag(obj_zzz, LV_OBJ_FLAG_HIDDEN);
        set_mood(M_NEUTRAL);
    }
}

void emo2_tick(void) {
    if (!active) return;
    uint32_t now = millis();
    // Test-pct TTL — once expired, repaint with the real session_pct.
    if (test_pct_override >= 0.0f && (int32_t)(test_pct_expires_ms - now) <= 0) {
        test_pct_override   = -1.0f;
        test_pct_expires_ms = 0;
        apply_colours();
        refresh_usage_info();
    }
    // Clock visibility is gated by clock_style (not V_CLOCK any more) — see
    // apply_view(). When CS_SECONDS the label needs a fresh value every
    // second; for the other styles only on minute roll-over, which
    // update_clock() handles internally.
    if (clock_style != CS_OFF) update_clock(false);

    // Deferred-NVS flush. Setters mark the dirty flag + a timestamp; we
    // commit once the user has been idle for NVS_FLUSH_QUIET_MS. Cheap —
    // one branch when clean. Runs in the Arduino main-loop context (not
    // the BLE callback), so the flash erase doesn't block the BLE stack.
    if (nvs_dirty && (now - nvs_dirty_ms) >= NVS_FLUSH_QUIET_MS) {
        nvs_dirty = false;
        emo2_save_cfg_to_nvs();
    }

    // Phase A: ESP-side state-machine + rotation. Replaces daemon's
    // push_emo2_visuals on every TICK. No-op if manual_mode / OTA / diag /
    // cfg not yet loaded — safe to call always.
    emo2_tick_state_machine();

    // Pearls layout needs continuous tick so drops keep falling. Cheap —
    // only ~10 lv_obj position updates per tick. Throttled to 30ms.
    static uint32_t pearl_last_tick = 0;
    if (stats_layout == LAYOUT_TEAR_PEARLS && !ota_active &&
        (now - pearl_last_tick >= 30)) {
        pearl_last_tick = now;
        refresh_usage_info();
    }

    // Loading-mode continuous tick — while there's no Claude data
    // (disconnected / token expired / first boot), every layout needs a
    // periodic refresh so its chrome can animate as a looping loading
    // indicator. refresh_usage_info handles the synth-pct generation.
    // Pearls already get their own 30ms tick above; this one covers
    // bezel / columns / ribbon / chip / ECG (and is a cheap no-op when
    // we have real data and emo2_tick_state_machine handles the rest).
    static uint32_t loading_last_tick = 0;
    bool loading_mode = (!connected) || token_expired || (!last_data.valid) || data_stale();
    if (loading_mode && !ota_active &&
        stats_layout != LAYOUT_TEAR_PEARLS &&
        stats_layout != LAYOUT_OFF &&
        (now - loading_last_tick >= 60)) {
        loading_last_tick = now;
        refresh_usage_info();
    }

    // ECG monitor: scroll the waveform one sample per ~60ms. Spike frequency
    // tracks session_pct, amplitude tracks weekly_pct. Cheap — ~60-point
    // polyline update.
    if (stats_layout == LAYOUT_ECG && ecg_line && (now - ecg_last_tick >= 60)) {
        ecg_last_tick = now;
        // Spike period: 1800ms @ 0% → 400ms @ 100%.
        // Loading mode (no real data): drive sess/week from the same
        // triangle phase refresh_usage_info uses, so the trace stays
        // alive as a looping loading waveform.
        float sess, week;
        if ((!connected) || token_expired || !last_data.valid || data_stale()) {
            uint32_t ps = now % 2400;
            uint32_t pw = (now + 1200) % 3000;
            sess = (ps < 1200) ? (ps * 100.0f / 1200.0f)
                               : ((2400 - ps) * 100.0f / 1200.0f);
            week = (pw < 1500) ? (pw * 100.0f / 1500.0f)
                               : ((3000 - pw) * 100.0f / 1500.0f);
        } else {
            sess = last_data.session_pct;
            week = last_data.weekly_pct;
        }
        if (sess < 0) sess = 0; if (sess > 100) sess = 100;
        if (week < 0) week = 0; if (week > 100) week = 100;
        uint32_t spike_period = (uint32_t)(1800 - (sess / 100.0f) * 1400);
        if (spike_period < 200) spike_period = 200;
        int32_t amp = (int32_t)(6 + (week / 100.0f) * 22);
        ecg_phase_ms = (ecg_phase_ms + 60) % spike_period;
        float u = (float)ecg_phase_ms / (float)spike_period;
        // Classic P-QRS-T-ish shape compressed into the first 22% of cycle.
        int32_t dy = 0;
        if      (u > 0.05f && u < 0.10f) dy = (int32_t)(-amp * 0.4f * (u - 0.05f) / 0.05f);
        else if (u >= 0.10f && u < 0.13f) dy = (int32_t)(-amp * 0.4f + amp * 1.4f * (u - 0.10f) / 0.03f);
        else if (u >= 0.13f && u < 0.18f) dy = (int32_t)(amp * 1.0f - amp * 1.5f * (u - 0.13f) / 0.05f);
        else if (u >= 0.18f && u < 0.22f) dy = (int32_t)(-amp * 0.5f + amp * 0.5f * (u - 0.18f) / 0.04f);
        else if (u > 0.30f && u < 0.45f)  dy = (int32_t)(-amp * 0.25f * sinf((u - 0.30f) / 0.15f * 3.14159f));
        // Shift points left and append new on the right.
        for (int i = 0; i < ECG_POINTS - 1; i++) {
            ecg_pts[i].y = ecg_pts[i + 1].y;
        }
        ecg_pts[ECG_POINTS - 1].y = ECG_Y_MID + dy;
        lv_line_set_points(ecg_line, ecg_pts, ECG_POINTS);
    }

    // --- Reconnecting overlay: replace eyes with animated dots ---------
    // Skip while OTA is active — emo2_set_ota_progress owns obj_status then.
    if (!connected && !ota_active) {
        if (now - recon_last_ms >= 450) {
            recon_last_ms = now; recon_dots++;
            char buf[40];
            int n = recon_dots & 3;
            snprintf(buf, sizeof(buf), "%s%.*s", TR(STR_RECONNECT), n, "...");
            if (obj_status) lv_label_set_text(obj_status, buf);
        }
        return;
    }
    if (ota_active) return;

    // --- Diagnostics scripted sequence: cycles form + colour + motion ---
    // Unlike the old behaviour, we DON'T return here — motion handlers
    // (position chain + pulse_alt) below still run so eye-roll / wave /
    // shake actually animate. Only the random schedulers are paused.
    if (diag_active) {
        uint32_t dt = now - diag_start_ms;
        if (dt >= DIAG_DUR_MS) {
            diag_active = false;
            diag_step = -1;
            apply_colours();                       // restore real halo colour
            set_mood(M_NEUTRAL);
            mood_next_ms     = now + FORM_PACE(60000 + (esp_random() % 60000));
            blink_next_ms    = now + 1500;
            // Don't clear motion states here — let them finish their tails
            // gracefully via the normal handlers.
        } else {
            int8_t step = dt / 1000;
            if (step != diag_step && step < DIAG_STEPS) {
                diag_step = step;
                const DiagStep& s = DIAG_SEQ[step];
                set_mood(s.mood);
                // Apply the scripted halo colour to BOTH the silhouette AND
                // the halo layer — mirroring apply_colours(). Previously only
                // the halo was tinted; the cyan silhouette dominated so the
                // "diagnose" sequence looked like nothing was happening.
                lv_color_t tint = halo_color((float)s.pct);
                for (int i = 0; i < 2; i++) {
                    lv_obj_set_style_image_recolor    (eye_base[i], tint,         0);
                    lv_obj_set_style_image_recolor_opa(eye_base[i], LV_OPA_COVER, 0);
                    lv_obj_set_style_image_recolor    (eye_halo[i], tint,         0);
                    lv_obj_set_style_image_recolor_opa(eye_halo[i], LV_OPA_COVER, 0);
                }
                diag_apply_motion(s.motion, now);
            }
        }
    }

    if (sleeping) return;  // limit reached: animations stay on the sleep loop

    // --- Smooth random blink (2.5–6 s × anim_pace) ---------------------
    if (!diag_active && !manual_mode && now >= blink_next_ms) {
        do_blink();
        blink_next_ms = now + ANIM_PACE(2500 + (esp_random() % 3500));
    }

    // --- Halo heartbeat pulse — suppressed while PULSE_ALT runs --------
    if (!diag_active && !manual_mode && pulse_alt_start_ms == 0 && now >= heartbeat_next_ms) {
        do_heartbeat();
        heartbeat_next_ms = now + ANIM_PACE(4000 + (esp_random() % 2000));
    }

    // --- PULSE_ALT: out-of-phase scale on the two eyes -----------------
    if (pulse_alt_start_ms != 0) {
        uint32_t dt = now - pulse_alt_start_ms;
        if (dt >= PULSE_ALT_DUR_MS) {
            pulse_alt_start_ms = 0;
            // restore default scales
            for (int i = 0; i < 2; i++) {
                lv_image_set_scale(eye_base[i], BASE_SCALE);
                lv_image_set_scale(eye_halo[i], HALO_SCALE);
            }
        } else {
            float u = (float)dt / PULSE_ALT_DUR_MS;
            float phase = u * 6.2832f * 3.0f;             // 3 full cycles
            float lScale = 1.0f + 0.12f * sinf(phase);    // left  ±12 %
            float rScale = 1.0f + 0.12f * sinf(phase + 3.14159f);  // right opposite
            lv_image_set_scale(eye_base[0], (int)(BASE_SCALE * lScale));
            lv_image_set_scale(eye_halo[0], (int)(HALO_SCALE * lScale));
            lv_image_set_scale(eye_base[1], (int)(BASE_SCALE * rScale));
            lv_image_set_scale(eye_halo[1], (int)(HALO_SCALE * rScale));
        }
    }

    // --- Single-eye wink (every 15–30 s) -------------------------------
    if (!diag_active && !manual_mode && now >= wink_next_ms) {
        do_wink((int)(esp_random() & 1));
        wink_next_ms = now + ANIM_PACE(15000 + (esp_random() % 15000));
    }

    // helper: is a higher-priority motion already active?
    bool motion_busy = (eyeroll_start_ms || shake_start_ms ||
                        bounce_start_ms || nod_start_ms ||
                        wave_start_ms || lookaround_start_ms);

    // --- Schedule longer-form motions (paused during diag + manual) ---
    if (!diag_active && !manual_mode) {
        if (!motion_busy && now >= eyeroll_next_ms) {
            eyeroll_start_ms = now;
            eyeroll_next_ms  = now + 45000 + (esp_random() % 45000);
            motion_busy = true;
        }
        if (!motion_busy && now >= nod_next_ms) {
            nod_start_ms    = now;
            nod_next_ms     = now + 90000 + (esp_random() % 60000);
            motion_busy = true;
        }
        if (!motion_busy && now >= wave_next_ms) {
            wave_start_ms = now;
            wave_next_ms  = now + 75000 + (esp_random() % 60000);
            motion_busy = true;
        }
        if (!motion_busy && now >= lookaround_next_ms) {
            lookaround_start_ms = now;
            lookaround_next_ms  = now + 35000 + (esp_random() % 25000);
            motion_busy = true;
        }
        if (now >= confused_next_ms) {
            do_confused((int)(esp_random() & 1));
            confused_next_ms = now + 110000 + (esp_random() % 70000);
        }
        if (pulse_alt_start_ms == 0 && now >= pulse_alt_next_ms) {
            pulse_alt_start_ms = now;
            pulse_alt_next_ms  = now + 95000 + (esp_random() % 70000);
        }
    }

    // --- Position chain: eyeroll > shake > bounce > nod > lookaround ---
    bool pos_applied = false;
    if (eyeroll_start_ms != 0) {
        uint32_t dt = now - eyeroll_start_ms;
        if (dt >= EYEROLL_DUR_MS) {
            eyeroll_start_ms = 0;
            apply_eye_offset(0, 0);
        } else {
            float phase = ((float)dt / EYEROLL_DUR_MS) * 6.2832f;  // 2π
            int dx = (int)(7.0f * cosf(phase));
            int dy = (int)(4.5f * sinf(phase));
            apply_eye_offset(dx, dy);
        }
        pos_applied = true;
    } else if (shake_start_ms != 0) {
        uint32_t dt = now - shake_start_ms;
        if (dt >= SHAKE_DUR_MS) {
            shake_start_ms = 0;
            apply_eye_offset(0, 0);
        } else {
            // High-freq side-to-side ±7 px (was ±3 — too subtle to register)
            float amp = 7.0f * (1.0f - (float)dt / SHAKE_DUR_MS);
            int dx = (int)(amp * sinf(dt * 0.08f));
            apply_eye_offset(dx, 0);
        }
        pos_applied = true;
    } else if (bounce_start_ms != 0) {
        // Two-bounce decay: |sin(2πt/T·2)| with amplitude tapering linearly
        uint32_t dt = now - bounce_start_ms;
        if (dt >= BOUNCE_DUR_MS) {
            bounce_start_ms = 0;
            apply_eye_offset(0, 0);
        } else {
            float u = (float)dt / BOUNCE_DUR_MS;
            float amp = 12.0f * (1.0f - u);   // was 6 — bumped for visibility
            int dy = (int)(-amp * fabsf(sinf(u * 6.2832f * 2.0f)));
            apply_eye_offset(0, dy);
        }
        pos_applied = true;
    } else if (nod_start_ms != 0) {
        // Slow gentle nod: ±7 px (was ±3) over 2 full cycles in NOD_DUR_MS
        uint32_t dt = now - nod_start_ms;
        if (dt >= NOD_DUR_MS) {
            nod_start_ms = 0;
            apply_eye_offset(0, 0);
        } else {
            float u = (float)dt / NOD_DUR_MS;
            int dy = (int)(7.0f * sinf(u * 6.2832f * 2.0f));
            apply_eye_offset(0, dy);
        }
        pos_applied = true;
    } else if (wave_start_ms != 0) {
        // WAVE: 2.5 horizontal cycles, ±9 px sine drift over WAVE_DUR_MS.
        uint32_t dt = now - wave_start_ms;
        if (dt >= WAVE_DUR_MS) {
            wave_start_ms = 0;
            apply_eye_offset(0, 0);
        } else {
            float u = (float)dt / WAVE_DUR_MS;
            int dx = (int)(9.0f * sinf(u * 6.2832f * 2.5f));
            apply_eye_offset(dx, 0);
        }
        pos_applied = true;
    } else if (lookaround_start_ms != 0) {
        uint32_t dt = now - lookaround_start_ms;
        if (dt >= LOOKAROUND_DUR_MS) {
            lookaround_start_ms = 0;
            apply_eye_offset(0, 0);
        } else {
            // 4 phases: left-hold (0..400) → center (400..600) → right-hold
            // (600..1000) → return (1000..1600). 6 px lateral throw.
            int dx;
            if (dt < 400)       dx = -6;
            else if (dt < 600)  dx = -6 + (int)((dt - 400) * 6 / 200);  // -6 → 0
            else if (dt < 1000) dx = +6;
            else                dx = +6 - (int)((dt - 1000) * 6 / 600);  // +6 → 0
            apply_eye_offset(dx, 0);
        }
        pos_applied = true;
    }

    if (!pos_applied && !diag_active && !manual_mode && now >= saccade_next_ms) {
        int dx = ((int)(esp_random() % 5)) - 2;
        int dy = ((int)(esp_random() % 3)) - 1;
        apply_eye_offset(dx, dy);
        saccade_next_ms = now + ANIM_PACE(1500 + (esp_random() % 3500));
    }

    if (!diag_active && !manual_mode && now >= curious_next_ms) {
        if (lookaround_start_ms == 0 && eyeroll_start_ms == 0 &&
            shake_start_ms == 0) {
            lookaround_start_ms = now;
        } else {
            saccade_next_ms = now;
        }
        curious_next_ms = now + ANIM_PACE(30000 + (esp_random() % 30000));
    }

    // --- Auto mood change (1–3 min) — skip SLEEP/LOVE/CROSS and the
    //     "manual-only" forms (OVAL_TALL/DIAMOND/PUPIL_LEFT). -------------
    if (!diag_active && !manual_mode && now >= mood_next_ms) {
        uint8_t next = cur_mood;
        for (int tries = 0; tries < 16 &&
             (next == cur_mood || next == M_SLEEP || next == M_LOVE ||
              next == M_CROSS || next == M_OVAL_TALL ||
              next == M_DIAMOND || next == M_PUPIL_LEFT); tries++) {
            next = esp_random() % M_COUNT;
        }
        set_mood(next);
        mood_next_ms = now + FORM_PACE(60000 + (esp_random() % 120000));
    }

    // --- Warning flicker + anger shake at high usage (≥80 %) -----------
    float pct = last_data.valid ? last_data.session_pct : 0.0f;
    if (!diag_active && !manual_mode && pct >= 80.0f && now >= warning_next_ms) {
        do_warning_flicker();
        if (shake_start_ms == 0 && eyeroll_start_ms == 0 &&
            lookaround_start_ms == 0) {
            shake_start_ms = now;
        }
        warning_next_ms = now + ANIM_PACE(8000 + (esp_random() % 4000));
    } else if (pct < 80.0f) {
        warning_next_ms = now + 8000;
    }
}
