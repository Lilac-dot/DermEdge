idf_component_register(SRCS "main.c" "app_sm.c" "scratch.c" "touch.c" "display.c" "st7735.c" "fonts.c" "ui.c" "adxl345.c" "ble.c" "skin.c" "skin_adc.c" "ui_flow.c" "art.c" "store.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_driver_spi esp_driver_gpio esp_driver_i2c esp_driver_uart esp_timer esp_adc esp_partition)
