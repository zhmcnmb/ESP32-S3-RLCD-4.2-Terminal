idf_component_register(SRCS "cloud_transport.c" "cloud_transport_stream.c" "cloud_transport_conn.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_client esp_rom mbedtls esp_timer)
