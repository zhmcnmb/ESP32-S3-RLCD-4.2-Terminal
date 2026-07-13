idf_component_register(SRCS "sensor_shtc3.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board_rlcd42 esp_driver_i2c)
