// Passive buzzer via LEDC. See buzzer.h. Compiled to a no-op shell when
// CONFIG_HARNESS_PIN_BUZZER == -1, so the rest of the firmware never has to
// care whether a buzzer is fitted.

#include "buzzer.h"
#include "sdkconfig.h"

#include <stdint.h>

#if CONFIG_HARNESS_PIN_BUZZER < 0

bool buzzer_init(void) { return true; }
void buzzer_beep(buzzer_pattern_t pattern) { (void)pattern; }

#else   // CONFIG_HARNESS_PIN_BUZZER >= 0

#include <stddef.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buzzer";

#define BUZZ_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BUZZ_LEDC_TIMER    LEDC_TIMER_1
#define BUZZ_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BUZZ_DUTY_RES      LEDC_TIMER_10_BIT
#define BUZZ_DUTY          ((1 << BUZZ_DUTY_RES) / 2)   // 50 %

// One tone step of a pattern: freq Hz, duration ms, then a pause of the same
// length between steps.
typedef struct {
    uint16_t freq;
    uint16_t on_ms;
} tone_t;

typedef struct {
    tone_t tones[2];
    int    count;
} pattern_def_t;

static const pattern_def_t PATTERNS[] = {
    [BUZZER_BEEP_DONE]     = { .tones = {{ 1760, 120 }},           .count = 1 },
    [BUZZER_BEEP_QUESTION] = { .tones = {{ 2093, 90 }, { 1568, 140 }}, .count = 2 },
    [BUZZER_BEEP_ERROR]    = { .tones = {{ 440, 250 }},            .count = 1 },
};

static void tone_off(void)
{
    ledc_set_duty(BUZZ_LEDC_MODE, BUZZ_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZ_LEDC_MODE, BUZZ_LEDC_CHANNEL);
}

static void buzzer_task(void *arg)
{
    const buzzer_pattern_t pattern = (buzzer_pattern_t)(intptr_t)arg;
    const pattern_def_t *def = &PATTERNS[pattern];
    for (int i = 0; i < def->count; i++) {
        ledc_set_freq(BUZZ_LEDC_MODE, BUZZ_LEDC_TIMER, def->tones[i].freq);
        ledc_set_duty(BUZZ_LEDC_MODE, BUZZ_LEDC_CHANNEL, BUZZ_DUTY);
        ledc_update_duty(BUZZ_LEDC_MODE, BUZZ_LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(def->tones[i].on_ms));
        tone_off();
        if (i + 1 < def->count) vTaskDelay(pdMS_TO_TICKS(40));
    }
    vTaskDelete(NULL);
}

bool buzzer_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BUZZ_LEDC_MODE,
        .duty_resolution = BUZZ_DUTY_RES,
        .timer_num = BUZZ_LEDC_TIMER,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer failed");
        return false;
    }
    const ledc_channel_config_t channel = {
        .gpio_num = CONFIG_HARNESS_PIN_BUZZER,
        .speed_mode = BUZZ_LEDC_MODE,
        .channel = BUZZ_LEDC_CHANNEL,
        .timer_sel = BUZZ_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel failed");
        return false;
    }
    ESP_LOGI(TAG, "buzzer on GPIO%d", CONFIG_HARNESS_PIN_BUZZER);
    return true;
}

void buzzer_beep(buzzer_pattern_t pattern)
{
    if ((unsigned)pattern > (unsigned)BUZZER_BEEP_ERROR) return;
    // One short-lived task per pattern keeps the caller non-blocking; a new
    // beep simply plays over (LEDC writes are last-writer-wins, and patterns
    // are rare — a question or a finished turn).
    if (xTaskCreate(buzzer_task, "buzzer", 1536, (void *)(intptr_t)pattern, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "beep dropped (no task)");
    }
}

#endif  // CONFIG_HARNESS_PIN_BUZZER
