// Harness C3 — entry point. Init order (SPEC.md §3):
//
//   NVS → settings → display (backlight off) → touch → UI (draws the boot
//   screen, THEN lights the panel) → buttons → buzzer → cable_client (store
//   lock) → cable_link (reader task; the session machine runs as its tick)
//   → esp_ota_mark_app_valid_cancel_rollback() once all init succeeded.
//
// A failure in any single subsystem is logged and survived where possible:
// a dial that shows "Not connected" is diagnosable from across the room,
// a boot loop is not.

#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "buzzer.h"
#include "cable_client.h"
#include "cable_link.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "settings.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "app";

// cable_client's clock (the seam that lets host tests drive the session
// machine with a fake one).
uint32_t cable_platform_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:               return "other";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot: reset reason %s", reset_reason_name(esp_reset_reason()));

    // ── NVS (settings + OTA plumbing) ──
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS unavailable (%s) — defaults only", esp_err_to_name(err));
    settings_load();

    // ── display (backlight stays off until the UI has drawn) + touch ──
    const bool display_ok = display_init();
    if (!display_ok) ESP_LOGE(TAG, "display init failed — running headless");
    const bool touch_ok = touch_init();

    // Identity for `hello`: version from the RUNNING image (never a constant
    // — the daemon's offer decision compares this string, PROTOCOL.md
    // §10.10), suffixed "-c3" for this port; hw = base name + the touch
    // controller detected at boot; MAC = WiFi STA MAC, uppercase,
    // colon-separated (also the device's USB serial number).
    char fw[40];
    snprintf(fw, sizeof(fw), "%.32s-c3", esp_app_get_description()->version);
    char hw[40];
    snprintf(hw, sizeof(hw), "%s%s%s", CABLE_HW_NAME, touch_ok ? "+" : "",
             touch_ok ? touch_chip_name() : "");
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cable_client_set_identity(fw, hw, mac_str);

    bool ui_ok = false;
    if (display_ok) {
        if (touch_ok && display_lock(1000)) {
            if (!touch_register_lvgl(display_get())) ESP_LOGE(TAG, "touch indev registration failed");
            display_unlock();
        }
        ui_ok = ui_init(fw, touch_ok);
    }

    // ── buttons + buzzer ──
    if (ui_ok) buttons_init(ui_button_queue());
    buzzer_init();   // no-op when CONFIG_HARNESS_PIN_BUZZER == -1

    // ── message layer FIRST (its lock must exist before any frame can
    //    arrive), then the link, whose reader task also drives the session
    //    machine (hello cadence + silence watchdog) as its tick ──
    cable_client_init();
    if (!cable_client_start() ||
        !cable_link_start(cable_client_handle_frame, cable_client_poll, NULL)) {
        ESP_LOGE(TAG, "cable link failed to start — no connection to the daemon");
    }

    // ── all init done: this image is good, cancel any pending rollback ──
    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "harness-c3 %s up (hw %s, proto %d, %s, free heap %u)", fw, hw,
             CABLE_PROTO_VERSION, touch_ok ? "touch" : "no touch",
             (unsigned)esp_get_free_heap_size());
}
