set(OC_SIG_SRCS oc_sig_kdf.c oc_sig_selftest.c oc_sig_milenage.c oc_sig_number.c oc_sig_msg.c oc_sig_qr.c
                oc_sig_frag.c oc_sig_prot.c oc_sig_keys.c oc_sig_chan.c oc_sig_term.c)
if(ESP_PLATFORM)
  idf_component_register(SRCS ${OC_SIG_SRCS} crypto_psa.c
                         INCLUDE_DIRS "include"
                         REQUIRES oc_link mbedtls)
else()
  find_package(OpenSSL REQUIRED)
  set(OC_SIG_NET_TERMS 32 CACHE STRING "oc_sig_net sessions (terminals) per cell")
  add_library(oc_sig STATIC ${OC_SIG_SRCS} oc_sig_net.c oc_sig_hss.c crypto_openssl.c)
  target_include_directories(oc_sig PUBLIC include)
  target_compile_definitions(oc_sig PUBLIC OC_SIG_NET_TERMS=${OC_SIG_NET_TERMS}u)
  target_link_libraries(oc_sig PUBLIC oc_link OpenSSL::Crypto)
endif()
