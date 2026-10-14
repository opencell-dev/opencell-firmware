# oc_vox as opencell-core builds it (OC_VOX_REF OFF): the fixed-point
# production sources only. Configured and built by test_vox_prod_only.sh.
cmake_minimum_required(VERSION 3.16)
project(oc_vox_prod C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)
set(OC_VOX_REF OFF CACHE BOOL "oc_vox's floating-point reference" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/oc_vox oc_vox)
add_executable(vox_prod_link vox_prod_link.c)
target_link_libraries(vox_prod_link PRIVATE oc_vox)
