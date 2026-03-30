idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE -D_GNU_SOURCE)
