// Display driver glue: GC9A01 round 240×240 SPI panel via esp_lcd, LVGL v9
// port, LEDC backlight. See display.h.

#include "display.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "draw/sw/lv_draw_sw.h"   // lv_draw_sw_rgb565_swap()

static const char *TAG = "display";

// Kconfig bools that are OFF are not defined at all: turn each into 0/1.
#ifdef CONFIG_HARNESS_LCD_MIRROR_X
#define LCD_MIRROR_X 1
#else
#define LCD_MIRROR_X 0
#endif
#ifdef CONFIG_HARNESS_LCD_MIRROR_Y
#define LCD_MIRROR_Y 1
#else
#define LCD_MIRROR_Y 0
#endif
#ifdef CONFIG_HARNESS_LCD_SWAP_XY
#define LCD_SWAP_XY 1
#else
#define LCD_SWAP_XY 0
#endif
#ifdef CONFIG_HARNESS_LCD_INVERT_COLORS
#define LCD_INVERT 1
#else
#define LCD_INVERT 0
#endif
#ifdef CONFIG_HARNESS_LCD_BGR
#define LCD_BGR 1
#else
#define LCD_BGR 0
#endif

// Two partial-frame draw buffers in DMA-capable internal RAM. 240×48 RGB565
// = 23 040 B each, ≈45 KB for the pair — the SPEC.md §3 RAM budget. Double
// buffering: LVGL renders into one while the SPI DMA sends the other.
#define DRAW_BUF_HEIGHT 48
#define DRAW_BUF_BYTES  (DISPLAY_WIDTH * DRAW_BUF_HEIGHT * 2)

// LVGL tick period via esp_timer.
#define TICK_PERIOD_MS 2

#define LEDC_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_TIMER    LEDC_TIMER_0
#define LEDC_CHANNEL  LEDC_CHANNEL_0
#define LEDC_FREQ_HZ  5000
#define LEDC_DUTY_RES LEDC_TIMER_10_BIT

static esp_lcd_panel_handle_t s_panel;
static lv_display_t          *s_disp;
static SemaphoreHandle_t      s_lvgl_lock;
static int                    s_backlight = -1;

// Flush completion. The SPI "transfer done" callback runs in ISR context: it
// counts the transfer down and wakes whoever waits in flush_wait_cb.
static SemaphoreHandle_t s_flush_done;
static atomic_int        s_inflight;

// ── LVGL port ───────────────────────────────────────────────────────────────

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    atomic_fetch_sub(&s_inflight, 1);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    // LVGL keeps RGB565 little-endian in memory; the panel wants MSB first.
    // LVGL v9 has no LV_COLOR_16_SWAP Kconfig — this is the v9 swap.
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h));
    atomic_fetch_add(&s_inflight, 1);
    if (esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                                  px_map) != ESP_OK) {
        atomic_fetch_sub(&s_inflight, 1);   // nothing queued, nothing will complete
    }
}

// Called by LVGL (UI task) whenever it needs a draw buffer back. BLOCKS on a
// semaphore instead of LVGL's default busy-spin: on a single-core chip the
// spin would starve every lower-priority task (the USB reader included) for
// the whole DMA transfer. With a wait callback set, LVGL treats the flush as
// complete when this returns — so wait for EVERY queued transfer.
static void flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    while (atomic_load(&s_inflight) > 0) {
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(50)) != pdTRUE) {
            // A lost completion would hang the UI forever; a 50 ms transfer
            // is impossible at 40–80 MHz (full frame < 25 ms), so resync.
            if (atomic_load(&s_inflight) > 0) {
                ESP_LOGW(TAG, "flush timeout, %d transfer(s) outstanding", atomic_load(&s_inflight));
                atomic_store(&s_inflight, 0);
            }
        }
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(TICK_PERIOD_MS);
}

lv_display_t *display_get(void) { return s_disp; }

