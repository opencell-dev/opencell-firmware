# oc-vox: the OpenVoxNB command-line tool (encode, decode, inspect, time).
# Standalone:  cmake -S tools/vox -B tools/vox/build -G Ninja && cmake --build tools/vox/build
# or as part of host-tests (which adds it for test_vox_tool).
cmake_minimum_required(VERSION 3.16)
project(oc_vox_tools C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET oc_vox)
  add_compile_options(-Wall -Wextra -Werror)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_vox oc_vox)
endif()

add_executable(oc-vox oc-vox.c)
target_link_libraries(oc-vox PRIVATE oc_vox)
