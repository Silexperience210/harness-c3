// Up to two push buttons, active-low (SPEC.md §2). Either may be absent
// (Kconfig pin -1): on the ESP32-2424S012C only the BOOT button exists and is
// configured as button B; the touch panel does everything else.
//
// 10 ms poll, 30 ms debounce, short/long (≥600 ms) events posted to a queue
// the UI task drains. BTN_A short = next, long = confirm; BTN_B short =
// back / cancel, long = screen off / on.
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    BTN_EVENT_A_SHORT = 0,
    BTN_EVENT_A_LONG,
    BTN_EVENT_B_SHORT,
    BTN_EVENT_B_LONG,
} btn_event_t;

// Install the GPIO inputs and start the polling task. `queue` receives one
// btn_event_t per completed press; it is owned by the caller (the UI).
// Returns true with nothing to do when no button is configured.
bool buttons_init(QueueHandle_t queue);
