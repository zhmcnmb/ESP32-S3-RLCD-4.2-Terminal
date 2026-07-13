idf_component_register(SRCS "board_rlcd42.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_driver_spi esp_driver_gpio)
