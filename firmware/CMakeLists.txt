cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# RadioLib hides the LR2021 packet-type/modulation commands unless GODMODE is
# set; lc_radio needs them to switch LoRa <-> FLRC per slot without a reset.
idf_build_set_property(COMPILE_DEFINITIONS "RADIOLIB_GODMODE=1" APPEND)
# ...and then emits a #warning in every file that includes it; keep it visible
# but not fatal under ESP-IDF's -Werror.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=cpp" APPEND)
# Bench-only low-power build: idf.py -DLC_BENCH_LOW_POWER=1 build
# (915 PA bypassed, minimum chip drive) for unattenuated over-the-air tests.
if(LC_BENCH_LOW_POWER)
  idf_build_set_property(COMPILE_DEFINITIONS "LC_BENCH_LOW_POWER=1" APPEND)
endif()
project(opencell_w12)
