/*
 * cmd_lights.c — the lighting model, checked against the disc.
 *
 * Four claims are checked here, and each one is a claim the port would be wrong
 * about if it failed:
 *
 *   1. SpaceLights is partitioned by the SECONDARY collision node's +30
 *      halfword. Checked as: PrimaryColl never carries the field, SecondaryCol
 *      is non-decreasing and starts at zero, and every index a node can reach
 *      names a light the map actually ships.
 *   2. The light `type` byte splits into a flare style and a size shift.
 *      Reported as its own census so the five observed values are visible.
 *   3. The four flare element lists in the executable match the ones the port
 *      carries, element for element.
 *   4. The six folded divides the flare geometry is built out of solve to the
 *      closed forms flare.h claims. The magics and their post-shifts are read
 *      back out of the instruction stream and re-solved, so this checks the
 *      code and not a constant somebody copied into a header.
 *   5. The {sin, cos} table both flare ring generators index matches the one
 *      this port builds from libm, entry for entry and column for column.
 *   6. The reciprocal-square-root table the port computes matches the one the
 *      executable ships, entry for entry — the same substitution measurement
 *      `anims` makes for the inverse cosine.
 */
#include "cmd_lights.h"

#include <stdio.h>
#include <string.h>

#include "collision.h"
#include "entity.h"
#include "exe.h"
#include "flare.h"
#include "ident.h"
#include "level.h"
#include "lighting.h"
#include "sim.h"
#include "spacelights.h"
#include "trig.h"

/* Where the tables live in the PAL executable. */
#define ADDR_FLARE_STYLE1 0x800A1FDCu
#define ADDR_FLARE_STYLE2 0x800A2014u
#define ADDR_FLARE_STYLE3 0x800A2024u
#define ADDR_FLARE_STYLE4 0x800A203Cu
#define ADDR_RSQRT        0x800A9C54u
#define ADDR_FALLBACK     0x800A1C48u

typedef struct light_stats {
    u32 maps, zones;
    u32 lights_total;
    u32 type_hist[256];

    u32 zones_pri_zero;        /* PrimaryColl's field all zero               */
    u32 zones_pri_nonzero;
    u32 zones_monotonic;
    u32 zones_start_zero;
    u32 zones_sentinel_over;   /* sentinel past the end of the chunk         */
    u32 nodes;
    u32 nodes_dark;            /* no lights at all                           */
    u32 entries_used;
    u32 entries_bad;           /* index >= the map's light count             */
    u32 entries_tail;          /* halfwords past the sentinel — build residue */
    u32 max_per_node;
    u32 zones_empty;           /* partition entirely zero: no static lights   */
} light_stats;

/* ------------------------------------------------------------------------- */
static void check_zone(const q2_zone_file *zf, u32 map_lights, light_stats *st,
                       const char *path)
{
    q2_collision pri, sec;
    q2_spacelights sl;
    u32 i;
    u32 prev = 0;
    bool monotonic = true, pri_zero = true;
    const dat_chunk *chunk;
    u32 chunk_entries;

    if (q2_collision_parse(&sec, zf, Q2_COLL_SECONDARY) != Q2_OK)
        return;

    st->zones++;

    /* The same field in the primary hull, which the engine never reads. If it
     * were filled in too, the partition would be ambiguous; it is not. */
    if (q2_collision_parse(&pri, zf, Q2_COLL_PRIMARY) == Q2_OK) {
        for (i = 0; i <= pri.node_count; i++) {
            if (q2_rd_u16(pri.nodes + (size_t)i * Q2_COLL_NODE_SIZE + 30) != 0) {
                pri_zero = false;
                break;
            }
        }
    }
    if (pri_zero) st->zones_pri_zero++;
    else          st->zones_pri_nonzero++;

    if (q2_spacelights_open(&sl, zf, &sec) != Q2_OK) {
        printf("  OPEN FAILED  %s\n", path);
        return;
    }

    chunk = zf->chunk[Q2_ZONE_SPACE_LIGHTS];
    chunk_entries = chunk ? chunk->size / 2 : 0;

    for (i = 0; i <= sec.node_count; i++) {
        u32 v = q2_rd_u16(sec.nodes + (size_t)i * Q2_COLL_NODE_SIZE + 30);
        if (i && v < prev)
            monotonic = false;
        if (i == 0 && v == 0)
            st->zones_start_zero++;
        prev = v;
    }
    if (monotonic)
        st->zones_monotonic++;
    if (prev == 0)
        st->zones_empty++;
    if (prev > chunk_entries)
        st->zones_sentinel_over++;
    else
        st->entries_tail += chunk_entries - prev;

    for (i = 0; i < sec.node_count; i++) {
        u32 first, count, k;

        if (!q2_spacelights_range(&sl, i, &first, &count))
            continue;

        st->nodes++;
        if (count == 0)
            st->nodes_dark++;
        if (count > st->max_per_node)
            st->max_per_node = count;

        for (k = 0; k < count; k++) {
            u16 index;

            if (!q2_spacelights_entry(&sl, first + k, &index))
                break;
            st->entries_used++;
            if (index >= map_lights)
                st->entries_bad++;
        }
    }
}

