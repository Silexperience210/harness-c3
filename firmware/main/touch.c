// CST816x capacitive touch over I2C → LVGL pointer indev. See touch.h.
//
// Register map (Hynitron CST816S datasheet; the T and D variants share it):
//   0x01 GestureID  0x02 FingerNum  0x03 XposH (bits 3:0; 7:6 = event)
//   0x04 XposL      0x05 YposH      0x06 YposL
//   0xA7 ChipID (0xB4 = CST816S, 0xB5 = CST816T, 0xB6 = CST816D)
//   0xFE DisAutoSleep (non-zero = never enter standby)
//
// THE SLEEP TRAP. Left alone, the controller drops into standby a few
// seconds after the last touch and then NACKs every I2C read until a finger
// wakes it. The ESP-IDF I2C driver logs each NACK at ERROR level — which,
// once a session is up, becomes a LOG frame to the daemon 50 times a second.
// So: auto-sleep is disabled right after reset, the driver's own log tag is
// silenced after the probe, and a failed read is simply "not touched".
//
// INT is used as a hint, not a dependency: while the glass is idle the
// controller is read only when INT fired, plus a 100 ms safety poll (some
// CST816 firmwares pulse INT differently) — so a mis-wired INT degrades to
// polling instead of to a dead touchscreen.

#include "touch.h"
#include "sdkconfig.h"

#if !CONFIG_HARNESS_TOUCH_ENABLE

bool touch_init(void) { return false; }
lv_indev_t *touch_register_lvgl(lv_display_t *disp) { (void)disp; return NULL; }
const char *touch_chip_name(void) { return NULL; }
void touch_set_wake_hook(bool (*hook)(void)) { (void)hook; }

#else

#include <stdatomic.h>
#include <string.h>

#include "display.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define CST816_ADDR          0x15
#define REG_GESTURE          0x01
#define REG_CHIP_ID          0xA7
#define REG_DIS_AUTO_SLEEP   0xFE
#define I2C_TIMEOUT_MS       20
#define IDLE_POLL_US         (100 * 1000)

#ifdef CONFIG_HARNESS_TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 1
#else
#define TOUCH_SWAP_XY 0
#endif
#ifdef CONFIG_HARNESS_TOUCH_MIRROR_X
#define TOUCH_MIRROR_X 1
#else
#define TOUCH_MIRROR_X 0
#endif
#ifdef CONFIG_HARNESS_TOUCH_MIRROR_Y
#define TOUCH_MIRROR_Y 1
#else
#define TOUCH_MIRROR_Y 0
#endif

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static const char             *s_chip;
static atomic_bool             s_irq;
static int64_t                 s_last_read_us;
static bool                    s_pressed;
static bool                    s_swallow;      // this stroke only woke the screen
static bool                    s_configured;   // ID read + auto-sleep disabled
static lv_point_t              s_last;
static uint32_t                s_read_errors;
static bool                  (*s_wake_hook)(void);

void touch_set_wake_hook(bool (*hook)(void)) { s_wake_hook = hook; }

static void IRAM_ATTR int_isr(void *arg)
{
    (void)arg;
    atomic_store(&s_irq, true);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t w[2] = { reg, val };
    return i2c_master_transmit(s_dev, w, sizeof(w), I2C_TIMEOUT_MS);
}

static void hw_reset(void)
{
#if CONFIG_HARNESS_PIN_TOUCH_RST >= 0
    gpio_reset_pin(CONFIG_HARNESS_PIN_TOUCH_RST);
    gpio_set_direction(CONFIG_HARNESS_PIN_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_HARNESS_PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_HARNESS_PIN_TOUCH_RST, 1);
#endif
    // The controller needs ~50 ms after reset before it answers on I2C.
    vTaskDelay(pdMS_TO_TICKS(60));
}

bool touch_init(void)
{
    hw_reset();

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,   // any free controller (the C3 has one)
        .sda_io_num = CONFIG_HARNESS_PIN_TOUCH_SDA,
        .scl_io_num = CONFIG_HARNESS_PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The board has 4.7 kΩ pull-ups; the internal ones only help a
        // hand-wired panel that forgot them.
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed (sda=%d scl=%d)", CONFIG_HARNESS_PIN_TOUCH_SDA,
                 CONFIG_HARNESS_PIN_TOUCH_SCL);
        return false;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDR,
        .scl_speed_hz = CONFIG_HARNESS_TOUCH_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed");
        return false;
    }

    // Identify. Retried: right after reset some parts need a moment more.
    uint8_t id = 0;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = reg_read(REG_CHIP_ID, &id, 1);
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) {
        // NOT fatal. Some CST816 firmwares ignore I2C entirely until a finger
        // has touched the glass once (ESPHome ships a `skip_probe` option for
        // exactly this). The board profile says a panel is fitted, so trust
        // it: keep the device and let the first touch wake it. A board that
        // really has none only costs a silenced NACK every 100 ms.
        ESP_LOGW(TAG, "CST816 at 0x%02X did not answer the ID probe (%s) — assuming it is "
                 "asleep; touch stays enabled", CST816_ADDR, esp_err_to_name(err));
        s_chip = "cst816";
        s_configured = false;   // finished on the first touch (finish_setup)
    } else {
        s_configured = true;
        switch (id) {
        case 0xB4: s_chip = "cst816s"; break;
        case 0xB5: s_chip = "cst816t"; break;
        case 0xB6: s_chip = "cst816d"; break;
        default:   s_chip = "cst816"; break;
        }
        // Stay awake: see "THE SLEEP TRAP" at the top of this file.
        if (reg_write(REG_DIS_AUTO_SLEEP, 0x01) != ESP_OK) {
            ESP_LOGW(TAG, "could not disable auto-sleep (reads may NACK while idle)");
        }
    }
    // From here a NACK is an ordinary "nobody touching a sleeping chip".
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

