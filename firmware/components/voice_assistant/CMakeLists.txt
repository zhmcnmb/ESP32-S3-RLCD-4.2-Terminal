idf_component_register(SRCS "voice_assistant.c" "voice_phase.c" "voice_wav.c" "voice_tts_play.c" "voice_tts_stream.c" "tts_text.c" "voice_reminder.c" "voice_text_turn.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sys_state wakeword audio_pipeline asr_client tts_client
                                agent_runtime storage_sd esp_timer)
