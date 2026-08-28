#include "cmd_hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud.h"
#include "hudtables.h"
#include "icontable.h"
#include "itemtable.h"
#include "statusbar.h"
#include "ident.h"
#include "raster.h"
#include "vram.h"

/* The atlas rows the tables address, named by what is drawn on them. Used only
 * to make the dump readable. */
static const char *atlas_row(u8 v)
{
    switch (v) {
    case 0x80: return "lower case";
    case 0x98: return "words: Player/HyperBlaster/SuperShotgun";
    case 0xA0: return "words: Machine/Chain/Grenade/Launcher/Rail";
    case 0xA8: return "words: Rocket/BFG/was/Ammo/die/door/Sequence";
    case 0xB0: return "digits and punctuation";
    case 0xB8: return "'A'..'P'";
    case 0xC0: return "'Q'..'Z' and punctuation";
    case 0xF0: return "message backdrop tiles";
    case 0x00: return "blank";
    default:   return "";
    }
}

static void dump_tables(const q2_hud_tables *t)
{
    int i;
    unsigned live = 0;

    printf("Glyphs (0x%08X, %d entries, 8x8 sprites, index = c - 32)\n",
           Q2_HUD_ADDR_GLYPH_UV, Q2_HUD_GLYPH_COUNT);
    {
        u8 seen[256];
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < Q2_HUD_GLYPH_COUNT; i++)
            seen[t->glyph[i].v] = 1;
        for (i = 0; i < 256; i++)
            if (seen[i])
                printf("  v=0x%02X  %s\n", i, atlas_row((u8)i));
    }
    printf("  '0'..'9' at u=0x%02X..0x%02X v=0x%02X, 'A' at (0x%02X,0x%02X),"
           " 'a' at (0x%02X,0x%02X)\n",
           t->glyph['0' - 32].u, t->glyph['9' - 32].u, t->glyph['0' - 32].v,
           t->glyph['A' - 32].u, t->glyph['A' - 32].v,
           t->glyph['a' - 32].u, t->glyph['a' - 32].v);

    printf("\nIcons (0x%08X, %d entries) — pre-rendered words, deliberately"
           " overlapping\n", Q2_HUD_ADDR_ICON, Q2_HUD_ICON_COUNT);
    for (i = 0; i < Q2_HUD_ICON_COUNT; i++)
        printf("  %2d  u=0x%02X..0x%02X  v=0x%02X  %2dx%-2d\n", i,
               t->icon[i].u, (u8)(t->icon[i].u + t->icon[i].w), t->icon[i].v,
               t->icon[i].w, t->icon[i].h);

    printf("\nIcon escapes (jump table 0x800ACA1C, 'A'..'W')\n  ");
    for (i = 'A'; i <= 'W'; i++) {
        u8 ic[Q2_HUD_ICON_ESCAPE_MAX];
        int n = q2_hud_icon_escape((char)i, ic);
        if (n == 0)
            printf("&%c:-  ", i);
        else if (n == 1)
            printf("&%c:%d  ", i, ic[0]);
        else
            printf("&%c:%d+%d  ", i, ic[0], ic[1]);
    }
    printf("\n");

    printf("\nWeapon glyphs (0x%08X, 1-based; slot 0 is blank by design)\n",
           Q2_HUD_ADDR_WEAPON_GLYPH);
    for (i = 0; i < Q2_HUD_WEAPON_SLOTS; i++)
        printf("  %2d  \"%s\"\n", i, t->weapon_glyph[i]);

    printf("\nBackdrop tiles (0x%08X / 0x%08X)\n",
           Q2_HUD_ADDR_BOX_UV, Q2_HUD_ADDR_BOX_RGB);
    for (i = 0; i < Q2_HUD_BOX_LEVELS; i++)
        printf("  *%d  uv=(0x%02X,0x%02X)  rgb=(%3u,%3u,%3u)\n", i,
               t->box[i].u, t->box[i].v,
               t->box_rgb[i][0], t->box_rgb[i][1], t->box_rgb[i][2]);

    printf("\nNotification lines by player count (0x%08X)\n  ",
           Q2_HUD_ADDR_MSG_LINES);
    for (i = 0; i < Q2_HUD_MSG_TIERS; i++)
        printf("%d:%u  ", i, t->message_lines[i]);
    printf("\n");

    for (i = 0; i < Q2_HUD_PALETTE_MAX; i++)
        if (t->palette[i].present)
            live++;
    printf("\nBuilt-in palettes (0x%08X): %u of %u slots, plus the 256-entry"
           " block at (0,255)\n", Q2_HUD_ADDR_PALETTES, live,
           t->palette_count);
    {
        static const struct { u32 id; const char *what; } named[] = {
            { Q2_HUD_PALETTE_MASK,     "menu mask"          },
            { Q2_HUD_PALETTE_FONT,     "font `|0`, crosshair" },
            { Q2_HUD_PALETTE_FONT_ALT, "font `|1`, default" },
            { Q2_HUD_PALETTE_BOX,      "backdrop ramp"      }
        };
        size_t k;
        for (k = 0; k < sizeof(named) / sizeof(named[0]); k++) {
            const q2_hud_palette *p = q2_hud_palette_get(t, named[k].id);
            int e;
            if (!p)
                continue;
            printf("  %3u  vram=(%3d,%3d) clut=0x%04X stp=0x%04X  %-20s ",
                   named[k].id, p->vram_x, p->vram_y, p->clut_id, p->stp_mask,
                   named[k].what);
            for (e = 0; e < 4; e++)
                printf("%04X ", p->entry[e]);
            printf("...\n");
        }
    }
}

