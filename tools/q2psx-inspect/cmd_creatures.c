#include "cmd_creatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crebind.h"
#include "creature.h"
#include "creworld.h"
#include "dat.h"
#include "ident.h"
#include "level.h"
#include "model.h"
#include "monster.h"
#include "reloc.h"
#include "sha256.h"
#include "vag.h"

/* Where a module image is relocated for inspection. Any address works; this
 * one is far from the executable's own so a stray pointer is obvious. */
#define CRE_BASE 0x80100000u

static int g_modules;
static int g_distinct;
static int g_covered;
static int g_reloc_fail;
static int g_actions_total;
static int g_actions_done;
static int g_actions_empty;
static const u8 *g_img;
static size_t g_img_size;
static int g_named;
static int g_unnamed;

/*
 * The engine function behind an import slot.
 *
 * All 71 are named in creature.h, read one target at a time out of the loader
 * at 0x8007DA00. Only the ones a think function is actually seen to call are
 * listed here — the rest would be noise in a census — and anything not listed
 * prints as `call`, which is the old behaviour and is still honest.
 */
static const char *cre_import_name(u32 slot)
{
    switch (slot) {
    case 0x14:  return "rand";
    case 0x1C:  return "link";
    case 0x20:  return "sound";
    case 0x28:  return "local2wld";
    case 0x2C:  return "muzzleofs";
    case 0x30:  return "poseat";
    case 0x34:  return "poseblend";
    case 0x38:  return "muzzlepoint";
    case 0x54:  return "ai_run";
    case 0x58:  return "ai_walk";
    case 0x5C:  return "ai_stand";
    case 0x60:  return "ai_move";
    case 0x64:  return "ai_charge";
    case 0x70:  return "radiusdamage";
    case 0x74:  return "findmove";
    case 0x7C:  return "beam";
    case 0x80:  return "FIRE.blaster";
    case 0x84:  return "FIRE.bullet";
    case 0x88:  return "FIRE.shotgun";
    case 0x8C:  return "FIRE.railgun";
    case 0x90:  return "FIRE.bfg";
    case 0x94:  return "FIRE.grenade";
    case 0x98:  return "FIRE.rocket";
    case 0x9C:  return "FIRE.laser";
    case 0xA0:  return "flashlight";
    case 0xA4:  return "findradius";
    case 0xAC:  return "foundtarget";
    case 0xB0:  return "hunttarget";
    case 0xB4:  return "range";
    case 0xB8:  return "veclen";
    case 0xBC:  return "veclensq";
    case 0xC0:  return "vectorma";
    case 0xC4:  return "vecnorm";
    case 0xC8:  return "vectoangles";
    case 0xCC:  return "vectoyaw";
    case 0xD0:  return "infront";
    case 0xD4:  return "anglemod";
    case 0xD8:  return "anglevectors";
    case 0xDC:  return "projectsrc";
    case 0xE4:  return "visible";
    case 0xE8:  return "checkattack";
    case 0xEC:  return "fire_hit";
    case 0xF0:  return "T_Damage";
    case 0xF4:  return "trace";
    case 0xFC:  return "walkstart";
    case 0x100: return "flystart";
    case 0x104: return "swimstart";
    case 0x108: return "maxs.x";
    case 0x10C: return "maxs.y";
    case 0x110: return "mins.y";
    case 0x114: return "mins.x";
    case 0x120: return "explosion";
    case 0x12C: return "proximity";
    default:    return "call";
    }
}
static int g_unclaimed;
static int g_moves;

