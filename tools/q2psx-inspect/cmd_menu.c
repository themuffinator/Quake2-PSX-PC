/*
 * cmd_menu.c — the menu reconstruction, checked against the disc.
 *
 * `src/menu/pages.c` claims that a particular array of 24-byte records lives at
 * a particular address in the boot executable and says a particular thing. This
 * reads those records back off a real disc and compares them field by field, so
 * "the menu is transcribed correctly" is something the harness evaluates rather
 * than something a comment asserts. A mistyped coordinate or a label off by a
 * character fails the command.
 *
 * The record layout is the one the loader at 0x8001A474 walks:
 *
 *     +0x00 label   +0x04 x   +0x06 y   +0x08 action
 *     +0x0C toggle  +0x10 slider        +0x14 act-on-release
 *
 * Only the fields the port transcribes are compared: the pointers themselves
 * are addresses in a MIPS image and mean nothing here, but *whether* each is
 * null decides the widget, so that is checked too.
 */
#include "cmd_menu.h"
#include "level.h"
#include "levelbin.h"

#include "exe.h"
#include "hudtables.h"
#include "ident.h"
#include "memcard.h"
#include "menu.h"
#include "menudraw.h"
#include "menufont.h"
#include "raster.h"
#include "reloc.h"
#include "vram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC 24u

static bool exe_str(const q2_exe *e, u32 addr, char *out, size_t n)
{
    size_t i;

    if (addr == 0) {
        out[0] = '\0';
        return true;
    }
    for (i = 0; i + 1 < n; i++) {
        u8 c;
        if (!q2_exe_u8(e, addr + (u32)i, &c))
            return false;
        out[i] = (char)c;
        if (c == 0)
            return true;
    }
    out[n - 1] = '\0';
    return true;
}

/* The widget the record's bindings imply, so the port's classification is
 * checked rather than assumed. */
static const char *widget_of(u32 toggle, u32 slider)
{
    if (slider) return "slider";
    if (toggle) return "toggle";
    return "text";
}

static const char *widget_name(u8 w)
{
    switch (w) {
    case Q2_WIDGET_TOGGLE: return "toggle";
    case Q2_WIDGET_SLIDER: return "slider";
    case Q2_WIDGET_CHOICE: return "choice";
    default:               return "text";
    }
}

static const char *page_name(int id)
{
    switch (id) {
    case Q2_PAGE_SCREEN_POSITION:  return "SCREEN POSITION";
    case Q2_PAGE_PAUSE_SP:         return "PAUSE (single player)";
    case Q2_PAGE_OPTIONS:          return "OPTIONS";
    case Q2_PAGE_PLAYER:           return "PLAYER";
    case Q2_PAGE_SOUND:            return "SOUND";
    case Q2_PAGE_VIDEO:            return "VIDEO";
    case Q2_PAGE_CONTROLLER:       return "CONTROLLER";
    case Q2_PAGE_RESTART_CONFIRM:  return "RESTART LEVEL?";
    case Q2_PAGE_RESTARTING:       return "RESTARTING LEVEL";
    case Q2_PAGE_RESUPPLY_CONFIRM: return "RESUPPLY?";
    case Q2_PAGE_RESUPPLYING:      return "RESUPPLYING";
    case Q2_PAGE_QUIT_CONFIRM:     return "QUIT GAME?";
    case Q2_PAGE_QUITTING:         return "QUITTING";
    case Q2_PAGE_NO_CONTROLLER:    return "NO CONTROLLER";
    case Q2_PAGE_DEATH:            return "DEATH";
    case Q2_PAGE_VARIABLES:        return "GAME VARIABLES";
    case Q2_PAGE_PAUSE_MP:         return "PAUSE (multiplayer)";
    case Q2_PAGE_LOADING:          return "LOADING";
    default:                       return "?";
    }
}

/* Records in the table at `addr`; the loader stops at a null label. */
static u32 table_length(const q2_exe *e, u32 addr)
{
    u32 i;

    if (!addr)
        return 0;
    for (i = 0; i < 64; i++) {
        u32 lbl;
        if (!q2_exe_u32(e, addr + i * REC, &lbl))
            break;
        if (lbl == 0)
            break;
    }
    return i;
}

