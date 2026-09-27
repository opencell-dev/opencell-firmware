set(LC_SIG_SRCS lc_sig_kdf.c lc_sig_selftest.c lc_sig_milenage.c)
if(ESP_PLATFORM)
  idf_component_register(SRCS ${LC_SIG_SRCS} crypto_psa.c
                         INCLUDE_DIRS "include"
                         REQUIRES lc_link mbedtls)
else()
  find_package(OpenSSL REQUIRED)
  add_library(lc_sig STATIC ${LC_SIG_SRCS} crypto_openssl.c)
  target_include_directories(lc_sig PUBLIC include)
  target_link_libraries(lc_sig PUBLIC lc_link OpenSSL::Crypto)
endif()
