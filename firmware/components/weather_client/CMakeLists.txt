idf_component_register(SRCS "weather_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES cloud_transport cjson sys_state)
