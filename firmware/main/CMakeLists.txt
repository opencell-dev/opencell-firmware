idf_component_register(SRCS "main.c" "app_store.c" "app_link.c" "app_exec.c" "app_oled.c"
                       INCLUDE_DIRS "."
                       REQUIRES lc_phy lc_link lc_clock lc_exec lc_fwupd lc_bsr lc_oled lc_radio w12_board
                                nvs_flash app_update esp_timer esp_driver_gpio esp_driver_uart
                                esp_driver_tsens esp_driver_i2c esp_lcd)
