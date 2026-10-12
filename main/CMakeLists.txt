file(GLOB_RECURSE UI_SOURCES "ui/*.c")

set(APP_SOURCES
    "main.c"
    "hv_monitor.c"
    "geiger_counter.c"
    "cw2015.c"
    ${UI_SOURCES})

idf_component_register(SRCS ${APP_SOURCES}
                       INCLUDE_DIRS "." "ui"
                       REQUIRES "esp_lcd_nv3007" "lvgl" "esp_adc" "esp_driver_ledc")
