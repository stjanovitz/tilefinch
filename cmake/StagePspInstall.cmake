if(NOT LAUNCHER_EBOOT OR NOT BROWSER_EBOOT OR NOT BROWSER_MODULE OR NOT WASM_COMPONENT_PRX
        OR NOT XMB_REDIRECT_PRX
        OR NOT ASSET_DIR OR NOT OUTPUT OR NOT SOURCE_DIR)
    message(FATAL_ERROR
        "StagePspInstall requires LAUNCHER_EBOOT, BROWSER_EBOOT, BROWSER_MODULE, "
        "WASM_COMPONENT_PRX, "
        "XMB_REDIRECT_PRX, ASSET_DIR, SOURCE_DIR, and OUTPUT")
endif()
foreach(required IN ITEMS
        "${LAUNCHER_EBOOT}" "${BROWSER_EBOOT}" "${BROWSER_MODULE}" "${WASM_COMPONENT_PRX}"
        "${XMB_REDIRECT_PRX}"
        "${ASSET_DIR}/roots.pem" "${ASSET_DIR}/boot-defaults.cfg"
        "${ASSET_DIR}/fonts"
        "${SOURCE_DIR}/psp-assets/themes"
        "${SOURCE_DIR}/LICENSE"
        "${SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "${SOURCE_DIR}/third_party/notices"
        "${SOURCE_DIR}/third_party/public_suffix"
        "${SOURCE_DIR}/third_party/POCKETSPHINX_LICENSE.txt"
        "${SOURCE_DIR}/psp-assets/voice-model/en-us/README")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing PSP install asset: ${required}")
    endif()
endforeach()
# Keep a normal PBP: its MEMSIZE metadata is essential when old launchers
# or recovery use LoadExec. A raw PRX with this filename boots in 32 MB mode
# on real firmware, despite appearing functional at the native home screen.
file(READ "${BROWSER_EBOOT}" _browser_header LIMIT 40 HEX)
string(LENGTH "${_browser_header}" _browser_header_length)
if(NOT _browser_header_length EQUAL 80)
    message(FATAL_ERROR "Browser entry is not a complete PSP package")
endif()
string(SUBSTRING "${_browser_header}" 0 16 _browser_identity)
if(NOT _browser_identity STREQUAL "0050425000000100")
    message(FATAL_ERROR "Browser entry must retain its PSP package metadata")
endif()
set(_browser_offset_hex "")
foreach(_byte IN ITEMS 70 68 66 64)
    string(SUBSTRING "${_browser_header}" ${_byte} 2 _part)
    string(APPEND _browser_offset_hex "${_part}")
endforeach()
math(EXPR _browser_offset "0x${_browser_offset_hex}")
file(SIZE "${BROWSER_EBOOT}" _browser_size)
math(EXPR _browser_last_header "${_browser_size} - 20")
if(_browser_offset LESS 40 OR _browser_offset GREATER _browser_last_header
        OR _browser_size GREATER 16777216)
    message(FATAL_ERROR "Browser package payload is out of bounds")
endif()
file(READ "${BROWSER_EBOOT}" _module_header OFFSET ${_browser_offset} LIMIT 20 HEX)
if(NOT _module_header MATCHES "^7f454c46010101[0-9a-f]+a0ff0800$")
    message(FATAL_ERROR "Browser package must contain the relocatable browser, not the static diagnostic executable")
endif()
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY
    "${OUTPUT}/slot-a" "${OUTPUT}/slot-b" "${OUTPUT}/data"
    "${OUTPUT}/data/themes" "${OUTPUT}/OPTIONAL")
file(COPY_FILE "${LAUNCHER_EBOOT}" "${OUTPUT}/EBOOT.PBP")
file(COPY_FILE "${BROWSER_EBOOT}" "${OUTPUT}/slot-a/EBOOT.PBP")
file(SHA256 "${BROWSER_MODULE}" _startup_digest)
file(SIZE "${BROWSER_MODULE}" _startup_size)
math(EXPR _startup_marker_offset "${_browser_offset} + ${_startup_size}")
math(EXPR _expected_package_size "${_startup_marker_offset} + 40")
file(READ "${BROWSER_EBOOT}" _startup_marker
    OFFSET ${_startup_marker_offset} LIMIT 40 HEX)
if(NOT _browser_size EQUAL _expected_package_size
        OR NOT _startup_marker STREQUAL "5446424f4f543031${_startup_digest}")
    message(FATAL_ERROR "First-install module does not match the browser bundle")
