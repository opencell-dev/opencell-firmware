if(ESP_PLATFORM)
  idf_component_register(SRCS "oc_bsr.c" INCLUDE_DIRS "include"
                         REQUIRES oc_link oc_clock oc_exec oc_fwupd)
else()
  add_library(oc_bsr STATIC oc_bsr.c)
  target_include_directories(oc_bsr PUBLIC include)
  target_link_libraries(oc_bsr PUBLIC oc_link oc_clock oc_exec oc_fwupd)
endif()
