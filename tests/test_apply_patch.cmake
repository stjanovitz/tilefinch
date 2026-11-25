# cmake/apply_patch.cmake keeps a dependency tree's patches current: an edit
# to a patch the tree already holds replaces the applied version, patches on
# the same lines come off newest first, a re-run with nothing changed runs no
# patch at all, and a tree patched before applied patches were recorded
# either adopts its patches or fails with the command that repairs it.
# Synthetic trees and patches only.
#   cmake -DTILEFINCH_ROOT=<source> -DTILEFINCH_SCRATCH=<dir>
#         -DPATCH_EXECUTABLE=<patch> -P test_apply_patch.cmake
cmake_minimum_required(VERSION 3.21)
foreach(required TILEFINCH_ROOT TILEFINCH_SCRATCH PATCH_EXECUTABLE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "test_apply_patch needs ${required}")
    endif()
endforeach()
set(script "${TILEFINCH_ROOT}/cmake/apply_patch.cmake")
set(failures 0)

set(base "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n")
# A rewrites two lines; A2 is A edited. B rewrites a line A added, so A only
# reverses once B is off. C patches a different file.
set(patch_a [==[--- a/a.txt
+++ b/a.txt
@@ -1,10 +1,10 @@
 one
-two
+alpha
 three
 four
 five
 six
 seven
 eight
-nine
+nine A1
 ten
]==])
string(REPLACE "nine A1" "nine A2" patch_a2 "${patch_a}")
set(patch_b [==[--- a/a.txt
+++ b/a.txt
@@ -1,5 +1,5 @@
 one
-alpha
+alpha beta
 three
 four
 five
]==])
string(REPLACE "+alpha beta" "+alpha BETA" patch_b2 "${patch_b}")
set(patch_c [==[--- a/b.txt
+++ b/b.txt
@@ -1 +1 @@
-x
+y
]==])

function(expected_text out two nine)
    string(REPLACE "two\n" "${two}\n" text "${base}")
    string(REPLACE "nine\n" "${nine}\n" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# A fresh case directory: src/ with a.txt and b.txt, and patches/.
function(new_case label)
    set(dir "${TILEFINCH_SCRATCH}/${label}")
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}/src" "${dir}/patches")
    file(WRITE "${dir}/src/a.txt" "${base}")
    file(WRITE "${dir}/src/b.txt" "x\n")
    set(case_dir "${dir}" PARENT_SCOPE)
endfunction()

function(write_patch name text)
    file(WRITE "${case_dir}/patches/${name}" "${text}")
endfunction()

# Runs apply_patch.cmake on the case tree with the named patches in order.
# Sets run_status and run_output.
function(run_script)
    cmake_parse_arguments(arg "" "PATCH" "" ${ARGN})
    if(NOT arg_PATCH)
        set(arg_PATCH "${PATCH_EXECUTABLE}")
    endif()
    set(files "")
    foreach(name IN LISTS arg_UNPARSED_ARGUMENTS)
        list(APPEND files "${case_dir}/patches/${name}")
    endforeach()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPATCH_SOURCE_DIR=${case_dir}/src"
            "-DPATCH_FILE=${files}"
            "-DPATCH_EXECUTABLE=${arg_PATCH}"
            -P "${script}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    set(run_status "${status}" PARENT_SCOPE)
    set(run_output "${output}" PARENT_SCOPE)
endfunction()

# Applies a patch the way trees were patched before applied patches were
# recorded.
function(apply_by_hand name)
    execute_process(
        COMMAND "${PATCH_EXECUTABLE}" --forward --silent -p1
            -i "${case_dir}/patches/${name}"
        WORKING_DIRECTORY "${case_dir}/src"
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "setup: ${name} did not apply by hand")
    endif()
endfunction()

macro(fail label why)
    message("FAIL ${label}: ${why}\n${run_output}")
    math(EXPR failures "${failures} + 1")
endmacro()

macro(expect_success label)
    if(NOT run_status STREQUAL "0")
        fail(${label} "apply_patch.cmake failed (status ${run_status})")
    endif()
endmacro()

macro(expect_tree label file wanted)
    file(READ "${case_dir}/src/${file}" _actual)
    if(NOT _actual STREQUAL "${wanted}")
        fail(${label} "${file} is\n${_actual}wanted\n${wanted}")
    else()
        message("ok ${label}")
    endif()
endmacro()

