idf_component_register(
    SRCS
        "app_main.c"
        "cable_frame.c"
        "cable_link.c"
        "cable_client.c"
        "display.c"
        "touch.c"
        "ui.c"
        "buttons.c"
        "buzzer.c"
        "settings.c"
        "fonts/font_ui_14.c"
        "fonts/font_ui_20.c"
        "fonts/font_ui_28.c"
        "fonts/font_logo_72.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_usb_serial_jtag   # USB-Serial-JTAG transport (+ its VFS)
        esp_driver_gpio              # buttons, touch INT/RST
        esp_driver_i2c               # CST816D touch controller
        esp_driver_spi               # GC9A01 panel bus
        esp_driver_ledc              # backlight + buzzer PWM
        esp_lcd                      # panel abstraction
        esp_timer                    # LVGL tick, clocks
        esp_system                   # task watchdog, reset reason
        nvs_flash                    # settings, OTA plumbing
        app_update                   # esp_ota_mark_app_valid_cancel_rollback
        json                         # cJSON
        lvgl                         # UI
)
