idf_component_register(SRCS "memory_engine.c" "memory_store.c" "memory_profile.c" "memory_recall.c" "memory_writer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES storage_sd time_service cjson esp_hw_support esp_timer)
