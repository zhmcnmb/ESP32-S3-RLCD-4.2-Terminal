idf_component_register(SRCS "voice_assistant.c" "voice_wav.c" "voice_tts_stream.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sys_state wakeword audio_pipeline asr_client tts_client
                                agent_runtime storage_sd esp_timer)
