idf_component_register(SRCS "cloud_transport.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client esp_rom mbedtls)
