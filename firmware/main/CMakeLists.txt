idf_component_register(SRCS "app_main.c" "p51_selftest.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES app_runtime esp_psram spi_flash esp_timer freertos
                                      audio_pipeline wakeword storage_sd)
