    .set noreorder

#include "pspimport.s"

    /* Keep every sceMpeg function used by the bounded PSP backend in one
       import object. Mixing one custom NID with the SDK's per-function
       archive split the module's stub range and made psp-fixup-imports reject
       the ELF even when SDK archives were linked last. */
    IMPORT_START "sceMpeg",0x00090000
    IMPORT_FUNC  "sceMpeg",0x682A619B,sceMpegInit
    IMPORT_FUNC  "sceMpeg",0x874624D6,sceMpegFinish
    IMPORT_FUNC  "sceMpeg",0xC132E22F,sceMpegQueryMemSize
    IMPORT_FUNC  "sceMpeg",0xD8C5F121,sceMpegCreate
    IMPORT_FUNC  "sceMpeg",0x606A4649,sceMpegDelete
    IMPORT_FUNC  "sceMpeg",0x167AFD9E,sceMpegInitAu
    IMPORT_FUNC  "sceMpeg",0x0E3C2E9D,sceMpegAvcDecode
    IMPORT_FUNC  "sceMpeg",0x740FCCD1,sceMpegAvcDecodeStop
    IMPORT_FUNC  "sceMpeg",0x4571CC64,sceMpegAvcDecodeFlush
    IMPORT_FUNC  "sceMpeg",0x11F95CF1,sceMpegGetAvcNalAu
    IMPORT_FUNC  "sceMpeg",0xCF3547A2,sceMpegAvcDecodeDetail2

    /* The VSH raw-NAL decoder emits Media Engine YUV planes. Convert the
       selected plane set into the browser's protected RGBA surface. */
    IMPORT_START "sceMpegbase",0x00090000
    IMPORT_FUNC  "sceMpegbase",0x91929A21,sceMpegBaseCscAvc