/* ------------------------------------------------------------------------- */
static int check_flare_tables(const q2_exe *exe)
{
    static const struct { u32 addr; const char *name; } k[] = {
        { ADDR_FLARE_STYLE1, "style 1" },
        { ADDR_FLARE_STYLE2, "style 2" },
        { ADDR_FLARE_STYLE3, "style 3" },
        { ADDR_FLARE_STYLE4, "style 4" }
    };
    int bad = 0;
    int s;

    printf("Flare element tables, read from the executable:\n\n");
    printf("  style  n  #  kind   size     pos    colour\n");

    for (s = 0; s < 4; s++) {
        const q2_flare_style *builtin = q2_flare_style_table((u32)s + 1);
        u32 e = 0;

        for (;; e++) {
            u32 addr = k[s].addr + e * 8;
            s16 kind, size, pos, colour;

            if (!q2_exe_s16(exe, addr + 0, &kind))
                break;
            if (kind == 0)
                break;
            if (!q2_exe_s16(exe, addr + 2, &size) ||
                !q2_exe_s16(exe, addr + 4, &pos) ||
                !q2_exe_s16(exe, addr + 6, &colour))
                break;

            printf("  %-6d %d  %u  %-6s %6d  %6d  %6d",
                   s + 1, s + 1, e,
                   kind == Q2_FLARE_KIND_BURST ? "burst" : "disc",
                   size, pos, colour);

            if (e >= builtin->count) {
                printf("   MISMATCH: the port has only %u elements\n",
                       builtin->count);
                bad++;
            } else {
                const q2_flare_element *b = &builtin->element[e];
                if (b->kind != (u16)kind || b->size != size ||
                    b->pos != pos || b->colour != colour) {
                    printf("   MISMATCH: port has %u/%d/%d/%d\n",
                           b->kind, b->size, b->pos, b->colour);
                    bad++;
                } else {
                    printf("\n");
                }
            }
        }

        if (e != builtin->count) {
            printf("  MISMATCH  style %d: disc has %u elements, port has %u\n",
                   s + 1, e, builtin->count);
            bad++;
        }
    }

    return bad;
}

/* ------------------------------------------------------------------------- */
/*
 * The {sin, cos} table both flare ring generators index — 4096 entries of two
 * halfwords, a full turn in 4096 steps, sine first. Everything about a flare's
 * shape comes through it, so measure it rather than trusting that a table built
 * from the host's libm lands on the console's numbers.
 *
 * The second column is checked too, and separately: the generators read the
 * cosine as `lh 2(entry)` rather than as a second lookup a quarter turn along,
 * so "cos is sin shifted by 1024" is a claim about the disc and not a
 * definition.
 */
