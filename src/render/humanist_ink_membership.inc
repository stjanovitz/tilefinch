/* Candidate membership mirrors the sparse cold ink-bound profile. A
   constant-size key normalization shares all unmodified glyphs with native
   family entries; no outline measurement is performed on a warm access.
   The typography regression compares every ASCII bit against the bounds. */
static const uint32_t humanist_ink_candidate_bits[3][3] = {
    {UINT32_C(0x0c00a400), UINT32_C(0x90000400), UINT32_C(0x10000001)},
    {UINT32_C(0x00008420), UINT32_C(0x90000400), UINT32_C(0x10000001)},
    {UINT32_C(0x0c00a400), UINT32_C(0x80000400), UINT32_C(0x10000001)}
};

static unsigned glyph_cache_ink_profile(unsigned profile, unsigned codepoint)
{
    if (profile < 1 || profile > 3 || codepoint < 32 || codepoint > 126)
        return 0;
    unsigned at = codepoint - 32;
    return (humanist_ink_candidate_bits[profile - 1][at / 32]
            & (UINT32_C(1) << (at % 32))) != 0 ? profile : 0;
}
