// Host simulator: shared declarations between the platform fakes and the
// scenario. See sim_platform.c / sim_main.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "lvgl.h"
#include "sdkconfig.h"

#define SIM_SENT_MAX 512

extern int  sim_verbose;
extern char sim_sent[SIM_SENT_MAX][2048];
extern int  sim_sent_count;
extern int  sim_beeps;
extern int  s_brightness_saves;

bool sim_display_init(void);
bool sim_screenshot(const char *path);
void sim_touch(int x, int y, bool down);
void sim_clock_advance_ms(uint32_t ms);
void ui_sim_step(void);
