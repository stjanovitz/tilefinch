/* Fixed-pixel research corpus, not a presentation/FPS benchmark. All storage
   is Budget-owned and released before returning; no EDRAM is claimed. */
typedef struct {
    uint64_t cpu, begin, end;
    bool valid;
} CanvasSetupClock;

static CanvasSetupClock canvas_setup_clock(void)
{
    CanvasSetupClock result = {0};
#if defined(PSP)
    SceKernelThreadRunStatus status;
    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    result.begin = (uint64_t) sceKernelGetSystemTimeWide();
    int rc = sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &status);
    result.end = (uint64_t) sceKernelGetSystemTimeWide();
    result.valid = rc >= 0;
    result.cpu = ((uint64_t) status.runClocks.hi << 32) | status.runClocks.low;
#endif
    return result;
}

static bool canvas_setup_state_check(RasterTarget *target,
    const DrawCommand *command, const ImageResource *image, TileCache *metrics)
{
#if defined(_MIPS_ARCH_ALLEGREX)
    uint32_t saved[4] __attribute__((aligned(16)));
    uint32_t after[4] __attribute__((aligned(16)));
    const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        UINT32_C(0x01234567), UINT32_C(0x89abcdef),
        UINT32_C(0x76543210), UINT32_C(0xfedcba98)};
    uint32_t original_s, original_t, original_d;
    __asm__ volatile ("sv.q C000,0(%3)\n"
        "mfvc %0,$128\n" "mfvc %1,$129\n" "mfvc %2,$130\n"
        : "=r" (original_s), "=r" (original_t), "=r" (original_d)
        : "r" (saved) : "memory");
    uint32_t ps = UINT32_C(0xf4321), pt = UINT32_C(0xe9876), pd = UINT32_C(0xa5a);
    __asm__ volatile ("lv.q C000,0(%3)\n"
        "mtvc %0,$128\n" "mtvc %1,$129\n" "mtvc %2,$130\n"
        : : "r" (ps), "r" (pt), "r" (pd), "r" (sentinel) : "memory");
#endif
    rasterize_opaque_canvas_fast(target, command, image, metrics);
    bool okay = true;
#if defined(_MIPS_ARCH_ALLEGREX)
    uint32_t as, at, ad;
    __asm__ volatile ("sv.q C000,0(%3)\n"
        "mfvc %0,$128\n" "mfvc %1,$129\n" "mfvc %2,$130\n"
        : "=r" (as), "=r" (at), "=r" (ad) : "r" (after) : "memory");
    okay = as == ps && at == pt && ad == pd
        && memcmp(after, sentinel, sizeof(after)) == 0;
    __asm__ volatile ("lv.q C000,0(%3)\n"
        "mtvc %0,$128\n" "mtvc %1,$129\n" "mtvc %2,$130\n"
        : : "r" (original_s), "r" (original_t), "r" (original_d), "r" (saved)
        : "memory");
#endif
    return okay;
}

