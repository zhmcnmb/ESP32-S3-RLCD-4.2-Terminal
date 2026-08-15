idf_component_register(SRCS "rtc_pcf85063.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_rlcd42 esp_driver_i2c)
