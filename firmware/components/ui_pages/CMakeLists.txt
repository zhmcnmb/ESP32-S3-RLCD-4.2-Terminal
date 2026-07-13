idf_component_register(SRCS "pages/page_home.c" "pages/page_diag.c" "pages/ui_icons.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ui_manager board_rlcd42 display_st7305
                                sensor_shtc3 time_service net_manager
                                battery storage_sd)
