#include "hudtables.h"

#include "gpu.h"

#include <stdlib.h>
#include <string.h>

/*
 * Virtual address to file offset. A PS-X EXE is a 2 KiB header followed by one
 * flat segment at t_addr, so the map is a single subtraction — and this build's
 * constant is already pinned by every other table in the project: the level
 * table's documented pair (0x8009C6C8 / 0x084EC8) gives exactly this bias.
 */
#define Q2_HUD_VADDR_BIAS 0x80017800u

static const u8 *at(const q2_buf *exe, u32 vaddr, size_t len)
{
    u32 off = vaddr - Q2_HUD_VADDR_BIAS;

    if (vaddr < Q2_HUD_VADDR_BIAS)
        return NULL;
    if ((size_t)off + len > exe->size)
        return NULL;
    return exe->data + off;
}

/* ------------------------------------------------------------------------- */
/*
 * The `&` dispatch, transcribed from the jump table at 0x800ACA1C and the arms
 * it lands on. The second column is the arm's address, kept so that a reader
 * can check any row against the disassembly without re-deriving the table.
 */
typedef struct icon_escape {
    char letter;
    u8   count;
    u8   icon[Q2_HUD_ICON_ESCAPE_MAX];
} icon_escape;

static const icon_escape k_icon_escape[] = {
    { 'A', 2, { 11, 5 } },   /* 0x800431F4  Rail + gun          */
    { 'B', 1, {  2, 0 } },   /* 0x80043188  Blaster             */
    { 'C', 2, {  7, 5 } },   /* 0x800431C4  Chain + gun         */
    { 'D', 1, {  9, 0 } },   /* 0x800431DC  Grenade             */
    { 'E', 1, {  8, 0 } },   /* 0x800431D0  GrenadeLauncher     */
    { 'F', 1, { 13, 0 } },   /* 0x80043218  BFG                 */
    { 'G', 1, {  5, 0 } },   /* 0x800431AC  gun                 */
    { 'H', 1, {  1, 0 } },   /* 0x8004317C  HyperBlaster        */
    { 'I', 0, {  0, 0 } },   /* 0x80043248  no-op               */
    { 'J', 0, {  0, 0 } },   /* 0x80043248                      */
    { 'K', 0, {  0, 0 } },   /* 0x80043248                      */
    { 'L', 1, { 10, 0 } },   /* 0x800431E8  Launcher            */
    { 'M', 2, {  6, 5 } },   /* 0x800431B8  Machine + gun       */
    { 'N', 0, {  0, 0 } },   /* 0x80043248                      */
    { 'O', 2, { 12, 10 } },  /* 0x80043230  Rocket + Launcher   */
    { 'P', 1, {  0, 0 } },   /* 0x80043174  Player              */
    { 'Q', 0, {  0, 0 } },   /* 0x80043248                      */
    { 'R', 1, { 12, 0 } },   /* 0x8004320C  Rocket              */
    { 'S', 1, {  4, 0 } },   /* 0x800431A0  Shotgun             */
    { 'T', 0, {  0, 0 } },   /* 0x80043248                      */
    { 'U', 1, {  3, 0 } },   /* 0x80043194  SuperShotgun        */
    { 'V', 0, {  0, 0 } },   /* 0x80043248                      */
    { 'W', 1, { 14, 0 } }    /* 0x80043224  was                 */
};

int q2_hud_icon_escape(char letter, u8 out[Q2_HUD_ICON_ESCAPE_MAX])
{
    unsigned idx = (unsigned)((unsigned char)letter - 'A');
    int i;

    /* `sltiu v0, v1, 23` at 0x8004314C — the original's own bound. */
    if (idx >= sizeof(k_icon_escape) / sizeof(k_icon_escape[0]))
        return 0;
    if (!out)
        return k_icon_escape[idx].count;

    for (i = 0; i < k_icon_escape[idx].count; i++)
        out[i] = k_icon_escape[idx].icon[i];
    return k_icon_escape[idx].count;
}

/* ------------------------------------------------------------------------- */
/*
 * The built-in palette bank. `0x8007610C` walks eight-byte records from
 * 0x800A2FF4 — one record past the base, because record 0 is the 256-entry
 * palette it uploads separately — until it meets a NULL data pointer.
 *
 * Placement is columns of seven starting at (0, 248), because the loop bumps y
 * after each record and resets to 248 with x += 16 once y reaches 255. Row 255
 * belongs to the 256-entry palette.
 */