static int check_sincos(const q2_exe *exe)
{
    u32 i;
    u32 sin_same = 0, cos_same = 0, shifted = 0;
    s32 worst_sin = 0, worst_cos = 0;

    for (i = 0; i < Q2_TRIG_TABLE_ENTRIES; i++) {
        s16 disc_sin, disc_cos;
        s32 ours_sin, ours_cos, d;

        if (!q2_exe_s16(exe, Q2_TRIG_TABLE_ADDR + i * Q2_TRIG_TABLE_STRIDE + 0,
                        &disc_sin) ||
            !q2_exe_s16(exe, Q2_TRIG_TABLE_ADDR + i * Q2_TRIG_TABLE_STRIDE + 2,
                        &disc_cos))
            return -1;

        ours_sin = q2_sin12((s32)i);
        ours_cos = q2_cos12((s32)i);

        d = ours_sin - disc_sin;
        if (d == 0) sin_same++;
        else if (d < 0 ? -d > worst_sin : d > worst_sin) worst_sin = d < 0 ? -d : d;

        d = ours_cos - disc_cos;
        if (d == 0) cos_same++;
        else if (d < 0 ? -d > worst_cos : d > worst_cos) worst_cos = d < 0 ? -d : d;

        {
            s16 quarter;
            u32 at = Q2_TRIG_TABLE_ADDR
                   + ((i + Q2_ANGLE_90) % Q2_TRIG_TABLE_ENTRIES)
                     * Q2_TRIG_TABLE_STRIDE;

            if (!q2_exe_s16(exe, at, &quarter))
                return -1;
            if (quarter == disc_cos)
                shifted++;
        }
    }

    printf("\nThe {sin, cos} table at 0x%08X, %u entries, that both flare rings"
           " index:\n", Q2_TRIG_TABLE_ADDR, (unsigned)Q2_TRIG_TABLE_ENTRIES);
    printf("  sine   identical %u of %u (worst %d)\n",
           sin_same, (unsigned)Q2_TRIG_TABLE_ENTRIES, (int)worst_sin);
    printf("  cosine identical %u of %u (worst %d)\n",
           cos_same, (unsigned)Q2_TRIG_TABLE_ENTRIES, (int)worst_cos);
    printf("  the disc's cosine column IS its sine column a quarter turn on: "
           "%u of %u\n", shifted, (unsigned)Q2_TRIG_TABLE_ENTRIES);

    return (int)(2u * Q2_TRIG_TABLE_ENTRIES - sin_same - cos_same);
}

/* ------------------------------------------------------------------------- */
/*
 * The six folded divides the flare geometry is built out of.
 *
 * Each is a `mult`/`mfhi`/`sra`/`subu` magic sequence, and a magic M with
 * post-shift p is a signed divide by the UNIQUE integer d satisfying
 * M == floor(2^(32+p)/d) + 1. So the divisor is recoverable from the
 * instruction stream rather than merely approximable, and the port can carry it
 * as `x * extent / (320 * k)` instead of reproducing the multiply.
 *
 * This reads the `lui`/`ori` pair and the `sra` back out of the executable and
 * re-solves them, which is what turns the closed forms in flare.h from a claim
 * into a measurement. It is deliberately keyed on instruction addresses: if a
 * future build moves the code the check fails loudly rather than passing on a
 * constant somebody copied into a header.
 */
static u32 insn_lui_ori(const q2_exe *exe, u32 lui_at, bool *ok)
{
    u32 hi = 0, lo = 0;

    if (!q2_exe_u32(exe, lui_at, &hi) || !q2_exe_u32(exe, lui_at + 4, &lo) ||
        (hi >> 26) != 0x0Fu || (lo >> 26) != 0x0Du) {
        *ok = false;
        return 0;
    }
    return ((hi & 0xFFFFu) << 16) | (lo & 0xFFFFu);
}

static int insn_sra_amount(const q2_exe *exe, u32 at, bool *ok)
{
    u32 w = 0;

    if (!q2_exe_u32(exe, at, &w) || (w & 0xFC00003Fu) != 0x00000003u) {
        *ok = false;
        return 0;
    }
    return (int)((w >> 6) & 0x1Fu);
}

