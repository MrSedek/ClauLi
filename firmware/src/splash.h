#pragma once
#include <stdint.h>
#include <lvgl.h>

// Initialize splash module. No canvas buffer — renders directly via GFX.
void splash_init(void);

// Advance animation frame if hold time elapsed. Call from main loop.
// Only renders when splash is the active screen.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash screen.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering.
bool splash_is_active(void);

// Called after rotation change — splash re-renders at new orientation.
void splash_relayout(void);
