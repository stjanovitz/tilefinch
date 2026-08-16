include_guard(GLOBAL)

function(tilefinch_restore_cache_variable name had_value value type help)
    if(had_value)
        set("${name}" "${value}" CACHE "${type}" "${help}" FORCE)
    else()
        unset("${name}" CACHE)
    endif()
endfunction()

function(tilefinch_configure_psp_voice)
    if(NOT PSP OR NOT PSP_BROWSER_ENABLE_PSP_VOICE)
        return()
    endif()

    set(_voice_cache_names
        FIXED_POINT PS_THREAD_LOCAL_RNG BUILD_SHARED_LIBS BUILD_TESTING)
    foreach(_name IN LISTS _voice_cache_names)
        get_property(_had CACHE "${_name}" PROPERTY TYPE SET)
        set("_voice_had_${_name}" "${_had}")
        if(_had)
            get_property(_type CACHE "${_name}" PROPERTY TYPE)
            get_property(_help CACHE "${_name}" PROPERTY HELPSTRING)
            get_property(_value CACHE "${_name}" PROPERTY VALUE)
            set("_voice_type_${_name}" "${_type}")
            set("_voice_help_${_name}" "${_help}")
            set("_voice_value_${_name}" "${_value}")
        endif()
    endforeach()

    set(FIXED_POINT ON CACHE BOOL "PocketSphinx fixed-point arithmetic" FORCE)
    set(PS_THREAD_LOCAL_RNG OFF CACHE BOOL
        "PocketSphinx thread-local random state" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL
        "PocketSphinx static library" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL
        "PocketSphinx dependency tests" FORCE)

    set(_patch_driver
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apply_pocketsphinx_patches.cmake")
    if(PSP_BROWSER_POCKETSPHINX_SOURCE_DIR)
        set(pocketsphinx_SOURCE_DIR
            "${PSP_BROWSER_POCKETSPHINX_SOURCE_DIR}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                "-DPATCH_SOURCE_DIR=${pocketsphinx_SOURCE_DIR}"
                "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
                "-DTILEFINCH_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                -P "${_patch_driver}"
            RESULT_VARIABLE _patch_result)
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR
                "Could not prepare the local PocketSphinx source")
        endif()
        add_subdirectory(
            "${pocketsphinx_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/pocketsphinx-build"
            EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(
            pocketsphinx
            URL
                "https://files.pythonhosted.org/packages/source/p/pocketsphinx/pocketsphinx-5.1.1.tar.gz"
            URL_HASH
                "SHA256=675778b309a22dfc9b7d37f7621976bba491d2a5f8c59696bd77fd6d07271355"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            PATCH_COMMAND "${CMAKE_COMMAND}"
                "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
                "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
                "-DTILEFINCH_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                -P "${_patch_driver}")
        FetchContent_MakeAvailable(pocketsphinx)
    endif()

    if(NOT TARGET pocketsphinx)
        message(FATAL_ERROR "PocketSphinx did not define its library target")
    endif()
    # PocketSphinx 5.1.1 assumes it is the top-level project when spelling
    # generated include paths. Correct those paths at this one adapter seam.
    target_include_directories(pocketsphinx PUBLIC
        "${pocketsphinx_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/_deps/pocketsphinx-build/include"
        PRIVATE
        "${pocketsphinx_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/_deps/pocketsphinx-build")
    if(PSP_BROWSER_PACKED_VOICE_LEXICON)
        target_compile_definitions(
            pocketsphinx PRIVATE TILEFINCH_PACKED_LEXICON=1)
    endif()
    if(PSP_BROWSER_COMPACT_FIXED_VOICE)
        target_compile_definitions(pocketsphinx PRIVATE
            TILEFINCH_COMPACT_LM_QUANT=1
            TILEFINCH_FIXED_DICT=1
            TILEFINCH_FIXED_RECOGNIZER=1
            TILEFINCH_STREAM_SENDUMP=1
            TILEFINCH_THREE_STATE_HMM=1
            TILEFINCH_COMPACT_FIXED_SEARCH=1)
    endif()
    add_library(tilefinch_pocketsphinx INTERFACE)
    target_link_libraries(tilefinch_pocketsphinx INTERFACE pocketsphinx)

    foreach(_name IN LISTS _voice_cache_names)
        tilefinch_restore_cache_variable(
            "${_name}" "${_voice_had_${_name}}"
            "${_voice_value_${_name}}" "${_voice_type_${_name}}"
            "${_voice_help_${_name}}")
    endforeach()
endfunction()