/* ------------------------------------------------------------------------- */
/*
 * The status bar's data, and the check that it is a grid.
 *
 * This exists because §11.1 of FORMATS.md said for a long time that there was
 * no status bar. There is; the enumeration that concluded otherwise looked only
 * at text, and the bar is sprites. Printing the table with its regularity
 * measured is the cheapest way to keep that from being re-forgotten.
 */
static int dump_icons(const disc *d, const q2_build_id *id)
{
    q2_icon_tables it;
    u32 i, on_grid = 0, blank = 0;
    int rows_seen[16];
    int nrows = 0;

    if (q2_icon_tables_load(&it, d, id) != Q2_OK) {
        printf("\nstatus-bar tables: not catalogued for this build\n");
        return 0;
    }

    memset(rows_seen, 0, sizeof(rows_seen));

    printf("\nThe status bar — RETRACTION of \"there is no status bar\"\n");
    printf("Health, ammo and armour are drawn as SPRITES, not text, so the"
           " format-string sweep\n"
           "that produced the old finding could not have seen them. See"
           " FORMATS.md 11.1.\n\n");

    printf("Icon rects (0x%08X, %u entries, 5 bytes each)\n",
           Q2_ICON_ADDR_RECTS, it.rect_count);

    for (i = 0; i < it.rect_count; i++) {
        const q2_icon_rect *r = &it.rect[i];

        if (r->w == 1 && r->h == 1) {
            blank++;
            continue;
        }
        if (q2_icon_on_grid(r)) {
            int row = r->v / Q2_ICON_CELL_H;
            on_grid++;
            if (row >= 0 && row < 16 && !rows_seen[row]) {
                rows_seen[row] = 1;
                nrows++;
            }
        } else {
            printf("  %2u  (%3u,%3u) %ux%u  OFF GRID\n", i, r->u, r->v,
                   r->w, r->h);
        }
    }

    printf("  %u of %u are %ux%u cells on an %u-wide grid, %u rows;"
           " %u are the 1x1 blank\n",
           on_grid, it.rect_count, Q2_ICON_CELL_W, Q2_ICON_CELL_H,
           Q2_ICON_PER_ROW, (unsigned)nrows, blank);
    printf("  the table ends at 0x%08X; what follows is not rectangles\n",
           Q2_ICON_ADDR_AFTER);

    /*
     * The rect is selected by INDEX. Its fifth byte is the palette index the
     * sprite is drawn with; all three status-bar sub-draws copy it into field
     * byte +8 and none scans the table (icontable.h). Item names still line up
     * for pickup captions because that path deliberately uses the item's
     * effect as the rect index, which is a different fact.
     */
    {
        q2_item_table itm;
        bool have = (q2_item_table_load(&itm, d, id) == Q2_OK);
        u32 named = 0, k;

        printf("\nIcon rects - selected by index; fifth byte is a palette\n");
        for (i = 0; i < it.rect_count; i++) {
            const char *what = NULL;

            if (have && i)
                for (k = 0; k < itm.count; k++)
                    if (itm.def[k].effect == i) {
                        what = itm.def[k].model;
                        break;
                    }
            if (what)
                named++;
            else
                printf("  rect %2u  palette %3u  (no item uses this rect)\n",
                       i, it.rect[i].id);
        }
        printf("  %u of %u rect indices are also item effect ids\n",
               named, it.rect_count);

        printf("\nWeapon -> ammo (0x%08X, RECT INDICES, consumed 1-based)\n",
               Q2_ICON_ADDR_AMMO_ICON);
        for (i = 0; i < Q2_ICON_WEAPONS; i++) {
            const char *what = "(none)";

            if (have && it.ammo_icon[i])
                for (k = 0; k < itm.count; k++)
                    if (itm.def[k].effect == it.ammo_icon[i]) {
                        what = itm.def[k].model;
                        break;
                    }
            printf("  weapon %2u -> rect %3u -> %-14s%s\n", i,
                   it.ammo_icon[i], what, i == 0 ? "(no weapon)" : "");
        }
        printf("  the executable multiplies each entry by the five-byte rect\n"
               "  stride before adding the rect-table base (0x80035374).\n");
    }

    printf("\nSplit-screen size (0x800353C8): 1P %ux%u, 2P %ux%u, 3P+ %ux%u\n",
           q2_icon_draw_size(1, 1).w, q2_icon_draw_size(1, 1).h,
           q2_icon_draw_size(2, 1).w, q2_icon_draw_size(2, 1).h,
           q2_icon_draw_size(4, 1).w, q2_icon_draw_size(4, 1).h);

    printf("\nThe numerals (0x%08X): %d digits, then minus and blank;"
           " %dx%d at v=%d, u = %d * digit\n",
           Q2_SBAR_DIGIT_ADDR, Q2_SBAR_DIGITS, Q2_SBAR_DIGIT_W,
           Q2_SBAR_DIGIT_H, Q2_SBAR_DIGIT_V, Q2_SBAR_DIGIT_PITCH);

    printf("\nWhere it is drawn: 0x800337D0, the PER-VIEWPORT draw hook the\n"
           "layout stores at view+308 - not a screen. Anchored at view+304 /\n"
           "view+306, which screen.h carried as pad_a/pad_b, and which the\n"
           "one-player layout sets to (93, 201).\n");
    {
        int c;
        static const char *cname[] = { "health", "ammo", "armour" };
        for (c = 0; c < Q2_SBAR_COUNTERS; c++) {
            int d0 = q2_sbar_digit_field((q2_sbar_counter)c, 0);
            int d1 = q2_sbar_digit_field((q2_sbar_counter)c, 1);
            int d2 = q2_sbar_digit_field((q2_sbar_counter)c, 2);
            int ic = q2_sbar_icon_field((q2_sbar_counter)c);
            printf("  %-7s digits x %+4d %+4d %+4d   icon x %+4d\n", cname[c],
                   q2_sbar_fields[d0].dx, q2_sbar_fields[d1].dx,
                   q2_sbar_fields[d2].dx, q2_sbar_fields[ic].dx);
        }
    }
    printf("  the call sites prove this order: 0x80035178 / 0x800352C0 /"
           " 0x80035554\n");

    printf("\nInstalled split hooks (all per viewport)\n"
           "  0x80033D30  two stacked:      16 fields, frags 13..15\n"
           "  0x80034288  two side-by-side: 16 fields, armour/frags at"
           " y + 40 - screen_h\n"
           "  0x80034830  quad:             11 fields from x[view*11]"
           " and y[view]\n");
    for (i = 0; i < Q2_SBAR_QUAD_VIEWS; i++)
        printf("    quad view %u  y=%d  health icon x=%d  ammo icon x=%d"
               "  frags x=%d/%d/%d\n", i,
               q2_sbar_fields_quad[i][0].dy,
               q2_sbar_fields_quad[i][0].dx,
               q2_sbar_fields_quad[i][4].dx,
               q2_sbar_fields_quad[i][8].dx,
               q2_sbar_fields_quad[i][9].dx,
               q2_sbar_fields_quad[i][10].dx);

    /*
     * "which rect is which item" is ANSWERED, and not through the fifth byte.
     * The pickup caption's sub-draw at 0x800359C0 indexes both the rect table
     * and the 57-name table with the same `client+84` — the effect the touch
     * dispatch stored — so for an item the rect index IS the effect id. The
     * fifth byte remains a palette index (icontable.h).
     */
    printf("\nRect index == item effect id (0x80035A58 / 0x80035B10), so\n"
           "`q2psx-inspect items` names every icon in its caption column.\n"
           "STILL open: the one-player auxiliary icon at +330"
           " (0x80037CAC).\n");

    q2_icon_tables_free(&it);
    return (on_grid + blank == it.rect_count) ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Check the atlas is where the executable says, on every map that has one.   */
static int check_atlas(const disc *d)
{
    int i, n = disc_file_count(d);
    char current[64];
    int with = 0, without = 0, bad = 0;

    current[0] = '\0';

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char map[64];
        size_t len;
        q2_vram_section vs;
        u32 index;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;
        len = (size_t)(slash - rest);
        if (len >= sizeof(map))
            len = sizeof(map) - 1;
        memcpy(map, rest, len);
        map[len] = '\0';

        if (strcmp(map, current) == 0)
            continue;
        strncpy(current, map, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';

        if (q2_vram_load(&vs, d, map) != Q2_OK)
            continue;

        if (!q2_vram_find_by_name(&vs, Q2_HUD_ATLAS_NAME, &index)) {
            without++;
        } else {
            /* 4bpp: bytes-per-row * 2 must cover the widest u the glyph and
             * icon tables reach, and the rows must cover v 0x80..0xFF. */
            u32 texels = (u32)vs.images[index].width * 2u;
            u32 rows   = vs.images[index].height;

            if (texels < 256 || rows < 128) {
                printf("  %-10s chars.lbm is %ux%u texels — too small\n",
                       map, texels, rows);
                bad++;
            } else {
                with++;
            }
        }
        q2_vram_free(&vs);
    }

    printf("\nAtlas: %d maps carry chars.lbm at 256x128 4bpp, %d do not,"
           " %d are the wrong size\n", with, without, bad);
    printf("It uploads to VRAM (%d,%d) — page (%d,%d), v origin %d — from"
           " slot 15 at 0x8003FEA4\n",
           Q2_HUD_ATLAS_VRAM_X, Q2_HUD_ATLAS_VRAM_Y,
           Q2_HUD_ATLAS_PAGE_X, Q2_HUD_ATLAS_PAGE_Y, 128);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Draw the overlay, so the binding can be looked at rather than asserted.    */
static int render_overlay(const disc *d, const q2_hud_tables *tab,
                          const char *map, const char *out_path)
{
    const int W = Q2_HUD_SPACE_W, H = Q2_HUD_SPACE_H;
    q2_vram_section vs;
    psx_vram *vram;
    psx_ot ot;
    psx_framebuffer fb;
    psx_raster_opts opts;
    q2_hud_font font;
    q2_hud_ctx ctx;
    q2_hud hud;
    q2_result r;
    int y;

    if (q2_vram_load(&vs, d, map) != Q2_OK) {
        fprintf(stderr, "cannot load %s/SNDVRAM.DAT\n", map);
        return 1;
    }

    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        q2_vram_free(&vs);
        return 1;
    }

    r = q2_hud_font_upload(&font, tab, &vs, vram);
    q2_vram_free(&vs);
    if (r != Q2_OK) {
        fprintf(stderr, "%s has no %s (%s)\n", map, Q2_HUD_ATLAS_NAME,
                q2_result_str(r));
        free(vram);
        return 1;
    }
    printf("\nFont resident: tpage 0x%04X, cluts |0=0x%04X |1=0x%04X"
           " box=0x%04X\n", font.tpage, font.clut_font, font.clut_alt,
           font.clut_box);

    if (psx_ot_init(&ot, 8, 4096) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        free(vram);
        return 1;
    }

    q2_hud_ctx_default(&ctx, W, H);
    q2_hud_init(&hud, tab, 1);

    /* Everything the overlay can show, at once. */
    q2_hud_pickup(&hud, "Rocket Launcher");
    q2_hud_weapon_selected(&hud, tab, 8);      /* &O — Rocket Launcher */
    q2_hud_need_key(&hud, "Blue Key");
    q2_hud_message(&hud, "^C8F000|0Kills 12/40    3/5 Secrets");
    q2_hud_centre(&hud, tab, &ctx, "Mission Objective Complete");
    q2_hud_track(&hud, 100, 50);
    q2_hud_track(&hud, 88, 50);                /* a health hit: red flash */

    /* A backdrop that is not flat, so transparency is visible. */
    for (y = 0; y < H; y++) {
        int x;
        for (x = 0; x < W; x++)
            fb.px[y * W + x] = psx_rgb555((u8)(x / 4), (u8)(y / 2), 60);
    }

    psx_raster_opts_default(&opts);
    q2_hud_build_ot(&hud, &font, &ctx, &ot, 0);
    psx_raster_ot(&fb, &ot, vram, &opts);

    if (psx_fb_write_ppm(&fb, out_path) == Q2_OK)
        printf("wrote %s\n", out_path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    return 0;
}

/* ------------------------------------------------------------------------- */
int cmd_hud(const disc *d, const char *map, const char *out_path)
{
    q2_build_id id;
    q2_hud_tables tab;
    q2_result r;
    int rc = 0;

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }

    r = q2_hud_tables_load(&tab, d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the HUD tables: %s\n", q2_result_str(r));
        return 1;
    }

    printf("HUD tables from %s\n\n", id.exe_name);
    printf("Two subsystems, both on screen at once.\n\n"
           "The OVERLAY is text: notifications, the centre line, the crosshair"
           " and the damage\n"
           "flash, all drawn through the markup layer out of chars.lbm. Nothing"
           " it formats is\n"
           "a player statistic — that sweep was exhaustive and still holds.\n\n"
           "The STATUS BAR is sprites, and it is why that sweep produced a"
           " wrong conclusion\n"
           "for so long: health, ammo and armour are pre-rendered numerals, so"
           " enumerating\n"
           "format strings could never have found them. See FORMATS.md 11.1.\n\n");

    dump_tables(&tab);

    if (!map) {
        rc = dump_icons(d, &id);
        rc |= check_atlas(d);
    } else {
        rc = render_overlay(d, &tab, map, out_path ? out_path : "hud.ppm");
    }

    q2_hud_tables_free(&tab);
    return rc;
}
