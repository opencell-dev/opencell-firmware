if(ESP_PLATFORM)
  idf_component_register(SRCS "lc_bsr.c" INCLUDE_DIRS "include"
                         REQUIRES lc_link lc_clock lc_exec lc_fwupd)
else()
  add_library(lc_bsr STATIC lc_bsr.c)
  target_include_directories(lc_bsr PUBLIC include)
  target_link_libraries(lc_bsr PUBLIC lc_link lc_clock lc_exec lc_fwupd)
endif()
