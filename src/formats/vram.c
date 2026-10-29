#include "vram.h"

#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t q2_packbits_decode(const u8 *src, size_t src_size,
                          u8 *dst, size_t target, size_t *consumed)
{
    size_t in = 0, out = 0;

    if (!src || !dst) {
        if (consumed) *consumed = 0;
        return 0;
    }

    /* Bounded by the expected output, not by the input — that is what the
     * engine's own loop does (bgtz on the remaining output count at
     * 0x80068C7C), and running to the end of the input instead overshoots on
     * 35 of the disc's 553 payloads, which all carry alignment padding. */
    while (out < target) {
        u8 control;

        if (in >= src_size)
            break;

        control = src[in++];

        if (control < 0x80) {
            size_t count = (size_t)control + 1;

            if (count > target - out)
                count = target - out;
            if (count > src_size - in)
                count = src_size - in;

            memcpy(dst + out, src + in, count);
            out += count;
            in  += count;
        } else {
            /* 257 - control, so 0x80 is 129 repeats and 0xFF is 2. This is NOT
             * the Apple PackBits no-op; the engine has no special case for
             * 0x80, and no stream on the disc uses it either way. */
            size_t count = (size_t)(257 - (int)control);
            u8 value;

            if (in >= src_size)
                break;
            value = src[in++];

            if (count > target - out)
                count = target - out;

            memset(dst + out, value, count);
            out += count;
        }
    }

    if (consumed)
        *consumed = in;

    return out;
}

/* ------------------------------------------------------------------------- */
/* Case-insensitive compare, so callers can pass a name in any case without
 * dragging in a platform-specific stricmp. */
static bool name_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (int)(u8)*a++;
        int cb = (int)(u8)*b++;

        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

q2_result q2_vram_load(q2_vram_section *out, const disc *d, const char *map)
{
    char path[256];
    q2_result r;
    u32 ofs_sound_bank, total_size;
    u32 count, i;
    u32 section_size, cursor, names_offset;
    const u8 *p;        /* the whole file            */
    const u8 *a;        /* section A base, == p + 12 */
    const char *name;

    if (!out || !d || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/SNDVRAM.DAT", map);

    r = disc_read_file(d, path, &out->buf);
    if (r != Q2_OK)
        return r;

    if (out->buf.size < 16) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    p = out->buf.data;

    if (q2_rd_u32(p) != Q2_VRAM_SECTION_BASE) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    ofs_sound_bank = q2_rd_u32(p + 4);
    total_size     = q2_rd_u32(p + 8);

    if (total_size != (u32)out->buf.size ||
        ofs_sound_bank < Q2_VRAM_SECTION_BASE ||
        ofs_sound_bank > total_size) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    a            = p + Q2_VRAM_SECTION_BASE;
    section_size = ofs_sound_bank - Q2_VRAM_SECTION_BASE;

    if (section_size < 4) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    out->texpage_count = q2_rd_u8(a + 0);
    out->clut4_count_a = q2_rd_u8(a + 2);
    out->clut4_count_b = q2_rd_u8(a + 3);
    out->clut4_count   = (u32)out->clut4_count_a + out->clut4_count_b;
    out->clut8_count   = q2_rd_u8(a + 1);

    count = out->texpage_count + out->clut8_count;

    if (count == 0) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    /* Walk the section's fixed-size arrays in order, checking each against the
     * section length before advancing. Getting this wrong is how the earlier
     * reading mis-sized the CLUT region: the arrays are contiguous and the name
     * list's position is derived from all three counts, so one bad count moves
     * the name list into the middle of a payload. */
    cursor = 4;
    if (count > (section_size - cursor) / Q2_VRAM_RECORD_SIZE) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }
    cursor += count * Q2_VRAM_RECORD_SIZE;

    out->clut4 = a + cursor;
    if (out->clut4_count > (section_size - cursor) / (Q2_VRAM_CLUT4_ENTRIES * 2)) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }
    cursor += out->clut4_count * (Q2_VRAM_CLUT4_ENTRIES * 2);

    out->clut8 = a + cursor;
    if (out->clut8_count > (section_size - cursor) / (Q2_VRAM_CLUT8_ENTRIES * 2)) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }
    cursor += out->clut8_count * (Q2_VRAM_CLUT8_ENTRIES * 2);

    names_offset = cursor;

    out->images = (q2_vram_image *)calloc(count, sizeof(q2_vram_image));
    if (!out->images) {
        q2_buf_free(&out->buf);
        return Q2_ERR_NO_MEMORY;
    }
    out->image_count = count;

    for (i = 0; i < count; i++) {
        const u8 *rec = a + 4 + (size_t)i * Q2_VRAM_RECORD_SIZE;

        /* Payload offsets are section-relative, not file-absolute. The raw
         * value lands twelve bytes inside the name list. */
        out->images[i].offset = Q2_VRAM_SECTION_BASE + q2_rd_u32(rec);
        out->images[i].width  = q2_rd_u16(rec + 4);
        out->images[i].height = q2_rd_u16(rec + 6);

        /* Texture pages ignore their stored dimensions (0x80068B74). */
        if (i < out->texpage_count)
            out->images[i].decoded_size =
                (u32)Q2_VRAM_TEXPAGE_W * Q2_VRAM_TEXPAGE_H;
        else
            out->images[i].decoded_size =
                (u32)out->images[i].width * out->images[i].height;
    }

    /* A payload runs until the next one starts; the last ends at the sound
     * bank. */
    for (i = 0; i < count; i++) {
        u32 end = (i + 1 < count) ? out->images[i + 1].offset : ofs_sound_bank;

        if (out->images[i].offset < Q2_VRAM_SECTION_BASE + names_offset ||
            end < out->images[i].offset || end > ofs_sound_bank) {
            Q2_ERROR("%s: image %u spans 0x%X..0x%X, which is out of bounds",
                     path, i, out->images[i].offset, end);
            free(out->images);
            q2_buf_free(&out->buf);
            return Q2_ERR_BAD_FORMAT;
        }
        out->images[i].packed_size = end - out->images[i].offset;
    }

    /* The packed name list runs from names_offset to the first payload, and
     * holds exactly `count` NUL-terminated names. This is the cross-check that
     * validates the whole layout formula: if any of the three counts were
     * wrong, the walk would not land on the first payload. */
    name = (const char *)(a + names_offset);
    for (i = 0; i < count; i++) {
        const char *end = (const char *)(p + out->images[0].offset);
        const char *q   = name;

        while (q < end && *q != '\0')
            q++;

        if (q >= end) {
            Q2_ERROR("%s: name list holds fewer than %u names — the section "
                     "layout does not add up", path, count);
            free(out->images);
            q2_buf_free(&out->buf);
            return Q2_ERR_BAD_FORMAT;
        }

        out->images[i].name = name;
        name = q + 1;
    }

    return Q2_OK;
}