expected_text(with_a1 "alpha" "nine A1")
expected_text(with_a2 "alpha" "nine A2")
expected_text(with_a1_b "alpha beta" "nine A1")
expected_text(with_a2_b "alpha beta" "nine A2")
expected_text(with_a2_b2 "alpha BETA" "nine A2")

# A fresh tree takes the patch and records the copy it applied.
new_case(fresh)
write_patch(a.patch "${patch_a}")
run_script(a.patch)
expect_success(fresh)
expect_tree(fresh a.txt "${with_a1}")
if(NOT EXISTS "${case_dir}/src/.tilefinch-applied/a.patch")
    fail(fresh "no recorded copy of a.patch")
endif()

# Nothing changed: the tree is left as it is and patch never runs.
run_script(a.patch PATCH "${case_dir}/no-such-patch-program")
expect_success(no-op-runs-no-patch)
expect_tree(no-op-runs-no-patch a.txt "${with_a1}")

# An edited patch replaces the version the tree holds.
write_patch(a.patch "${patch_a2}")
run_script(a.patch)
expect_success(edited)
expect_tree(edited a.txt "${with_a2}")
run_script(a.patch PATCH "${case_dir}/no-such-patch-program")
expect_success(edited-then-no-op)

# Two patches on one file, applied by separate calls: editing the first takes
# the second off and puts it back on; editing the second replaces only it.
new_case(stacked)
write_patch(a.patch "${patch_a}")
write_patch(b.patch "${patch_b}")
write_patch(c.patch "${patch_c}")
foreach(name a.patch b.patch c.patch)
    run_script(${name})
    expect_success(stacked-first-${name})
endforeach()
expect_tree(stacked-first a.txt "${with_a1_b}")
write_patch(a.patch "${patch_a2}")
foreach(name a.patch b.patch c.patch)
    run_script(${name})
    expect_success(stacked-edit-first-${name})
endforeach()
expect_tree(stacked-edit-first a.txt "${with_a2_b}")
expect_tree(stacked-edit-first-other-file b.txt "y\n")
write_patch(b.patch "${patch_b2}")
run_script(a.patch b.patch c.patch)
expect_success(stacked-edit-second)
expect_tree(stacked-edit-second a.txt "${with_a2_b2}")
expect_tree(stacked-edit-second-other-file b.txt "y\n")
run_script(a.patch b.patch c.patch PATCH "${case_dir}/no-such-patch-program")
expect_success(stacked-no-op)

# A tree patched by hand with a patch that has since changed fails, names
# the tree, gives the repair command, and is left as it was.
new_case(unrecorded-edited)
write_patch(a.patch "${patch_a}")
apply_by_hand(a.patch)
write_patch(a.patch "${patch_a2}")
run_script(a.patch)
if(run_status STREQUAL "0")
    fail(unrecorded-edited "succeeded on a tree holding an older version")
elseif(NOT run_output MATCHES "git -C [^\n]* show <older-commit>:"
       OR NOT run_output MATCHES "patch -R -p1 -d"
       OR NOT run_output MATCHES "unrecorded-edited/src")
    fail(unrecorded-edited "failure does not give the repair")
else()
    message("ok unrecorded-edited fails clearly")
endif()
expect_tree(unrecorded-edited-unchanged a.txt "${with_a1}")

# A tree patched by hand with the current patches adopts them, a patch
# rewritten by a later one in the same list included, and then heals.
new_case(unrecorded-current)
write_patch(a.patch "${patch_a}")
write_patch(b.patch "${patch_b}")
apply_by_hand(a.patch)
apply_by_hand(b.patch)
run_script(a.patch b.patch)
expect_success(unrecorded-current)
expect_tree(unrecorded-current a.txt "${with_a1_b}")
if(NOT run_output MATCHES "Adopted a\\.patch")
    fail(unrecorded-current-logged "adoption is not logged")
endif()
write_patch(a.patch "${patch_a2}")
run_script(a.patch b.patch)
expect_success(unrecorded-current-then-edited)
expect_tree(unrecorded-current-then-edited a.txt "${with_a2_b}")

