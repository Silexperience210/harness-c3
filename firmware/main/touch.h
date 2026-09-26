// Capacitive touch: Hynitron CST816S / CST816T / CST816D over I2C (the
// ESP32-2424S012C carries a CST816D), exposed to LVGL as a pointer device.
//
// Pins and axis transforms come exclusively from Kconfig (menu "Touch").
// Compiled to stubs when CONFIG_HARNESS_TOUCH_ENABLE is off.
#pragma once

#include <stdbool.h>

#include "lvgl.h"

// Reset the controller, open the I2C bus, identify the chip and disable its
// auto-sleep. Returns false only when the I2C bus itself cannot be set up
// (bad pins): a controller that does not answer the ID probe is assumed to
// be asleep (some CST816 firmwares only wake on a first touch) and stays
// enabled. With CONFIG_HARNESS_TOUCH_ENABLE off it returns false.
bool touch_init(void);

// Register the LVGL pointer input device on `disp`. Call with the LVGL lock
// held. Returns NULL when touch_init() did not succeed.
lv_indev_t *touch_register_lvgl(lv_display_t *disp);

// Called (UI task, LVGL lock held) on every finger-DOWN edge. Returning true
// means "that touch only woke the screen": the whole stroke is swallowed —
// LVGL sees nothing until the finger lifts — so a tap on a dark screen never
// presses whatever happens to be under it.
void touch_set_wake_hook(bool (*hook)(void));

// The detected controller ("cst816s", "cst816t", "cst816d", "cst816") for
// `hello.hw`, or NULL when no touch panel answered.
const char *touch_chip_name(void);
