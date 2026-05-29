#pragma once
#include <stdint.h>
#include <lvgl.h>
#include "data.h"

// Second-generation emo screen ("emo 2.0"). Same robot-eye personality as
// emo.cpp but rendered with 3 stacked LVGL layers per eye —
// halo (96×96 blurred bloom) + base (64×64 anti-aliased silhouette) +
// procedural specular highlight — to look closer to the Living.ai EMO.
//
// Hybrid colour:
//   • base+specular are always the EMO-cyan brand colour;
//   • halo lerps cyan → amber → red by session_pct (warning at 70 %, alert at 90 %)
//     so the usage metric still reads at a glance.

void emo2_init(void);
void emo2_show(void);
void emo2_hide(void);
void emo2_tick(void);
void emo2_set_usage(const UsageData* data);
void emo2_set_connected(bool connected);
void emo2_relang(void);
void emo2_relayout(void);
bool emo2_is_active(void);

// Manually advance to a new random mood (skip SLEEP + LOVE). Used by CTRL
// 0x08 "next anim" from the web UI.
void emo2_next_emotion(void);

// View cycling — like emo.cpp: 5 content layers × clock on/off = 10 modes.
// BOOT short-press calls emo2_next_view(); the chosen index is persisted to
// NVS by main.cpp.
void    emo2_next_view(void);
uint8_t emo2_get_view(void);
void    emo2_set_view(uint8_t idx);

// Run an 8-second scripted diagnostics sequence on the emo2 screen — cycles
// through every mood and across the full halo colour range so the user can
// visually verify the LCD/animations are working. Triggered from the web
// "Diagnose" button (CTRL 0x0A).
void emo2_run_diagnostics(void);

// Force a specific form (mood index 0..12 — see mood_t in emo2.cpp). Used
// by the form dropdown on the web UI via CTRL 0x50 + idx.
void emo2_set_mood_idx(uint8_t idx);

// Trigger a specific operation by its OP_* index (0..14 — see DiagMotion
// in emo2.cpp). Used by the op buttons on the web UI via CTRL 0x60 + idx.
void emo2_trigger_op(uint8_t op);

// State signals from the daemon. The daemon owns auth state and the manual
// toggle; the firmware just reacts to the CTRL bytes.
void emo2_set_token_expired(bool expired);   // CTRL 0x18 / 0x19
void emo2_set_manual_mode  (bool manual);    // CTRL 0x1A / 0x1B

// Halo colour override — CTRL 0x1C (cyan) / 0x1D (amber) / 0x1E (red) /
// 0x1F (auto by session_pct, the default).
void emo2_set_color_override(uint8_t mode);  // 0=cyan 1=amber 2=red 0xFF=auto

// Layout-fill colour override — independent palette for bezel/columns/
// ribbon/pearls/chip/classic fills. Same encoding (0=cyan 1=amber 2=red
// 0xFF=auto by pct). CTRL 0x4D/0x4E/0x4F/0x50. Persisted in NVS (`e2lc`).
void emo2_set_layout_color_override(uint8_t mode);
uint8_t emo2_get_layout_color_override(void);

// Clock-style picker. 0=off 1=minimal 2=big 3=mono 4=dot. CTRL 0x48-0x4C.
// Persisted in NVS (`e2cs`). off ⇒ clock never shown; else V_CLOCK still
// gates per view-mode.
void emo2_set_clock_style(uint8_t style);
uint8_t emo2_get_clock_style(void);

// Re-position every absolute-positioned layout object using the new
// scr_w()/scr_h() (driven by current_rotation). Called from main.cpp after
// gfx->setRotation + lv_display_set_resolution. Cheap — just calls
// lv_obj_set_pos/set_size on a fixed list of objects.
void emo2_relayout(void);

// OTA progress overlay. Called from ota.cpp during a BLE firmware update.
// state: 0=begin (init UI), 1=in-progress (use pct), 2=done, 3=error.
// pct: 0..100 — only used when state==1.
void emo2_set_ota_progress(uint8_t state, uint8_t pct);

// % stats layout picker (CTRL 0x20–0x23).
// 0=off (eyes only), 1=bezel_orbit, 2=twin_columns, 3=hud_ribbon.
void emo2_set_stats_layout(uint8_t layout);
uint8_t emo2_get_stats_layout(void);

// Usage text mode for the info labels (CTRL 0x44–0x47). LEGACY — calls
// decompose into (text_source, text_format) below for actual rendering.
// 0=none (hide), 1=pct only, 2=reset-time only, 3=both (default).
void emo2_set_text_mode(uint8_t mode);
uint8_t emo2_get_text_mode(void);

// Per-layout text axis split (Phase A6). source = which of (session, weekly,
// both, off) is displayed; format = how it's rendered (%, %+time, time).
// CTRL 0x51-0x54 for source; 0x55-0x57 for format.
void emo2_set_text_source(uint8_t src);    // TEXT_SRC_OFF/SESSION/WEEKLY/BOTH
uint8_t emo2_get_text_source(void);
void emo2_set_text_format(uint8_t fmt);    // TEXT_FMT_PCT/PCT_RESET/RESET
uint8_t emo2_get_text_format(void);

// Text placement — which Y-band the overlay text(s) sit in.
// CTRL 0x5C/0x5D/0x5E for top/middle/bottom. Persisted in NVS (`e2tp`).
void emo2_set_text_placement(uint8_t p);   // 0=top, 1=middle (default), 2=bottom
uint8_t emo2_get_text_placement(void);

// ─── State-machine + per-state config (Phase A) ────────────────────────────
// The ESP now owns the per-state form/op/color rotation. Daemon ships a
// single JSON config blob on connect + on web edits; firmware persists it
// to NVS, derives the active state from local signals (connected /
// token_expired / data freshness), and rotates forms/ops accordingly.

// Apply a JSON config blob received from the daemon over the RX channel.
// Expected shape: {"cfg": {"states": {...}, "layouts": {...}}}
// Returns true if the blob was recognised and applied. Saves to NVS.
bool emo2_apply_cfg_json(const char* json);

// Mark the moment a fresh data payload arrived. Used by state derivation
// to fall back to CONNECTING when data goes stale (>2.5× poll interval).
void emo2_set_data_received(void);
