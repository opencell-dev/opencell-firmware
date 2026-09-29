set(LC_SIG_SRCS lc_sig_kdf.c lc_sig_selftest.c lc_sig_milenage.c lc_sig_number.c lc_sig_msg.c lc_sig_qr.c
                lc_sig_frag.c lc_sig_prot.c lc_sig_keys.c lc_sig_chan.c lc_sig_term.c)
if(ESP_PLATFORM)
  idf_component_register(SRCS ${LC_SIG_SRCS} crypto_psa.c
                         INCLUDE_DIRS "include"
                         REQUIRES lc_link mbedtls)
else()
  find_package(OpenSSL REQUIRED)
  set(LC_SIG_NET_TERMS 32 CACHE STRING "lc_sig_net sessions (terminals) per cell")
  add_library(lc_sig STATIC ${LC_SIG_SRCS} lc_sig_net.c lc_sig_hss.c crypto_openssl.c)
  target_include_directories(lc_sig PUBLIC include)
  target_compile_definitions(lc_sig PUBLIC LC_SIG_NET_TERMS=${LC_SIG_NET_TERMS}u)
  target_link_libraries(lc_sig PUBLIC lc_link OpenSSL::Crypto)
endif()
