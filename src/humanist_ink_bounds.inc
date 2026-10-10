/* Independently measured printable-ASCII horizontal ink bounds, in a
   2048-unit em. Only the bounded metric-mismatch admission candidates are
   retained; these are measurements, not copied font outlines. */
typedef struct {
    uint8_t codepoint;
    int16_t left, right;
} HumanistInkBound;

static const HumanistInkBound humanist_sans_ink_bounds[] = {
    /* Regular */
    {42, 167, 1137},
    {45, 153, 777},
    {47, -30, 860},
    {58, 346, 585},
    {59, 230, 658},
    {74, 44, 746},
    {92, 70, 960},
    {95, -4, 1306},
    {96, 340, 762},
    {124, 378, 552},
    /* Bold */
    {37, 143, 2462},
    {42, 204, 1252},
    {47, 123, 1223},
    {74, 42, 966},
    {92, 189, 1289},
    {95, -16, 1472},
    {96, 374, 972},
    {124, 411, 702},
    /* Italic */
    {42, 295, 1255},
    {45, 154, 820},
    {47, -186, 1042},
    {58, 216, 715},
    {59, 23, 715},
    {74, -78, 962},
    {95, -202, 1136},
    {96, 565, 907},
    {124, 421, 595},
};

static const uint8_t humanist_sans_ink_offsets[] = {0, 10, 18, 27};

static const HumanistInkBound *humanist_ink_bound(
    unsigned profile, unsigned codepoint)
{
    if (profile < 1 || profile > 3) return NULL;
    for (unsigned i = humanist_sans_ink_offsets[profile - 1];
         i < humanist_sans_ink_offsets[profile]; i++) {
        if (humanist_sans_ink_bounds[i].codepoint == codepoint)
            return &humanist_sans_ink_bounds[i];
    }
    return NULL;
}

