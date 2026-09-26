#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) do { if (sim_verbose) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
extern int sim_verbose;
