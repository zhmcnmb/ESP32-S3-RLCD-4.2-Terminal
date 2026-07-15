idf_component_register(SRCS "asr_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES cloud_transport storage_sd cjson esp_http_client)
