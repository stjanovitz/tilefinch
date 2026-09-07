#ifndef TILEFINCH_STT_COMPONENT_H
#define TILEFINCH_STT_COMPONENT_H

#include "stt_engine.h"

#define TILEFINCH_STT_ABI 1u
#define TILEFINCH_STT_MAGIC UINT32_C(0x54465354)
/* Code/data/BSS and loader overhead; recognition/model memory remains in the
 * existing voice reservation. No independent newlib heap is created. */
#define TILEFINCH_STT_RESIDENT_LIMIT (768u * 1024u)

typedef struct {
    uint32_t magic, abi, size;
    void (*config_init)(SttEngineConfig *);
    void (*config_set_preset)(SttEngineConfig *, SttPreset);
    SttStatus (*create)(SttEngine **, const SttEngineConfig *);
    void (*destroy)(SttEngine *);
    uint64_t (*init_microseconds)(const SttEngine *);
    SttPreset (*preset)(const SttEngine *);
    SttStatus (*decode)(SttEngine *, int16_t *, size_t,
                        SttCancelCheck, void *, SttResult *);
    const char *(*status_name)(SttStatus);
    const char *(*input_status_name)(SttInputStatus);
    const char *(*preset_name)(SttPreset);
} TilefinchSttApi;

typedef struct {
    uint32_t magic, abi, size;
    TilefinchSttApi *api;
    const char *directory;
    void *(*allocate)(size_t);
    void *(*allocate_zero)(size_t, size_t);
    void *(*resize)(void *, size_t);
    void (*release)(void *);
    void *(*align)(size_t, size_t);
    void (*terminate)(int);
} TilefinchSttStart;

static inline int tilefinch_stt_api_valid(const TilefinchSttApi *api)
{
    return api && api->magic == TILEFINCH_STT_MAGIC
        && api->abi == TILEFINCH_STT_ABI && api->size == sizeof(*api)
        && api->config_init && api->config_set_preset && api->create
        && api->destroy && api->init_microseconds && api->preset && api->decode
        && api->status_name && api->input_status_name && api->preset_name;
}

#endif
