# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.51")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(metalio-hw-test)

# Register the complete font in ESP-IDF's standard flash manifest. Individual
# app/font update targets deliberately do not write partition tables or NVS.
idf_build_get_property(delivery_python PYTHON)
idf_build_get_property(delivery_build_dir BUILD_DIR)
set(full_font_pack "${CMAKE_SOURCE_DIR}/assets/fonts/harmony_full.fontpack")
set(full_font_bin "${delivery_build_dir}/font_data.bin")
add_custom_target(fontpack
    COMMAND "${delivery_python}" "${CMAKE_SOURCE_DIR}/tools/check_full_font.py"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${full_font_pack}" "${full_font_bin}"
    BYPRODUCTS "${full_font_bin}"
    COMMENT "Checking and staging the complete font pack"
    VERBATIM)
partition_table_add_check_size_target(fontpack-size-check
    DEPENDS fontpack
    BINARY_PATH "${full_font_bin}"
    PARTITION_TYPE data
    PARTITION_SUBTYPE 0x40)
esptool_py_flash_to_partition(flash font_data "${full_font_bin}")
esptool_py_custom_target(font-flash font "fontpack-size-check")
esptool_py_flash_to_partition(font-flash font_data "${full_font_bin}")

add_custom_target(delivery-manifest ALL
    COMMAND "${delivery_python}" "${CMAKE_SOURCE_DIR}/tools/check_build_delivery.py"
        --build-dir "${delivery_build_dir}"
    DEPENDS app bootloader partition_table_bin fontpack-size-check
    BYPRODUCTS "${delivery_build_dir}/delivery_manifest.json"
    COMMENT "Validating flash boundaries and writing delivery hashes"
    VERBATIM)
add_dependencies(flash delivery-manifest)
