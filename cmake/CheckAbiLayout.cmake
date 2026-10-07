# Compares the public struct layout probes (src/abi_layout_probe.c) compiled
# with tilefinch_core's settings against the same file compiled with each
# consumer's settings. Every probe symbol's size encodes a struct size or a
# field offset, so a public header whose layout depends on a private define
# fails here instead of silently shifting fields between libraries.
#
#   -DPSP_NM=<nm> -DABI_REFERENCE=<object> -DABI_CONSUMERS=<object|...>
if(NOT DEFINED PSP_NM OR NOT DEFINED ABI_REFERENCE
   OR NOT DEFINED ABI_CONSUMERS)
    message(FATAL_ERROR
        "CheckAbiLayout requires PSP_NM, ABI_REFERENCE and ABI_CONSUMERS")
endif()

function(read_probes object out_names out_sizes)
    execute_process(
        COMMAND "${PSP_NM}" -S --radix=d "${object}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "nm failed on ${object}: ${error}")
    endif()
    string(REGEX MATCHALL
        "[0-9]+[ \t]+([0-9]+)[ \t]+[BbCcDd][ \t]+tilefinch_abi_[A-Za-z0-9_]+"
        rows "${output}")
    set(names)
    set(sizes)
    foreach(row IN LISTS rows)
        string(REGEX REPLACE
            "^[0-9]+[ \t]+([0-9]+)[ \t]+[BbCcDd][ \t]+(tilefinch_abi_[A-Za-z0-9_]+)$"
            "\\2;\\1" pair "${row}")
        list(GET pair 0 name)
        list(GET pair 1 size)
        list(APPEND names "${name}")
        list(APPEND sizes "${size}")
    endforeach()
    if(NOT names)
        message(FATAL_ERROR "no layout probes found in ${object}")
    endif()
    set(${out_names} "${names}" PARENT_SCOPE)
    set(${out_sizes} "${sizes}" PARENT_SCOPE)
endfunction()

string(REPLACE "|" ";" ABI_CONSUMERS "${ABI_CONSUMERS}")
read_probes("${ABI_REFERENCE}" reference_names reference_sizes)
list(LENGTH reference_names probe_count)
set(failures "")
foreach(consumer IN LISTS ABI_CONSUMERS)
    read_probes("${consumer}" names sizes)
    list(LENGTH names count)
    if(NOT count EQUAL probe_count)
        string(APPEND failures
            "\n  ${consumer}: ${count} probes, core has ${probe_count}")
        continue()
    endif()
    math(EXPR last "${probe_count} - 1")
    foreach(index RANGE ${last})
        list(GET reference_names ${index} name)
        list(GET reference_sizes ${index} expected)
        list(FIND names "${name}" at)
        if(at EQUAL -1)
            string(APPEND failures "\n  ${consumer}: ${name} missing")
            continue()
        endif()
        list(GET sizes ${at} actual)
        if(NOT actual EQUAL expected)
            string(APPEND failures
                "\n  ${name}: core ${expected}, ${consumer} ${actual}")
        endif()
    endforeach()
endforeach()
if(failures)
    message(FATAL_ERROR
        "Public struct layout differs between tilefinch_core and its "
        "consumers (a public header depends on a private define):${failures}")
endif()
message(STATUS "Public struct layouts agree (${probe_count} probes)")