#if CONFIG_HARNESS_PIN_TOUCH_INT >= 0
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_HARNESS_PIN_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t isr_err = gpio_config(&int_cfg);
    if (isr_err == ESP_OK) {
        isr_err = gpio_install_isr_service(0);
        if (isr_err == ESP_ERR_INVALID_STATE) isr_err = ESP_OK;   // already installed
    }
    if (isr_err == ESP_OK) isr_err = gpio_isr_handler_add(CONFIG_HARNESS_PIN_TOUCH_INT, int_isr, NULL);
    if (isr_err != ESP_OK) ESP_LOGW(TAG, "INT interrupt unavailable (%s) — polling", esp_err_to_name(isr_err));
#endif

    ESP_LOGI(TAG, "%s (id 0x%02X) on sda=%d scl=%d int=%d rst=%d, swap=%d mirror x=%d y=%d",
             s_chip, id, CONFIG_HARNESS_PIN_TOUCH_SDA, CONFIG_HARNESS_PIN_TOUCH_SCL,
             CONFIG_HARNESS_PIN_TOUCH_INT, CONFIG_HARNESS_PIN_TOUCH_RST, TOUCH_SWAP_XY,
             TOUCH_MIRROR_X, TOUCH_MIRROR_Y);
    return true;
}

const char *touch_chip_name(void) { return s_chip; }

// A controller that slept through the boot probe is awake NOW (a finger is on
// it): identify it and switch its auto-sleep off, once.
static void finish_setup(void)
{
    uint8_t id = 0;
    if (reg_read(REG_CHIP_ID, &id, 1) != ESP_OK) return;
    s_configured = true;
    if (id == 0xB4) s_chip = "cst816s";
    else if (id == 0xB5) s_chip = "cst816t";
    else if (id == 0xB6) s_chip = "cst816d";
    reg_write(REG_DIS_AUTO_SLEEP, 0x01);
    ESP_LOGI(TAG, "%s (id 0x%02X) answered on first touch; auto-sleep off", s_chip, id);
}

// One sample. Returns true while a finger is on the glass.
static bool sample(lv_point_t *pt)
{
    uint8_t d[6];
    if (reg_read(REG_GESTURE, d, sizeof(d)) != ESP_OK) {
        s_read_errors++;
        return false;
    }
    const uint8_t fingers = d[1];
    if (fingers == 0 || fingers > 2) return false;   // >2 = bus garbage
    int x = ((d[2] & 0x0F) << 8) | d[3];
    int y = ((d[4] & 0x0F) << 8) | d[5];
    if (TOUCH_SWAP_XY) { const int t = x; x = y; y = t; }
    if (TOUCH_MIRROR_X) x = DISPLAY_WIDTH - 1 - x;
    if (TOUCH_MIRROR_Y) y = DISPLAY_HEIGHT - 1 - y;
    if (x < 0) x = 0;
    if (x >= DISPLAY_WIDTH) x = DISPLAY_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;
    pt->x = x;
    pt->y = y;
    return true;
}

// LVGL pointer read callback (UI task, LVGL lock held).
static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const int64_t now = esp_timer_get_time();
    bool poll = s_pressed || atomic_exchange(&s_irq, false) ||
                (now - s_last_read_us) >= IDLE_POLL_US;
#if CONFIG_HARNESS_PIN_TOUCH_INT < 0
    poll = true;
#endif
    if (poll) {
        s_last_read_us = now;
        lv_point_t pt;
        const bool was = s_pressed;
        s_pressed = sample(&pt);
        if (s_pressed) s_last = pt;
        if (s_pressed && !s_configured) finish_setup();
        if (s_pressed && !was && s_wake_hook && s_wake_hook()) s_swallow = true;
        if (!s_pressed) s_swallow = false;
    }
    data->point = s_last;
    data->state = s_pressed && !s_swallow ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_indev_t *touch_register_lvgl(lv_display_t *disp)
{
    if (!s_dev || !disp) return NULL;
    lv_indev_t *indev = lv_indev_create();
    if (!indev) return NULL;
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_display(indev, disp);
    return indev;
}

#endif  // CONFIG_HARNESS_TOUCH_ENABLE