/* Where item `i` lives, given the page's one or two tables. */
static u32 record_addr(const q2_menu_page *p, u32 i)
{
    if (p->addr2 && i >= p->first)
        return p->addr2 + (i - p->first) * REC;
    return p->addr + i * REC;
}

/*
 * Compare one page against the tables it was transcribed from. Returns the
 * number of mismatches and prints each one.
 */
static int check_page(const q2_exe *e, const q2_menu_page *p, bool verbose)
{
    u32 i, bad = 0, na, nb, want_a, want_b;
    char label[64];

    /*
     * A FRONT-END page is not in the executable and checking it there is not a
     * failure — it is the wrong file.
     *
     * The front end is `QFRONT`'s LevelBin (#44), and its pages are static
     * 24-byte arrays in the module at 0x8010xxxx. This checker reads the
     * executable's segment, so every one of those pages came back as six lines
     * of `record escapes the segment` and a `holds 0 records, the port
     * transcribes N` — 26 complaints that said nothing except that the reader
     * was pointed at the wrong place. `menu front` reads them out of the module
     * and prints them against the same transcription.
     */
    if (p->addr >= 0x80100000u && p->addr < 0x80200000u) {
        if (verbose)
            printf("  %-22s %08X  %u item%s  (QFRONT's LevelBin, not the "
                   "EXE - see `menu front`)\n",
                   page_name(p->id), p->addr, p->count,
                   p->count == 1 ? "" : "s");
        return 0;
    }

    na = table_length(e, p->addr);
    nb = table_length(e, p->addr2);

    /* Group A holds everything before `first` when there is a second table,
     * and the whole page when there is not. */
    want_a = (p->addr2 && p->first > 0) ? p->first : p->count;
    want_b = p->count - want_a;

    if (verbose)
        printf("  %-22s %08X  %u item%s\n", page_name(p->id), p->addr,
               p->count, p->count == 1 ? "" : "s");

    for (i = 0; i < p->count; i++) {
        u32 base = record_addr(p, i);
        u32 lbl, act, tog, sld, rel;
        s16 x, y;
        const q2_menu_item *it = &p->items[i];
        const char *want_widget;

        if (!q2_exe_u32(e, base + 0x00, &lbl) ||
            !q2_exe_s16(e, base + 0x04, &x) ||
            !q2_exe_s16(e, base + 0x06, &y) ||
            !q2_exe_u32(e, base + 0x08, &act) ||
            !q2_exe_u32(e, base + 0x0C, &tog) ||
            !q2_exe_u32(e, base + 0x10, &sld) ||
            !q2_exe_u32(e, base + 0x14, &rel)) {
            printf("    ! item %u: record escapes the segment\n", i);
            bad++;
            continue;
        }

        if (!exe_str(e, lbl, label, sizeof(label))) {
            printf("    ! item %u: label pointer %08X is not readable\n", i, lbl);
            bad++;
            continue;
        }

        if (strcmp(label, it->label) != 0) {
            printf("    ! item %u: label \"%s\" on disc, \"%s\" in the port\n",
                   i, label, it->label);
            bad++;
        }
        if (x != it->x || y != it->y) {
            printf("    ! item %u (%s): (%d,%d) on disc, (%d,%d) in the port\n",
                   i, label, x, y, it->x, it->y);
            bad++;
        }
        if ((rel & 1u) != (u32)it->on_release) {
            printf("    ! item %u (%s): fires on %s, the port says %s\n",
                   i, label, (rel & 1u) ? "release" : "press",
                   it->on_release ? "release" : "press");
            bad++;
        }
        /*
         * The death screen is the one page whose records carry no action on
         * disc: its hook installs all three once the 600-tick countdown at
         * 0x800205B0 expires, which is what makes the screen inert for a
         * moment after you die.
         *
         * And `0x8001FD80` is `jr ra; nop`. A record pointing at it HAS an
         * action pointer and DOES nothing, which is not a discrepancy — it is
         * the memory-card front end's design, and it is the same idiom as slot
         * 0 of the weapon array being a shot that does nothing (§13.1). The
         * port models it as no action, correctly, so the comparison has to know
         * the stub rather than count pointers.
         */
        if (act == Q2_MENU_ACTION_NOP)
            act = 0;

        /* SAVE?'s YES enters page 39 and installs the card state machine
         * (memcard.h). That is not a page transition in this engine's sense, so
         * the port carries no action for it and the client opens the front end
         * itself; the pointer is still checked, by identity. */
        if (act == Q2_MCARD_ACTION_ENTER)
            act = 0;

        if (p->id != Q2_PAGE_DEATH &&
            (act != 0) != (it->action != Q2_ACT_NONE)) {
            printf("    ! item %u (%s): %s an action on disc, the port %s\n",
                   i, label, act ? "has" : "has no",
                   it->action ? "has one" : "has none");
            bad++;
        }

        /*
         * The CONTROLLER page is the documented exception: its records carry
         * no bindings because its own hook writes the configuration block
         * directly, so the port's widgets are that hook's effect, not the
         * table's. Everything else must agree.
         */
        want_widget = widget_of(tog, sld);
        if (p->id != Q2_PAGE_CONTROLLER &&
            strcmp(want_widget, widget_name(it->widget)) != 0) {
            printf("    ! item %u (%s): %s on disc, %s in the port\n",
                   i, label, want_widget, widget_name(it->widget));
            bad++;
        }

        if (verbose)
            printf("      %2u  %-26s x=%3d y=%3d  %-6s %s\n", i,
                   label[0] ? label : "(empty)", x, y,
                   widget_name(it->widget),
                   (rel & 1u) ? "on-release" : "");
    }

    /*
     * The port may transcribe fewer records than a table holds when the page
     * excludes some — the single-player pause menu drops its trailing empty
     * record with the `count -= 1` at 0x8001D6F4. Anything else is a gap.
     */
    if (na != want_a && !(p->id == Q2_PAGE_PAUSE_SP && na == want_a + 1u)) {
        printf("    ! %08X holds %u records, the port transcribes %u\n",
               p->addr, na, want_a);
        bad++;
    }
    if (p->addr2 && nb != want_b) {
        printf("    ! %08X holds %u records, the port transcribes %u\n",
               p->addr2, nb, want_b);
        bad++;
    }

    return (int)bad;
}

