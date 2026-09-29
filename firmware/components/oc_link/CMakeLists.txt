if(ESP_PLATFORM)
  idf_component_register(SRCS "oc_crc16.c" "oc_cobs.c" "oc_msg.c" "oc_framer.c"
                         INCLUDE_DIRS "include"
                         REQUIRES oc_phy)
else()
  add_library(oc_link STATIC oc_crc16.c oc_cobs.c oc_msg.c oc_framer.c)
  target_include_directories(oc_link PUBLIC include)
  target_link_libraries(oc_link PUBLIC oc_phy)
endif()
