idf_component_register(
    SRCS "main.c" "navigation.c" "brightness.c" "buzzer.c" "vibration.c" "xlr_test.c"
    INCLUDE_DIRS "."
    REQUIRES
        lvgl
        squareline_ui
        esp_driver_gpio
        esp_driver_spi
        esp_driver_ledc
        esp_timer
)
