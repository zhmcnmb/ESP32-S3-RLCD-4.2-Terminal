idf_component_register(SRCS "wifi_setup.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sys_state net_manager esp_wifi esp_event)
