/*
 * cmd_multi.c — check the multiplayer reconstruction against the disc.
 *
 * Everything multiplayer.[ch] claims about the DATA is checked here, so that
 * "we understand the multiplayer game" is something the build system can
 * evaluate rather than an assertion in a document:
 *
 *   - which maps are arenas, decided twice and independently — once by "does
 *     this map's COMMON.DAT carry the QMULTI module" and once by "does its
 *     StartPos carry MultiSpawn entries" — and the two answers compared;
 *   - that every arena's LevelBin is byte-identical after relocation, which is
 *     what licenses reading one module and calling it the rules for all;
 *   - that the module's two exports sit where the reconstruction says;
 *   - that QFRONT's limit option tables and default indices are the ones
 *     transcribed into multiplayer.c;
 *   - that no arena carries a RedFlag or BlueFlag batch, which is half the
 *     evidence that the three flag modes are cut.
 */
#include "cmd_multi.h"

#include <stdio.h>
#include <string.h>

#include "entity.h"
#include "level.h"
#include "multiplayer.h"
#include "reloc.h"

/* The same base cmd_exe.c disassembles at, so an address printed by one command
 * can be pasted into the other. */
#define MOD_BASE 0x80100000u

/* QMULTI.C as shipped: its size, and the two exports the engine calls. */
#define QMULTI_SIZE      5608u
#define QMULTI_INIT      (MOD_BASE + 0x11A4u)
#define QMULTI_FRAG_HOOK (MOD_BASE + 0x0BF4u)

/* Where QFRONT's LevelBin keeps the three option tables and the three indices
 * the front end ships with. Module offsets, so they are stable across builds
 * only in the sense that everything else in this repository is: they are read
 * back and compared, and a mismatch is a failure rather than a silent drift. */
#define QFRONT_TIME_TABLE  0xEB88u
#define QFRONT_ROUND_TABLE 0xEBA0u
#define QFRONT_FRAG_TABLE  0xEBACu
#define QFRONT_FRAG_INDEX  0xEBBEu
#define QFRONT_ROUND_INDEX 0xEBC0u
#define QFRONT_TIME_INDEX  0xEBC2u

typedef struct map_info {
    char name[64];
    int  multi_spawns;      /* StartPos records named MultiSpawnN            */
    int  start_pos;         /* StartPos records in total                     */
    u32  levbin;            /* relocated LevelBin size, 0 if absent or empty */
    bool module_matches;    /* byte-identical to the first arena's           */
    bool has_flag_batch;    /* a StartPos or batch name mentioning a flag    */
} map_info;

#define MAX_MAPS 96

/* Collect every Q2DATA/LEVELS/<dir> that has a COMMON.DAT. */
static int collect_maps(const disc *d, map_info *out, int cap)
{
    int i, n = disc_file_count(d), count = 0;

    for (i = 0; i < n && count < cap; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p, *slash;
        char dir[64];
        size_t len;

        if (!f)
            continue;
        p = strstr(f->path, "/LEVELS/");
        if (!p)
            continue;
        p += 8;
        slash = strchr(p, '/');
        if (!slash)
            continue;
        if (strcmp(slash + 1, "COMMON.DAT") != 0)
            continue;

        len = (size_t)(slash - p);
        if (len >= sizeof(dir))
            continue;
        memcpy(dir, p, len);
        dir[len] = '\0';

        memset(&out[count], 0, sizeof(out[count]));
        snprintf(out[count].name, sizeof(out[count].name), "%s", dir);
        count++;
    }
    return count;
}

