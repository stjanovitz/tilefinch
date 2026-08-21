#include "tilefinch/swdec_component.h"

#include <pspkernel.h>
#include <string.h>

#include "swdec.h"
#include "swdec_arena.h"
#include "swdec_me.h"

PSP_MODULE_INFO("tilefinch_swdec", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

/* The proven asm22 library was built with its attribution hooks present but
 * disabled. Keep the optional component's shipping path equally cheap while
 * satisfying that diagnostic ABI; validation runners may replace these with
 * real counters without rebuilding the decoder core. */
unsigned swdec_stage_us[12];
int swdec_stage_on;
int swdec_probe_norecon;
unsigned swdec_stage_now(void) { return 0; }

_Static_assert(sizeof(TilefinchSwdecPicture) == sizeof(swdec_picture),
               "swdec picture ABI drift");
_Static_assert(sizeof(TilefinchSwdecStats) == sizeof(swdec_stats),
               "swdec statistics ABI drift");
_Static_assert(TILEFINCH_SWDEC_CSC_SLOT_COUNT == SWDEC_ME_CSC_SLOTS,
               "swdec CSC slot ABI drift");

static int component_decode(
    void *decoder, const uint8_t *access_unit, size_t access_unit_bytes,
    uint64_t pts, TilefinchSwdecPicture *picture)
{
    return swdec_decode(
        decoder, access_unit, access_unit_bytes, pts,
        (swdec_picture *) picture);
}

static void component_stats(
    const void *decoder, TilefinchSwdecStats *stats)
{
    swdec_stats_get(decoder, (swdec_stats *) stats);
}

static int component_me_failed(void)
{
    return swdec_me_failed;
}

static int component_audio_setup(void)
{
    return swdec_me_audio_setup(NULL);
}

static void component_bind_aux_arena(void *arena, size_t arena_bytes)
{
    swdec_arena_bind_aux(arena, arena_bytes);
}

int main(int argument_size, char *argument_data[])
{
    void *arguments = argument_data;
    if (arguments == NULL
        || argument_size < (int) sizeof(TilefinchSwdecComponentStart))
        return -1;
    TilefinchSwdecComponentStart *start = arguments;
    if (start->magic != TILEFINCH_SWDEC_COMPONENT_MAGIC
        || start->abi_version != TILEFINCH_SWDEC_COMPONENT_ABI_VERSION
        || start->struct_size < sizeof(*start)
        || start->api == NULL) return -2;

    TilefinchSwdecComponentApi api = {
        .magic = TILEFINCH_SWDEC_COMPONENT_MAGIC,
        .abi_version = TILEFINCH_SWDEC_COMPONENT_ABI_VERSION,
        .struct_size = sizeof(api),
        .arena_bytes = swdec_arena_bytes,
        .open = (void *(*)(void *, size_t, int, int, int)) swdec_open,
        .decode = component_decode,
        .stats = component_stats,
        .set_speed = (void (*)(void *, int)) swdec_set_speed,
        .close = (void (*)(void *)) swdec_close,
        .bind_aux_arena = component_bind_aux_arena,
        .attach_me = swdec_me_attach_path,
        .recover_me = swdec_me_recover,
        .detach_me = swdec_me_detach,
        .restore_me = swdec_me_restore,
        .me_failed = component_me_failed,
        .audio_setup = component_audio_setup,
        .audio_submit = swdec_me_audio_submit,
        .audio_poll = swdec_me_audio_poll,
        .audio_done = swdec_me_audio_done,
        .audio_pcm = swdec_me_audio_pcm,
        .audio_reset = swdec_me_audio_reset,
        .audio_shutdown = swdec_me_audio_shutdown,
        .csc_begin = swdec_me_csc_begin,
        .csc_close = swdec_me_csc_close,
        .csc_off = swdec_me_csc_off
    };
    memcpy(start->api, &api, sizeof(api));
    return 0;
}
