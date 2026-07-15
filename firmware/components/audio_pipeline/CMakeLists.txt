idf_component_register(SRCS "audio_pipeline.c" "codec_dev_factory.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_rlcd42 esp_driver_i2s esp_driver_gpio esp_codec_dev)
