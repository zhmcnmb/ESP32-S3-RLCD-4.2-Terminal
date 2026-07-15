# ESP-IDF 6.0 移除了内置 json 组件(改为 espressif/cjson managed component)，
# 但 esp-sr 2.4.0 的 CMakeLists 仍 REQUIRES json。此组件做桥接：
# REQUIRES espressif__cjson 使 cJSON.h 头文件路径对依赖 json 的组件可见。
idf_component_register(INCLUDE_DIRS "include" REQUIRES espressif__cjson)
