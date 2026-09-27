// Persistent user settings (NVS namespace "harness"). Tiny on purpose: what
// the person chooses ON the dial and expects to survive a replug.
#pragma once

#include <stdbool.h>

// Load from NVS (nvs_flash_init() must have run). Missing keys take the
// Kconfig defaults. Never fails: a broken NVS means defaults, not no dial.
void settings_load(void);

// Backlight brightness, 5–100 %.
int  settings_brightness(void);
// Set (clamped) and persist. Writes only when the value actually changed.
void settings_set_brightness(int percent);

// Lamp mode: the screen used as a light. Level 5–100 %, warmth 0 (candle)
// to LAMP_WARMTHS - 1 (cool daylight). Saved together, once per gesture.
#define LAMP_WARMTHS 5
int  settings_lamp_level(void);
int  settings_lamp_warmth(void);
void settings_set_lamp(int level, int warmth);
