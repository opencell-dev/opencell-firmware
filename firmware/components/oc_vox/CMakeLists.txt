# oc_vox: OpenVoxNB, the speech codec (docs/openvox/PLAN.md). Portable C11,
# no dependencies on other components. The board gets the fixed-point
# production code; the host also builds the floating-point reference
# (ref/, the oc_vox_ref_* symbols) for the tests and tools.
set(OC_VOX_SRCS oc_vox_modes.c oc_vox_layout.c oc_vox_bits.c oc_vox_fec.c oc_vox_quant.c oc_vox_conceal.c oc_vox_pipe.c oc_vox.c)
set(OC_VOX_REF_SRCS ref/oc_vox_ref_dsp.c ref/oc_vox_ref_analysis.c ref/oc_vox_ref_synth.c oc_vox_ref.c)
if(ESP_PLATFORM)
  idf_component_register(SRCS ${OC_VOX_SRCS} INCLUDE_DIRS "include")
else()
  add_library(oc_vox STATIC ${OC_VOX_SRCS} ${OC_VOX_REF_SRCS})
  target_include_directories(oc_vox PUBLIC include)
  target_include_directories(oc_vox PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/ref) # internals: the reference's headers, for the tests and tools
  target_link_libraries(oc_vox PUBLIC m)
endif()
