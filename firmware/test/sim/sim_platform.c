// Host simulator platform: everything ui.c and cable_client.c expect from
// ESP-IDF, FreeRTOS, the panel, the touch controller and the USB link —
// replaced by deterministic fakes that the scenario (sim_main.c) drives.
//
//   clock    esp_timer_get_time()/cable_platform_millis() = a simulated clock
//   panel    a 240×240 RGB565 framebuffer; screenshots are written as PPM
//   touch    an LVGL pointer indev fed by sim_touch(); same wake-hook
//            semantics as touch.c (a stroke that wakes the screen is swallowed)
//   link     cable_link_send() records every outbound JSON frame

#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "buzzer.h"
#include "cable_link.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "settings.h"
#include "touch.h"

int sim_verbose;

// ── clock ───────────────────────────────────────────────────────────────────

static int64_t s_now_us = 1000000;
int64_t esp_timer_get_time(void) { return s_now_us; }
uint32_t cable_platform_millis(void) { return (uint32_t)(s_now_us / 1000); }
void sim_clock_advance_ms(uint32_t ms) { s_now_us += (int64_t)ms * 1000; lv_tick_inc(ms); }

size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 123456; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 100000; }

// ── FreeRTOS shim ───────────────────────────────────────────────────────────

struct sim_queue {
    uint8_t *buf;
    UBaseType_t len, size, head, count;
};

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    struct sim_queue *q = calloc(1, sizeof(*q));
    q->buf = calloc(len, item_size);
    q->len = len;
    q->size = item_size;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->count == q->len) return pdFALSE;
    memcpy(q->buf + ((q->head + q->count) % q->len) * q->size, item, q->size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (q->count == 0) return pdFALSE;
    memcpy(item, q->buf + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->len;
    q->count--;
    return pdTRUE;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = NULL;
    return pdPASS;   // never started: the simulator calls ui_sim_step()
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }

// ── panel ───────────────────────────────────────────────────────────────────

static uint16_t     s_fb[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static lv_display_t *s_disp;
static int          s_backlight;
static uint8_t      s_draw_buf[DISPLAY_WIDTH * 48 * 2];

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++) {
        for (int x = a->x1; x <= a->x2; x++) s_fb[y * DISPLAY_WIDTH + x] = *src++;
    }
    lv_display_flush_ready(disp);
}

bool sim_display_init(void)
{
    lv_init();
    s_disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    return true;
}

lv_display_t *display_get(void) { return s_disp; }
bool display_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
void display_unlock(void) {}
void display_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_backlight = percent;
}
int display_get_backlight(void) { return s_backlight; }

// PPM (P6) of what the round panel shows: pixels outside the circle are
// painted as the bezel so the screenshot reads like the device.
bool sim_screenshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    const float bl = s_backlight / 100.0f;
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            const float dx = x - 119.5f, dy = y - 119.5f;
            uint8_t rgb[3] = { 40, 40, 44 };
            if (dx * dx + dy * dy <= 120.0f * 120.0f) {
                const uint16_t c = s_fb[y * DISPLAY_WIDTH + x];
                rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31 * bl);
                rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63 * bl);
                rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31 * bl);
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

// ── touch ───────────────────────────────────────────────────────────────────

static struct { int x, y; bool down; } s_touch;
static bool   s_pressed, s_swallow;
static bool (*s_wake_hook)(void);

void sim_touch(int x, int y, bool down)
{
    s_touch.x = x;
    s_touch.y = y;
    s_touch.down = down;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const bool was = s_pressed;
    s_pressed = s_touch.down;
    if (s_pressed && !was && s_wake_hook && s_wake_hook()) s_swallow = true;   // as touch.c
    if (!s_pressed) s_swallow = false;
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_pressed && !s_swallow ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool touch_init(void) { return true; }
lv_indev_t *touch_register_lvgl(lv_display_t *disp)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, disp);
    return indev;
}
const char *touch_chip_name(void) { return "cst816d"; }
void touch_set_wake_hook(bool (*hook)(void)) { s_wake_hook = hook; }

// ── settings, buzzer ────────────────────────────────────────────────────────

static int s_brightness = CONFIG_HARNESS_BACKLIGHT_DEFAULT;
int s_brightness_saves;
void settings_load(void) {}
int settings_brightness(void) { return s_brightness; }
void settings_set_brightness(int percent)
{
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    if (percent != s_brightness) s_brightness_saves++;
    s_brightness = percent;
}

int sim_beeps;
bool buzzer_init(void) { return true; }
void buzzer_beep(buzzer_pattern_t pattern) { (void)pattern; sim_beeps++; }

// ── USB link ────────────────────────────────────────────────────────────────

char sim_sent[SIM_SENT_MAX][2048];
int  sim_sent_count;

bool cable_link_send(uint8_t type, const uint8_t *payload, size_t len)
{
    if (type != CABLE_TYPE_JSON || sim_sent_count >= SIM_SENT_MAX) return true;
    if (len >= sizeof(sim_sent[0])) len = sizeof(sim_sent[0]) - 1;
    memcpy(sim_sent[sim_sent_count], payload, len);
    sim_sent[sim_sent_count][len] = '\0';
    sim_sent_count++;
    return true;
}
void cable_link_set_log_framing(bool on) { (void)on; }
bool cable_link_host_present(void) { return true; }
void cable_link_counters(uint32_t *c, uint32_t *d) { if (c) *c = 0; if (d) *d = 0; }
