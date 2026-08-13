idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        lvgl
        squareline_ui
        esp_driver_gpio
        esp_driver_spi
        esp_timer
)
