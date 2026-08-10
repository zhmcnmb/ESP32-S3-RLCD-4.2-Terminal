idf_component_register(SRCS "app_runtime.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_rlcd42 display_st7305 ui_manager ui_pages
                                     sensor_shtc3 rtc_pcf85063 time_service net_manager
                                     battery storage_sd env_logger
                                     weather_client input_manager sys_state scheduler
                                     llm_provider asr_client tts_client audio_pipeline web_client
                                     agent_runtime voice_assistant wifi_setup esp_timer text_console)
