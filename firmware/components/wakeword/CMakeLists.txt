idf_component_register(SRCS "wakeword.c" "wakeword_loop.c"
                       INCLUDE_DIRS "include"
                       REQUIRES audio_pipeline esp-sr)
