idf_component_register(SRCS "main.c" "../../../firmware/main/app_nvs.c"
                       INCLUDE_DIRS "../../../firmware/main"
                       REQUIRES nvs_flash esp_partition oc_nvs_mig log)
