idf_component_register(SRCS "sys_state.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sensor_shtc3 battery net_manager storage_sd time_service esp_app_format)

