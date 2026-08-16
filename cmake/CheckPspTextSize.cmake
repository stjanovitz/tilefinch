if(NOT DEFINED PSP_OBJDUMP OR NOT DEFINED PSP_ELF
   OR NOT DEFINED PSP_TEXT_LIMIT)
    message(FATAL_ERROR
        "CheckPspTextSize requires PSP_OBJDUMP, PSP_ELF, and PSP_TEXT_LIMIT")
endif()

execute_process(
    COMMAND "${PSP_OBJDUMP}" -h "${PSP_ELF}"
    RESULT_VARIABLE objdump_status
    OUTPUT_VARIABLE objdump_output
    ERROR_VARIABLE objdump_error)
if(NOT objdump_status EQUAL 0)
    message(FATAL_ERROR "psp-objdump failed: ${objdump_error}")
endif()

function(sum_elf_sections prefix output)
    string(REGEX MATCHALL
        "[\r\n][ \t]*[0-9]+[ \t]+\\.${prefix}(\\.[^ \t\r\n]+)?[ \t]+[0-9A-Fa-f]+"
        section_rows "${objdump_output}")
    set(total 0)
    foreach(section_row IN LISTS section_rows)
        string(REGEX MATCH "[0-9A-Fa-f]+$" section_size "${section_row}")
        math(EXPR total "${total} + 0x${section_size}")
    endforeach()
    set("${output}" "${total}" PARENT_SCOPE)
endfunction()

sum_elf_sections("text" text_bytes)
sum_elf_sections("rodata" rodata_bytes)
if(text_bytes EQUAL 0)
    message(FATAL_ERROR
        "could not find an ELF .text section in:\n${objdump_output}")
endif()
if(text_bytes GREATER PSP_TEXT_LIMIT)
    message(FATAL_ERROR
        "PSP .text grew to ${text_bytes} bytes; measured ceiling is "
        "${PSP_TEXT_LIMIT}. Re-measure and justify the new device cost "
        "before raising the ratchet.")
endif()
message(STATUS
    "PSP .text ${text_bytes}/${PSP_TEXT_LIMIT} bytes; "
    ".rodata ${rodata_bytes} bytes (${PSP_ELF})")