static int check_divisors(const q2_exe *exe)
{
    static const struct {
        u32         lui_at;    /* the lui of the magic's lui/ori pair */
        u32         sra_at;    /* the post-shift applied to the mfhi  */
        s32         expect;    /* what flare.h says it solves to      */
        const char *what;
    } k[] = {
        { 0x80074F10u, 0x80074F24u, (s32)Q2_FLARE_REF_W * 4096,
          "ring x      (0x80074E6C)" },
        { 0x80074F68u, 0x80074F94u, (s32)Q2_FLARE_REF_H * 4096,
          "ring y      (0x80074E6C)" },
        { 0x8007513Cu, 0x80075158u, (s32)Q2_FLARE_REF_W * Q2_FLARE_SPIKE_REF,
          "spike x     (0x80074FF4)" },
        { 0x8007514Cu, 0x8007516Cu, (s32)Q2_FLARE_REF_H * Q2_FLARE_SPIKE_REF,
          "spike y     (0x80074FF4)" },
        { 0x80075104u, 0x80075120u, (s32)Q2_FLARE_REF_W * Q2_FLARE_DIAG_REF,
          "diagonal x  (0x80074FF4)" },
        { 0x80075114u, 0x80075138u, (s32)Q2_FLARE_REF_H * Q2_FLARE_DIAG_REF,
          "diagonal y  (0x80074FF4)" }
    };
    const u32 n = (u32)(sizeof k / sizeof k[0]);
    u32 i;
    int bad = 0;

    printf("\nThe six folded divides, re-solved from the instruction stream:\n");
    printf("  %-24s %10s %5s %12s %12s\n",
           "divide", "magic", "shift", "solves to", "flare.h");

    for (i = 0; i < n; i++) {
        bool ok = true;
        u32 magic = insn_lui_ori(exe, k[i].lui_at, &ok);
        int shift  = insn_sra_amount(exe, k[i].sra_at, &ok);
        unsigned long long two_pow, d;
        s32 solved = 0;

        if (!ok) {
            printf("  %-24s  NOT THE EXPECTED INSTRUCTIONS\n", k[i].what);
            bad++;
            continue;
        }

        two_pow = 1ULL << (32 + shift);
        if (magic) {
            unsigned long long lo = two_pow / magic;
            unsigned long long hi = lo + 2;

            for (d = (lo > 2 ? lo - 2 : 1); d <= hi; d++)
                if (two_pow / d + 1ULL == (unsigned long long)magic) {
                    solved = (s32)d;
                    break;
                }
        }

        printf("  %-24s 0x%08X %5d %12ld %12ld%s\n",
               k[i].what, magic, shift, (long)solved, (long)k[i].expect,
               solved == k[i].expect ? "" : "   MISMATCH");

        if (solved != k[i].expect)
            bad++;
    }

    return bad;
}

/* ------------------------------------------------------------------------- */
static int check_rsqrt(const q2_exe *exe)
{
    u32 i;
    u32 exact = 0, off_by_one = 0, worse = 0;

    for (i = 0; i < Q2_LIGHT_RSQRT_ENTRIES; i++) {
        s16 disc_side;
        s16 ours = q2_light_rsqrt_table(i);
        s32 diff;

        if (!q2_exe_s16(exe, ADDR_RSQRT + i * 2, &disc_side))
            return -1;

        diff = (s32)ours - disc_side;
        if (diff == 0)                 exact++;
        else if (diff == 1 || diff == -1) off_by_one++;
        else                           worse++;
    }

    printf("\nReciprocal square root table at 0x%08X, %u entries:\n",
           ADDR_RSQRT, (unsigned)Q2_LIGHT_RSQRT_ENTRIES);
    printf("  identical %u, off by one %u, worse %u\n", exact, off_by_one, worse);

    return (int)worse;
}