bool display_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_lock) return false;
    return xSemaphoreTakeRecursive(s_lvgl_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void display_unlock(void)
{
    if (s_lvgl_lock) xSemaphoreGiveRecursive(s_lvgl_lock);
}

// ── backlight ───────────────────────────────────────────────────────────────

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t channel = {
        .gpio_num = CONFIG_HARNESS_PIN_LCD_BL,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");
    return ESP_OK;
}

void display_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == s_backlight) return;
    s_backlight = percent;
    // Perceived brightness is roughly quadratic in duty: square the percentage
    // so the low end of the slider is usable instead of all "bright".
    const uint32_t max = (1u << LEDC_DUTY_RES) - 1;
    const uint32_t duty = (uint32_t)percent * (uint32_t)percent * max / 10000u;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, percent > 0 && duty == 0 ? 1 : duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

int display_get_backlight(void) { return s_backlight < 0 ? 0 : s_backlight; }

// ── init ────────────────────────────────────────────────────────────────────

bool display_init(void)
{
    // Backlight first, at duty 0: the panel shows noise while being
    // initialised, and that flash reads as a fault.
    if (backlight_init() != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed");
        return false;
    }
    display_set_backlight(0);

    lv_init();
    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_lvgl_lock || !s_flush_done) {
        ESP_LOGE(TAG, "no memory for the LVGL lock");
        return false;
    }

    uint8_t *buf1 = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *buf2 = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "no DMA RAM for draw buffers (%d B each)", DRAW_BUF_BYTES);
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        return false;
    }

    ESP_LOGI(TAG, "SPI bus: sck=%d mosi=%d cs=%d dc=%d rst=%d bl=%d @ %d Hz",
             CONFIG_HARNESS_PIN_SPI_SCK, CONFIG_HARNESS_PIN_SPI_MOSI, CONFIG_HARNESS_PIN_LCD_CS,
             CONFIG_HARNESS_PIN_LCD_DC, CONFIG_HARNESS_PIN_LCD_RST, CONFIG_HARNESS_PIN_LCD_BL,
             CONFIG_HARNESS_LCD_SPI_FREQ_HZ);
    const spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_HARNESS_PIN_SPI_SCK,
        .mosi_io_num = CONFIG_HARNESS_PIN_SPI_MOSI,
        .miso_io_num = -1,    // the panel is write-only
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DRAW_BUF_BYTES,
    };
    if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return false;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_HARNESS_PIN_LCD_DC,
        .cs_gpio_num = CONFIG_HARNESS_PIN_LCD_CS,
        .pclk_hz = CONFIG_HARNESS_LCD_SPI_FREQ_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io) != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed");
        return false;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        // -1 on the ESP32-2424S012C: the panel reset is tied to EN, and the
        // driver falls back to the SWRESET command.
        .reset_gpio_num = CONFIG_HARNESS_PIN_LCD_RST,
#if LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,   // RGB565
    };
    if (esp_lcd_new_panel_gc9a01(io, &panel_config, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "GC9A01 panel init failed");
        return false;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "GC9A01 did not accept its init sequence");
        return false;
    }
    // IPS GC9A01 modules are inverted by default; without this every colour
    // reads as its complement.
    esp_lcd_panel_invert_color(s_panel, LCD_INVERT);
    esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY);
    esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return false;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    lv_display_set_buffers(s_disp, buf1, buf2, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick = NULL;
    if (esp_timer_create(&tick_args, &tick) != ESP_OK ||
        esp_timer_start_periodic(tick, TICK_PERIOD_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL tick timer failed");
        return false;
    }

    ESP_LOGI(TAG, "GC9A01 %dx%d up (mirror x=%d y=%d swap=%d inv=%d bgr=%d), 2x%d B draw buffers",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, LCD_MIRROR_X, LCD_MIRROR_Y, LCD_SWAP_XY, LCD_INVERT,
             LCD_BGR, DRAW_BUF_BYTES);
    return true;
}
