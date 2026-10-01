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

function(check_absent_symbol prefix)
    set(symbol_types "[Tt]")
    if(ARGC GREATER 1)
        set(symbol_types "${ARGV1}")
    endif()
    string(REGEX MATCH
        "${symbol_types}[ \t]+${prefix}(\\.[^ \t\r\n]+)?([\r\n]|$)"
        row "${nm_output}")
    if(row)
        message(FATAL_ERROR
            "Disabled PSP instrumentation ${prefix} remains in ${PSP_ELF}")
    endif()
endfunction()

# FreeType must share the browser's global zlib symbols, not retain a second
# private (local-text) inflater. Global inflate remains required by HTTPS.
if(PSP_SHARED_FONT_ZLIB)
    check_absent_symbol(inflate "t")
    check_absent_symbol(inflateInit2_ "t")
    check_absent_symbol(inflate_table "t")
endif()

# A runtime-off feature can otherwise retain tens of KiB unnoticed while
# staying under the global ceiling. Pin its absence on the actual linked
# image, including GCC's cloned (.isra/.constprop) symbol variants.
if(PSP_NO_SCRIPT_SAMPLER)
    check_absent_symbol(script_profile_sample)
    check_absent_symbol(script_profile_charge)
    check_absent_symbol(youtube_resolve_job_log_response)
    check_absent_symbol(js_rt_dump_interpreter_stack)
endif()
if(PSP_NO_FETCH_TRACE)
    check_absent_symbol(trace_capture_response)
    check_absent_symbol(trace_capture_response_at)
    check_absent_symbol(trace_replay_response)
    check_absent_symbol(trace_replay_response_with_scratch)
endif()

function(hot_symbol_size symbol out)
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
    set(${out} ${size} PARENT_SCOPE)
endfunction()

function(check_hot_symbol symbol limit)
    hot_symbol_size(${symbol} size)
    if(size GREATER limit)
        message(FATAL_ERROR
            "PSP hot function ${symbol} grew to ${size} bytes; measured "
            "ceiling is ${limit}. Split cold paths or re-measure the device "
            "cost before raising this ratchet.")
    endif()
    message(STATUS "PSP hot function ${symbol}: ${size}/${limit} bytes")
endfunction()

# The code one kind of frame runs through: the sum of its step functions.
function(check_hot_set label limit)
    set(total 0)
    foreach(symbol IN LISTS ARGN)
        hot_symbol_size(${symbol} size)
        math(EXPR total "${total} + ${size}")
    endforeach()
    if(total GREATER limit)
        message(FATAL_ERROR
            "PSP ${label} steps grew to ${total} bytes; measured ceiling is "
            "${limit}. Move work that does not run every such frame into a "
            "step behind its own check, or re-measure the device cost before "
            "raising this ratchet.")
    endif()
    message(STATUS "PSP ${label} steps: ${total}/${limit} bytes")
endfunction()

# These ratchets have deliberately different meanings. `main` is a process
# growth tripwire over boot and teardown. The browser and media compositors
# are actual per-frame instruction-cache footprints; psp_ui_composite itself
# is only their dispatcher and is not a useful hot-path measurement.
#
# The interactive loop is psp_app_run_interactive, a cold driver around
# psp_loop_frame, one frame's sequence of steps. Work a frame does not always
# need lives in a psp_loop_* step behind its own check, so what a frame runs
# is psp_loop_frame plus the steps listed for its kind below; rare steps
# (suspend/resume, update, downloads, a navigation's end) are unbounded here.
# The two sets keep the 14,336 B shipping envelope the single loop function
# was measured against on the device (2026-09-23); psp_loop_frame itself is a
# tighter tripwire so new work arrives as a named step, not inline.
if(NOT DEFINED PSP_MAIN_LIMIT)
    set(PSP_MAIN_LIMIT 10752)
endif()
check_hot_symbol(main ${PSP_MAIN_LIMIT})
if(NOT DEFINED PSP_LOOP_FRAME_LIMIT)
    set(PSP_LOOP_FRAME_LIMIT 5120)
endif()
if(NOT DEFINED PSP_FRAME_SET_LIMIT)
    set(PSP_FRAME_SET_LIMIT 14336)
endif()
check_hot_symbol(psp_loop_frame ${PSP_LOOP_FRAME_LIMIT})
set(page_frame_steps
    psp_loop_frame psp_loop_media_frame psp_loop_update_frame
    psp_loop_home_preconnect psp_loop_pump_navigation psp_loop_render_job
    psp_loop_track_recovery)
set(media_frame_steps
    psp_loop_frame psp_loop_media_frame psp_loop_media_input
    psp_loop_update_frame psp_loop_track_recovery)
if(PSP_VALIDATION_LOG)
    # Scripted input and the power test's tick run every validation frame.
    list(APPEND page_frame_steps
        psp_loop_scripted_input psp_loop_power_test_tick)
    list(APPEND media_frame_steps
        psp_loop_scripted_input psp_loop_power_test_tick)
endif()
check_hot_set("page frame" ${PSP_FRAME_SET_LIMIT} ${page_frame_steps})
check_hot_set("media frame" ${PSP_FRAME_SET_LIMIT} ${media_frame_steps})
check_hot_symbol(layout_block_impl 36864)
check_hot_symbol(psp_ui_composite_browser 4096)
check_hot_symbol(psp_ui_media_composite_layers 4096)
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
