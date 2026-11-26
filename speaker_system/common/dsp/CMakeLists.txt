idf_component_register(SRCS "dsp.c" INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
