idf_component_register(SRCS "storage_sd.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_rlcd42 esp_driver_sdmmc fatfs)
