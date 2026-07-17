idf_component_register(SRCS "display_st7305.c" "display_text.c" "font_cn16.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd board_rlcd42 esp_driver_gpio)
