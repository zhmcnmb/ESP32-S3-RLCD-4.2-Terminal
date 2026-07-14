idf_component_register(SRCS "app_main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES board_rlcd42 display_st7305 ui_manager ui_pages
                                      sensor_shtc3 rtc_pcf85063 time_service net_manager
                                      battery storage_sd env_logger claude_quota_client
                                      weather_client esp_psram spi_flash)
