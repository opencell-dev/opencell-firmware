if(ESP_PLATFORM)
  idf_component_register(SRCS "lc_crc16.c" "lc_cobs.c" "lc_msg.c"
                         INCLUDE_DIRS "include"
                         REQUIRES lc_phy)
else()
  add_library(lc_link STATIC lc_crc16.c lc_cobs.c lc_msg.c)
  target_include_directories(lc_link PUBLIC include)
  target_link_libraries(lc_link PUBLIC lc_phy)
endif()
