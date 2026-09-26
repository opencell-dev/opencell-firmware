if(ESP_PLATFORM)
  idf_component_register(SRCS "lc_air.c"
                         INCLUDE_DIRS "include"
                         REQUIRES lc_phy)
else()
  add_library(lc_air STATIC lc_air.c)
  target_include_directories(lc_air PUBLIC include)
  target_link_libraries(lc_air PUBLIC lc_phy)
endif()