static void report(const q2_creature *c, const q2_cre_impl *impl)
{
    u8  idx[64];
    u32 n, i, missing = 0;

    printf("\n  %-10s  classes", c->name);
    for (i = 0; i < c->class_count; i++)
        printf(" %u", c->class_byte[i]);
    printf("   scale %d   mass %d\n", c->speed_scale, c->mass);

    printf("    callbacks :");
    for (i = 0; i < 13; i++)
        if (c->callback[i])
            printf(" %s", q2_cre_callback_names[i]);
    printf("\n");

    /* The callback addresses, so one that installs no move can be gone and read
     * rather than guessed at — the same reason `at:` prints the methods. */
    printf("    cb at     :");
    for (i = 0; i < 13; i++)
        if (c->callback[i])
            printf(" %s=%08X", q2_cre_callback_names[i], c->callback[i]);
    printf("\n");

    printf("    moves %2u, frames %3u, methods %2u\n",
           c->move_count, c->frame_count, c->method_count);

    /*
     * Every move's frame RANGE, which is the number a port needs and the one
     * the census never printed. A move is named by its first frame (crebind.h),
     * and the highest last_frame a module reaches is what a drawing side has to
     * be able to show — so the two together say how a creature's animation
     * numbering relates to the CastList clips its model actually carries.
     */
    {
        s32 hi = -1;
        printf("    ranges    :");
        for (i = 0; i < c->move_count; i++) {
            printf(" %d-%d", c->move[i].first_frame, c->move[i].last_frame);
            if (c->move[i].last_frame > hi)
                hi = c->move[i].last_frame;
        }
        printf("\n    highest frame : %d\n", hi);

        /*
         * The module's own labels for its moves, when it carries them. The
         * Soldier's module has none; every other one on the disc does.
         */
        {
            static const char *names[Q2_CRE_MAX_MOVES];
            u32 named;

    /* Cast for MSVC: an array of pointers-to-const reaching a void *
     * parameter reads as discarding const (C4090). */
    memset((void *)names, 0, sizeof(names));
            named = q2_creature_move_names(c, g_img, g_img_size, names,
                                           (u32)(sizeof(names) /
                                                 sizeof(names[0])));
            if (named) {
                u32 k;
                printf("    named     : %u of %u\n", named, c->move_count);
                for (k = 0; k < c->move_count; k++)
                    if (names[k])
                        printf("      %3d-%-3d  \"%.16s\"\n",
                               c->move[k].first_frame, c->move[k].last_frame,
                               names[k]);
            }
            /*
             * Which moves the module names but the DECODER never found, and
             * which moves the decoder found that the module does not name. The
             * first set says the disc knows about behaviour this port has not
             * decoded; the second says the reverse. Printing both keeps
             * "unnamed" from reading as "absent from the disc".
             */
            {
                u32 k;
                for (k = 0; k < c->move_count; k++)
                    if (!names[k]) {
                        printf("      %3d-%-3d  (this module does not name it)\n",
                               c->move[k].first_frame, c->move[k].last_frame);
                        g_unnamed++;
                    }
                {
                    static const char *un[64];
                    u32 nu, z;
    /* Cast for MSVC: an array of pointers-to-const reaching a void *
     * parameter reads as discarding const (C4090). */
    memset((void *)un, 0, sizeof(un));
                    nu = q2_creature_unclaimed_names(c, g_img, g_img_size, un,
                                                     64);
                    g_unclaimed += nu;
                    if (nu) {
                        printf("    module names %u range(s) no decoded move "
                               "claims:", nu);
                        printf("\n");
                        for (z = 0; z < nu && z < 16; z++)
                            if (un[z])
                                printf("      %3d-%-3d  \"%.16s\"  "
                                       "(no decoded move has this range)\n",
                                       (int)(s16)q2_rd_u16((const u8 *)un[z] - 4),
                                       (int)(s16)q2_rd_u16((const u8 *)un[z] - 2),
                                       un[z]);
                    }
                }
            }
            g_named += (int)named;
            g_moves += (int)c->move_count;
        }
    }

    /*
     * Which THINK each move's frames call, and which callback installed the
     * move. A think index only ever runs because a frame names it, so "the
     * creature never reaches think 8" is answered here and nowhere else: if no
     * frame of any move carries it, nothing the AI does will run it.
     */
    /* The module's sound-name table, so "this creature is silent" can be told
     * apart from "this creature's table was not found". */
    {
        static const char *sn[32];
        u32 ns, z;
    /* Cast for MSVC: an array of pointers-to-const reaching a void *
     * parameter reads as discarding const (C4090). */
    memset((void *)sn, 0, sizeof(sn));
        ns = q2_creature_sound_names(c, g_img, g_img_size, sn, 32);
        printf("    sounds    : %u named", ns);
        for (z = 0; z < ns && z < 12; z++)
            if (sn[z])
                printf(" [%u]%.12s", z, sn[z]);
        printf("%s\n", ns > 12 ? " ..." : "");
    }

    /*
     * And the REGISTRATIONS, decoded from the module's own init rather than
     * found by a run heuristic. Each is a name the module hands to import slot
     * 9 and the BSS word it parks the handle in — which is the address the
     * decoder already prints at every `sound(...)` play site, so the two join
     * up directly and no ordering has to be guessed.
     */
    {
        static q2_cre_sound_bind sb[32];
        u32 nb, z;

        memset(sb, 0, sizeof(sb));
        nb = q2_creature_sound_bindings(g_img, g_img_size, CRE_BASE, sb, 32);
        printf("    registers : %u", nb);
        for (z = 0; z < nb && z < 12; z++)
            printf(" %08X=%.12s", sb[z].addr, sb[z].name);
        printf("%s\n", nb > 12 ? " ..." : "");
    }

    printf("    per move  : (via) range -> think bytes\n");
    for (i = 0; i < c->move_count; i++) {
        const q2_cre_move *mv = &c->move[i];
        u8 seen[64];
        u32 nseen = 0, f;

        printf("      (%2d) %08X end %08X->%08X %3d-%-3d ->", mv->via, mv->addr,
               mv->endfunc_addr, mv->endfunc_move, mv->first_frame,
               mv->last_frame);

        /*
         * In FRAME ORDER, run-length encoded. WHICH thinks a move calls is only
         * half the question; WHERE they sit decides whether an animation that is
         * cut short ever reaches them. `0*12 3 4` and `3 4 0*12` are the same
         * set and a very different creature.
         */
        (void)seen;
        (void)nseen;
        {
            u32 run = 0;
            int prev = -1;

            for (f = 0; f < mv->frame_count &&
                        mv->frame_index + f < Q2_CRE_MAX_FRAMES; f++) {
                int th = c->frames[mv->frame_index + f].think;

                if (th == prev) {
                    run++;
                    continue;
                }
                if (prev >= 0)
                    printf(run > 1 ? " %d*%u" : " %d", prev, run);
                prev = th;
                run  = 1;
            }
            if (prev >= 0)
                printf(run > 1 ? " %d*%u" : " %d", prev, run);
        }

        /*
         * And the AI VERB each frame runs, the same way. This is what decides
         * whether an animation plays out or hands the creature back to the
         * chase on the next tick, so it belongs beside the thinks.
         */
        {
            u32 arun = 0;
            int aprev = -1;

            printf("  ai:");
            for (f = 0; f < mv->frame_count &&
                        mv->frame_index + f < Q2_CRE_MAX_FRAMES; f++) {
                int a = c->frames[mv->frame_index + f].ai;

                if (a == aprev) {
                    arun++;
                    continue;
                }
                if (aprev >= 0)
                    printf(arun > 1 ? " %d*%u" : " %d", aprev, arun);
                aprev = a;
                arun  = 1;
            }
            if (aprev >= 0)
                printf(arun > 1 ? " %d*%u" : " %d", aprev, arun);
        }
        printf("\n");
    }

    n = q2_creature_think_indices(c, idx, sizeof(idx));
    printf("    think     :");
    for (i = 0; i < n; i++) {
        /* A generic trampoline exists for every index. Its existence does
         * not establish that the function's branches or attacks are ported. */
        bool own  = impl && impl->name && impl->method[idx[i]] != NULL &&
                    impl->method[idx[i]] != q2_cre_generic.method[idx[i]];
        bool acts = q2_cre_generic.method[idx[i]] != NULL;

        printf(" %u%s", idx[i], own ? "" : (acts ? "d" : "*"));
        if (!own)
            missing++;
    }
    if (n == 0)
        printf(" (none)");
    printf("\n");

    /* The addresses, so an index the port does not cover can be gone and
     * read rather than guessed at. */
    printf("    at        :");
    for (i = 0; i < n; i++)
        printf(" [%u]=%08X", idx[i], c->method[idx[i]]);
    printf("\n");

    /* What each think index actually does, decoded from the module. */
    {
        static q2_cre_think th[Q2_CLASS_METHOD_COUNT];
        u32 got = q2_creature_decode_thinks(c, g_img, g_img_size, CRE_BASE,
                                            th, Q2_CLASS_METHOD_COUNT);
        /*
         * A think with no steps is not necessarily a failed decode. The
         * Berserk's think 7 is `jr ra; nop` at 0x80101008 — an EMPTY function,
         * and "this frame does nothing" is a real answer the disc gives. Count
         * those separately so a deliberate no-op stops reading as a gap.
         */
        {
            u32 empty = 0;
            for (i = 0; i < n; i++)
                if (!th[idx[i]].step_count &&
                    q2_creature_think_is_empty(c, g_img, g_img_size, CRE_BASE,
                                               idx[i]))
                    empty++;
            printf("    actions   : %u of %u think indices decode to an action",
                   got, n);
            if (empty)
                printf(", and %u is an empty function on the disc", empty);
            printf("\n");
            g_actions_empty += (int)empty;
        }
        for (i = 0; i < n; i++) {
            const q2_cre_think *t = &th[idx[i]];
            u32 k;
            if (!t->step_count)
                continue;
            printf("      [%2u]", idx[i]);
            for (k = 0; k < t->step_count; k++) {
                const q2_cre_step *st = &t->step[k];
                switch (st->op) {
                case Q2_CRE_OP_SOUND:
                    printf(" sound(%08X)%s", st->addr, st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_MELEE:
                    printf(" melee(aim %d,%d,%d dmg %d+r%%%d kick %d)",
                           st->aim[0], st->aim[1], st->aim[2],
                           st->damage_base, st->damage_rand, st->kick);
                    break;
                case Q2_CRE_OP_NEXTFRAME:
                    printf(" nextframe(%d)%s", st->frame,
                           st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_MOVE:
                    printf(" move(%08X)%s", st->addr, st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_AIFLAG:
                    printf(" aiflags(+%X-%X)%s", st->flag_set,
                           st->flag_clear & 0xFFFFu, st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_CALL:
                    /*
                     * Named where the import loader names it. The census used
                     * to print `call(+C0)?` and leave the reader to go and look
                     * the slot up, which is exactly the state that let three of
                     * the eight projectile spawners sit unclassified and let
                     * `walkmonster_start` be counted as a shot.
                     */
                    printf(" %s(+%02X)%s", cre_import_name(st->import_ofs),
                           st->import_ofs, st->gated ? "?" : "");
                    break;
                default:
                    break;
                }
            }
            printf("\n");
        }
        g_actions_total += n;
        g_actions_done  += got;
    }

    if (impl && impl->name) {
        u32 cb_own = 0, cb_have = 0;

        for (i = 0; i < 13; i++) {
            if (!c->callback[i])
                continue;
            cb_have++;
            if (impl->callback[i] &&
                impl->callback[i] != q2_cre_generic.callback[i])
                cb_own++;
        }

        if (impl->scripted) {
            printf("    port      : %s — native spawn-installed think/use/pain "
                   "handlers (no monsterinfo callback table)\n", impl->name);
            g_covered++;
            return;
        }
        printf("    port      : %s — %u of %u callbacks written by hand, "
               "%u of %u think indices written by hand (d = decoded only)\n",
               impl->name, cb_own, cb_have, n - missing, n);
        if (missing == 0 && cb_own == cb_have)
            g_covered++;
    } else {
        printf("    port      : UNSUPPORTED native behavior; decoded actions "
               "do not establish working weapons or control flow\n");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * THE WAKE CENSUS — every creature every map places, woken the way the client
 * wakes it, and what its eye and turn rate came out as.
 *
 * The decode above says what each module IS. This says what the port DOES with
 * it once a map is loaded. q2_creature_world_load binds dormant records; wake
 * runs export 0 only for active actors. The Insane then picks the fly or walk
 * wrapper on its prone bit (module+0x974). Each creature's `model_ext2` is
 * then filled from its own CastList model — the halfword the console's loader
 * writes to obj+0xF8 at 0x80056710, and what the client's creature model loop
 * has to copy from the q2_model it resolves, before it wakes the world — and
 * q2_creature_world_wake runs q2_monster_start_go, which dispatches to the
 * go-routine the wrapper parked. Leave the fill out and every walker below
 * comes out at the stand-in, which is the check this census exists to make.
 *
 * What that must give, off the three go-routines (`ai` pins the immediates): a
 * walker's eye is ~ext2 (0x80062450 `nor`) and it turns 228 (0x80062438); a
 * flyer's eye is a flat -250 (0x800624F0) and a swimmer's -100 (0x80062584),
 * and both turn 114 (0x800624E8, 0x8006257C). A creature whose model could not
 * be found keeps the port's -290 stand-in (monster.c, q2_monster_start_go) and
 * is counted rather than hidden.
 *
 * Then the two sound substitutions modules make in their own spawn code
 * (creature.c, k_sound_fallbacks): the Soldier's pain trio (module+0xE50) and
 * the Tank Commander's idle, overwritten with its footstep (module+0x808). On
 * every map that places one, what the host plays must be a name that map's
 * SNDVRAM bank carries.
 */
#define CENSUS_KINDS  40
#define CENSUS_SHAPES 6

typedef struct census_kind {
    char name[13];
    u32  maps, placed;
    int  last_map;
    struct { s16 eye, turn; bool fly, swim; u32 n; } shape[CENSUS_SHAPES];
    u32  shapes, overflow;
} census_kind;

/* Expectations measured off the disc: ext2 251, 507, 217, 380 and 304 re-read
 * with `q2psx-inspect models`, so ~ext2 is -252, -508, -218, -381 and -305. */
typedef struct census_expect {
    const char *map, *kind;
    s16         eye, turn;
    bool        fly;
    u32         hit, total;
} census_expect;

static bool census_name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

/*
 * "Does this map's bank carry it", exactly and without case — the client's
 * first pass (client_find_sound). Its second, prefix pass only applies to a key
 * that fills a twelve-byte table field, and every name asked about here is
 * shorter than that.
 */
static bool census_bank_has(const char *name, void *user)
{
    const q2_sound_bank *bank = (const q2_sound_bank *)user;
    q2_vag v;
    u32 i;

    if (!bank || !name || !name[0])
        return false;
    for (i = 0; i < bank->count; i++)
        if (q2_sound_bank_get(bank, i, &v) && census_name_ieq(v.name, name))
            return true;
    return false;
}

static census_kind *census_kind_for(census_kind *k, u32 *n, const char *name)
{
    u32 i;

    for (i = 0; i < *n; i++)
        if (strcmp(k[i].name, name) == 0)
            return &k[i];
    if (*n >= CENSUS_KINDS)
        return NULL;
    memset(&k[*n], 0, sizeof(k[*n]));
    snprintf(k[*n].name, sizeof(k[*n].name), "%s", name);
    k[*n].last_map = -1;
    return &k[(*n)++];
}

static int census_wake(const disc *d)
{
    static census_kind kinds[CENSUS_KINDS];
    census_expect expect[] = {
        { "BASE0",    "Soldier",  -252, 228, false, 0, 0 },
        { "BASE1",    "Soldier",  -252, 228, false, 0, 0 },
        { "COMMAND",  "Tankcomm", -508, 228, false, 0, 0 },
        { "POWER1",   "Arachner", -218, 228, false, 0, 0 },
        { "POWER1",   "Gunner",   -381, 228, false, 0, 0 },
        { "LAB",      "Insane",   -250, 114, true,  0, 0 },   /* prone   */
        { "LAB",      "Insane",   -305, 228, false, 0, 0 },   /* upright */
    };
    const u32 n_expect = (u32)(sizeof(expect) / sizeof(expect[0]));
    q2_build_id id;
    q2_creature_world *w;
    u32 nkinds = 0, total = 0, unresolved = 0, maps_with = 0, odd = 0;
    u32 sol_maps = 0, sol_ok = 0, tnk_maps = 0, tnk_ok = 0;
    u32 sol_names[3] = { 0, 0, 0 };
    u32 st_records = 0, st_nomod = 0;
    int i, n = disc_file_count(d), map_ord = 0, bad = 0;
    u32 k;
    s16 stand_in;

    if (q2_identify(d, &id) != Q2_OK) {
        printf("\n  wake census: cannot identify this disc\n");
        return 1;
    }
    {
        q2_monster fresh;

        q2_monster_init(&fresh);
        stand_in = fresh.view_height;
    }
    w = (q2_creature_world *)calloc(1, sizeof(*w));
    if (!w)
        return 1;

    /*
     * Every record is woken here, the held batches too: the client holds a
     * script's groups dormant and CREBATCH later wakes them through the same
     * q2_monster_start_go (creworld.c, q2_creature_world_summon), so what they
     * wake up as is the same either way.
     */
    printf("\nWake census: every placed creature, woken as the client wakes"
           " it\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path, *rest, *slash;
        char map[32], path[160];
        q2_buf buf;
        q2_common_file cf;
        q2_model_bank cbank;
        bool have_cbank;
        q2_buf zbuf[8];
        q2_zone_file zf[8];
        q2_model_bank zbank[8];
        bool zopen[8];
        q2_sound_bank sb, *sbp;
        bool have_sb;
        bool sol_done = false, tnk_done = false;
        s32 origin[3] = { 0, 0, 0 };
        u32 m, z;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;
        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash || strcmp(slash + 1, "COMMON.DAT") != 0 ||
            (size_t)(slash - rest) >= sizeof(map))
            continue;
        memcpy(map, rest, (size_t)(slash - rest));
        map[slash - rest] = '\0';

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(d, path, &buf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }
        if (q2_creature_world_load(w, d, &id, &cf, NULL, NULL, NULL) != Q2_OK) {
            q2_creature_world_free(w);
            q2_common_close(&cf);
            continue;
        }
        st_records += w->stats.records;
        st_nomod   += w->stats.no_module;
        if (w->set.count == 0) {
            q2_creature_world_free(w);
            q2_common_close(&cf);
            continue;
        }
        map_ord++;
        maps_with++;

        /* The models: COMMON's bank, then every zone's, which is where most
         * creature models live (see `mob`). */
        have_cbank = (q2_model_bank_from_common(&cbank, &cf) == Q2_OK);
        for (z = 0; z < 8; z++) {
            char zpath[160];

            zopen[z] = false;
            snprintf(zpath, sizeof(zpath), "Q2DATA/LEVELS/%s/ZONE%u.DAT",
                     map, z);
            if (disc_read_file(d, zpath, &zbuf[z]) != Q2_OK)
                continue;
            if (q2_zone_open(&zf[z], &zbuf[z]) != Q2_OK) {
                q2_buf_free(&zbuf[z]);
                continue;
            }
            zopen[z] = (q2_model_bank_from_zone(&zbank[z], &zf[z]) == Q2_OK);
            if (!zopen[z])
                q2_zone_close(&zf[z]);
        }

        for (m = 0; m < w->set.count; m++) {
            q2_monster *mo = &w->set.monsters[m];
            const char *name = q2_creature_world_model_name(w, mo);
            q2_model mdl;
            s32 idx = -1;
            bool got = false;

            memset(&mdl, 0, sizeof(mdl));  /* `got` guards it; MSVC C4701 */
            if (!mo->in_use || !name)
                continue;
            if (have_cbank) {
                idx = q2_model_bank_find(&cbank, name);
                got = idx >= 0 && q2_model_get(&cbank, (u32)idx, &mdl) == Q2_OK;
            }
            for (z = 0; z < 8 && !got; z++) {
                if (!zopen[z])
                    continue;
                idx = q2_model_bank_find(&zbank[z], name);
                got = idx >= 0 &&
                      q2_model_get(&zbank[z], (u32)idx, &mdl) == Q2_OK;
            }
            if (got)
                mo->model_ext2 = mdl.hdr.ext2;   /* obj+0xF8, 0x80056710 */
        }

        /* A census stands a full-health player in the map; the health is
         * what the AI's dead-enemy gates read. */
        q2_creature_world_wake(w, origin, 100);

        have_sb = (q2_sound_bank_load(&sb, d, map) == Q2_OK);
        sbp     = have_sb ? &sb : NULL;

        for (m = 0; m < w->set.count; m++) {
            const q2_monster *mo = &w->set.monsters[m];
            const char *name = q2_creature_world_model_name(w, mo);
            census_kind *ck;
            bool fly  = (mo->flags & Q2_FL_FLY) != 0;
            bool swim = (mo->flags & Q2_FL_SWIM) != 0;
            s16 want_eye, want_turn;
            u32 s;

            if (!mo->in_use || !name)
                continue;
            total++;

            /* What the go-routine that was parked must have left. */
            if (fly) {
                want_eye = -250;  want_turn = 114;
            } else if (swim) {
                want_eye = -100;  want_turn = 114;
            } else if (mo->model_ext2 != 0) {
                want_eye = (s16)(u16)~(u16)mo->model_ext2;  want_turn = 228;
            } else {
                /* No model, so nobody filled ext2: the port's guard keeps
                 * q2_monster_init's stand-in (monster.c). */
                want_eye = stand_in;  want_turn = 228;
                unresolved++;
            }
            if (mo->view_height != want_eye || mo->yaw_speed != want_turn) {
                printf("  MISMATCH  %s %s: eye %d turn %d, want %d/%d\n",
                       map, name, mo->view_height, mo->yaw_speed,
                       want_eye, want_turn);
                odd++;
            }

            ck = census_kind_for(kinds, &nkinds, name);
            if (ck) {
                if (ck->last_map != map_ord) {
                    ck->last_map = map_ord;
                    ck->maps++;
                }
                ck->placed++;
                for (s = 0; s < ck->shapes; s++)
                    if (ck->shape[s].eye == mo->view_height &&
                        ck->shape[s].turn == mo->yaw_speed &&
                        ck->shape[s].fly == fly && ck->shape[s].swim == swim)
                        break;
                if (s == ck->shapes && s < CENSUS_SHAPES) {
                    ck->shape[s].eye  = mo->view_height;
                    ck->shape[s].turn = mo->yaw_speed;
                    ck->shape[s].fly  = fly;
                    ck->shape[s].swim = swim;
                    ck->shape[s].n    = 0;
                    ck->shapes++;
                }
                if (s < ck->shapes)
                    ck->shape[s].n++;
                else
                    ck->overflow++;
            }

            for (k = 0; k < n_expect; k++) {
                if (strcmp(expect[k].map, map) != 0 ||
                    strcmp(expect[k].kind, name) != 0)
                    continue;
                expect[k].total++;
                if (mo->view_height == expect[k].eye &&
                    mo->yaw_speed == expect[k].turn && fly == expect[k].fly)
                    expect[k].hit++;
            }

            /* The sound substitutions, once per map per module. */
            if (!sol_done && strcmp(name, "Soldier") == 0) {
                const char *r = q2_cre_sound_resolve(mo, "sol_pain3",
                                                     census_bank_has, sbp);
                sol_done = true;
                sol_maps++;
                if (r && census_bank_has(r, sbp)) {
                    sol_ok++;
                    if (strcmp(r, "sol_pain2") == 0)      sol_names[0]++;
                    else if (strcmp(r, "sol_pain1") == 0) sol_names[1]++;
                    else if (strcmp(r, "sol_pain3") == 0) sol_names[2]++;
                } else {
                    printf("  MISMATCH  %s: the Soldier's sol_pain3 plays %s,"
                           " which the bank does not carry\n",
                           map, r ? r : "(nothing)");
                }
            }
            if (!tnk_done && strcmp(name, "Tankcomm") == 0) {
                const char *r = q2_cre_sound_resolve(mo, "tnk_idle1",
                                                     census_bank_has, sbp);
                tnk_done = true;
                tnk_maps++;
                if (r && strcmp(r, "tnk_step") == 0 &&
                    census_bank_has(r, sbp)) {
                    tnk_ok++;
                } else {
                    printf("  MISMATCH  %s: the Tank Commander's tnk_idle1"
                           " plays %s, not a carried tnk_step\n",
                           map, r ? r : "(nothing)");
                }
            }
        }

        if (have_sb)
            q2_sound_bank_free(&sb);
        for (z = 0; z < 8; z++)
            if (zopen[z])
                q2_zone_close(&zf[z]);
        q2_creature_world_free(w);
        q2_common_close(&cf);
    }
    free(w);

    printf("  creature    maps  placed  eye/turn after q2_monster_start_go\n");
    for (k = 0; k < nkinds; k++) {
        u32 s;

        printf("  %-10s  %4u  %6u ", kinds[k].name, kinds[k].maps,
               kinds[k].placed);
        for (s = 0; s < kinds[k].shapes; s++)
            printf(" %d/%d%s x%u", kinds[k].shape[s].eye,
                   kinds[k].shape[s].turn,
                   kinds[k].shape[s].fly ? " FLY"
                                         : (kinds[k].shape[s].swim ? " SWIM"
                                                                   : ""),
                   kinds[k].shape[s].n);
        if (kinds[k].overflow)
            printf("  (+%u more)", kinds[k].overflow);
        printf("\n");
    }
    printf("  %u creatures on %u maps, from %u spawn records (%u name a class"
           " with no loaded implementation)\n", total, maps_with, st_records, st_nomod);
    printf("  %u found no model to read ext2 from and kept the %d stand-in\n",
           unresolved, stand_in);
    printf("  flyer -250/114, swimmer -100/114, walker ~ext2/228: %s\n",
           odd ? "MISMATCHES above" : "every creature");
    if (odd)
        bad++;

    for (k = 0; k < n_expect; k++) {
        const census_expect *e = &expect[k];
        /* Every one of that kind on that map must fall under one of its
         * expectations, and every expectation must be met at least once. */
        u32 j, covered = 0;
        bool ok;

        for (j = 0; j < n_expect; j++)
            if (strcmp(expect[j].map, e->map) == 0 &&
                strcmp(expect[j].kind, e->kind) == 0)
                covered += expect[j].hit;
        ok = e->hit > 0 && covered == e->total;
        printf("  %-8s %-9s %d/%d%s  %u of %u  %s\n", e->map, e->kind, e->eye,
               e->turn, e->fly ? " FLY" : "    ", e->hit, e->total,
               ok ? "ok" : "MISMATCH");
        if (!ok)
            bad++;
    }

    printf("  sol_pain3 plays a carried name on %u of %u Soldier maps"
           " (sol_pain2 %u, sol_pain1 %u, sol_pain3 %u)\n",
           sol_ok, sol_maps, sol_names[0], sol_names[1], sol_names[2]);
    printf("  tnk_idle1 plays tnk_step on %u of %u Tank Commander maps\n",
           tnk_ok, tnk_maps);
    if (sol_maps == 0 || sol_ok != sol_maps || tnk_maps == 0 ||
        tnk_ok != tnk_maps)
        bad++;

    return bad ? 1 : 0;
}

int cmd_creatures(const disc *d)
{
    int mi, files = disc_file_count(d);
    char seen[32][13];
    u8 seen_hash[32][SHA256_DIGEST_SIZE];
    int nseen = 0;
    int variants = 0;

    printf("Creature modules, decoded from their own code\n");

    for (mi = 0; mi < files; mi++) {
        const char *path = disc_file_at(d, mi)->path;
        q2_buf b;
        dat_archive ar;
        const dat_chunk *bin, *rel;
        u32 boff = 0, roff = 0;

        if (*path == '/') path++;
        if (strncmp(path, "Q2DATA/LEVELS/", 14) != 0 ||
            !strstr(path, ".DAT"))
            continue;
        if (disc_read_file(d, path, &b) != Q2_OK)
            continue;
        if (dat_open_buf(&ar, &b) != Q2_OK) {
            q2_buf_free(&b);
            continue;
        }

        bin = dat_find(&ar, "CreAIBin");
        rel = dat_find(&ar, "CreAIRel");

        while (bin && rel && bin->size > 4 && boff + 16 < bin->size &&
               roff + 16 < rel->size) {
            char nm[13];
            u32 bnext, rnext;
            size_t body;
            u8 *img;
            q2_reloc_stats st;
            int k, dup = -1;

            memcpy(nm, bin->data + boff, 12);
            nm[12] = 0;
            bnext = q2_rd_u32(bin->data + boff + 12);
            rnext = q2_rd_u32(rel->data + roff + 12);
            if ((bnext && (bnext <= boff + 16 || bnext > bin->size)) ||
                (rnext && (rnext <= roff + 16 || rnext > rel->size))) {
                printf("  invalid module chain: %s at %u/%u\n", path, boff, roff);
                g_reloc_fail++;
                break;
            }

            g_modules++;

            for (k = 0; k < nseen; k++)
                if (strcmp(seen[k], nm) == 0)
                    dup = k;

            {
                body = (bnext > boff ? bnext : bin->size) - boff - 16;
                img  = (u8 *)malloc(body);
                if (img) {
                    memcpy(img, bin->data + boff + 16, body);
                    if (q2_reloc_apply(img, body, rel->data + roff + 16,
                                       (rnext > roff ? rnext : rel->size)
                                           - roff - 16,
                                       CRE_BASE, &st) == Q2_OK) {
                        u8 digest[SHA256_DIGEST_SIZE];
                        sha256_buffer(img, body, digest);
                        if (dup >= 0) {
                            if (memcmp(digest, seen_hash[dup], sizeof(digest))) {
                                printf("  DIFFERENT module body: %s in %s\n", nm, path);
                                variants++;
                            }
                        } else {
                            q2_creature c;
                            printf("\n  source: %s\n", path);
                            g_img = img; g_img_size = body;
                            if (q2_creature_decode(&c, img, body, CRE_BASE, nm)) {
                                const q2_cre_impl *impl = q2_cre_impl_find(nm);
                                g_distinct++;
                                report(&c, impl);
                            }
                            if (nseen < 32) {
                                memcpy(seen[nseen], nm, 13);
                                memcpy(seen_hash[nseen], digest, sizeof(digest));
                                nseen++;
                            }
                        }
                    } else {
                        printf("\n  %-10s  RELOCATION FAILED\n", nm);
                        g_reloc_fail++;
                    }
                    free(img);
                }
            }

            if (bnext <= boff || bnext >= bin->size)
                break;
            boff = bnext;
            roff = rnext;
        }

        dat_close(&ar);
    }

    printf("\n  %d modules over the disc, %d distinct, %d relocation failures\n",
           g_modules, g_distinct, g_reloc_fail);
    printf("  %d of %d modules have native callback/frame-method or "
           "direct-think implementations\n",
           g_covered, g_distinct);
    printf("  %d same-name copies differ from the first relocated module body\n",
           variants);
    printf("  %d of %d moves carry the module's own name for them — a 20-byte"
           " {s16 first, s16 last, char[16]} record, matched by frame range\n",
           g_named, g_moves);
    printf("  %d moves the module does not name, and %d name records no decoded"
           " move claims\n", g_unnamed, g_unclaimed);
    printf("  %d of %d think indices across the disc decode to an action;"
           " a ? marks one behind a branch\n", g_actions_done, g_actions_total);
    if (g_actions_empty)
        printf("  %d of the remainder is an EMPTY function on the disc — `jr ra`"
               " and nothing else, which is an answer rather than a gap\n",
               g_actions_empty);
    printf("  d marks a generic decoded callback, not verified behavior; "
           "* marks an index with no callback\n");

    {
        int rc = g_reloc_fail ? 1 : 0;

        rc |= census_wake(d);
        return rc;
    }
}
