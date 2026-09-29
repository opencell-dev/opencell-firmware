if(ESP_PLATFORM)
  idf_component_register(SRCS "oc_term_plan.c" "oc_term.c" "oc_term_gatt.c" "oc_term_screen.c" "oc_term_sig.c" "oc_term_pair.c" "oc_term_ui.c" "oc_term_scan.c"
                         INCLUDE_DIRS "include"
                         REQUIRES oc_phy oc_air oc_exec oc_sig oc_link)
else()
  add_library(oc_term STATIC oc_term_plan.c oc_term.c oc_term_gatt.c oc_term_screen.c oc_term_sig.c oc_term_pair.c oc_term_ui.c oc_term_scan.c)
  target_include_directories(oc_term PUBLIC include)
  target_link_libraries(oc_term PUBLIC oc_phy oc_air oc_exec oc_sig oc_link)
endif()
