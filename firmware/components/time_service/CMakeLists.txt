idf_component_register(SRCS "time_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES rtc_pcf85063 esp_netif lwip)
