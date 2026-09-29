idf_component_register(SRCS "main.c" "app_store.c" "app_link.c" "app_exec.c" "app_oled.c" "app_role.c"
                            "term_app.c" "term_ble.c" "term_oled.c" "term_ident.c" "term_scan.c"
                       INCLUDE_DIRS "."
                       REQUIRES oc_phy oc_link oc_clock oc_exec oc_fwupd oc_bsr oc_radio w12_board
                                oc_air oc_term oc_oled oc_sig
                                nvs_flash app_update esp_timer esp_driver_gpio esp_driver_uart
                                esp_driver_tsens esp_driver_i2c esp_lcd bt esp_driver_usb_serial_jtag
                                bootloader_support)
