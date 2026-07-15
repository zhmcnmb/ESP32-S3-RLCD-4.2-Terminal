idf_component_register(SRCS "app_main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES app_runtime esp_psram spi_flash)
