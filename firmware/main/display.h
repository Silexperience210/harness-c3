// Display driver glue: GC9A01 round 240×240 SPI panel via esp_lcd, LVGL v9
// port, LEDC backlight.
//
// Pins, SPI clock, orientation and colour order come exclusively from
// Kconfig (menu "Harness C3 Configuration", board defaults for the
// ESP32-2424S012C) — no pin number appears here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240

// Bring up the SPI bus, the GC9A01 panel, the LVGL display with its two DMA
// draw buffers (240×48 RGB565 each, ~45 KB total — SPEC.md §3 RAM budget)
// and the esp_timer tick. The backlight is left OFF: the panel's RAM holds
// noise until LVGL's first frame lands, so the UI switches the light on only
// after it has drawn (ui_init). Returns false on any step's failure, leaving
// the firmware alive (the cable link still works headless).
bool display_init(void);

// Re-applies the panel orientation (invert / swap / mirror) from Kconfig.
//
// The panel is write-only (the SPI bus has no MISO), so the orientation cannot
// be read back — only written, and written again later. Two failure modes seen
// on this clone GC9A01: it ignores the first commands while it is still waking
// up (cold power-up), and it can latch a glitch on the SPI lines while the
// ESP32 resets. In both cases it silently keeps its own default scan direction
// and the picture comes out mirrored. Re-applying the values is idempotent, so
// display_init() does it once, a slow timer repeats it over the first two
// seconds, and the UI does it again on every screen change (a rewrite during a
// transition is invisible).
void display_reassert_orientation(void);

// The LVGL display (NULL before display_init succeeded).
lv_display_t *display_get(void);

// The LVGL lock (recursive). LVGL is single-threaded and runs with no OS
// layer: every LVGL call from any task must hold this.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

// Backlight duty, 0–100 %. LEDC PWM on CONFIG_HARNESS_PIN_LCD_BL, active high.
void display_set_backlight(int percent);
int  display_get_backlight(void);
