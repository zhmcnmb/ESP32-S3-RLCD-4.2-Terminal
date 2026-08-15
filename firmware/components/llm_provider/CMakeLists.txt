idf_component_register(SRCS "llm_provider.c" "llm_request.c" "llm_sse.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cloud_transport esp_http_client)