# A tree whose adoption stopped partway (its first patch recorded, the rest
# applied by hand) finishes it, the rewritten patch included.
new_case(partly-recorded)
write_patch(c.patch "${patch_c}")
write_patch(a.patch "${patch_a}")
write_patch(b.patch "${patch_b}")
run_script(c.patch)
expect_success(partly-recorded-setup)
apply_by_hand(a.patch)
apply_by_hand(b.patch)
run_script(c.patch a.patch b.patch)
expect_success(partly-recorded)
expect_tree(partly-recorded a.txt "${with_a1_b}")
run_script(c.patch a.patch b.patch PATCH "${case_dir}/no-such-patch-program")
expect_success(partly-recorded-no-op)

# A recorded tree edited by hand stops the replacement and is put back.
new_case(edited-by-hand)
write_patch(a.patch "${patch_a}")
write_patch(b.patch "${patch_b}")
run_script(a.patch b.patch)
expect_success(edited-by-hand-setup)
file(READ "${case_dir}/src/a.txt" text)
string(REPLACE "nine A1" "nine local" text "${text}")
file(WRITE "${case_dir}/src/a.txt" "${text}")
write_patch(a.patch "${patch_a2}")
run_script(a.patch b.patch)
expected_text(with_local "alpha beta" "nine local")
if(run_status STREQUAL "0")
    fail(edited-by-hand "replaced a patch in a tree edited by hand")
elseif(NOT run_output MATCHES "edited by hand"
       OR NOT run_output MATCHES "\\.tilefinch-applied/a\\.patch")
    fail(edited-by-hand "failure does not name the recorded copy")
else()
    message("ok edited-by-hand fails clearly")
endif()
expect_tree(edited-by-hand-restored a.txt "${with_local}")

# Hunks must match their context exactly: a patch that would need fuzz is
# refused and the tree is left as it was. An offset is accepted.
new_case(fuzz)
write_patch(fuzzy.patch [==[--- a/a.txt
+++ b/a.txt
@@ -1,7 +1,7 @@
 ONE
 two
 three
-four
+FOUR
 five
 six
 seven
]==])
run_script(fuzzy.patch)
if(run_status STREQUAL "0")
    fail(fuzz-refused "applied a hunk whose context does not match")
else()
    message("ok fuzz-refused")
endif()
expect_tree(fuzz-refused-unchanged a.txt "${base}")
if(EXISTS "${case_dir}/src/a.txt.orig" OR EXISTS "${case_dir}/src/a.txt.rej"
   OR EXISTS "${case_dir}/src/.tilefinch-applied")
    fail(fuzz-refused-clean "left a backup, reject or record behind")
endif()
write_patch(offset.patch [==[--- a/a.txt
+++ b/a.txt
@@ -3,7 +3,7 @@
 one
 two
 three
-four
+FOUR
 five
 six
 seven
]==])
run_script(offset.patch)
expect_success(offset-accepted)
string(REPLACE "four\n" "FOUR\n" with_four "${base}")
expect_tree(offset-accepted a.txt "${with_four}")

# A patch inserted between two recorded ones that only applies with the
# later one off (it rewrites A's line, and B is edited to follow it).
new_case(inserted)
write_patch(a.patch "${patch_a}")
write_patch(b.patch "${patch_b}")
run_script(a.patch b.patch)
expect_success(inserted-setup)
write_patch(n.patch [==[--- a/a.txt
+++ b/a.txt
@@ -1,3 +1,3 @@
 one
-alpha
+alpha2
 three
]==])
string(REPLACE "alpha" "alpha2" patch_b_n "${patch_b}")
write_patch(b.patch "${patch_b_n}")
run_script(a.patch n.patch b.patch)
expect_success(inserted)
expected_text(with_a1_n_b "alpha2 beta" "nine A1")
expect_tree(inserted a.txt "${with_a1_n_b}")
set(stack "")
if(EXISTS "${case_dir}/src/.tilefinch-applied/stack")
    file(READ "${case_dir}/src/.tilefinch-applied/stack" stack)
endif()
if(NOT stack STREQUAL "a.patch\nn.patch\nb.patch\n")
    fail(inserted-record "stack is\n${stack}")
endif()

# A list call that fails partway, after replacing an edited patch, leaves
# the tree and its record exactly as it found them.
new_case(list-restored)
write_patch(a.patch "${patch_a}")
write_patch(b.patch "${patch_b}")
write_patch(c.patch "${patch_c}")
run_script(a.patch b.patch c.patch)
expect_success(list-restored-setup)
file(READ "${case_dir}/src/.tilefinch-applied/stack" stack_before)
write_patch(a.patch "${patch_a2}")
string(REPLACE "-x" "-q" patch_c_broken "${patch_c}")
write_patch(c.patch "${patch_c_broken}")
run_script(a.patch b.patch c.patch)
if(run_status STREQUAL "0")
    fail(list-restored "a patch that does not apply succeeded")