/* ------------------------------------------------------------------------- */

static void dump_page(const q2_menu_page *p, int cheat_level)
{
    q2_menu_settings set;
    q2_menu m;
    u32 i;

    q2_menu_settings_defaults(&set);
    q2_menu_init(&m, &set, Q2_MENU_SCREEN_H);
    m.cheat_level = cheat_level;
    m.page        = p;
    m.page_id     = p->id;
    m.cursor      = p->first;
    m.open        = true;

    printf("\npage %-2u  %-22s  table %08X\n", p->id, page_name(p->id), p->addr);
    if (p->title)
        printf("  title  \"%s\"  at (256,%d), font 32\n",
               p->title, q2_menu_title_y(Q2_MENU_SCREEN_H));
    if (p->first >= p->count)
        printf("  (nothing on this page is selectable)\n");

    for (i = 0; i < p->count; i++) {
        char line[80];
        const q2_menu_item *it = &p->items[i];

        m.cursor = (int)i;
        q2_menu_item_display(&m, (int)i, line, (u32)sizeof(line));

        printf("  %c%2u  %-30s x=%3d y=%3d  %-6s%s\n",
               (i >= p->first) ? ' ' : '.', i,
               line[0] ? line : "(empty)", it->x, it->y,
               widget_name(it->widget),
               it->on_release ? "  on-release" : "");
    }

    if (p->back != Q2_ACT_NONE)
        printf("  triangle: back\n");
}

/*
 * Dump a 4bpp VRAM page through one of the executable's palettes, with the
 * face's cell grid drawn over it.
 *
 * This is the diagnostic that settles the font: the locator computes a cell
 * rather than reading a table, so a wrong cell size or column count does not
 * fail — it draws legible-looking text out of the wrong halves of the right
 * letters. Seeing the grid land on the letterforms is the check.
 */
