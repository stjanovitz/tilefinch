if(NOT DEFINED PSP_NM OR NOT DEFINED PSP_ELF)
    message(FATAL_ERROR
        "CheckPspHotSymbolSizes requires PSP_NM and PSP_ELF")
endif()

execute_process(
    COMMAND "${PSP_NM}" -S --size-sort "${PSP_ELF}"
    RESULT_VARIABLE nm_status
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error)
if(NOT nm_status EQUAL 0)
    message(FATAL_ERROR "psp-nm failed: ${nm_error}")
endif()

function(check_hot_symbol symbol limit)
    string(REGEX MATCH
        "[0-9A-Fa-f]+[ \t]+([0-9A-Fa-f]+)[ \t]+[Tt][ \t]+${symbol}([\r\n]|$)"
        row "${nm_output}")
    if(NOT row)
        message(FATAL_ERROR
            "PSP hot-symbol ratchet could not find ${symbol} in ${PSP_ELF}")
    endif()
    string(REGEX REPLACE
        ".*[ \t]([0-9A-Fa-f]+)[ \t]+[Tt][ \t]+${symbol}([\r\n]|$)"
        "\\1" size_hex "${row}")
    math(EXPR size "0x${size_hex}")
    if(size GREATER limit)
        message(FATAL_ERROR
            "PSP hot function ${symbol} grew to ${size} bytes; measured "
            "ceiling is ${limit}. Split cold paths or re-measure the device "
            "cost before raising this ratchet.")
    endif()
    message(STATUS "PSP hot function ${symbol}: ${size}/${limit} bytes")
endfunction()

# These ratchets have deliberately different meanings. `main` is a process
# growth tripwire over boot and teardown; psp_app_run_interactive is the real
# resident frame-loop footprint. The browser and media compositors are also
# actual per-frame instruction-cache footprints; psp_ui_composite itself is
# only their dispatcher and is not a useful hot-path measurement. Re-measured
# 2026-08-13 after the owner/loop and composition splits.
if(NOT DEFINED PSP_MAIN_LIMIT)
    set(PSP_MAIN_LIMIT 10752)
endif()
check_hot_symbol(main ${PSP_MAIN_LIMIT})
if(NOT DEFINED PSP_INTERACTIVE_LIMIT)
    set(PSP_INTERACTIVE_LIMIT 15360)
endif()
check_hot_symbol(psp_app_run_interactive ${PSP_INTERACTIVE_LIMIT})
check_hot_symbol(layout_block_impl 36864)
check_hot_symbol(psp_ui_composite_browser 4096)
check_hot_symbol(psp_ui_media_composite_with_preview 4096)
check_hot_symbol(rasterize_command 8192)
# Spatial navigation and page scrolling enter this receiver on every d-pad
# repeat.  Keep its rare text/media/update arms behind the cold dispatcher;
# otherwise Allegrex pays that multi-kilobyte frame and I-cache footprint for
# every focus step.
check_hot_symbol(psp_app_dispatch_action 1024)

# Transitional frontend access to the engine's expiring navigation/controller
# views must only decrease. The data-model migration is complete when this
# reaches zero; a source ratchet is more honest than a generation counter that
# could miss an uninstrumented mutation. Count only PSP frontend call sites,
# not the API declarations or engine implementation.
if(DEFINED TILEFINCH_SOURCE_DIR)
    file(GLOB raw_view_sources
        "${TILEFINCH_SOURCE_DIR}/src/psp_script_main.c"
        "${TILEFINCH_SOURCE_DIR}/src/psp_app/*.c")
    set(raw_view_count 0)
    foreach(source IN LISTS raw_view_sources)
        file(READ "${source}" source_text)
        string(REGEX MATCHALL
            "browser_engine_(navigation|controller)_view[ 	]*\\("
            raw_view_matches "${source_text}")
        list(LENGTH raw_view_matches source_count)
        math(EXPR raw_view_count "${raw_view_count} + ${source_count}")
    endforeach()
    set(raw_view_limit 0)
    if(raw_view_count GREATER raw_view_limit)
        message(FATAL_ERROR
            "PSP raw engine-view call sites grew to ${raw_view_count}; "
            "nonincreasing ceiling is ${raw_view_limit}. Refresh a copied "
            "frame snapshot instead of retaining another expiring view.")
    endif()
    message(STATUS
        "PSP raw engine-view call sites: ${raw_view_count}/${raw_view_limit} "
        "(migration target 0)")
endif()
