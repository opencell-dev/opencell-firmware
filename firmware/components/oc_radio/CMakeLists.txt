if(ESP_PLATFORM)
  idf_component_register(SRCS "oc_radio.cpp" "oc_lr_rx.c"
                         INCLUDE_DIRS "include"
                         REQUIRES oc_phy oc_exec w12_board esp_driver_gpio
                         PRIV_REQUIRES RadioLib)
else()
  # host tests: the LR2021 readout decoding only
  add_library(oc_lr_rx STATIC oc_lr_rx.c)
  target_include_directories(oc_lr_rx PUBLIC include)
  target_link_libraries(oc_lr_rx PUBLIC oc_exec)
endif()