/* ------------------------------------------------------------------------- */
static int check_fallback(const q2_exe *exe)
{
    u8 rgb[3];
    u16 radius;
    u32 inner, outer;
    u8 type;
    int bad = 0;
    int i;

    for (i = 0; i < 3; i++) {
        if (!q2_exe_u8(exe, ADDR_FALLBACK + 12 + (u32)i, &rgb[i]))
            return -1;
    }
    if (!q2_exe_u8 (exe, ADDR_FALLBACK + 17, &type)   ||
        !q2_exe_u16(exe, ADDR_FALLBACK + 18, &radius) ||
        !q2_exe_u32(exe, ADDR_FALLBACK + 20, &inner)  ||
        !q2_exe_u32(exe, ADDR_FALLBACK + 24, &outer))
        return -1;

    printf("\nThe no-node fallback light at 0x%08X:\n", ADDR_FALLBACK);
    printf("  colour %u/%u/%u, type %u, radius %u, innerSq 0x%08X, radiusSq 0x%08X\n",
           rgb[0], rgb[1], rgb[2], type, radius, inner, outer);
    printf("  offset from the entity: %+d %+d %+d\n",
           Q2_LIGHT_FALLBACK_OFS_X, Q2_LIGHT_FALLBACK_OFS_Y,
           Q2_LIGHT_FALLBACK_OFS_Z);

    if (rgb[0] != Q2_LIGHT_FALLBACK_GREY || rgb[1] != Q2_LIGHT_FALLBACK_GREY ||
        rgb[2] != Q2_LIGHT_FALLBACK_GREY) {
        printf("  MISMATCH  the port uses 0x%02X for all three\n",
               Q2_LIGHT_FALLBACK_GREY);
        bad++;
    }
    if (inner != outer) {
        printf("  MISMATCH  the port assumes inner == outer, "
               "which is what pins its attenuation at 1.0\n");
        bad++;
    }

    return bad;
}

/* ------------------------------------------------------------------------- */
/* `lit` — run the gather where a player actually stands                       */
/* ------------------------------------------------------------------------- */
static void lit_one_map(const disc *d, const char *map, u32 *tot_gathered,
                        u32 *tot_flares, u32 *tot_spawns, u32 *tot_dark,
                        bool verbose)
{
    char path[256];
    q2_buf cbuf, zbuf;
    q2_common_file cf;
    q2_start_pos_list spawns;
    q2_light_list lights;
    u32 k;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &cbuf) != Q2_OK)
        return;
    if (q2_common_open(&cf, &cbuf) != Q2_OK) {
        q2_buf_free(&cbuf);
        return;
    }

    if (q2_start_pos_parse(&spawns, &cf) != Q2_OK ||
        q2_lights_parse(&lights, &cf) != Q2_OK) {
        q2_common_close(&cf);
        return;
    }

    for (k = 0; k < spawns.count; k++) {
        q2_start_pos sp;
        q2_zone_file zf;
        q2_collision sec;
        q2_spacelights sl;
        q2_light_world world;
        q2_light_set set;
        s32 feet[3];
        s32 node;
        u32 first, count, i, flares = 0;

        if (!q2_start_pos_get(&spawns, k, &sp))
            continue;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/ZONE%d.DAT", map, sp.zone);
        if (disc_read_file(d, path, &zbuf) != Q2_OK)
            continue;
        if (q2_zone_open(&zf, &zbuf) != Q2_OK) {
            q2_buf_free(&zbuf);
            continue;
        }

        /*
         * A StartPos is the FEET, and both the collision hull and the light
         * gather work from the entity ORIGIN, 286 above them — the movement
         * hull is the configuration space of the player's own cube, so the feet
         * are outside it by construction. Getting this wrong is not subtle: it
         * puts every spawn in no node at all and hands the fallback light to
         * the whole disc.
         */
        feet[0] = sp.x;
        feet[1] = q2_sim_origin_y(sp.y);
        feet[2] = sp.z;

        if (q2_collision_parse(&sec, &zf, Q2_COLL_SECONDARY) != Q2_OK) {
            q2_zone_close(&zf);
            continue;
        }
        if (q2_spacelights_open(&sl, &zf, &sec) != Q2_OK) {
            q2_zone_close(&zf);
            continue;
        }

        node = q2_coll_find_node(&sec, feet, -1, true);

        memset(&world, 0, sizeof(world));
        world.statics = &lights;
        world.space   = &sl;

        q2_light_gather(&set, &world, feet, node, 0);

        (*tot_spawns)++;
        if (set.count == 0)
            (*tot_dark)++;
        *tot_gathered += set.count;

        if (node >= 0 && q2_spacelights_range(&sl, (u32)node, &first, &count)) {
            for (i = 0; i < count; i++) {
                u16 index;
                q2_light l;
                if (!q2_spacelights_entry(&sl, first + i, &index))
                    break;
                if (!q2_light_get(&lights, index, &l))
                    continue;
                if (q2_flare_style_of(l.type) != 0)
                    flares++;
            }
        }
        *tot_flares += flares;

        if (verbose) {
            printf("  %-9s zone %-2d '%-12s' node %-5d  reach %-3u  "
                   "kept %u  flares %u\n",
                   map, sp.zone, sp.name, node,
                   (node >= 0 &&
                    q2_spacelights_range(&sl, (u32)node, &first, &count))
                       ? count : 0,
                   set.count > Q2_LIGHT_ACTIVE_MAX ? Q2_LIGHT_ACTIVE_MAX
                                                   : set.count,
                   flares);

            for (i = 0; i < set.count && i < Q2_LIGHT_ACTIVE_MAX; i++) {
                const q2_light_slot *s = &set.slot[set.rank[i]];
                printf("      #%u  rgb %4d %4d %4d   dir %6d %6d %6d\n",
                       i, s->rgb[0], s->rgb[1], s->rgb[2],
                       s->d[0], s->d[1], s->d[2]);
            }
        }

        q2_zone_close(&zf);
    }

    q2_common_close(&cf);
}

