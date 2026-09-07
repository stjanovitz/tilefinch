#include "stt_component_loader.h"
#include <pspkernel.h>
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned loads, starts, stops, unloads;
static int fail_load, fail_query, fail_start, bad_status, bad_abi,
           missing_function, fail_stop, fail_unload, oversized;
static const char *expected_path = "ms0:/PSP/GAME/TILEFINCH/slot-a/tilefinch-voice.prx";
void *memalign(size_t alignment, size_t size)
{
    void *p = NULL;
    return posix_memalign(&p, alignment, size) == 0 ? p : NULL;
}
static void config_init(SttEngineConfig *c) { memset(c, 0, sizeof(*c)); }
static void config_preset(SttEngineConfig *c, SttPreset p) { c->preset = p; }
static SttStatus create(SttEngine **e, const SttEngineConfig *c)
{ (void)e; (void)c; return STT_STATUS_DECODER_INIT_FAILED; }
static void destroy(SttEngine *e) { (void)e; }
static uint64_t elapsed(const SttEngine *e) { (void)e; return 0; }
static SttPreset preset(const SttEngine *e) { (void)e; return STT_PRESET_QUALITY; }
static SttStatus decode(SttEngine *e, int16_t *s, size_t n,
                        SttCancelCheck c, void *u, SttResult *r)
{ (void)e;(void)s;(void)n;(void)c;(void)u;(void)r;return STT_STATUS_CANCELLED; }
static const char *status_name(SttStatus s) { (void)s; return "status"; }
static const char *input_name(SttInputStatus s) { (void)s; return "input"; }
static const char *preset_name(SttPreset s) { (void)s; return "preset"; }
int sceKernelLoadModule(const char *path, int flags, void *options)
{
    (void)flags; (void)options;
    assert(strcmp(path, expected_path) == 0);
    ++loads; return fail_load ? -1 : 7;
}
int sceKernelQueryModuleInfo(int id, SceKernelModuleInfo *info)
{
    assert(id == 7); assert(info->size == sizeof(*info));
    info->nsegment = 2;
    info->segmentsize[0] = 400000;
    info->segmentsize[1] = oversized ? TILEFINCH_STT_RESIDENT_LIMIT : 100000;
    return fail_query ? -1 : 0;
}
int sceKernelStartModule(int id, SceSize size, void *data, int *status, void *opt)
{
    (void)opt; assert(id == 7); assert(size == sizeof(TilefinchSttStart));
    ++starts;
    TilefinchSttStart *start = data;
    assert(strcmp(start->directory, "ms0:/PSP/GAME/TILEFINCH/slot-a") == 0);
    assert(start->magic == TILEFINCH_STT_MAGIC && start->abi == TILEFINCH_STT_ABI);
    assert(start->allocate && start->allocate_zero && start->resize
           && start->release && start->align && start->terminate);
    *status = bad_status ? -1 : 0;
    if (fail_start) return -1;
    *start->api = (TilefinchSttApi){
        TILEFINCH_STT_MAGIC, bad_abi ? 99 : TILEFINCH_STT_ABI, sizeof(TilefinchSttApi),
        config_init, config_preset, create, destroy, elapsed, preset, decode,
        status_name, input_name, missing_function ? NULL : preset_name
    };
    return id;
}
int sceKernelStopModule(int id, SceSize size, void *data, int *status, void *opt)
{
    (void)size;(void)data;(void)opt;assert(id == 7);
    ++stops; *status = 0; return fail_stop ? -1 : 0;
}
int sceKernelUnloadModule(int id)
{ assert(id == 7); ++unloads; return fail_unload ? -1 : 0; }

int main(void)
{
    Budget budget;
    budget_init(&budget, 2 * TILEFINCH_STT_RESIDENT_LIMIT);
    psp_voice_component_configure("ms0:/PSP/GAME/TILEFINCH/slot-a");
    assert(loads == 0 && budget.current == 0); /* boot is metadata-only */
    for (unsigned i = 0; i < 8; ++i) {
        assert(psp_voice_component_load(&budget));
        unsigned once = loads;
        assert(psp_voice_component_load(&budget) && loads == once);
        assert(tilefinch_stt_api_valid(&tilefinch_stt_api));
        assert(budget.current >= TILEFINCH_STT_RESIDENT_LIMIT);
        psp_voice_component_unload();
        assert(!tilefinch_stt_api_valid(&tilefinch_stt_api));
        assert(budget.current == 0 && budget.external_reserved == 0);
    }
    int *faults[] = {&fail_load,&fail_query,&oversized,&fail_start,
                    &bad_status,&bad_abi,&missing_function};
    for (unsigned i = 0; i < sizeof(faults)/sizeof(faults[0]); ++i) {
        *faults[i] = 1;
        assert(!psp_voice_component_load(&budget));
        assert(budget.current == 0);
        *faults[i] = 0;
    }
    budget.limit = TILEFINCH_STT_RESIDENT_LIMIT - 1;
    unsigned once = loads;
    assert(!psp_voice_component_load(&budget) && loads == once);
    budget.limit = 2 * TILEFINCH_STT_RESIDENT_LIMIT;
    assert(psp_voice_component_load(&budget));
    fail_stop = 1;
    psp_voice_component_unload();
    assert(budget.current != 0 && tilefinch_stt_api_valid(&tilefinch_stt_api));
    fail_stop = 0; fail_unload = 1;
    psp_voice_component_unload();
    assert(budget.current != 0 && !tilefinch_stt_api_valid(&tilefinch_stt_api));
    once = loads;
    assert(!psp_voice_component_load(&budget) && loads == once);
    fail_unload = 0;
    psp_voice_component_unload();
    assert(budget.current == 0);
    psp_voice_component_configure(NULL);
    assert(!psp_voice_component_load(&budget) && budget.current == 0);
    psp_voice_component_configure("relative/path");
    assert(!psp_voice_component_load(&budget) && budget.current == 0);
    assert(starts && stops && unloads);
    /* A terminal decoder quarantine cannot enter module_stop's libc
       cleanup, including via disable/retry. The charge remains owned. */
    psp_voice_component_configure("ms0:/PSP/GAME/TILEFINCH/slot-a");
    assert(psp_voice_component_load(&budget));
    unsigned saved_stops = stops, saved_unloads = unloads;
    size_t saved_charge = budget.current;
    psp_voice_component_quarantine();
    assert(psp_voice_component_is_quarantined());
    psp_voice_component_unload();
    assert(!psp_voice_component_load(&budget));
    psp_voice_component_unload();
    assert(stops == saved_stops && unloads == saved_unloads);
    assert(budget.current == saved_charge && saved_charge != 0);
    assert(!tilefinch_stt_api_valid(&tilefinch_stt_api));
    return 0;
}
