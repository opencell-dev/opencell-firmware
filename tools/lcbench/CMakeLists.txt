# lcbench: Linux bench tool that drives W12 bs-radio boards over their UART.
# Standalone:  cmake -S tools/lcbench -B tools/lcbench/build -G Ninja && cmake --build tools/lcbench/build
cmake_minimum_required(VERSION 3.16)
project(lcbench C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET lc_link)
  add_compile_options(-Wall -Wextra -Werror)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/lc_phy lc_phy)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/lc_link lc_link)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/lc_clock lc_clock)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/lc_exec lc_exec)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/lc_air lc_air)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/lc_term lc_term)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/lc_sig lc_sig)
endif()

add_library(lcbench_core STATIC lcbench_core.c lcb_cell.c lcb_hss.c lcb_net.c lcb_merge.c)
target_include_directories(lcbench_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(lcbench_core PUBLIC lc_link lc_clock lc_exec lc_air lc_term lc_sig m)

add_executable(lcbench lcbench.c)
target_link_libraries(lcbench PRIVATE lcbench_core)
