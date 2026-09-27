// LVGL configuration for the HOST simulator (test/sim). Mirrors the device's
// sdkconfig.defaults, except the pool: LVGL objects are ~1.8x larger on a
// 64-bit host, so the pool is doubled — the simulator checks BEHAVIOUR and
// LAYOUT; the device's RAM budget is checked by `idf.py size` in CI.
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
/* Lamp mode's radial glow (as in sdkconfig.defaults). */
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1
#define LV_GRADIENT_MAX_STOPS 3

#ifndef LV_MEM_SIZE
#define LV_MEM_SIZE (96 * 1024U)
#endif
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 20
#define LV_DPI_DEF 130

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Stricter than the device: every misuse of the API trips here, in CI.
#define LV_USE_ASSERT_NULL   1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE  1
#define LV_USE_ASSERT_OBJ    1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#endif
