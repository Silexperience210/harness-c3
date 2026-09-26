// Button polling: 10 ms poll, 30 ms debounce, short/long (≥600 ms). See
// buttons.h. Pins come exclusively from Kconfig (menu "Harness C3
// Configuration"); -1 means "not fitted".

#include "buttons.h"
#include "sdkconfig.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

#define POLL_MS     10
#define DEBOUNCE_MS 30
#define LONG_MS     600

// Debounce in poll ticks (30 ms / 10 ms = 3 identical reads to accept a level).
#define DEBOUNCE_TICKS (DEBOUNCE_MS / POLL_MS)
#define LONG_TICKS     (LONG_MS / POLL_MS)

typedef struct {
    int         gpio;
    btn_event_t short_event;
    btn_event_t long_event;
    int         level;        // accepted level, 1 = released (idle)
    int         candidate;    // level being debounced
    int         same_ticks;   // how long `candidate` has held
    int         held_ticks;   // ticks since an accepted press
    bool        long_fired;   // long event already sent for this press
} button_t;

static QueueHandle_t s_queue;
static button_t      s_btns[2];
static int           s_count;

static void poll_button(button_t *b)
{
    const int raw = gpio_get_level((gpio_num_t)b->gpio);
    if (raw == b->candidate) {
        if (b->same_ticks < DEBOUNCE_TICKS) b->same_ticks++;
    } else {
        b->candidate = raw;
        b->same_ticks = 0;
    }
    if (b->same_ticks < DEBOUNCE_TICKS || b->candidate == b->level) {
        // Level unchanged (or still bouncing): only the long-press timer runs.
        if (b->level == 0 && !b->long_fired) {
            if (++b->held_ticks >= LONG_TICKS) {
                b->long_fired = true;
                xQueueSend(s_queue, &b->long_event, 0);
            }
        }
        return;
    }

    // A debounced edge.
    const int prev = b->level;
    b->level = b->candidate;
    if (b->level == 0) {           // pressed
        b->held_ticks = 0;
        b->long_fired = false;
    } else if (prev == 0 && !b->long_fired) {   // released before LONG_MS
        xQueueSend(s_queue, &b->short_event, 0);
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < s_count; i++) poll_button(&s_btns[i]);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static void add_button(int gpio, btn_event_t short_ev, btn_event_t long_ev)
{
    if (gpio < 0) return;
    button_t *b = &s_btns[s_count++];
    b->gpio = gpio;
    b->short_event = short_ev;
    b->long_event = long_ev;
    b->level = 1;
    b->candidate = 1;
}

bool buttons_init(QueueHandle_t queue)
{
    if (!queue) return false;
    s_queue = queue;
    s_count = 0;
    add_button(CONFIG_HARNESS_PIN_BTN_A, BTN_EVENT_A_SHORT, BTN_EVENT_A_LONG);
    add_button(CONFIG_HARNESS_PIN_BTN_B, BTN_EVENT_B_SHORT, BTN_EVENT_B_LONG);
    if (s_count == 0) {
        ESP_LOGI(TAG, "no buttons configured");
        return true;
    }

    uint64_t mask = 0;
    for (int i = 0; i < s_count; i++) mask |= 1ULL << s_btns[i].gpio;
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // active-low buttons
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,       // polled, not interrupted
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed");
        return false;
    }

    if (xTaskCreate(buttons_task, "buttons", 2048, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    ESP_LOGI(TAG, "buttons: A=GPIO%d B=GPIO%d (-1 = none; 30 ms debounce, long >= %d ms)",
             CONFIG_HARNESS_PIN_BTN_A, CONFIG_HARNESS_PIN_BTN_B, LONG_MS);
    return true;
}
