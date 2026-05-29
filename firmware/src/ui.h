#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_EMO,
    SCREEN_BLUETOOTH,
    SCREEN_EMO2,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);

// Called after rotation change to recalculate layout positions
void ui_relayout(void);

// Re-apply all localized labels after a language switch
void ui_relang(void);

// Trigger animation change on current screen (button 1-click)
void ui_trigger_animation(void);
