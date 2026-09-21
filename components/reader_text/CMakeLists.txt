idf_component_register(
    SRCS "reader_text.c" "generated/cp950_sparse_table.c"
    INCLUDE_DIRS "include" "generated"
    REQUIRES fatfs
)
