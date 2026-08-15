idf_component_register(SRCS "agent_runtime.c" "agent_text.c" "agent_session.c" "agent_args.c" "agent_tools.c"
                            "agent_memory_tools.c" "agent_scheduler_tools.c"
                       INCLUDE_DIRS "include"
                       REQUIRES llm_provider storage_sd sys_state web_client skill_manager memory_engine scheduler cjson esp_hw_support
                       PRIV_REQUIRES esp_http_client)
