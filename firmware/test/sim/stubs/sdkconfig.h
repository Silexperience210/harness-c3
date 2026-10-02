// Host simulator: the Kconfig values ui.c / cable_client.h read (board
// defaults of the ESP32-2424S012C profile).
#pragma once
#define CONFIG_HARNESS_MAX_AGENTS 8
#define CONFIG_HARNESS_DIM_AFTER_S 60
#define CONFIG_HARNESS_OFF_AFTER_S 600
#ifndef CONFIG_HARNESS_LAMP_AUTO_AFTER_S   // run_sim.sh also builds it with 0
#define CONFIG_HARNESS_LAMP_AUTO_AFTER_S 30
#endif
#define CONFIG_HARNESS_LAMP_OFF_AFTER_S 3600
#define CONFIG_HARNESS_BACKLIGHT_DEFAULT 80
#define CONFIG_HARNESS_LANG_FR 1
#define CONFIG_HARNESS_QUICK_REPLIES "Continue|Oui|Non|Résume où tu en es|Lance les tests"
#define CONFIG_HARNESS_RELAUNCH_ENGINES "claude|hermes"
