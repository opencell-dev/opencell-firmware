if(ESP_PLATFORM)
  idf_component_register(SRCS "oc_air.c"
                         INCLUDE_DIRS "include"
                         REQUIRES oc_phy)
else()
  add_library(oc_air STATIC oc_air.c)
  target_include_directories(oc_air PUBLIC include)
  target_link_libraries(oc_air PUBLIC oc_phy)
endif()
