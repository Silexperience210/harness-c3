// UI: LVGL v9 screens for the 240×240 round touch panel (SPEC.md §6).
//
//   boot → not-connected → home (agent carousel: swipe ←/→, tap = open on the
//   computer, ↓ = settings, ↑ = scrollpad) → question screen (tap options,
//   ✓ / ✕) — plus toasts, a pulsing status ring for unanswered questions, a
//   STOP control for running turns and screen dimming / sleep.
//
// Round-panel rule: all critical text stays inside the inscribed square
// (~170×170 centred); controls hug the circle.
//
// ui_init() creates the screens and the UI task, and registers this UI as
// the cable_client event sink. Buttons post to ui_button_queue() (buttons.c).
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Requires display_init() to have succeeded. `fw_version` is printed on the
// boot and settings screens (the running image's version, e.g. "0.2.0-c3").
// `has_touch` selects touch hints vs button hints. Draws the boot screen and
// only THEN switches the backlight on (the panel RAM is noise until then).
bool ui_init(const char *fw_version, bool has_touch);

// The queue buttons_init() posts btn_event_t to.
QueueHandle_t ui_button_queue(void);
