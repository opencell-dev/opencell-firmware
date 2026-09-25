idf_component_register(SRCS "lc_radio.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES lc_phy lc_exec w12_board esp_driver_gpio
                       PRIV_REQUIRES RadioLib)
