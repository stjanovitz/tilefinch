# Public clones generate data-only test resources without network downloads.
# Shipping code retains only the immutable resource keys and integrity pins.
if(NOT PSP)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(TILEFINCH_UI_RESOURCE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ui-resources")
    set(_tilefinch_ui_resource_files)
    execute_process(COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_ui_translations.py" --list-resources
        OUTPUT_VARIABLE _tilefinch_ui_resource_keys
        RESULT_VARIABLE _tilefinch_ui_resource_result OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _tilefinch_ui_resource_result EQUAL 0)
        message(FATAL_ERROR "Cannot enumerate immutable UI resource dependencies")
    endif()
    string(REPLACE "\n" ";" _tilefinch_ui_resource_keys "${_tilefinch_ui_resource_keys}")
    foreach(_resource IN LISTS _tilefinch_ui_resource_keys)
        list(APPEND _tilefinch_ui_resource_files "${TILEFINCH_UI_RESOURCE_ROOT}/${_resource}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_ui_translations.py")
    file(GLOB _tilefinch_ui_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ui/v*/source.tsv")
    add_custom_command(OUTPUT ${_tilefinch_ui_resource_files}
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_ui_translations.py"
            --resources-only --resource-dir "${TILEFINCH_UI_RESOURCE_ROOT}"
        DEPENDS ${_tilefinch_ui_sources}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_ui_translations.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/devanagari_sequences.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/indic_sequences.py"
        COMMENT "Generate bounded interface test resources in the build tree"
        VERBATIM)
    add_custom_target(tilefinch-ui-resources DEPENDS ${_tilefinch_ui_resource_files})
endif()
