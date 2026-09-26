#pragma once
#include "freertos/FreeRTOS.h"
typedef int esp_err_t;
#define ESP_OK 0
static inline esp_err_t esp_task_wdt_add(void *t) { (void)t; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
