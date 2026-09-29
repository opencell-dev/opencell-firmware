# ocbench: Linux bench tool that drives W12 bs-radio boards over their UART.
# Standalone:  cmake -S tools/ocbench -B tools/ocbench/build -G Ninja && cmake --build tools/ocbench/build
cmake_minimum_required(VERSION 3.16)
project(ocbench C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET oc_link)
  add_compile_options(-Wall -Wextra -Werror)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_phy oc_phy)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_link oc_link)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_clock oc_clock)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_exec oc_exec)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_air oc_air)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_term oc_term)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_sig oc_sig)
endif()

add_library(ocbench_core STATIC ocbench_core.c ocb_cell.c ocb_hss.c ocb_net.c ocb_merge.c)
target_include_directories(ocbench_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(ocbench_core PUBLIC oc_link oc_clock oc_exec oc_air oc_term oc_sig m)

add_executable(ocbench ocbench.c)
target_link_libraries(ocbench PRIVATE ocbench_core)
