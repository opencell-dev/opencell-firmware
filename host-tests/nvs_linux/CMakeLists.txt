# The NVS migration (firmware/main/app_nvs.c, oc_nvs_mig) on ESP-IDF's real
# NVS code, built for the linux target, over a flash image file that holds a
# board's dumped NVS partition. See main/main.c.
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_nvs_mig")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nvs_linux)
