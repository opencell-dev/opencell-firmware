idf_component_register(SRCS "oc_radio.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES oc_phy oc_exec w12_board esp_driver_gpio
                       PRIV_REQUIRES RadioLib)