static int dump_page_image(const psx_vram *vram, int vram_x, int vram_y,
                           u16 clut_word, const q2_menu_face *grid,
                           const char *out)
{
    const int W = 256, H = 256;
    psx_framebuffer fb;
    int clut_x = (clut_word & 0x3F) * 16;
    int clut_y = (clut_word >> 6) & 0x1FF;
    int y;

    if (psx_fb_init(&fb, W, H) != Q2_OK)
        return 1;

    for (y = 0; y < H; y++) {
        int x;
        if (vram_y + y >= PSX_VRAM_HEIGHT)
            break;
        for (x = 0; x < W; x++) {
            int hw = vram_x + (x >> 2);
            u16 word, entry;
            int nib;

            if (hw >= PSX_VRAM_WIDTH)
                break;
            word = vram->px[vram_y + y][hw];
            nib  = (word >> ((x & 3) * 4)) & 0xF;
            entry = vram->px[clut_y][clut_x + nib];
            fb.px[y * W + x] = entry;
        }
    }

    /* The grid, in a colour the palettes do not contain. */
    if (grid) {
        int c;
        for (c = 0; c * grid->cell_w < W; c++)
            for (y = 0; y < H; y++)
                fb.px[y * W + c * grid->cell_w] = psx_rgb555(255, 0, 255);
        for (c = 0; grid->v_origin + c * grid->cell_h < H; c++) {
            int gy = grid->v_origin + c * grid->cell_h;
            int x;
            for (x = 0; x < W; x++)
                fb.px[gy * W + x] = psx_rgb555(0, 255, 255);
        }
    }

    if (psx_fb_write_ppm(&fb, out) != Q2_OK) {
        psx_fb_free(&fb);
        return 1;
    }
    printf("wrote %s (%dx%d)\n", out, W, H);
    psx_fb_free(&fb);
    return 0;
}

/*
 * Draw a page at the console's own resolution, with the console's own font.
 *
 * The geometry pipeline was brought up the same way — write a PPM, look at it —
 * and a menu is no different: a coordinate that is right in the table can still
 * land in the wrong place once it is drawn. What is new is that this now needs
 * a MAP, because the letterforms are texture data on the disc: `chars.lbm` for
 * the 8-pixel face and `frontend.lbm` for the 16- and 32-pixel ones. Drawing a
 * page is therefore also a check that the atlases are where the executable's
 * slot tables say they are — a wrong VRAM origin does not fail, it produces
 * legible-looking text made of the wrong glyphs.
 */
