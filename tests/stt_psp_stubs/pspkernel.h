#ifndef TILEFINCH_TEST_STT_PSPKERNEL_H
#define TILEFINCH_TEST_STT_PSPKERNEL_H
#include <stddef.h>
typedef int SceUID;
typedef unsigned SceSize;
typedef struct {
    size_t size;
    unsigned nsegment;
    unsigned segmentsize[4];
} SceKernelModuleInfo;
int sceKernelLoadModule(const char *, int, void *);
int sceKernelQueryModuleInfo(int, SceKernelModuleInfo *);
int sceKernelStartModule(int, SceSize, void *, int *, void *);
int sceKernelStopModule(int, SceSize, void *, int *, void *);
int sceKernelUnloadModule(int);
#endif
