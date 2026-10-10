# Public clones generate data-only test resources without network downloads.
# Shipping code retains only the immutable resource keys and integrity pins.
if(NOT PSP)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(TILEFINCH_UI_RESOURCE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ui-resources")
    set(_tilefinch_ui_resource_files)
    foreach(_version RANGE 1 5)
        if(_version EQUAL 1)
            set(_languages es fr de ja)
        elseif(_version EQUAL 2)
            set(_languages ru uk zh-hans ko)
        elseif(_version EQUAL 3)
            set(_languages hi ar)
        else()
            set(_languages es fr de ja ru uk zh-hans ko hi ar)
        endif()
        foreach(_language IN LISTS _languages)
            list(APPEND _tilefinch_ui_resource_files
                "${TILEFINCH_UI_RESOURCE_ROOT}/translations/ui/v${_version}/${_language}.tful")
        endforeach()
    endforeach()
    file(GLOB _tilefinch_ui_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ui/v*/source.tsv")
    add_custom_command(OUTPUT ${_tilefinch_ui_resource_files}
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_ui_translations.py"
            --resources-only --resource-dir "${TILEFINCH_UI_RESOURCE_ROOT}"
        DEPENDS ${_tilefinch_ui_sources}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_ui_translations.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/devanagari_sequences.py"
        COMMENT "Generate bounded interface test resources in the build tree"
        VERBATIM)
    add_custom_target(tilefinch-ui-resources DEPENDS ${_tilefinch_ui_resource_files})
endif()
