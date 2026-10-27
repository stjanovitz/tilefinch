# Frontend access to engine internals must only decrease. The PSP frontend is
# meant to talk to the engine through the BrowserEngine facade and copied
# snapshots; each pattern below is a way it still reaches into engine-owned
# state instead:
#   - accessor calls that hand out the engine's own NavigationSession,
#     BrowserController or BrowserSession (mutable or `_view`);
#   - `->navigation->` / `.navigation->` dereferences, which read the live
#     NavigationSession through the PspEngineViews alias (the views hold
#     pointers, not copies);
#   - navigation_current() calls on that session;
#   - `NavigationSession *` declarations (locals, parameters, fields): a
#     read through a local alias (`NavigationSession *nav = ...;
#     nav->generation`) matches none of the patterns above, so each alias
#     is counted where it is declared.
# Each count was frozen when the check was widened (2026-10-25, main
# b5099193) and must only go down; lower a limit in the same commit that
# migrates sites. 2026-10-26: the generation, loaded, current-entry and
# history-peek reads moved to copied facade queries
# (browser_engine_page_generation(), browser_engine_current_entry(), ...)
# and validation reports to browser_engine_validation_counters(). A source
# ratchet is more honest than a generation counter that could miss an
# uninstrumented mutation. Count only PSP frontend sources, not the API
# declarations or the engine implementation.
#
# What is counted (2026-10-26, review TB-5): the shipping frontend, that is
# src/psp_script_main.c and src/psp_app/*.c and *.h, without the units only
# validation builds compile (TILEFINCH_PSP_VALIDATION_LOG in
# TilefinchTargets.cmake), so moving a shipping read into a probe is not a
# migration. Comments, string and character literals are stripped first.
# Those rules took the counts from 15/107/3 to 13/104/1 (the validation
# units held 2, 3 and 2; comments and headers held none) and added the alias
# count at 19.
#
#   cmake -DTILEFINCH_SOURCE_DIR=<repo> -P CheckPspFrontendEngineAccess.cmake
#
# checks the ratchet. With -DTILEFINCH_FRONTEND_ACCESS_EXPECT="a;b;c;d" it
# instead requires exactly those four counts (the counter's own test).

if(NOT DEFINED TILEFINCH_SOURCE_DIR)
    message(FATAL_ERROR "CheckPspFrontendEngineAccess requires TILEFINCH_SOURCE_DIR")
endif()

# Built only into validation EBOOTs (psp-browser-script under
# TILEFINCH_PSP_VALIDATION_LOG).
set(frontend_validation_only_sources
    psp_app_input_script.c
    psp_app_validation_cadence.c
    psp_app_validation_loop.c
    psp_app_validation_present.c
    psp_app_validation_qualify.c)

file(GLOB frontend_engine_candidates
    "${TILEFINCH_SOURCE_DIR}/src/psp_script_main.c"
    "${TILEFINCH_SOURCE_DIR}/src/psp_app/*.c"
    "${TILEFINCH_SOURCE_DIR}/src/psp_app/*.h")
set(frontend_engine_sources)
foreach(source IN LISTS frontend_engine_candidates)
    get_filename_component(source_name "${source}" NAME)
    if(source_name MATCHES "^psp_app_validation_.*\\.c$")
        if(NOT source_name IN_LIST frontend_validation_only_sources)
            message(FATAL_ERROR
                "${source_name} looks validation-only but is not listed in "
                "CheckPspFrontendEngineAccess.cmake; list it if only "
                "validation builds compile it, or rename it.")
        endif()
    endif()
    if(NOT source_name IN_LIST frontend_validation_only_sources)
        list(APPEND frontend_engine_sources "${source}")
    endif()
endforeach()
if(NOT frontend_engine_sources)
    message(FATAL_ERROR "No PSP frontend sources under ${TILEFINCH_SOURCE_DIR}/src")
endif()

# C source text without comments, string literals or character literals
# (each literal becomes empty, so `"//"` cannot open a comment). Not a full
# lexer: a string that continues across a backslash-newline keeps its text.
function(frontend_code_text out_var source)
    file(READ "${source}" text)
    string(REGEX REPLACE "'([^'\\\\\n]|\\\\.)'" "''" text "${text}")
    string(REGEX REPLACE "\"([^\"\\\\\n]|\\\\.)*\"" "\"\"" text "${text}")
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " text "${text}")
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

set(frontend_engine_texts)
set(frontend_engine_text_count 0)
foreach(source IN LISTS frontend_engine_sources)
    frontend_code_text(code "${source}")
    set(frontend_engine_text_${frontend_engine_text_count} "${code}")
    math(EXPR frontend_engine_text_count "${frontend_engine_text_count} + 1")
endforeach()

function(count_frontend_engine_access out_var pattern)
    set(total 0)
    math(EXPR last "${frontend_engine_text_count} - 1")
    foreach(index RANGE ${last})
        string(REGEX MATCHALL "${pattern}" matches "${frontend_engine_text_${index}}")
        list(LENGTH matches source_count)
        math(EXPR total "${total} + ${source_count}")
    endforeach()
    set(${out_var} ${total} PARENT_SCOPE)
endfunction()

set(frontend_engine_counts)
function(check_frontend_engine_access label limit pattern advice)
    count_frontend_engine_access(access_count "${pattern}")
    set(counts ${frontend_engine_counts} ${access_count})
    set(frontend_engine_counts ${counts} PARENT_SCOPE)
    if(DEFINED TILEFINCH_FRONTEND_ACCESS_EXPECT)
        return()
    endif()
    if(access_count GREATER limit)
        message(FATAL_ERROR
            "PSP frontend ${label} grew to ${access_count}; nonincreasing "
            "ceiling is ${limit}. ${advice}")
    endif()
    message(STATUS
        "PSP frontend ${label}: ${access_count}/${limit} (migration target 0)")
endfunction()

check_frontend_engine_access("engine-internals accessor calls" 13
    "browser_engine_(navigation|controller|session)(_view)?[ 	]*\\("
    "Add or use a BrowserEngine facade call instead of borrowing the engine's session or controller.")
check_frontend_engine_access("NavigationSession alias reads" 104
    "(->|\\.)navigation->"
    "Copy the fact into a snapshot or facade query instead of reading the live NavigationSession.")
check_frontend_engine_access("navigation_current() calls" 1
    "(^|[^A-Za-z0-9_])navigation_current[ 	]*\\("
    "Use a facade query for the current entry instead.")
check_frontend_engine_access("NavigationSession pointer declarations" 19
    "(^|[^A-Za-z0-9_])NavigationSession[ 	\n]*\\*"
    "A local NavigationSession alias reads the live session out of sight of the other counts; copy the fact into a snapshot or facade query instead.")

if(DEFINED TILEFINCH_FRONTEND_ACCESS_EXPECT
   AND NOT "${frontend_engine_counts}" STREQUAL "${TILEFINCH_FRONTEND_ACCESS_EXPECT}")
    message(FATAL_ERROR
        "PSP frontend engine-access counts are ${frontend_engine_counts}; "
        "expected ${TILEFINCH_FRONTEND_ACCESS_EXPECT}")
endif()