void q2_vram_free(q2_vram_section *section)
{
    if (!section)
        return;
    free(section->images);
    q2_buf_free(&section->buf);
    memset(section, 0, sizeof(*section));
}

size_t q2_vram_decoded_size(const q2_vram_section *section, u32 index)
{
    if (!section || index >= section->image_count)
        return 0;
    return section->images[index].decoded_size;
}

q2_result q2_vram_decode(const q2_vram_section *section, u32 index,
                         u8 *out, size_t out_capacity, size_t *out_size)
{
    const q2_vram_image *img;
    size_t target, produced, consumed = 0;

    if (!section || !out || index >= section->image_count)
        return Q2_ERR_INVALID_ARG;

    img    = &section->images[index];
    target = img->decoded_size;

    if (target == 0)
        return Q2_ERR_BAD_FORMAT;
    if (out_capacity < target)
        return Q2_ERR_RANGE;

    produced = q2_packbits_decode(section->buf.data + img->offset,
                                  img->packed_size,
                                  out, target, &consumed);

    if (produced != target) {
        Q2_ERROR("vram: image %u decoded to %zu bytes, expected %zu",
                 index, produced, target);
        return Q2_ERR_BAD_FORMAT;
    }

    /* Only the final record of a file carries padding, and never more than
     * three bytes, so anything else means the stream is not what we think. */
    if (img->packed_size - consumed > 3) {
        Q2_WARN("vram: image %u left %zu bytes unconsumed",
                index, (size_t)img->packed_size - consumed);
    }

    if (out_size)
        *out_size = produced;

    return Q2_OK;
}

