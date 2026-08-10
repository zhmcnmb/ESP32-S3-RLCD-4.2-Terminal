idf_component_register(SRCS "pages/page_jarvis.c" "pages/page_home.c"
                       "pages/page_env_monitor.c" "pages/page_weather.c"
                       "pages/page_wifi.c"
                       "pages/ui_icons.c" "pages/ui_v2.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ui_manager display_st7305 sys_state wifi_setup)
