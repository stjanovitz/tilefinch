if(NOT DEFINED PATCH_SOURCE_DIR
   OR NOT DEFINED PATCH_EXECUTABLE
   OR NOT DEFINED TILEFINCH_SOURCE_DIR)
    message(FATAL_ERROR "PocketSphinx patch inputs are incomplete")
endif()

# One ordered list: later patches rewrite lines earlier ones added (the
# fixed-recognizer patch rewrites packed-lexicon lines), which
# cmake/apply_patch.cmake can only sort out when it sees the whole chain.
set(patch_files "")
foreach(patch_name
        pocketsphinx-5.1.1-psp-int32.patch
        pocketsphinx-5.1.1-psp-no-mmap.patch
        pocketsphinx-5.1.1-psp-timer.patch
        pocketsphinx-5.1.1-packed-lexicon.patch
        pocketsphinx-5.1.1-fixed-recognizer.patch
        pocketsphinx-5.1.1-stream-sendump.patch
        pocketsphinx-5.1.1-three-state-hmm.patch
        pocketsphinx-5.1.1-compact-search.patch)
    list(APPEND patch_files
        "${TILEFINCH_SOURCE_DIR}/patches/pocketsphinx/${patch_name}")
endforeach()
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPATCH_SOURCE_DIR=${PATCH_SOURCE_DIR}"
        "-DPATCH_FILE=${patch_files}"
        "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
        -P "${TILEFINCH_SOURCE_DIR}/cmake/apply_patch.cmake"
    RESULT_VARIABLE patch_result)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "Could not apply the PocketSphinx PSP patches")
endif()