endif()
# This executable is first-install-only, not a new signed update-package path.
file(COPY_FILE "${BROWSER_MODULE}"
    "${OUTPUT}/slot-a/startup-${_startup_digest}.prx")
file(COPY_FILE "${WASM_COMPONENT_PRX}"
    "${OUTPUT}/slot-a/tilefinch-wasm.prx")
# Voice-disabled validation/replay builds have no speech component. Ordinary
# builds always stage the PRX with the signed browser slot, not with models.
if(VOICE_COMPONENT_PRX)
    file(COPY_FILE "${VOICE_COMPONENT_PRX}"
        "${OUTPUT}/slot-a/tilefinch-voice.prx")
endif()
file(COPY_FILE "${XMB_REDIRECT_PRX}"
    "${OUTPUT}/OPTIONAL/tilefinch_xmb.prx")
file(COPY
    "${ASSET_DIR}/roots.pem"
    "${ASSET_DIR}/boot-defaults.cfg"
    "${ASSET_DIR}/fonts"
    DESTINATION "${OUTPUT}/slot-a")
file(COPY "${SOURCE_DIR}/psp-assets/themes/"
    DESTINATION "${OUTPUT}/data/themes")
# Voice recognition is an explicit, separately signed in-app component.
# The one default Tilefinch distribution never duplicates its roughly 9 MiB
# model inside browser A/B slots. Developer EBOOT directories may still stage
# a model beside the executable for hardware validation and legacy migration.

# The OFL (and DejaVu's Vera terms) require the license texts to travel
# with the font files, not only with the notices index.
file(COPY
    "${SOURCE_DIR}/fonts/LICENSE-DejaVu.txt"
    "${SOURCE_DIR}/fonts/LICENSE-TilefinchSans.txt"
    "${SOURCE_DIR}/fonts/LICENSE-Unifont.txt"
    DESTINATION "${OUTPUT}/slot-a/fonts")

# Stage the complete redistribution notice set required by the
# "Distributing binaries" checklist in THIRD_PARTY_NOTICES.md.
file(MAKE_DIRECTORY "${OUTPUT}/NOTICES")
file(COPY_FILE "${SOURCE_DIR}/LICENSE" "${OUTPUT}/NOTICES/LICENSE")
file(COPY_FILE "${SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${OUTPUT}/NOTICES/THIRD_PARTY_NOTICES.md")
file(GLOB _tilefinch_notice_entries "${SOURCE_DIR}/third_party/notices/*")
file(COPY ${_tilefinch_notice_entries} DESTINATION "${OUTPUT}/NOTICES")
file(COPY "${SOURCE_DIR}/third_party/public_suffix"
    DESTINATION "${OUTPUT}/NOTICES")
file(MAKE_DIRECTORY "${OUTPUT}/NOTICES/pocketsphinx")
file(COPY_FILE "${SOURCE_DIR}/third_party/POCKETSPHINX_LICENSE.txt"
    "${OUTPUT}/NOTICES/pocketsphinx/POCKETSPHINX_LICENSE.txt")
file(MAKE_DIRECTORY "${OUTPUT}/NOTICES/voice-model")
file(COPY_FILE "${SOURCE_DIR}/psp-assets/voice-model/en-us/README"
    "${OUTPUT}/NOTICES/voice-model/ALPHA_CEPHEI_LICENSE.txt")
file(MAKE_DIRECTORY "${OUTPUT}/NOTICES/fonts")
file(COPY
    "${SOURCE_DIR}/fonts/LICENSE-DejaVu.txt"
    "${SOURCE_DIR}/fonts/LICENSE-TilefinchSans.txt"
    "${SOURCE_DIR}/fonts/LICENSE-Unifont.txt"
    DESTINATION "${OUTPUT}/NOTICES/fonts")

# Verify the staged tree against the checklist manifest so the tree and
# THIRD_PARTY_NOTICES.md cannot drift apart.
include("${CMAKE_CURRENT_LIST_DIR}/PspNoticesManifest.cmake")
set(_tilefinch_missing_notices "")
foreach(entry IN LISTS TILEFINCH_NOTICES_MANIFEST)
    if(NOT EXISTS "${OUTPUT}/${entry}")
        list(APPEND _tilefinch_missing_notices "${entry}")
    endif()
endforeach()
if(_tilefinch_missing_notices)
    list(JOIN _tilefinch_missing_notices "\n  " _tilefinch_missing_report)
    message(FATAL_ERROR
        "Staged PSP install tree is missing required license/notice files "
        "(see cmake/PspNoticesManifest.cmake and THIRD_PARTY_NOTICES.md):\n"
        "  ${_tilefinch_missing_report}")
endif()
