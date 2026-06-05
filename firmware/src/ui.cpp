#include "ui.h"
#include "emo2.h"
#include <lvgl.h>
#include "display_cfg.h"
#include "theme.h"

// ─────────────────────────────────────────────────────────────────────────────
// ClauLi now renders a SINGLE screen — the emo2 character (the "heroes":
// ClauLi / Pixl / Old-TV). The legacy Usage ("Использование"), Claude (splash)
// and Bluetooth screens — and the first-gen `emo` screen — were removed. ui.cpp
// is kept as a thin shim that forwards the historical screen API to emo2 so
// main.cpp / ota.cpp need no churn.
//
// Driving responsibilities (unchanged, to avoid double-calls):
//   • emo2_tick()        ← ui_tick_anim()            (sole per-loop tick)
//   • emo2_set_usage()   ← ui_update()
//   • emo2_set_connected ← main.cpp (debounced)      — NOT here
//   • emo2_relayout()    ← main.cpp (action_set_rotation_persisted) — NOT here
// ─────────────────────────────────────────────────────────────────────────────

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, THEME_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    emo2_init();
}

void ui_update(const UsageData* data) {
    if (!data || !data->valid) return;
    emo2_set_usage(data);
}

void ui_tick_anim(void) {
    emo2_tick();
}

void ui_show_screen(screen_t screen) {
    (void)screen;            // only emo2 exists now
    emo2_show();
}

void ui_cycle_screen(void) {
    // No screens left to cycle — the button short-press now cycles the emo2
    // VIEW mode (text / bars / clock) instead, which is the most useful action.
    emo2_next_view();
}

screen_t ui_get_current_screen(void) {
    return SCREEN_EMO2;
}

void ui_trigger_animation(void) {
    emo2_next_view();
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    // The Bluetooth status screen was removed; emo2's connected-state is driven
    // (debounced) from main.cpp, so there is nothing to render here.
    (void)state; (void)name; (void)mac;
}

void ui_relayout(void) {
    // emo2 layout reflow is invoked directly from main.cpp after a rotation
    // change (emo2_relayout); nothing else to reposition.
}

void ui_relang(void) {
    emo2_relang();
}