bool q2_vram_find_by_name(const q2_vram_section *section, const char *name,
                          u32 *index_out)
{
    u32 i;

    if (!section || !name)
        return false;

    for (i = 0; i < section->image_count; i++) {
        if (section->images[i].name &&
            name_equal_ci(section->images[i].name, name)) {
            if (index_out)
                *index_out = i;
            return true;
        }
    }
    return false;
}

/*
 * The two placement tables, 0x800A3274 and 0x800A329C. Slots 0..14 march across
 * the texture half of VRAM in 64-halfword steps; slot 15 is the cell at x = 0
 * that also holds the CLUT array, which is why chars.lbm registers with a v
 * offset of 128. Slots 16..19 are zero on this build and are registered by
 * nothing.
 */
const s16 q2_vram_slot_x[Q2_VRAM_IMAGE_SLOTS] = {
     64, 128, 192, 256, 320, 384, 448, 512, 576, 640,
    704, 768, 832, 896, 960,   0,   0,   0,   0,   0
};

const s16 q2_vram_slot_y[Q2_VRAM_IMAGE_SLOTS] = {
    256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
    256, 256, 256, 256, 256, 256,   0,   0,   0,   0
};

/*
 * 0x8003FE20, in the order it registers them. Where a name appears once with a
 * non-zero v offset that offset is the third argument to 0x8006901C; the rest
 * are zero.
 */
const q2_vram_ui_image q2_vram_ui_images[] = {
    /* name              slot  v    bpp */
    { "control.lbm",     10,   0,   8 },  /* 0x8003FE34, via 0x80068FB0     */
    { "mouse.lbm",        6,   0,   8 },  /* the pad diagrams, with callouts */
    { "multipics.lbm",    8,   0,   8 },  /* ten deathmatch previews         */
    { "multipic2.lbm",   12,   0,   8 },  /* two more                        */
    { "frontend.lbm",    13,   0,   4 },  /* 0x8003FE74, via 0x8006901C     */
    { "Squiggle.lbm",    12, 159,   4 },  /* BIGGUN LevelBin+0x410 GetTPage */
    { "chars.lbm",       15, 128,   4 },
    { "qkm_menu.lbm",    14,   0,   4 },  /* the three are one slot, picked  */
    { "qk2_menu.lbm",    14,   0,   4 },  /* by session mode at 0x8003FEAC   */
    { "qk_menu.lbm",     14,   0,   4 },
    { "EndDemo1.lbm",     2,   0,   0 },
    { "FrDemo1.lbm",      2,   0,   0 },
    { "DemoSp1.lbm",     11,   0,   0 },
    { "DemoSp2.lbm",     12,   0,   0 },
    { "HamLogo.lbm",      0,   0,   0 },
    { "IdLogo.lbm",       4,   0,   0 },
    { "wipteam1.lbm",     4,   0,   0 },
    { "ActLogo.lbm",      8,   0,   0 },
    { "Legal.lbm",        4,   0,   0 },
    { "background3.lbm",  2,   0,   0 },  /* 256 halfwords: neither 4 nor 8
                                           * fits one page, so unsettled     */
    { "Screena.lbm",      0,   0,   0 },
    { "Screenb.lbm",      2,   0,   0 },
    { "Globe0.lbm",       4,   0,   0 },
    { "Globe1.lbm",       5,   0,   0 },
    { "Globe2.lbm",       6,   0,   0 },
    { "Globe3.lbm",       7,   0,   0 },
    { "Globe4.lbm",       8,   0,   0 },
    { "Globe5.lbm",       9,   0,   0 },
    { "Globe6.lbm",      10,   0,   0 },
    { "Globe7.lbm",      11,   0,   0 }
};

u32 q2_vram_ui_image_count(void)
{
    return (u32)(sizeof(q2_vram_ui_images) / sizeof(q2_vram_ui_images[0]));
}

bool q2_vram_ui_slot(const char *name, u32 *slot, int *v_offset)
{
    u32 i, n = q2_vram_ui_image_count();

    if (!name)
        return false;

    for (i = 0; i < n; i++) {
        if (!name_equal_ci(q2_vram_ui_images[i].name, name))
            continue;
        if (slot)
            *slot = q2_vram_ui_images[i].slot;
        if (v_offset)
            *v_offset = q2_vram_ui_images[i].v_offset;
        return true;
    }
    return false;
}