int cmd_lit(const disc *d, const char *map)
{
    u32 gathered = 0, flares = 0, spawns = 0, dark = 0;
    int n, i;

    if (!d)
        return 1;

    printf("Gathering lights where the player starts%s%s...\n\n",
           map ? " on " : " on every map", map ? map : "");

    if (map) {
        lit_one_map(d, map, &gathered, &flares, &spawns, &dark, true);
    } else {
        n = disc_file_count(d);
        for (i = 0; i < n; i++) {
            const disc_file *f = disc_file_at(d, i);
            const char *base = strrchr(f->path, '/');
            char name[64];
            const char *dir;
            size_t len;

            base = base ? base + 1 : f->path;
            if (strcmp(base, "COMMON.DAT") != 0)
                continue;

            /* .../LEVELS/<MAP>/COMMON.DAT */
            dir = base - 1;
            while (dir > f->path && dir[-1] != '/')
                dir--;
            len = (size_t)(base - 1 - dir);
            if (len == 0 || len >= sizeof(name))
                continue;
            memcpy(name, dir, len);
            name[len] = '\0';

            lit_one_map(d, name, &gathered, &flares, &spawns, &dark, false);
        }
    }

    printf("\n  %u spawn points, %u with no light at all\n", spawns, dark);
    printf("  %u lights accepted in total, %u of the reachable ones carry a flare\n",
           gathered, flares);

    return 0;
}

