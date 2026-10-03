idf_component_register(SRCS "canon_bjl.c" "testpage.c" "bjl_render.c"
                       INCLUDE_DIRS "include")
# identical float evaluation on host and target (see host/Makefile)
target_compile_options(${COMPONENT_LIB} PRIVATE -ffp-contract=off)
