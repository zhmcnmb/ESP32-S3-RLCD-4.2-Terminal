idf_component_register(SRCS "app_runtime.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_rlcd42 display_st7305 ui_manager ui_pages
                                     sensor_shtc3 rtc_pcf85063 time_service net_manager
                                     battery storage_sd env_logger claude_quota_client
                                     weather_client input_manager sys_state
                                     llm_provider asr_client tts_client audio_pipeline)