static int shoot_page(const disc *d, const q2_menu_page *p, int cheat_level,
                      const char *out, const char *map)
{
    const int W = Q2_MENU_SCREEN_W, H = Q2_MENU_SCREEN_H;
    q2_build_id id;
    q2_hud_tables tab;
    q2_vram_section vs;
    q2_menu_font font;
    q2_menu_draw_opts opts;
    psx_framebuffer fb;
    psx_raster_opts ropts;
    psx_vram *vram;
    psx_ot ot;
    q2_menu_settings set;
    q2_menu m;
    q2_result r;
    u32 prims;

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }
    if (q2_hud_tables_load(&tab, d, &id) != Q2_OK) {
        fprintf(stderr, "cannot read the font tables out of %s\n", id.exe_name);
        return 1;
    }
    if (q2_vram_load(&vs, d, map) != Q2_OK) {
        fprintf(stderr, "cannot load %s/SNDVRAM.DAT\n", map);
        q2_hud_tables_free(&tab);
        return 1;
    }

    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        q2_vram_free(&vs);
        q2_hud_tables_free(&tab);
        return 1;
    }

    r = q2_menu_font_upload(&font, &tab, &vs, vram, false, 1);
    q2_vram_free(&vs);
    if (r != Q2_OK) {
        fprintf(stderr, "%s carries neither %s nor %s\n", map,
                Q2_MENU_ATLAS_NAME, Q2_HUD_ATLAS_NAME);
        free(vram);
        q2_hud_tables_free(&tab);
        return 1;
    }

    printf("\nfont from %s: %s%s%s\n", map,
           font.item_resident  ? "frontend.lbm " : "",
           font.small_resident ? "chars.lbm "    : "",
           font.icons_resident ? "and the icon sheet" : "");
    printf("  tpage item=0x%04X small=0x%04X icons=0x%04X\n",
           font.tpage_item, font.tpage_small, font.tpage_icons);
    printf("  clut  %d=0x%04X  %d=0x%04X  %d=0x%04X  %d=0x%04X\n",
           Q2_MENU_PALETTE_TEXT,     font.clut_text,
           Q2_MENU_PALETTE_ITEM_HI,  font.clut_item_hi,
           Q2_MENU_PALETTE_TITLE_HI, font.clut_title_hi,
           Q2_MENU_PALETTE_SMALL,    font.clut_small);

    if (psx_ot_init(&ot, 256, 8192) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        free(vram);
        q2_hud_tables_free(&tab);
        return 1;
    }

    q2_menu_settings_defaults(&set);
    q2_menu_init(&m, &set, Q2_MENU_SCREEN_H);
    m.cheat_level = cheat_level;
    m.open        = true;
    q2_menu_goto(&m, p->id);
    m.page = p;                    /* honour the variant the caller picked */
    if (p->id == Q2_PAGE_DEATH) {
        q2_menu_set_resupplies(&m, 2);
        q2_menu_goto(&m, Q2_PAGE_DEATH);
        m.arm_ticks = 0;
    }
    if (p->id == Q2_PAGE_PAUSE_SP)
        q2_menu_set_stats(&m, 12, 40, 1, 3);

    /*
     * The pages as VRAM holds them. `.item` and `.title` carry the face grid,
     * which is the check that matters: the locator COMPUTES a cell rather than
     * reading a table, so a wrong cell size or column count does not fail — it
     * draws legible-looking text out of the wrong halves of the right letters.
     * `.icons` is the sheet from slot 14, which is not a menu frame; see the
     * note in menufont.h.
     */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s.frontend.ppm", out);
        dump_page_image(vram, Q2_MENU_ATLAS_PAGE_X * 64, 256,
                        font.clut_text, NULL, path);
        snprintf(path, sizeof(path), "%s.item.ppm", out);
        dump_page_image(vram, Q2_MENU_ATLAS_PAGE_X * 64, 256, font.clut_text,
                        q2_menu_face_get(Q2_MENU_FACE_ITEM), path);
        snprintf(path, sizeof(path), "%s.title.ppm", out);
        dump_page_image(vram, Q2_MENU_ATLAS_PAGE_X * 64, 256,
                        font.clut_title_hi,
                        q2_menu_face_get(Q2_MENU_FACE_TITLE), path);
        snprintf(path, sizeof(path), "%s.icons.ppm", out);
        dump_page_image(vram, Q2_MENU_ICONS_PAGE_X * 64, 256,
                        font.clut_text, NULL, path);
    }

    q2_menu_draw_opts_default(&opts, &font);
    /* Nothing behind it here, so give the page something to sit on. The
     * console has the frozen world there instead. */
    psx_fb_clear(&fb, psx_rgb555(16, 16, 40));

    /* The OT here is a bare 256 buckets rather than the screen's sliced 217,
     * so the menu's own bucket is used as the absolute index it is. */
    prims = q2_menu_build_ot(&m, &ot, &opts);

    psx_raster_opts_default(&ropts);
    psx_raster_ot(&fb, &ot, vram, &ropts);

    if (psx_fb_write_ppm(&fb, out) != Q2_OK) {
        fprintf(stderr, "cannot write %s\n", out);
        psx_fb_free(&fb);
        psx_ot_free(&ot);
        free(vram);
        q2_hud_tables_free(&tab);
        return 1;
    }

    printf("wrote %s (%dx%d), %u primitives\n", out, fb.width, fb.height,
           prims);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    q2_hud_tables_free(&tab);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* The FRONT END's pages, which are not in the executable                     */
/* ------------------------------------------------------------------------- */
/*
 * `QFRONT`'s `LevelBin` is the front end (#44). Its pages are static 24-byte
 * record arrays in the module, in the executable's own layout, so this reads
 * them the way the engine's own page walker would — and prints them beside the
 * hand transcription in that entry, which is what makes the decode checkable
 * rather than plausible.
 */
