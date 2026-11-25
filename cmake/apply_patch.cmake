# Applies Tilefinch's patches to one dependency source tree and keeps them
# current when a patch file changes; docs/DEVELOPMENT.md#dependency-patches
# describes the behaviour.
#
#   cmake -DPATCH_SOURCE_DIR=<tree> -DPATCH_EXECUTABLE=<patch>
#         "-DPATCH_FILE=<patch>[;<patch>...]" -P apply_patch.cmake
#
# PATCH_FILE is one patch or an ordered list for the same tree. Each call
# leaves the tree holding every listed patch in its current form, or fails
# and leaves the tree as it found it.
#
# <tree>/.tilefinch-applied/ holds an exact copy of each patch applied and
# `stack`, their file names in the order they went in. A patch whose copy
# matches is left alone, and a call where every patch matches runs no patch
# at all. A patch whose copy differs is replaced by reversing the recorded
# patches from the top of the stack down to it, newest first, so that
# patches on the same lines come off in the opposite order they went on. The
# patches reversed above it are applied again by the calls (or list entries)
# that name them. A patch renamed without other change takes over its old
# record.
#
# A patch the tree has no record of, as in a tree patched before this record
# existed, is adopted when it reverses cleanly. One that neither applies nor
# reverses may be held off by a later patch in the same list (one that
# rewrote its lines, or one it was inserted under); the later ones are taken
# off and it is tried again, and they go back on after it.
#
# Every hunk must match its context exactly (-F0); offsets are accepted.
# Before changing anything, a call saves the files its patches name, and
# the record, in <tree>/.tilefinch-transaction/. A failure puts them back;
# a run that was killed is put back by the next one.
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED PATCH_SOURCE_DIR OR NOT DEFINED PATCH_FILE OR
   NOT DEFINED PATCH_EXECUTABLE)
    message(FATAL_ERROR
        "PATCH_SOURCE_DIR, PATCH_FILE, and PATCH_EXECUTABLE are required")
endif()

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
set(_state "${PATCH_SOURCE_DIR}/.tilefinch-applied")
set(_stack_file "${_state}/stack")
set(_txn "${PATCH_SOURCE_DIR}/.tilefinch-transaction")

