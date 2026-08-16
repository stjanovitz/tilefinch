if(NOT LAUNCHER_EBOOT OR NOT BROWSER_EBOOT OR NOT ASSET_DIR OR NOT OUTPUT
        OR NOT SOURCE_DIR)
    message(FATAL_ERROR
        "StagePspInstall requires LAUNCHER_EBOOT, BROWSER_EBOOT, ASSET_DIR, "
        "SOURCE_DIR, and OUTPUT")
endif()
foreach(required IN ITEMS
        "${LAUNCHER_EBOOT}" "${BROWSER_EBOOT}"
        "${ASSET_DIR}/roots.pem" "${ASSET_DIR}/boot-defaults.cfg"
        "${ASSET_DIR}/fonts"
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
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY
    "${OUTPUT}/slot-a" "${OUTPUT}/slot-b" "${OUTPUT}/data")
file(COPY_FILE "${LAUNCHER_EBOOT}" "${OUTPUT}/EBOOT.PBP")
file(COPY_FILE "${BROWSER_EBOOT}" "${OUTPUT}/slot-a/EBOOT.PBP")
file(COPY
    "${ASSET_DIR}/roots.pem"
    "${ASSET_DIR}/boot-defaults.cfg"
    "${ASSET_DIR}/fonts"
    DESTINATION "${OUTPUT}/slot-a")
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