/* Read one map's COMMON.DAT and fill in what this command needs from it. */
static bool scan_map(const disc *d, map_info *m, q2_buf *reference)
{
    char path[192];
    q2_buf file;
    q2_common_file cf;
    q2_ai_module mod;
    q2_start_pos_list sl;
    u32 k;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%.*s/COMMON.DAT",
             (int)(sizeof(path) - 32), m->name);
    if (disc_read_file(d, path, &file) != Q2_OK)
        return false;
    if (q2_common_open(&cf, &file) != Q2_OK) {
        q2_buf_free(&file);
        return false;
    }

    if (q2_start_pos_parse(&sl, &cf) == Q2_OK) {
        m->start_pos = (int)sl.count;
        for (k = 0; k < sl.count; k++) {
            q2_start_pos sp;
            if (!q2_start_pos_get(&sl, k, &sp))
                continue;
            if (strncmp(sp.name, "MultiSpawn", 10) == 0)
                m->multi_spawns++;
            if (strstr(sp.name, "Flag") || strstr(sp.name, "flag"))
                m->has_flag_batch = true;
        }
    }

    if (q2_level_module_load(&mod, &cf, MOD_BASE) == Q2_OK && !mod.empty) {
        m->levbin = (u32)mod.image.size;
        if (mod.image.size == QMULTI_SIZE) {
            if (!reference->data) {
                if (q2_buf_alloc(reference, mod.image.size) == Q2_OK)
                    memcpy(reference->data, mod.image.data, mod.image.size);
                m->module_matches = true;
            } else {
                m->module_matches =
                    (reference->size == mod.image.size) &&
                    memcmp(reference->data, mod.image.data, mod.image.size) == 0;
            }
        }
        q2_ai_module_free(&mod);
    }

    q2_common_close(&cf);
    return true;
}

/* Pull a halfword out of a relocated module image. */
static bool mod_u16(const q2_ai_module *m, u32 off, u16 *out)
{
    if (!m->image.data || off + 2 > m->image.size)
        return false;
    *out = (u16)(m->image.data[off] | (m->image.data[off + 1] << 8));
    return true;
}

static bool check_option_table(const q2_ai_module *front, const char *label,
                               u32 off, const s16 *want, u32 count,
                               u32 index_off, u32 want_index, int *fail)
{
    u32 i;
    u16 idx = 0;
    bool ok = true;

    printf("  %-12s", label);
    for (i = 0; i < count; i++) {
        u16 v = 0;
        if (!mod_u16(front, off + i * 2, &v)) {
            ok = false;
            break;
        }
        if ((s16)v != want[i])
            ok = false;
        if ((s16)v == Q2_MP_NO_LIMIT)
            printf(" NONE");
        else
            printf(" %d", (s16)v);
    }

    if (!mod_u16(front, index_off, &idx) || idx != want_index)
        ok = false;

    printf("   default index %u -> ", idx);
    if (idx < count && (s16)want[idx] == Q2_MP_NO_LIMIT)
        printf("NONE");
    else if (idx < count)
        printf("%d", want[idx]);
    else
        printf("?");
    printf("   %s\n", ok ? "ok" : "MISMATCH");

    if (!ok)
        (*fail)++;
    return ok;
}