static int module_pages(const disc *d, const char *map)
{
    char path[256];

    q2_common_file cf;
    q2_buf buf;
    const dat_chunk *lb;
    static q2_lb_menu_page pages[64];
    u32 n, i, rows = 0;

    if (!map || !map[0])
        map = "QFRONT";

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK) {
        printf("  %s is not on this disc\n", map);
        return 1;
    }
    if (q2_common_open(&cf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        printf("  %s's COMMON.DAT will not open\n", map);
        return 1;
    }

    lb = cf.chunk[Q2_COMMON_LEVEL_BIN];
    if (!lb || !lb->data || !lb->size) {
        q2_common_close(&cf);
        printf("  %s carries no LevelBin\n", map);
        return 1;
    }

    printf("\nMenu pages in %s's %u-byte LevelBin\n", map, lb->size);

        /* The chunk is UNRELOCATED, so its pointers are still the module-relative
     * offsets the fixups would turn into addresses — the same reason the
     * mission-event scan passes a zero base (levelbin.h). */
    n = q2_levelbin_menu_pages(lb->data, lb->size, 0, pages, 64);
    for (i = 0; i < n && i < 64; i++) {
        u32 k;

        printf("  page at module+0x%05X, %u rows\n", pages[i].offset,
               pages[i].count);
        for (k = 0; k < pages[i].count; k++) {
            const q2_lb_menu_row *r = &pages[i].row[k];

            printf("    +0x%05X  %-18s %4d,%4d  ", r->offset, r->name,
                   r->x, r->y);
            if (r->action)
                printf("-> module+0x%X\n", r->action - 0x80100000u);
            else
                printf("(no action)\n");
        }
        rows += pages[i].count;
    }

    printf("  %u pages, %u rows\n", n, rows);

    /*
     * And the credit roll, which is NOT one of those pages — see levelbin.h.
     * Printed here because "the words are on the disc and the layout is not"
     * is a claim worth being able to check.
     */
    {
        static const char *cred[Q2_LB_CREDITS_MAX];
        u32 nc = q2_levelbin_credits(lb->data, lb->size, cred,
                                     Q2_LB_CREDITS_MAX);

        if (nc) {
            u32 k;

            printf("\n  credit roll: %u lines, none of them a page record\n", nc);
            for (k = 0; k < nc && k < 8; k++)
                printf("    %s\n", cred[k]);
            if (nc > 8)
                printf("    ... and %u more, ending on \"%s\"\n",
                       nc - 8, cred[nc - 1]);
        }
    }

    /*
     * And the SCENE the pages are drawn over, which #44 left open and which is
     * not a scene at all: five item table ids the module hands the engine's own
     * item spawner. Printed with each id's table record, because "the title
     * screen's logo is an item and that is why it spins" is exactly the kind of
     * claim that should fail loudly if the ids ever stop resolving.
     */
    {
        q2_lb_scene scene = { 0 };   /* `got` guards it; MSVC C4701 */
        q2_ai_module mod;
        bool got = false;

        /* The relocated image, not the chunk — see q2_levelbin_scene. */
        if (q2_level_module_load(&mod, &cf, 0x80100000u) == Q2_OK) {
            if (!mod.empty)
                got = q2_levelbin_scene(mod.image.data, (u32)mod.image.size,
                                        mod.base, &scene);
            q2_ai_module_free(&mod);
        }

        if (got) {
            const q2_item_table *t = q2_item_table_builtin();
            u32 k;

            printf("\n  scene: %u objects from module+0x%05X, each spawned at "
                   "(0, 0, %d) facing yaw %d\n",
                   scene.count, scene.offset,
                   Q2_LB_SCENE_DIST, Q2_LB_SCENE_YAW);
            for (k = 0; k < scene.count; k++) {
                const q2_item_def *def = q2_item_find(t, (s32)scene.id[k]);

                printf("    id %2u  %-14s %s\n", scene.id[k],
                       def ? def->model : "(no table record)",
                       (def && (def->flags & Q2_ITEM_SPIN))
                           ? "spin — 0x80059330 turns it"
                           : "does not spin");
            }
        } else {
            printf("\n  scene: none — this module places no objects\n");
        }
    }

    q2_common_close(&cf);
    return 0;
}