bool q2_vram_image_rect(const q2_vram_section *section, u32 index, u32 slot,
                        int v_offset, q2_vram_rect *out)
{
    const q2_vram_image *img;
    u32 w_bytes, h_rows;

    if (!section || !out || index >= section->image_count)
        return false;
    if (slot >= Q2_VRAM_IMAGE_SLOTS)
        return false;

    img = &section->images[index];

    if (index < section->texpage_count) {
        /* A texture page ignores its stored dimensions (0x80068B74). */
        w_bytes = Q2_VRAM_TEXPAGE_W;
        h_rows  = Q2_VRAM_TEXPAGE_H;
    } else {
        w_bytes = img->width;
        h_rows  = img->height;
    }

    out->x = q2_vram_slot_x[slot];
    out->y = (s16)(q2_vram_slot_y[slot] + v_offset);

    /* The engine halves the byte width to get halfwords (srl at 0x80069214). */
    out->w = (s16)(w_bytes >> 1);
    out->h = (s16)h_rows;

    return true;
}

q2_result q2_vram_upload_named(const q2_vram_section *section, const char *name,
                               u32 slot, int v_offset, struct psx_vram *vram,
                               q2_vram_rect *placed)
{
    psx_vram *dst = (psx_vram *)vram;
    q2_vram_rect rect;
    q2_vram_image *img;
    u32 index;
    size_t need, got = 0;
    u8 *pixels;
    q2_result r;
    int row;

    if (!section || !name || !dst)
        return Q2_ERR_INVALID_ARG;

    if (!q2_vram_find_by_name(section, name, &index))
        return Q2_ERR_NOT_FOUND;
    if (!q2_vram_image_rect(section, index, slot, v_offset, &rect))
        return Q2_ERR_INVALID_ARG;

    need = q2_vram_decoded_size(section, index);
    if (need == 0)
        return Q2_ERR_BAD_FORMAT;

    pixels = (u8 *)malloc(need);
    if (!pixels)
        return Q2_ERR_NO_MEMORY;

    r = q2_vram_decode(section, index, pixels, need, &got);
    if (r != Q2_OK) {
        free(pixels);
        return r;
    }

    img = &section->images[index];

    /*
     * LoadImage, clipped. The source stride is the record's own `width` in
     * bytes even for a texture page, because that is what the decoder wrote;
     * the rect's width is what reaches VRAM.
     */
    psx_vram_transfer(dst);
    for (row = 0; row < rect.h; row++) {
        int vy = rect.y + row;
        const u8 *src = pixels + (size_t)row * img->width;
        int hw;

        if (vy < 0 || vy >= PSX_VRAM_HEIGHT)
            break;
        if ((size_t)(row + 1) * img->width > got)
            break;

        for (hw = 0; hw < rect.w; hw++) {
            int vx = rect.x + hw;
            if (vx < 0 || vx >= PSX_VRAM_WIDTH)
                break;
            dst->px[vy][vx] = (u16)(src[hw * 2] | ((u16)src[hw * 2 + 1] << 8));
        }
    }

    free(pixels);

    if (placed)
        *placed = rect;
    return Q2_OK;
}

void q2_vram_clut4_rect(const q2_vram_section *section, q2_vram_rect *out)
{
    u32 rows;

    if (!out)
        return;

    /* Four 16-halfword CLUTs per 64-halfword row, rounded up. */
    rows = section ? (section->clut4_count + 3) / 4 : 0;

    out->x = Q2_VRAM_CLUT4_X;
    out->y = Q2_VRAM_CLUT4_Y;
    out->w = Q2_VRAM_CLUT4_W;
    out->h = (s16)rows;
}

u16 q2_vram_clut4_id(u32 index)
{
    u32 x = 16u * (index & 3u);
    u32 y = (u32)Q2_VRAM_CLUT4_Y + (index >> 2);

    return (u16)(((y & 0x1FFu) << 6) | ((x & 0x3FFu) >> 4));
}

bool q2_vram_get_clut4(const q2_vram_section *section, u32 index, u16 out[16])
{
    const u8 *src;
    u32 i;

    if (!section || !out || !section->clut4 || index >= section->clut4_count)
        return false;

    src = section->clut4 + (size_t)index * (Q2_VRAM_CLUT4_ENTRIES * 2);

    for (i = 0; i < Q2_VRAM_CLUT4_ENTRIES; i++) {
        u16 entry = q2_rd_u16(src + i * 2);

        /* The load-time fix-up at 0x800762B4: every non-zero entry gets the STP
         * bit. A zero entry stays zero, which is how fully transparent texels
         * are expressed. */
        if (entry != 0)
            entry |= Q2_VRAM_STP_BIT;

        out[i] = entry;
    }
    return true;
}