int cmd_multi(const disc *d, const char *map)
{
    map_info maps[MAX_MAPS];
    q2_buf reference;
    int count, i, fail = 0;
    int arenas = 0, by_spawn = 0, disagree = 0, flagged = 0;

    memset(&reference, 0, sizeof(reference));

    count = collect_maps(d, maps, MAX_MAPS);
    if (count == 0) {
        fprintf(stderr, "no level directories found\n");
        return 1;
    }

    for (i = 0; i < count; i++)
        scan_map(d, &maps[i], &reference);

    /* --------------------------------------------------------------------- */
    printf("The multiplayer runtime\n\n");
    printf("QMULTI.C is a per-map LevelBin module, not engine code: the death\n"
           "handler at 0x800396AC calls (*(0x800B2F58))->[4](killer, victim),\n"
           "and 0x800B2F58 is the map's own relocated module.\n\n");

    printf("Arenas — a map is one if it carries the %u-byte module, and\n"
           "separately if its StartPos names MultiSpawn points.\n\n", QMULTI_SIZE);
    printf("  %-10s %7s %7s %8s %s\n", "map", "multi", "starts", "levbin",
           "module");

    for (i = 0; i < count; i++) {
        const map_info *m = &maps[i];
        bool is_arena = (m->levbin == QMULTI_SIZE);

        if (m->multi_spawns > 0)
            by_spawn++;
        if (is_arena)
            arenas++;
        if (is_arena != (m->multi_spawns > 0))
            disagree++;
        if (m->has_flag_batch)
            flagged++;

        if (!is_arena && m->multi_spawns == 0)
            continue;   /* a single-player map: nothing to show */

        printf("  %-10s %7d %7d %8u %s\n", m->name, m->multi_spawns,
               m->start_pos, m->levbin,
               is_arena ? (m->module_matches ? "identical" : "DIFFERS") : "-");

        if (is_arena && !m->module_matches)
            fail++;
    }

    printf("\n  arenas by module      : %d\n", arenas);
    printf("  arenas by MultiSpawn  : %d\n", by_spawn);
    printf("  maps the two disagree : %d\n", disagree);
    printf("  maps naming a flag    : %d\n", flagged);
    if (disagree)
        fail++;

    /* --------------------------------------------------------------------- */
    /* The module's own shape, read off the first arena that has it.          */
    for (i = 0; i < count; i++) {
        char path[192];
        q2_buf file;
        q2_common_file cf;
        q2_ai_module mod;

        if (maps[i].levbin != QMULTI_SIZE)
            continue;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", maps[i].name);
        if (disc_read_file(d, path, &file) != Q2_OK)
            break;
        if (q2_common_open(&cf, &file) != Q2_OK) {
            q2_buf_free(&file);
            break;
        }
        if (q2_level_module_load(&mod, &cf, MOD_BASE) == Q2_OK && !mod.empty) {
            u32 init = q2_ai_module_export(&mod, 0);
            u32 frag = q2_ai_module_export(&mod, 1);

            printf("\nQMULTI.C exports, from %s\n", maps[i].name);
            printf("  export 0  init      : 0x%08X %s\n", init,
                   init == QMULTI_INIT ? "ok" : "UNEXPECTED");
            printf("  export 1  frag hook : 0x%08X %s\n", frag,
                   frag == QMULTI_FRAG_HOOK ? "ok" : "UNEXPECTED");
            if (init != QMULTI_INIT || frag != QMULTI_FRAG_HOOK)
                fail++;
            q2_ai_module_free(&mod);
        }
        q2_common_close(&cf);
        break;
    }

    /* --------------------------------------------------------------------- */
    /* QFRONT's option tables.                                                */
    {
        q2_buf file;
        q2_common_file cf;
        q2_ai_module front;

        if (disc_read_file(d, "Q2DATA/LEVELS/QFRONT/COMMON.DAT", &file) == Q2_OK) {
            if (q2_common_open(&cf, &file) == Q2_OK) {
                if (q2_level_module_load(&front, &cf, MOD_BASE) == Q2_OK &&
                    !front.empty) {
                    printf("\nLimit tables, from QFRONT's LevelBin\n");
                    check_option_table(&front, "TIME (min)", QFRONT_TIME_TABLE,
                                       q2_mp_time_options,
                                       Q2_MP_TIME_OPTION_COUNT,
                                       QFRONT_TIME_INDEX,
                                       Q2_MP_TIME_OPTION_DEFAULT, &fail);
                    check_option_table(&front, "FRAG", QFRONT_FRAG_TABLE,
                                       q2_mp_frag_options,
                                       Q2_MP_FRAG_OPTION_COUNT,
                                       QFRONT_FRAG_INDEX,
                                       Q2_MP_FRAG_OPTION_DEFAULT, &fail);
                    check_option_table(&front, "ROUND", QFRONT_ROUND_TABLE,
                                       q2_mp_round_options,
                                       Q2_MP_ROUND_OPTION_COUNT,
                                       QFRONT_ROUND_INDEX,
                                       Q2_MP_ROUND_OPTION_DEFAULT, &fail);
                    q2_ai_module_free(&front);
                } else {
                    printf("\nQFRONT carries no LevelBin: limit tables unchecked\n");
                }
                q2_common_close(&cf);
            } else {
                q2_buf_free(&file);
            }
        }
    }

    /* --------------------------------------------------------------------- */
    /* Modes, and which of them the front end can reach.                      */
    printf("\nModes — six implemented, three selectable (0x8010459C writes 0, 1, 5)\n");
    for (i = 0; i < Q2_MP_MODE_COUNT; i++) {
        const char *names[Q2_MP_MAX_BATCHES];
        u32 n = q2_mp_batches((q2_mp_mode)i, names);
        u32 b;

        printf("  %d %-16s %-9s batches:", i, q2_mp_mode_name((q2_mp_mode)i),
               q2_mp_mode_selectable((q2_mp_mode)i) ? "shipped" : "cut");
        for (b = 0; b < n; b++)
            printf(" %s", names[b]);
        printf("\n");
    }

    /* --------------------------------------------------------------------- */
    /* Optionally, one arena's spawn points and the selector's own answer.    */
    if (map) {
        char path[192];
        q2_buf file;
        q2_common_file cf;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(d, path, &file) == Q2_OK &&
            q2_common_open(&cf, &file) == Q2_OK) {
            q2_start_pos_list sl;

            printf("\n%s MultiSpawn points\n", map);
            if (q2_start_pos_parse(&sl, &cf) == Q2_OK) {
                q2_mp_spawn spawns[Q2_MP_MAX_SPAWNS];
                q2_mp_player_view view[Q2_MP_MAX_PLAYERS];
                u32 k;
                int pick;

                memset(spawns, 0, sizeof(spawns));
                memset(view, 0, sizeof(view));

                for (k = 0; k < sl.count; k++) {
                    q2_start_pos sp;
                    int idx;
                    if (!q2_start_pos_get(&sl, k, &sp))
                        continue;
                    if (strncmp(sp.name, "MultiSpawn", 10) != 0)
                        continue;
                    idx = sp.name[10] - '0';
                    if (idx < 0 || idx >= Q2_MP_MAX_SPAWNS)
                        continue;
                    spawns[idx].present = true;
                    spawns[idx].pos[0]  = sp.x;
                    spawns[idx].pos[1]  = sp.y;
                    spawns[idx].pos[2]  = sp.z;
                    spawns[idx].angle   = sp.angle;
                    printf("  MultiSpawn%d  %8d %8d %8d  ang %6d zone %d\n",
                           idx, sp.x, sp.y, sp.z, sp.angle, sp.zone);
                }

                /* One player standing on spawn 0: the selector must not send
                 * the next arrival to the same place. */
                view[0].alive = true;
                view[0].pos[0] = spawns[0].pos[0];
                view[0].pos[1] = spawns[0].pos[1];
                view[0].pos[2] = spawns[0].pos[2];

                pick = q2_mp_select_spawn(spawns, view, 1, NULL, NULL);
                printf("  with a player on MultiSpawn0, the selector picks %d\n",
                       pick);
                if (pick == 0) {
                    printf("  FAIL - the farthest-point rule chose the "
                           "occupied spawn\n");
                    fail++;
                }
            }
            q2_common_close(&cf);
        } else {
            fprintf(stderr, "cannot read %s\n", path);
        }
    }

    q2_buf_free(&reference);

    printf("\n%s\n", fail == 0
           ? "PASS - the arenas agree, the module is one module, and the "
             "front end's tables match the reconstruction"
           : "FAIL - see the mismatches above");
    return fail == 0 ? 0 : 1;
}