int cmd_menu(const disc *d, const char *want, const char *out, const char *map)
{
    /*
     * `menu pages <MAP>` reads a module's own page tables. The front end
     * is QFRONT's (#44) and the deathmatch scoreboard is QMRESULT's
     * (#46) - the same 24-byte records in both, because the engine's page
     * walker takes a module's record and the executable's without knowing
     * the difference.
     */
    if (want && strcmp(want, "front") == 0)
        return module_pages(d, "QFRONT");
    if (want && strcmp(want, "pages") == 0)
        return module_pages(d, map);

    q2_exe exe;
    const q2_menu_page *pages;
    u32 count, i;
    int bad = 0, checked = 0;
    q2_result r;

    pages = q2_menu_pages(&count);

    if (out) {
        const q2_menu_page *p = want ? q2_menu_page_find(atoi(want)) : NULL;

        if (!p) {
            fprintf(stderr, "a page id is needed to draw one; try 26\n");
            return 1;
        }
        /*
         * No size any more: the layout is 512x248 and the glyphs are 4bpp
         * texels, so drawing it at any other size would mean filtering the
         * atlas — which the console cannot do and neither can this pipeline.
         * The fourth argument is now the MAP the font comes off.
         */
        return shoot_page(d, p, 0, out, map ? map : "BASE1");
    }

    /* Dump first: the reconstruction is useful even without a disc to check
     * it against, and the check reads better after the thing it is checking. */
    for (i = 0; i < count; i++) {
        if (want && atoi(want) != (int)pages[i].id)
            continue;
        dump_page(&pages[i], 0);
    }

    /* The two pages with variants, which the id-keyed list holds only once. */
    if (!want || atoi(want) == Q2_PAGE_VARIABLES) {
        int lvl;
        for (lvl = 1; lvl <= 3; lvl++) {
            printf("\n-- GAME VARIABLES with cheats %s --",
                   q2_menu_cheat_level_name(lvl));
            dump_page(q2_menu_variables_page(lvl), lvl);
        }
    }
    if (!want || atoi(want) == Q2_PAGE_VIDEO) {
        printf("\n-- VIDEO in multiplayer --");
        dump_page(q2_menu_video_page(true), 0);
    }

    r = q2_exe_load(&exe, d, NULL);
    if (r != Q2_OK) {
        fprintf(stderr, "\ncannot read the boot executable: %s\n",
                q2_result_str(r));
        return 1;
    }

    printf("\nchecking the transcription against %s\n", exe.name);

    for (i = 0; i < count; i++) {
        bad += check_page(&exe, &pages[i], false);
        checked++;
    }
    for (i = 1; i <= 3; i++) {
        bad += check_page(&exe, q2_menu_variables_page((int)i), false);
        checked++;
    }
    bad += check_page(&exe, q2_menu_video_page(true), false);
    checked++;

    /*
     * The memory-card screens. They are not pages — they carry no page id and
     * are installed directly rather than through 0x8001A384 — but they are
     * built from the same 24-byte records, so the same check applies and the
     * transcription is evidence rather than assertion (memcard.h).
     */
    {
        u32 mc_count;
        const q2_menu_page *mc = q2_mcard_pages(&mc_count);
        u32 k;

        printf("\nthe memory-card front end (%u screens, no page ids)\n",
               mc_count);
        for (k = 0; k < mc_count; k++) {
            printf("  %-16s table %08X%s\n",
                   q2_mcard_screen_name((q2_mcard_screen)(k + 1)),
                   mc[k].addr,
                   mc[k].addr2 ? " + answers" : "");
            bad += check_page(&exe, &mc[k], false);
            checked++;
        }
    }

    printf("%d page%s checked, %d mismatch%s\n",
           checked, checked == 1 ? "" : "s", bad, bad == 1 ? "" : "es");

    q2_exe_free(&exe);
    return bad == 0 ? 0 : 1;
}
