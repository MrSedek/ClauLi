#pragma once
#include <stdint.h>
#include <lvgl.h>
#include "data.h"

// Initialize emo screen (creates LVGL objects — call once in ui_init)
void emo_init(void);

// Show emo screen, apply current expression
void emo_show(void);

// Hide emo screen
void emo_hide(void);

// Drive animations (blink, look-around) — call every loop
void emo_tick(void);

// Button short-press: cycle the 10 view modes (5 layers, then the same
// 5 with a top clock). Emotions auto-change on their own random timer.
void emo_next_mood(void);

// Jump straight to a view mode 0..9 (used by restore + self-test)
void emo_set_view(uint8_t idx);

// Current view mode 0..9 (so the caller can persist it)
uint8_t emo_get_view(void);

// Force an immediate random emotion change (used by the self-test build)
void emo_next_emotion(void);

// Push latest usage data (eye color + limit text/bars)
void emo_set_usage(const UsageData* data);

// Tell the emo screen whether the BLE link is up. When down, the usage
// text is replaced with an animated "Reconnecting…" message.
void emo_set_connected(bool connected);

// Re-apply localized text after a language switch
void emo_relang(void);

// True when emo screen is visible
bool emo_is_active(void);

// Called after rotation change
void emo_relayout(void);