endif()
expect_tree(list-restored a.txt "${with_a1_b}")
expect_tree(list-restored-other-file b.txt "y\n")
set(stack_after "")
set(a_copy "")
if(EXISTS "${case_dir}/src/.tilefinch-applied/a.patch")
    file(READ "${case_dir}/src/.tilefinch-applied/stack" stack_after)
    file(READ "${case_dir}/src/.tilefinch-applied/a.patch" a_copy)
endif()
if(NOT stack_after STREQUAL stack_before OR NOT a_copy STREQUAL patch_a
   OR NOT EXISTS "${case_dir}/src/.tilefinch-applied/c.patch"
   OR EXISTS "${case_dir}/src/.tilefinch-transaction")
    fail(list-restored-record "record is not as it was:\n${stack_after}")
endif()

# The same for files below the tree's top directory.
new_case(nested-restored)
file(WRITE "${case_dir}/src/sub/dir/d.txt" "x\n")
set(patch_d [==[--- a/sub/dir/d.txt
+++ b/sub/dir/d.txt
@@ -1 +1 @@
-x
+y
]==])
write_patch(d.patch "${patch_d}")
write_patch(e.patch [==[--- a/sub/dir/d.txt
+++ b/sub/dir/d.txt
@@ -1 +1 @@
-y
+w
]==])
run_script(d.patch e.patch)
expect_success(nested-restored-setup)
string(REPLACE "-x" "-q" patch_d_broken "${patch_d}")
write_patch(d.patch "${patch_d_broken}")
run_script(d.patch e.patch)
if(run_status STREQUAL "0")
    fail(nested-restored "a patch that does not apply succeeded")
endif()
expect_tree(nested-restored sub/dir/d.txt "w\n")

# A run killed partway through replacing a patch is put right by the next.
new_case(interrupted)
write_patch(a.patch "${patch_a}")
write_patch(b.patch "${patch_b}")
run_script(a.patch b.patch)
expect_success(interrupted-setup)
write_patch(a.patch "${patch_a2}")
set(killer "${case_dir}/patch-then-die.sh")
file(WRITE "${killer}" "#!/bin/sh
n=0
[ -f \"${case_dir}/calls\" ] && n=$(cat \"${case_dir}/calls\")
n=$((n + 1))
echo $n > \"${case_dir}/calls\"
if [ $n -eq 3 ]; then kill -9 $PPID; exit 1; fi
exec \"${PATCH_EXECUTABLE}\" \"$@\"
")
file(CHMOD "${killer}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
run_script(a.patch b.patch PATCH "${killer}")
if(run_status STREQUAL "0")
    fail(interrupted-kill "the run was not killed")
endif()
run_script(a.patch b.patch)
expect_success(interrupted-recovered)
expect_tree(interrupted-recovered a.txt "${with_a2_b}")

# A renamed patch takes over its old record, so a later replacement below
# it still unstacks.
new_case(renamed)
write_patch(c.patch "${patch_c}")
write_patch(a.patch "${patch_a}")
run_script(c.patch a.patch)
expect_success(renamed-setup)
file(RENAME "${case_dir}/patches/a.patch" "${case_dir}/patches/x.patch")
run_script(c.patch x.patch)
expect_success(renamed)
string(REPLACE "+y" "+z" patch_c2 "${patch_c}")
write_patch(c.patch "${patch_c2}")
run_script(c.patch x.patch)
expect_success(renamed-then-below-edited)
expect_tree(renamed-then-below-edited a.txt "${with_a1}")
expect_tree(renamed-then-below-edited-other-file b.txt "z\n")

# A record whose copy has gone missing fails with a clear message.
new_case(missing-copy)
write_patch(a.patch "${patch_a}")
run_script(a.patch)
expect_success(missing-copy-setup)
file(REMOVE "${case_dir}/src/.tilefinch-applied/a.patch")
run_script(a.patch)
if(run_status STREQUAL "0" OR NOT run_output MATCHES "lists a\\.patch, but its copy")
    fail(missing-copy "no clear failure for a missing copy")
else()
    message("ok missing-copy fails clearly")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} apply_patch check(s) failed")
endif()
