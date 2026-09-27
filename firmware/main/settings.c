// Persistent user settings. See settings.h.

#include "settings.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NS          "harness"
#define KEY_BRIGHT  "bright"
#define KEY_LAMP    "lamp_lvl"
#define KEY_WARM    "lamp_warm"
#define BRIGHT_MIN  5
#define BRIGHT_MAX  100

static int s_brightness = CONFIG_HARNESS_BACKLIGHT_DEFAULT;
static int s_lamp_level = 70;
static int s_lamp_warmth = 1;      // "warm", 2700 K

static int clamp_brightness(int v)
{
    if (v < BRIGHT_MIN) return BRIGHT_MIN;
    if (v > BRIGHT_MAX) return BRIGHT_MAX;
    return v;
}

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;   // first boot: defaults
    uint8_t v;
    if (nvs_get_u8(h, KEY_BRIGHT, &v) == ESP_OK) s_brightness = clamp_brightness(v);
    if (nvs_get_u8(h, KEY_LAMP, &v) == ESP_OK) s_lamp_level = clamp_brightness(v);
    if (nvs_get_u8(h, KEY_WARM, &v) == ESP_OK && v < LAMP_WARMTHS) s_lamp_warmth = v;
    nvs_close(h);
    ESP_LOGI(TAG, "brightness %d %%", s_brightness);
}

int settings_brightness(void) { return s_brightness; }

void settings_set_brightness(int percent)
{
    percent = clamp_brightness(percent);
    if (percent == s_brightness) return;
    s_brightness = percent;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable — brightness not saved");
        return;
    }
    if (nvs_set_u8(h, KEY_BRIGHT, (uint8_t)percent) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "brightness not saved");
    }
    nvs_close(h);
}

int settings_lamp_level(void) { return s_lamp_level; }
int settings_lamp_warmth(void) { return s_lamp_warmth; }

void settings_set_lamp(int level, int warmth)
{
    level = clamp_brightness(level);
    if (warmth < 0) warmth = 0;
    if (warmth >= LAMP_WARMTHS) warmth = LAMP_WARMTHS - 1;
    if (level == s_lamp_level && warmth == s_lamp_warmth) return;
    s_lamp_level = level;
    s_lamp_warmth = warmth;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable — lamp not saved");
        return;
    }
    if (nvs_set_u8(h, KEY_LAMP, (uint8_t)level) != ESP_OK || nvs_set_u8(h, KEY_WARM, (uint8_t)warmth) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "lamp not saved");
    }
    nvs_close(h);
}
