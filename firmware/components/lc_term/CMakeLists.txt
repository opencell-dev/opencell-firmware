if(ESP_PLATFORM)
  idf_component_register(SRCS "lc_term_plan.c" "lc_term.c" "lc_term_gatt.c" "lc_term_screen.c" "lc_term_sig.c" "lc_term_pair.c" "lc_term_ui.c" "lc_term_scan.c"
                         INCLUDE_DIRS "include"
                         REQUIRES lc_phy lc_air lc_exec lc_sig)
else()
  add_library(lc_term STATIC lc_term_plan.c lc_term.c lc_term_gatt.c lc_term_screen.c lc_term_sig.c lc_term_pair.c lc_term_ui.c lc_term_scan.c)
  target_include_directories(lc_term PUBLIC include)
  target_link_libraries(lc_term PUBLIC lc_phy lc_air lc_exec lc_sig)
endif()
