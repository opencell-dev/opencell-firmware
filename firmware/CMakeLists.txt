cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# RadioLib hides the LR2021 packet-type/modulation commands unless GODMODE is
# set; lc_radio needs them to switch LoRa <-> FLRC per slot without a reset.
idf_build_set_property(COMPILE_DEFINITIONS "RADIOLIB_GODMODE=1" APPEND)
# ...and then emits a #warning in every file that includes it; keep it visible
# but not fatal under ESP-IDF's -Werror.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=cpp" APPEND)
project(opencell_w12)