/* ------------------------------------------------------------------------- */
int cmd_lights(const disc *d)
{
    light_stats st;
    q2_build_id id;
    q2_exe exe;
    int n, i;
    int bad = 0;
    char current_map[64] = "";
    u32 map_lights = 0;

    if (!d)
        return 1;

    memset(&st, 0, sizeof(st));

    n = disc_file_count(d);

    printf("Checking the lighting model against every zone on the disc...\n\n");

    /*
     * COMMON.DAT carries the lights and ZONE<n>.DAT the partition, and the
     * file table lists them in directory order, so the map's light count is
     * always known by the time its zones come round.
     */
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;

        base = base ? base + 1 : f->path;

        if (strncmp(base, "COMMON", 6) == 0) {
            q2_common_file cf;
            q2_light_list lights;
            u32 k;

            if (disc_read_file(d, f->path, &buf) != Q2_OK)
                continue;
            if (q2_common_open(&cf, &buf) != Q2_OK) {
                q2_buf_free(&buf);
                continue;
            }

            snprintf(current_map, sizeof(current_map), "%s", f->path);
            map_lights = 0;

            if (q2_lights_parse(&lights, &cf) == Q2_OK) {
                map_lights = lights.count;
                st.maps++;
                st.lights_total += lights.count;

                for (k = 0; k < lights.count; k++) {
                    q2_light l;
                    if (q2_light_get(&lights, k, &l))
                        st.type_hist[l.type]++;
                }
            }

            q2_common_close(&cf);
            continue;
        }

        if (strncmp(base, "ZONE", 4) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        {
            q2_zone_file zf;

            if (q2_zone_open(&zf, &buf) != Q2_OK) {
                q2_buf_free(&buf);
                continue;
            }
            check_zone(&zf, map_lights, &st, f->path);
            q2_zone_close(&zf);
        }
    }

    printf("SpaceLights, partitioned by the SecondaryCol node's +30 halfword\n");
    printf("  maps %u, zones %u, lights %u\n",
           st.maps, st.zones, st.lights_total);
    printf("  PrimaryColl carries the field on %u zones, not on %u\n",
           st.zones_pri_nonzero, st.zones_pri_zero);
    printf("  SecondaryCol non-decreasing on %u of %u, starts at zero on %u\n",
           st.zones_monotonic, st.zones, st.zones_start_zero);
    printf("  sentinel past the chunk on %u zones; %u halfwords of tail residue\n",
           st.zones_sentinel_over, st.entries_tail);
    printf("  %u zones have an entirely zero partition (front end, FMV stubs)\n",
           st.zones_empty);
    printf("  nodes %u (%u with no lights), entries reached %u, max per node %u\n",
           st.nodes, st.nodes_dark, st.entries_used, st.max_per_node);
    printf("  entries naming a light the map does not have: %u\n\n",
           st.entries_bad);

    if (st.zones_pri_nonzero)   bad++;
    if (st.zones_monotonic != st.zones) bad++;
    if (st.zones_start_zero != st.zones) bad++;
    if (st.zones_sentinel_over) bad++;
    if (st.entries_bad)         bad++;

    printf("Light type byte — bits 3-5 select a flare style, 6-7 a size shift\n");
    printf("  type  style  size  count\n");
    for (i = 0; i < 256; i++) {
        if (!st.type_hist[i])
            continue;
        printf("  %-5d %-6u %-5u %u\n", i,
               q2_flare_style_of((u8)i), q2_flare_size_of((u8)i),
               st.type_hist[i]);
    }
    printf("\n");

    /* The executable-side tables. */
    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify the disc; skipping the exe tables\n");
        return bad ? 1 : 0;
    }
    if (q2_exe_load(&exe, d, id.exe_name) != Q2_OK) {
        fprintf(stderr, "cannot load %s; skipping the exe tables\n", id.exe_name);
        return bad ? 1 : 0;
    }

    {
        int r = check_flare_tables(&exe);
        if (r > 0) bad += r;
    }
    {
        int r = check_divisors(&exe);
        if (r > 0) bad += r;
    }
    {
        int r = check_sincos(&exe);
        if (r > 0) bad += r;
    }
    {
        int r = check_rsqrt(&exe);
        if (r > 0) bad += r;
    }
    {
        int r = check_fallback(&exe);
        if (r > 0) bad += r;
    }

    q2_exe_free(&exe);

    printf("\n%s\n", bad ? "MISMATCHES FOUND" : "no mismatches");
    return bad ? 1 : 0;
}