function(_tf_patch out_result out_log)
    execute_process(
        COMMAND "${PATCH_EXECUTABLE}" --forward --silent -F0 -p1 ${ARGN}
        WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    set(${out_result} "${result}" PARENT_SCOPE)
    set(${out_log} "${output}" PARENT_SCOPE)
endfunction()

# Dry-runs, then applies, a patch in either direction ("" or -R). A reject
# file the call did not start with counts as a failure.
function(_tf_patch_checked out_result out_log direction file)
    _tf_patch(result log --dry-run ${direction} -i "${file}")
    if(result EQUAL 0)
        _tf_patch(result log ${direction} -i "${file}")
        foreach(path IN LISTS _absent)
            if(path MATCHES "\\.rej$" AND EXISTS "${PATCH_SOURCE_DIR}/${path}")
                set(result "left ${path}")
            endif()
        endforeach()
    endif()
    set(${out_result} "${result}" PARENT_SCOPE)
    set(${out_log} "${log}" PARENT_SCOPE)
endfunction()

# The tree paths a patch names (-p1), plus their .orig and .rej siblings.
function(_tf_patch_paths out file)
    file(STRINGS "${file}" lines REGEX "^(---|\\+\\+\\+) ")
    set(paths "")
    foreach(line IN LISTS lines)
        # REGEX REPLACE re-anchors ^ after each match, so take the
        # first component off with MATCH.
        string(SUBSTRING "${line}" 4 -1 path)
        string(REGEX REPLACE "\t.*$" "" path "${path}")
        if(path MATCHES "^[^/]*/(.*)$")
            set(path "${CMAKE_MATCH_1}")
        endif()
        if(path STREQUAL "" OR path MATCHES "^/|(^|/)\\.\\.(/|$)|dev/null$")
            continue()
        endif()
        list(APPEND paths "${path}" "${path}.orig" "${path}.rej")
    endforeach()
    set(${out} "${paths}" PARENT_SCOPE)
endfunction()

# Puts back the files and the record saved in the transaction. Sets
# `out` to an empty string, or to what could not be restored.
function(_tf_restore out)
    set(problem "")
    file(STRINGS "${_txn}/absent" absent)
    foreach(path IN LISTS absent)
        if(path STREQUAL "")
            continue()
        endif()
        file(REMOVE "${PATCH_SOURCE_DIR}/${path}")
        if(EXISTS "${PATCH_SOURCE_DIR}/${path}")
            string(APPEND problem " ${path}")
        endif()
    endforeach()
    file(GLOB_RECURSE saved RELATIVE "${_txn}/files" "${_txn}/files/*")
    foreach(path IN LISTS saved)
        file(COPY_FILE "${_txn}/files/${path}" "${PATCH_SOURCE_DIR}/${path}"
            RESULT copied)
        if(NOT copied EQUAL 0)
            string(APPEND problem " ${path}")
        endif()
    endforeach()
    file(REMOVE_RECURSE "${_state}")
    if(EXISTS "${_txn}/state")
        file(COPY "${_txn}/state/" DESTINATION "${_state}")
        if(NOT EXISTS "${_stack_file}")
            string(APPEND problem " ${_stack_file}")
        endif()
    endif()
    set(${out} "${problem}" PARENT_SCOPE)
endfunction()

function(_tf_end_transaction)
    file(REMOVE "${_txn}/complete")
    file(REMOVE_RECURSE "${_txn}")
endfunction()

# Restores the tree, then stops the configure with _tf_message.
macro(_tf_abort)
    _tf_restore(_unrestored)
    if(_unrestored)
        message(FATAL_ERROR "${_tf_message}\n"
            "Could not then restore${_unrestored} in ${PATCH_SOURCE_DIR}; "
            "the files from before this configure are in ${_txn}/files.")
    endif()
    _tf_end_transaction()
    message(FATAL_ERROR "${_tf_message}")
endmacro()

macro(_tf_write_stack)
    list(JOIN _stack "\n" _stack_text)
    if(_stack_text)
        string(APPEND _stack_text "\n")
    endif()
    file(WRITE "${_stack_file}" "${_stack_text}")
endmacro()

macro(_tf_record patch name)
    file(MAKE_DIRECTORY "${_state}")
    file(COPY_FILE "${patch}" "${_state}/${name}")
    list(APPEND _stack "${name}")
    _tf_write_stack()
endmacro()

# Reverses the recorded patches above and at stack index `to`, newest
# first, dropping each from the record as it comes off.
macro(_tf_unwind to)
    list(LENGTH _stack _top)
    while(_top GREATER ${to})
        math(EXPR _top "${_top} - 1")
        list(GET _stack ${_top} _recorded)
        _tf_patch_checked(_result _log -R "${_state}/${_recorded}")
        if(NOT _result EQUAL 0)
            string(CONCAT _tf_message
                "${PATCH_SOURCE_DIR} no longer holds the applied copy of "
                "${_recorded} (${_state}/${_recorded}), so it cannot be "
                "replaced by the current patches; the tree was probably "
                "edited by hand. It is left as it was. Undo those edits "
                "until\n"
                "  patch -R -p1 -d \"${PATCH_SOURCE_DIR}\" -i "
                "\"${_state}/${_recorded}\"\n"
                "applies, or replace the tree with a fresh copy of the "
                "dependency, then reconfigure.\n${_log}")
            _tf_abort()
        endif()
        list(REMOVE_AT _stack ${_top})
        _tf_write_stack()
        file(REMOVE "${_state}/${_recorded}")
    endwhile()
endmacro()

# A run killed after saving its transaction is put back first.
if(EXISTS "${_txn}/complete")
    message(STATUS "Restoring ${PATCH_SOURCE_DIR} from an interrupted patch run")
    _tf_restore(_unrestored)
    if(_unrestored)
        message(FATAL_ERROR
            "Could not restore${_unrestored} in ${PATCH_SOURCE_DIR} after an "
            "interrupted patch run; the files from before it are in "
            "${_txn}/files.")
    endif()
    _tf_end_transaction()
elseif(EXISTS "${_txn}")
    file(REMOVE_RECURSE "${_txn}")
endif()

set(_stack "")
if(EXISTS "${_stack_file}")
    file(STRINGS "${_stack_file}" _stack)
endif()
foreach(_recorded IN LISTS _stack)
    if(NOT EXISTS "${_state}/${_recorded}")
        message(FATAL_ERROR
            "${_stack_file} lists ${_recorded}, but its copy "
            "${_state}/${_recorded} is missing, so the patch cannot be "
            "replaced or taken off. Put the copy back (the version of the "
            "patch the tree holds), or remove ${_state} so that the tree "
            "adopts the patches it holds, then reconfigure.")
    endif()
endforeach()

# Nothing to do when every patch matches its copy.
set(_names "")
set(_current "")
foreach(_patch IN LISTS PATCH_FILE)
    get_filename_component(_name "${_patch}" NAME)
    list(APPEND _names "${_name}")
    if(_current STREQUAL "" AND NOT _name IN_LIST _stack)
        set(_current FALSE)
    elseif(_current STREQUAL "")
        file(SHA256 "${_patch}" _current_hash)
        file(SHA256 "${_state}/${_name}" _recorded_hash)
        if(NOT _current_hash STREQUAL _recorded_hash)
            set(_current FALSE)
        endif()
    endif()
endforeach()
if(_current STREQUAL "")
    return()
endif()

# Save every file the current and recorded patches name, and the record.
file(REMOVE_RECURSE "${_txn}")
set(_tracked "")
foreach(_patch IN LISTS PATCH_FILE)
    _tf_patch_paths(_paths "${_patch}")
    list(APPEND _tracked ${_paths})
endforeach()
foreach(_recorded IN LISTS _stack)
    _tf_patch_paths(_paths "${_state}/${_recorded}")
    list(APPEND _tracked ${_paths})
endforeach()
list(REMOVE_DUPLICATES _tracked)
set(_absent "")
foreach(_path IN LISTS _tracked)
    set(_file "${PATCH_SOURCE_DIR}/${_path}")
    if(IS_DIRECTORY "${_file}")
        continue()
    elseif(EXISTS "${_file}")
        get_filename_component(_dir "${_txn}/files/${_path}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dir}")
        file(COPY_FILE "${_file}" "${_txn}/files/${_path}")
    else()
        list(APPEND _absent "${_path}")
    endif()
endforeach()
list(JOIN _absent "\n" _absent_text)
file(WRITE "${_txn}/absent" "${_absent_text}\n")
if(EXISTS "${_state}")
    file(COPY "${_state}/" DESTINATION "${_txn}/state")
endif()
file(WRITE "${_txn}/complete" "")

list(LENGTH PATCH_FILE _count)
set(_index 0)
while(_index LESS _count)
    list(GET PATCH_FILE ${_index} _patch)
    math(EXPR _index "${_index} + 1")
    get_filename_component(_name "${_patch}" NAME)
    file(SHA256 "${_patch}" _current_hash)

    list(FIND _stack "${_name}" _at)
    set(_replacing FALSE)
    if(_at GREATER -1)
        file(SHA256 "${_state}/${_name}" _recorded_hash)
        if(_current_hash STREQUAL _recorded_hash)
            continue()
        endif()
        _tf_unwind(${_at})
        set(_replacing TRUE)
    else()
        # A renamed patch takes over the record of its old name.
        set(_renamed "")
        foreach(_recorded IN LISTS _stack)
            if(NOT _recorded IN_LIST _names)
                file(SHA256 "${_state}/${_recorded}" _recorded_hash)
                if(_recorded_hash STREQUAL _current_hash)
                    set(_renamed "${_recorded}")
                    break()
                endif()
            endif()
        endforeach()
        if(_renamed)
            file(RENAME "${_state}/${_renamed}" "${_state}/${_name}")
            list(FIND _stack "${_renamed}" _at)
            list(REMOVE_AT _stack ${_at})
            list(INSERT _stack ${_at} "${_name}")
            _tf_write_stack()
            continue()
        endif()
    endif()

    _tf_patch_checked(_result _log "" "${_patch}")
    if(_result EQUAL 0)
        _tf_record("${_patch}" "${_name}")
        continue()
    endif()
    if(_replacing)
        string(CONCAT _tf_message
            "${_patch} does not apply to ${PATCH_SOURCE_DIR} once its "
            "earlier version is reversed; the tree is left as it was.\n${_log}")
        _tf_abort()
    endif()

    _tf_patch(_result _log --dry-run -R -i "${_patch}")
    if(NOT _result EQUAL 0 AND _index LESS _count)
        # Perhaps held off by a later patch in this list: take those off,
        # newest first, and try again. No longer recorded, they go back on
        # as the loop reaches them.
        set(_later ${_count})
        while(_later GREATER _index)
            math(EXPR _later "${_later} - 1")
            list(GET PATCH_FILE ${_later} _later_patch)
            get_filename_component(_later_name "${_later_patch}" NAME)
            list(FIND _stack "${_later_name}" _later_at)
            if(_later_at GREATER -1)
                _tf_unwind(${_later_at})
            else()
                _tf_patch_checked(_result _log -R "${_later_patch}")
            endif()
        endwhile()
        _tf_patch_checked(_result _log "" "${_patch}")
        if(_result EQUAL 0)
            _tf_record("${_patch}" "${_name}")
            continue()
        endif()
        _tf_patch(_result _log --dry-run -R -i "${_patch}")
    endif()
    if(NOT _result EQUAL 0)
        file(RELATIVE_PATH _relative "${_repo}" "${_patch}")
        string(CONCAT _tf_message
            "${_patch} neither applies to ${PATCH_SOURCE_DIR} nor is applied "
            "there in its current form; the tree is left as it was. If the "
            "tree holds an older version of it, applied before Tilefinch "
            "recorded applied patches in ${_state}, reverse that version "
            "(after any later patch that changes the same lines) and "
            "reconfigure:\n"
            "  git -C \"${_repo}\" log -2 --format=%h -- ${_relative}\n"
            "  git -C \"${_repo}\" show <older-commit>:${_relative} | "
            "patch -R -p1 -d \"${PATCH_SOURCE_DIR}\"\n"
            "where <older-commit> is the commit whose version the tree holds "
            "(usually the second line).")
        _tf_abort()
    endif()
    message(STATUS "Adopted ${_name}, already in ${PATCH_SOURCE_DIR} "
        "(a tree without a record cannot show hunks dropped from the patch "
        "since it was applied)")
    _tf_record("${_patch}" "${_name}")
endwhile()
_tf_end_transaction()
