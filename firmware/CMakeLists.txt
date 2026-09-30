cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# RadioLib hides the LR2021 packet-type/modulation commands unless GODMODE is
# set; oc_radio needs them to switch LoRa <-> FLRC per slot without a reset.
idf_build_set_property(COMPILE_DEFINITIONS "RADIOLIB_GODMODE=1" APPEND)
# No read-back after every SPI write: it doubles each command's cost and
# slot-time reconfiguration must fit the guard. RadioLib still checks the
# status byte every transfer returns (the previous command's result).
idf_build_set_property(COMPILE_DEFINITIONS "RADIOLIB_SPI_PARANOID=0" APPEND)
# ...and then emits a #warning in every file that includes it; keep it visible
# but not fatal under ESP-IDF's -Werror.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=cpp" APPEND)
# Bench-only low-power build: idf.py -DOC_BENCH_LOW_POWER=1 build
# (915 PA bypassed, minimum chip drive) for unattenuated over-the-air tests.
# The old name is refused rather than silently building full power. lc2oc: keep
if(DEFINED LC_BENCH_LOW_POWER) # lc2oc: keep
  message(FATAL_ERROR "LC_BENCH_LOW_POWER is now OC_BENCH_LOW_POWER: run idf.py fullclean, then build with -DOC_BENCH_LOW_POWER=1") # lc2oc: keep
endif()
if(OC_BENCH_LOW_POWER)
  idf_build_set_property(COMPILE_DEFINITIONS "OC_BENCH_LOW_POWER=1" APPEND)
endif()
# Measurement build: idf.py -DOC_RXT_TRACE=1 build times the RX turnaround
# (RX done -> next launch) and prints it as "@RXT" text while no host talks.
if(OC_RXT_TRACE)
  idf_build_set_property(COMPILE_DEFINITIONS "OC_RXT_TRACE=1" APPEND)
endif()
project(opencell_w12)
