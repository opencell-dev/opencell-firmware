if(ESP_PLATFORM)
  idf_component_register(SRCS "lc_term_plan.c" "lc_term.c"
                         INCLUDE_DIRS "include"
                         REQUIRES lc_phy lc_air lc_exec)
else()
  add_library(lc_term STATIC lc_term_plan.c lc_term.c)
  target_include_directories(lc_term PUBLIC include)
  target_link_libraries(lc_term PUBLIC lc_phy lc_air lc_exec)
endif()