bool q2_vram_get_clut8(const q2_vram_section *section, u32 image_index,
                       u16 out[256])
{
    const u8 *src;
    u32 i;

    if (!section || !out || !section->clut8 ||
        image_index >= section->clut8_count)
        return false;

    src = section->clut8 + (size_t)image_index * (Q2_VRAM_CLUT8_ENTRIES * 2);

    for (i = 0; i < Q2_VRAM_CLUT8_ENTRIES; i++)
        out[i] = q2_rd_u16(src + i * 2);

    return true;
}

/* ------------------------------------------------------------------------- */
/* Palette binding and VRAM upload                                            */
/* ------------------------------------------------------------------------- */
const u8 *q2_vram_clut(const q2_vram_section *section, u32 index)
{
    if (!section || !section->clut4 || index >= section->clut4_count)
        return NULL;
    return section->clut4 + (size_t)index * (Q2_VRAM_CLUT4_ENTRIES * 2);
}

q2_result q2_vram_upload(const q2_vram_section *section, struct psx_vram *vram)
{
    psx_vram *v = (psx_vram *)vram;
    u8 *page;
    u32 i;
    q2_result r = Q2_OK;

    if (!section || !v)
        return Q2_ERR_INVALID_ARG;

    psx_vram_transfer(v);
    /* Palettes first: they live at x 0..63, y 256+, which is exactly the cell
     * the texture pages deliberately skip over. */
    for (i = 0; i < section->clut4_count; i++) {
        const u8 *entries = q2_vram_clut(section, i);
        u32 x, y, e;

        if (!entries)
            break;

        q2_vram_clut_pos(i, &x, &y);
        if (y >= PSX_VRAM_HEIGHT)
            break;

        /*
         * The loader's own fix-up, which this path was missing while
         * `q2_vram_get_clut4` above applied it — two functions decoding the same
         * palettes and disagreeing about them.
         *
         * 0x800762B4 walks all `(clut4_count_a + clut4_count_b) * 16` entries
         * with `a3 = 0x8000` and does `beq v0, zero, skip` / `or v0, v1, a3`:
         * every NON-ZERO entry gets the STP bit, and a zero entry stays zero so
         * that fully transparent texels keep working.
         *
         * The rasterizer reads bits 0..14 for colour and bit 15 to decide
         * whether an ABE textured packet blends this particular texel.
         */
        for (e = 0; e < Q2_VRAM_CLUT4_ENTRIES; e++) {
            u16 entry;

            if (x + e >= PSX_VRAM_WIDTH)
                break;

            entry = q2_rd_u16(entries + e * 2);
            if (entry != 0)
                entry |= Q2_VRAM_STP_BIT;
            v->px[y][x + e] = entry;
        }
    }

    /* Texture pages: decode each one and blit it as (width>>1) halfwords by
     * height rows, at x = 64*(slot+1), y = 256. */
    page = (u8 *)malloc((size_t)Q2_VRAM_TEXPAGE_W * Q2_VRAM_TEXPAGE_H);
    if (!page)
        return Q2_ERR_NO_MEMORY;

    for (i = 0; i < section->texpage_count; i++) {
        size_t decoded = 0;
        u32 vx = 64u * (i + 1u);
        u32 row;

        if (q2_vram_decode(section, i,
                           page, (size_t)Q2_VRAM_TEXPAGE_W * Q2_VRAM_TEXPAGE_H,
                           &decoded) != Q2_OK) {
            r = Q2_ERR_BAD_FORMAT;
            continue;
        }

        for (row = 0; row < Q2_VRAM_TEXPAGE_H; row++) {
            u32 halfwords = Q2_VRAM_TEXPAGE_W / 2;
            u32 col;

            if (256u + row >= PSX_VRAM_HEIGHT)
                break;

            for (col = 0; col < halfwords; col++) {
                const u8 *src = page + (size_t)row * Q2_VRAM_TEXPAGE_W + col * 2;
                if (vx + col >= PSX_VRAM_WIDTH)
                    break;
                v->px[256u + row][vx + col] = q2_rd_u16(src);
            }
        }
    }

    free(page);
    return r;
}
