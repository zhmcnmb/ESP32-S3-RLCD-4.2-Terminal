idf_component_register(SRCS "env_logger.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sensor_shtc3 storage_sd sys_state)
