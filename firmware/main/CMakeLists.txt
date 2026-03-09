idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES app)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WEBUI_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../spiffs")
set(WEBUI_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/spiffs_min")
set(WEBUI_MINIFY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../tools/minify_spiffs.py")
set(WEBUI_MINIFY_STAMP "${WEBUI_OUT_DIR}/.minified.stamp")

file(GLOB_RECURSE WEBUI_SOURCE_FILES CONFIGURE_DEPENDS "${WEBUI_SRC_DIR}/*")

add_custom_command(
    OUTPUT "${WEBUI_MINIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${WEBUI_OUT_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${WEBUI_MINIFY_SCRIPT}" --src "${WEBUI_SRC_DIR}" --dst "${WEBUI_OUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${WEBUI_MINIFY_STAMP}"
    DEPENDS "${WEBUI_MINIFY_SCRIPT}" ${WEBUI_SOURCE_FILES}
    COMMENT "Minifying WebUI assets for SPIFFS image"
    VERBATIM
)

add_custom_target(webui_minify DEPENDS "${WEBUI_MINIFY_STAMP}")

spiffs_create_partition_image(storage "${WEBUI_OUT_DIR}" FLASH_IN_PROJECT DEPENDS webui_minify)
