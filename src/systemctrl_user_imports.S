    .set noreorder

#include "pspimport.s"

    /* Project-authored import stubs for the two SystemCtrlForUser entry
       points Tilefinch calls. The library name and NIDs are interface facts
       of the installed custom firmware's ABI; declaring them here keeps the
       GPL-3.0 psp-cfw-sdk stub archive and headers out of MIT-distributed
       binaries (see THIRD_PARTY_NOTICES.md). The 0x4009 attribute word
       matches the SDK archive's weak-import marking: stock firmware and
       PPSSPP do not export SystemCtrlForUser, and modules must still load
       there with these calls failing softly rather than the loader
       rejecting the executable. */
    IMPORT_START "SystemCtrlForUser",0x40090000
    IMPORT_FUNC  "SystemCtrlForUser",0x159AF5CC,sctrlHENFindFunction
    IMPORT_FUNC  "SystemCtrlForUser",0xABA7F1B0,sctrlKernelLoadExecVSHMs2

    /* KUBridge ships in the same GPL-3.0 psp-cfw-sdk package. The media
       backend calls into it to reach the Media Engine wrapper; declaring
       these two imports here keeps that archive out of the build too. */
    IMPORT_START "KUBridge",0x40090000
    IMPORT_FUNC  "KUBridge",0x4C25EA72,kuKernelLoadModule
    IMPORT_FUNC  "KUBridge",0x9060F69D,kuKernelCall
