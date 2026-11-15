idf_component_register(SRCS "cJSON/cJSON.c"
                       INCLUDE_DIRS "cJSON")

target_compile_definitions(${COMPONENT_LIB} PUBLIC
                           CJSON_NESTING_LIMIT=1000
                           CJSON_CIRCULAR_LIMIT=10000)