static q2_result load_palettes(q2_hud_tables *t)
{
    const u8 *rec;
    u32 k;
    const u8 *p0 = at(&t->exe, Q2_HUD_ADDR_PALETTES, 8);

    if (!p0)
        return Q2_ERR_BAD_FORMAT;

    /* Record 0: 256 entries at (0, 255). No STP fix-up is applied to it — the
     * loop that does that runs only over the 16-entry records. */
    {
        u32 ptr = q2_rd_u32(p0);
        const u8 *src = at(&t->exe, ptr, 512);
        u32 i;

        if (!src)
            return Q2_ERR_BAD_FORMAT;
        for (i = 0; i < 256; i++)
            t->palette256[i] = q2_rd_u16(src + i * 2);
        t->palette256_id = (u16)(((0 >> 4) & 0x3F) | ((255 & 0x1FF) << 6));
    }

    for (k = 1;; k++) {
        u32 ptr, index;
        u16 mask;
        const u8 *src;
        q2_hud_palette *pal;
        int i;
        int col = (int)((k - 1) / 7);
        int row = (int)((k - 1) % 7);

        rec = at(&t->exe, Q2_HUD_ADDR_PALETTES + k * 8, 8);
        if (!rec)
            return Q2_ERR_BAD_FORMAT;

        ptr = q2_rd_u32(rec);
        if (ptr == 0)
            break;                      /* the loop's own terminator */

        mask  = q2_rd_u16(rec + 4);
        index = q2_rd_u16(rec + 6);
        if (index >= Q2_HUD_PALETTE_MAX)
            return Q2_ERR_BAD_FORMAT;

        src = at(&t->exe, ptr, Q2_HUD_PALETTE_SIZE * 2);
        if (!src)
            return Q2_ERR_BAD_FORMAT;

        pal = &t->palette[index];
        for (i = 0; i < Q2_HUD_PALETTE_SIZE; i++) {
            u16 e = q2_rd_u16(src + i * 2);
            /* One mask bit per entry, consumed LSB first (`andi v0, a1, 1`
             * then `sra a1, a1, 1` at 0x800761D0). */
            if (mask & (1u << i))
                e |= 0x8000u;
            pal->entry[i] = e;
        }
        pal->stp_mask = mask;
        pal->vram_x   = (s16)(16 * col);
        pal->vram_y   = (s16)(248 + row);
        pal->clut_id  = (u16)(((pal->vram_x >> 4) & 0x3F) |
                              ((pal->vram_y & 0x1FF) << 6));
        pal->present  = true;

        if (index + 1 > t->palette_count)
            t->palette_count = index + 1;

        if (k > Q2_HUD_PALETTE_MAX)     /* runaway guard, never hit on this build */
            return Q2_ERR_BAD_FORMAT;
    }

    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
q2_result q2_hud_tables_load(q2_hud_tables *out, const disc *d,
                             const q2_build_id *id)
{
    q2_result r;
    const u8 *p;
    u32 i;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("HUD table locations are unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }
    if (!id->exe_name[0])
        return Q2_ERR_NOT_FOUND;

    r = disc_read_file(d, id->exe_name, &out->exe);
    if (r != Q2_OK)
        return r;

    p = at(&out->exe, Q2_HUD_ADDR_GLYPH_UV, Q2_HUD_GLYPH_COUNT * 2);
    if (!p) goto bad;
    for (i = 0; i < Q2_HUD_GLYPH_COUNT; i++) {
        out->glyph[i].u = p[i * 2 + 0];
        out->glyph[i].v = p[i * 2 + 1];
    }

    p = at(&out->exe, Q2_HUD_ADDR_ICON, Q2_HUD_ICON_COUNT * 4);
    if (!p) goto bad;
    for (i = 0; i < Q2_HUD_ICON_COUNT; i++) {
        out->icon[i].u = p[i * 4 + 0];
        out->icon[i].v = p[i * 4 + 1];
        out->icon[i].w = p[i * 4 + 2];
        out->icon[i].h = p[i * 4 + 3];
    }

    p = at(&out->exe, Q2_HUD_ADDR_BOX_UV, Q2_HUD_BOX_LEVELS * 2);
    if (!p) goto bad;
    for (i = 0; i < Q2_HUD_BOX_LEVELS; i++) {
        out->box[i].u = p[i * 2 + 0];
        out->box[i].v = p[i * 2 + 1];
    }

    p = at(&out->exe, Q2_HUD_ADDR_BOX_RGB, Q2_HUD_BOX_LEVELS * 4);
    if (!p) goto bad;
    for (i = 0; i < Q2_HUD_BOX_LEVELS; i++) {
        out->box_rgb[i][0] = p[i * 4 + 0];
        out->box_rgb[i][1] = p[i * 4 + 1];
        out->box_rgb[i][2] = p[i * 4 + 2];
    }

    p = at(&out->exe, Q2_HUD_ADDR_MSG_LINES, Q2_HUD_MSG_TIERS * 2);
    if (!p) goto bad;
    for (i = 0; i < Q2_HUD_MSG_TIERS; i++)
        out->message_lines[i] = (u8)q2_rd_u16(p + i * 2);

    p = at(&out->exe, Q2_HUD_ADDR_WEAPON_GLYPH, Q2_HUD_WEAPON_SLOTS * 3);
    if (!p) goto bad;
    for (i = 0; i < Q2_HUD_WEAPON_SLOTS; i++) {
        out->weapon_glyph[i][0] = (char)p[i * 3 + 0];
        out->weapon_glyph[i][1] = (char)p[i * 3 + 1];
        out->weapon_glyph[i][2] = (char)p[i * 3 + 2];
        out->weapon_glyph[i][3] = '\0';
    }

    p = at(&out->exe, Q2_HUD_ADDR_WEAPON_NAME, (Q2_HUD_WEAPON_SLOTS - 1) * 12);
    if (!p) goto bad;
    out->weapon_name[0][0] = 0;
    for (i = 1; i < Q2_HUD_WEAPON_SLOTS; i++) {
        memcpy(out->weapon_name[i], p + (i - 1) * 12, 12);
        out->weapon_name[i][12] = 0;
    }

    r = load_palettes(out);
    if (r != Q2_OK) {
        q2_hud_tables_free(out);
        return r;
    }

    return Q2_OK;

bad:
    q2_hud_tables_free(out);
    return Q2_ERR_BAD_FORMAT;
}

void q2_hud_tables_free(q2_hud_tables *t)
{
    if (!t)
        return;
    q2_buf_free(&t->exe);
    memset(t, 0, sizeof(*t));
}

const q2_hud_palette *q2_hud_palette_get(const q2_hud_tables *t, u32 id)
{
    if (!t || id >= Q2_HUD_PALETTE_MAX || !t->palette[id].present)
        return NULL;
    return &t->palette[id];
}

u16 q2_hud_palette_clut(const q2_hud_tables *t, u32 id)
{
    const q2_hud_palette *p = q2_hud_palette_get(t, id);
    return p ? p->clut_id : 0;
}

void q2_hud_palettes_upload(const q2_hud_tables *t, struct psx_vram *vram)
{
    psx_vram *dst = (psx_vram *)vram;
    u32 i;

    if (!t || !dst)
        return;

    /*
     * Record zero is not part of palette[]: the boot loop gives this one
     * exceptional 256-entry palette its own LoadImage at (0, 255) before it
     * walks the sixteen-entry records (0x80076150..0x8007619C). QFRONT's
     * `multipics.lbm` renderer selects CLUT id zero, which is this row, so
     * omitting it left the first ten arena previews sampling untouched VRAM.
     */
    for (i = 0; i < 256; i++)
        dst->px[255][i] = t->palette256[i];

    for (i = 0; i < Q2_HUD_PALETTE_MAX; i++) {
        const q2_hud_palette *p = &t->palette[i];
        int e;

        if (!p->present)
            continue;
        if (p->vram_y < 0 || p->vram_y >= PSX_VRAM_HEIGHT)
            continue;

        for (e = 0; e < Q2_HUD_PALETTE_SIZE; e++) {
            int x = p->vram_x + e;
            if (x < 0 || x >= PSX_VRAM_WIDTH)
                break;
            dst->px[p->vram_y][x] = p->entry[e];
        }
    }
}

static int name_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int q2_hud_weapon_by_model(const q2_hud_tables *t, const char *model_name)
{
    int i;

    if (!t || !model_name)
        return 0;
    for (i = 1; i < Q2_HUD_WEAPON_SLOTS; i++)
        if (name_casecmp(t->weapon_name[i], model_name) == 0)
            return i;
    return 0;
}
