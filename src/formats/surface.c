#include "surface.h"

/*
 * codeTable, 0x800AE614. Dumped straight out of the image:
 *
 *   800AE614  00 02 02 02 02 00 00 00
 *
 * Bit 1 of a PSX polygon command is ABE, so this reads as "selector 0 opaque,
 * 1..4 transparent, 5..7 opaque". The world uses entry 0 and every model face
 * on this disc uses entry 0 too, which is why the table looked inert until the
 * blend side was traced.
 */
const u8 q2_surf_code_table[Q2_SURF_SELECTOR_COUNT] = {
    0x00, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00
};

/*
 * blendTable, 0x800B36D8, as 0x80078378 leaves it:
 *
 *     for (i = 0; i < 4; i++) tbl[i + 1] = i * 32;
 *     tbl[0] = 32;
 *
 * Entries 5..7 are never written. They are BSS and therefore zero, and are
 * spelled out here rather than left to a partial initialiser so that "the
 * engine leaves these at zero" is a statement the file makes.
 */
const u16 q2_surf_blend_table[Q2_SURF_SELECTOR_COUNT] = {
    32, 0, 32, 64, 96, 0, 0, 0
};

bool q2_surf_selector_semi(u32 sel)
{
    if (sel >= Q2_SURF_SELECTOR_COUNT)
        return false;
    return (q2_surf_code_table[sel] & Q2_SURF_CODE_ABE) != 0;
}

psx_blend q2_surf_selector_blend(u32 sel)
{
    if (sel >= Q2_SURF_SELECTOR_COUNT)
        return PSX_BLEND_HALF;
    return (psx_blend)((q2_surf_blend_table[sel] >> 5) & 3u);
}

/* ------------------------------------------------------------------------- */
void q2_tpage_table_init(q2_tpage_table *t)
{
    u32 i;

    if (!t)
        return;

    /*
     * GetTPage(0, 0, 64*(i+1), 256) for i = 0..19, from 0x80077FE8. The first
     * literal is the colour mode — 4bpp, stated by the executable — and the
     * second is the ABR, which starts at 0. Page x is in halfwords and the
     * hardware field counts 64-halfword cells, so cell (i+1); y 256 is the
     * page's second row of cells.
     */
    for (i = 0; i < Q2_TPAGE_TABLE_LEN; i++)
        t->word[i] = psx_make_tpage((int)(i + 1u), 1, PSX_BLEND_HALF, PSX_TEX_4BIT);
}

u16 q2_tpage_world_opaque(q2_tpage_table *t, u32 page)
{
    u16 w;

    if (!t || page >= Q2_TPAGE_TABLE_LEN)
        return 0;

    /*
     * 0x80068314-0x80068324. The OR is blendTable[2]; the store back into the
     * table is what makes later transparent packet constructions on this page
     * additive, and is the only reason "world transparency is ADD" is true.
     */
    w = (u16)(t->word[page] | q2_surf_blend_table[Q2_SURF_WORLD_BLEND_SELECTOR]);
    t->word[page] = w;
    return w;
}

u16 q2_tpage_world_semi(const q2_tpage_table *t, u32 page)
{
    if (!t || page >= Q2_TPAGE_TABLE_LEN)
        return 0;

    /* 0x800682C8-0x800682E4: the transparent path reads and does not write. */
    return t->word[page];
}

u16 q2_tpage_model(const q2_tpage_table *t, u32 page, u32 sel)
{
    u16 blend;

    if (!t || page >= Q2_TPAGE_TABLE_LEN)
        return 0;

    blend = (sel < Q2_SURF_SELECTOR_COUNT) ? q2_surf_blend_table[sel] : 0;

    /* 0x8006DE50-0x8006DE5C. No write-back on this path. */
    return (u16)(t->word[page] | blend);
}

/* ------------------------------------------------------------------------- */
bool q2_surf_should_subdivide(q2_surf_variant variant,
                              u16 clut,
                              s32 depth_sum,
                              s32 threshold,
                              q2_surf_pool_pressure pressure)
{
    switch (variant) {
    case Q2_SURF_VARIANT_SUBDIVIDE:
        break;
    case Q2_SURF_VARIANT_TAGGED:
        /* 0x800AFBCC: the polygon has to carry the permission itself. */
        if (!q2_surf_poly_may_subdivide(clut))
            return false;
        break;
    case Q2_SURF_VARIANT_FLAT:
    case Q2_SURF_VARIANT_HIDDEN:
    default:
        /* Neither linker reaches the callback at all. */
        return false;
    }

    /* 0x800AF974 / 0x800AFBE4: strictly nearer than the threshold. */
    if (depth_sum >= threshold)
        return false;

    /* 0x800698A0-0x800698EC, the packet-pool budget. */
    switch (pressure) {
    case Q2_SURF_POOL_FREE:
        return true;
    case Q2_SURF_POOL_TIGHT:
        return depth_sum <= threshold / 4;
    case Q2_SURF_POOL_FULL:
    default:
        return false;
    }
}
