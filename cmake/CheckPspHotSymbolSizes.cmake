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
    string(REGEX MATCHALL
        "[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+[Tt][ \t]+${symbol}(\\.[^ \t\r\n]+)?([\r\n]|$)"
        rows "${nm_output}")
    if(NOT rows)
        message(FATAL_ERROR
            "PSP hot-symbol ratchet could not find ${symbol} in ${PSP_ELF}")
    endif()
    # Constant-propagation can replace a local function with a clone.
    # Charge every variant, not just a convenient surviving base symbol.
    set(size 0)
    foreach(row IN LISTS rows)
        string(REGEX REPLACE
            "^[0-9A-Fa-f]+[ \t]+([0-9A-Fa-f]+).*"
            "\\1" size_hex "${row}")
        math(EXPR size "${size} + 0x${size_hex}")
    endforeach()
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
# growth tripwire over boot and teardown: the ordered list of boot phases
# (src/psp_app/psp_app_boot.c), the validation hooks between them, and the
# shutdown. New boot work belongs in a phase, not inline in main. The browser and media compositors
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
    set(PSP_MAIN_LIMIT 6144)
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
    # Scripted input (its psp_loop_frame hook and the step it calls) and the
    # power test's tick run every validation frame.
    list(APPEND page_frame_steps
        psp_validation_scripted_frame psp_loop_scripted_input
        psp_loop_power_test_tick)
    list(APPEND media_frame_steps
        psp_validation_scripted_frame psp_loop_scripted_input
        psp_loop_power_test_tick)
endif()
check_hot_set("page frame" ${PSP_FRAME_SET_LIMIT} ${page_frame_steps})
check_hot_set("media frame" ${PSP_FRAME_SET_LIMIT} ${media_frame_steps})
# Every block runs layout_block_impl and its child-flow phase. Rare arms
# (controls, replaced content, list markers, <hr>, motion scenes, masks,
# positioned finalization) live in noinline helpers behind their guards;
# keep them there rather than growing the recursive hot path. Measured
# 2026-10-26: 11,768 + 7,240 bytes.
check_hot_symbol(layout_block_impl 12800)
check_hot_set("block layout" 20480 layout_block_impl layout_block_flow_children)
check_hot_symbol(psp_ui_composite_browser 4096)
check_hot_symbol(psp_ui_media_composite_layers 4096)
check_hot_symbol(rasterize_command 8192)
# Spatial navigation and page scrolling enter this receiver on every d-pad
# repeat.  Keep its rare text/media/update arms behind the cold dispatcher;
# otherwise Allegrex pays that multi-kilobyte frame and I-cache footprint for
# every focus step.
check_hot_symbol(psp_app_dispatch_action 1024)
# The streaming parser calls this for every closed element. Its </script>
# work (navigation_stream_script_closed) stays out of line so other elements
# do not pay that path's multi-kilobyte stack frame and register saves
# (4,416 B frame and 11 saved registers when it was inline, 2026-10-26).
check_hot_symbol(navigation_stream_element_closed 1024)

# Frontend access to engine internals must only decrease; the ratchet and
# its history live in CheckPspFrontendEngineAccess.cmake, which a host test
# also runs.
if(DEFINED TILEFINCH_SOURCE_DIR)
    include("${CMAKE_CURRENT_LIST_DIR}/CheckPspFrontendEngineAccess.cmake")
endif()
