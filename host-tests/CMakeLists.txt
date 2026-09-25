cmake_minimum_required(VERSION 3.16)
project(lc_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

include(FetchContent)
FetchContent_Declare(unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG v2.6.1)
FetchContent_MakeAvailable(unity)

add_subdirectory(../firmware/components/lc_phy lc_phy)
add_subdirectory(../firmware/components/lc_link lc_link)
add_subdirectory(../firmware/components/lc_clock lc_clock)
add_subdirectory(../firmware/components/lc_exec lc_exec)

enable_testing()
function(lc_test name lib)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE ${lib} unity)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

lc_test(test_crc16 lc_link)
lc_test(test_cobs lc_link)
lc_test(test_hop lc_phy)
lc_test(test_mode lc_phy)
lc_test(test_msg lc_link)
lc_test(test_framer lc_link)
lc_test(test_clock lc_clock)
lc_test(test_exec_assemble lc_exec)
lc_test(test_exec_run lc_exec)
