#include "stt_component.h"
#include <pspkernel.h>
#include <string.h>
#include <sys/reent.h>
#include <unistd.h>

PSP_MODULE_INFO("tilefinch_voice", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

/* All allocation, including newlib's reentrant stdio helpers, goes to the
 * browser's existing heap under its voice reservation. Linker wrapping also
 * covers calls made from the static PocketSphinx/newlib archives. */
static TilefinchSttStart host;
/* PSPDEV libcglue's non-heap CRT subset. The PRX omits startup code so it
 * cannot claim a second heap, but still needs its own descriptor/lock state. */
extern void __locks_init(void), __locks_deinit(void);
extern void __init_mutex(void), __deinit_mutex(void), __fdman_init(void);
extern struct _glue __sglue;
/* PocketSphinx retains its pre-existing fatal-library exit policy; use the
 * application's exit path, not a second PRX C-runtime/heap initializer. */
void _exit(int status)
{
    host.terminate(status);
    __builtin_trap();
}
struct _reent;
void *__wrap_malloc(size_t n) { return host.allocate ? host.allocate(n) : NULL; }
void *__wrap_calloc(size_t n, size_t s)
{ return host.allocate_zero ? host.allocate_zero(n, s) : NULL; }
void *__wrap_realloc(void *p, size_t n)
{ return host.resize ? host.resize(p, n) : NULL; }
void __wrap_free(void *p) { if (host.release) host.release(p); }
void *__wrap_memalign(size_t a, size_t n)
{ return host.align ? host.align(a, n) : NULL; }
void *__wrap__malloc_r(struct _reent *r, size_t n)
{ (void)r; return __wrap_malloc(n); }
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t s)
{ (void)r; return __wrap_calloc(n, s); }
void *__wrap__realloc_r(struct _reent *r, void *p, size_t n)
{ (void)r; return __wrap_realloc(p, n); }
void __wrap__free_r(struct _reent *r, void *p)
{ (void)r; __wrap_free(p); }
void *__wrap__memalign_r(struct _reent *r, size_t a, size_t n)
{ (void)r; return __wrap_memalign(a, n); }

int module_start(SceSize size, void *data)
{
    if (!data || size != sizeof(TilefinchSttStart)) return -1;
    const TilefinchSttStart *start = data;
    if (start->magic != TILEFINCH_STT_MAGIC || start->abi != TILEFINCH_STT_ABI
        || start->size != sizeof(*start) || !start->api || !start->allocate
        || !start->allocate_zero || !start->resize || !start->release
        || !start->align || !start->terminate || !start->directory) return -2;
    host = *start;
    __locks_init();
    __init_mutex();
    __fdman_init();
    /* libcglue's cwd is module-local; it is not initialized by this PRX's
     * intentionally absent CRT startup. Never inherit an empty cwd. */
    if (chdir(start->directory) != 0) {
        __deinit_mutex();
        __locks_deinit();
        memset(&host, 0, sizeof(host));
        return -3;
    }
    const TilefinchSttApi api = {
        .magic = TILEFINCH_STT_MAGIC, .abi = TILEFINCH_STT_ABI,
        .size = sizeof(api),
        .config_init = stt_engine_config_init,
        .config_set_preset = stt_engine_config_set_preset,
        .create = stt_engine_create, .destroy = stt_engine_destroy,
        .init_microseconds = stt_engine_init_microseconds,
        .preset = stt_engine_preset,
        .decode = stt_engine_decode_capture_pcm_cancelable,
        .status_name = stt_status_name,
        .input_status_name = stt_input_status_name,
        .preset_name = stt_preset_name
    };
    *start->api = api;
    return 0;
}

/* Caller joins the recognition worker and destroys its engine first. */
int module_stop(SceSize size, void *data)
{
    (void)size; (void)data;
    if (__stdio_exit_handler) __stdio_exit_handler();
    /* newlib deliberately skips reclaiming its current _impure_ptr. Move
     * that state into a temporary retired record, otherwise strtod/dtoa's
     * freelists leak on every module unload. No decoder/worker remains. */
    struct _reent retired = *_impure_ptr;
    _REENT_INIT_PTR(_impure_ptr);
    _reclaim_reent(&retired);
    /* Process exit normally discards these dynamic FILE slots with the
     * whole heap. Here the heap outlives the module. Descriptors are already
     * closed above; detach and free only the dynamic tail, not static __sf. */
    for (unsigned count = 0; __sglue._next && count < 1024u; ++count) {
        struct _glue *slot = __sglue._next;
        __sglue._next = slot->_next;
        host.release(slot);
    }
    if (__sglue._next) return -1;
    __deinit_mutex();
    __locks_deinit();
    memset(&host, 0, sizeof(host));
    return 0;
}
