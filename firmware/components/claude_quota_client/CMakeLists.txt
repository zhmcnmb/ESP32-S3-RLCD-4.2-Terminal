idf_component_register(SRCS "claude_quota_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client mdns cjson sys_state)
