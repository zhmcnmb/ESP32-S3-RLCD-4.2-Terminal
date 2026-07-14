idf_component_register(SRCS "weather_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client mdns cjson)