bool tile_cache_validation_canvas_setup_probe(FILE *output)
{
    if (output == NULL) return false;
    bool old_setup = canvas_validation_setup_once;
    bool old_vfpu = canvas_validation_vfpu_conversion;
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    enum { SOURCE_BYTES = 2048 * 180, OUTPUT_WORDS = 512 * 272 + 32,
           BATCH = 16 };
    unsigned char *source_base = budget_malloc(&budget, SOURCE_BYTES + 16u);
    uint16_t *a_base = budget_malloc(&budget, OUTPUT_WORDS * 2u + 16u);
    uint16_t *b_base = budget_malloc(&budget, OUTPUT_WORDS * 2u + 16u);
    TileCache *metrics = budget_calloc(&budget, 1, sizeof(*metrics));
    bool okay = source_base != NULL && a_base != NULL && b_base != NULL
        && metrics != NULL;
    uint32_t mismatches = 0, state_mismatches = 0, runs = 0;
    if (!okay) goto finish;
    unsigned char *source = (unsigned char *) (((uintptr_t) source_base + 15u) & ~(uintptr_t) 15u);
    uint16_t *a = (uint16_t *) (((uintptr_t) a_base + 15u) & ~(uintptr_t) 15u);
    uint16_t *b = (uint16_t *) (((uintptr_t) b_base + 15u) & ~(uintptr_t) 15u);
    uint32_t random = UINT32_C(0x1234567);
    for (size_t i = 0; i < SOURCE_BYTES / 4u; i++) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        ((uint32_t *) (void *) source)[i] = random;
    }
    canvas_validation_vfpu_conversion = canvas_validation_vfpu_usable;
    for (unsigned fixture = 0; okay && fixture < 2; fixture++) {
        ImageCanvasNativeSurface native = { .pixels = source,
            .stride = fixture == 0 ? 2048u : 1280u, .authoritative = true };
        ImageResource image = { .pixels = source, .width = 320, .height = 180,
            .is_canvas = true, .canvas_native_surface = &native };
        DrawCommand command = { .width = 480, .height = 272 };
        RasterTarget target = { .pixels = a + 8, .stride = 512,
            .width = 480, .height = 272, .right = 480, .bottom = 272 };
        for (size_t i = 0; i < OUTPUT_WORDS; i++) a[i] = b[i] = (uint16_t) (i * 317u);
        canvas_validation_setup_once = false;
        if (!canvas_setup_state_check(&target, &command, &image, metrics)) state_mismatches++;
        target.pixels = b + 8;
        canvas_validation_setup_once = true;
        if (!canvas_setup_state_check(&target, &command, &image, metrics)) state_mismatches++;
        if (memcmp(a, b, OUTPUT_WORDS * 2u) != 0) mismatches++;
        for (unsigned run = 0; okay && run < 6; run++) {
            canvas_validation_setup_once = (run & 1u) != 0;
            CanvasSetupClock before = canvas_setup_clock();
            for (unsigned batch = 0; batch < BATCH; batch++)
                rasterize_opaque_canvas_fast(&target, &command, &image, metrics);
            CanvasSetupClock after = canvas_setup_clock();
            if (memcmp(a, b, OUTPUT_WORDS * 2u) != 0) mismatches++;
            if (metrics->canvas_fast_conversion.setup_once != canvas_validation_setup_once)
                okay = false;
#if defined(PSP)
            okay = before.valid && after.valid && after.cpu >= before.cpu
                && after.begin >= before.end;
#endif
            fprintf(output, "tilefinch-canvas-setup-run: fixture=%u run=%u setup=%u "
                "batch=%u cpu=%llu wall-min=%llu wall-max=%llu observer=%llu "
                "kernel=%lu repeat=%lu vfpu=%lu valid=%u\n", fixture, run,
                canvas_validation_setup_once ? 1u : 0u, BATCH,
                (unsigned long long) (after.cpu - before.cpu),
                (unsigned long long) (after.begin - before.end),
                (unsigned long long) (after.end - before.begin),
                (unsigned long long) (before.end - before.begin + after.end - after.begin),
                (unsigned long) metrics->canvas_fast_conversion.kernel_rows,
                (unsigned long) metrics->canvas_fast_conversion.repeat_rows,
                (unsigned long) metrics->canvas_fast_conversion.vfpu_rows, okay ? 1u : 0u);
#if defined(_MIPS_ARCH_ALLEGREX)
            if (metrics->canvas_fast_conversion.vfpu_rows != 180u) okay = false;
#endif
            runs++;
        }
    }
finish:
    canvas_validation_setup_once = old_setup;
    canvas_validation_vfpu_conversion = old_vfpu;
    budget_free(&budget, metrics);
    budget_free(&budget, b_base);
    budget_free(&budget, a_base);
    budget_free(&budget, source_base);
    okay = okay && mismatches == 0 && state_mismatches == 0 && budget.current == 0;
    fprintf(output, "tilefinch-canvas-setup-probe: okay=%u runs=%lu mismatches=%lu "
        "state-mismatches=%lu remaining=%lu source=ram ownership=unchanged\n",
        okay ? 1u : 0u, (unsigned long) runs, (unsigned long) mismatches,
        (unsigned long) state_mismatches, (unsigned long) budget.current);
    return okay;
}
