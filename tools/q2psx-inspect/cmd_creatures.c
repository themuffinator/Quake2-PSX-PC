#include "cmd_creatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crebind.h"
#include "creature.h"
#include "dat.h"
#include "level.h"
#include "reloc.h"

/* Where a module image is relocated for inspection. Any address works; this
 * one is far from the executable's own so a stray pointer is obvious. */
#define CRE_BASE 0x80100000u

/* The disc's level directories. Kept here rather than read from the level
 * table so the command works on a disc whose table location is unknown. */
static const char *const g_maps[] = {
    "BASE0", "BASE1", "BASE2", "BASE3", "BIGGUN", "BOSS1", "BOSS2",
    "CITY1", "CITY2", "CITY3", "COMMAND", "COMPLEX", "CORE", "FACT1",
    "FACT2", "FACT3", "HANGAR1", "HANGAR2", "JAIL2", "JAIL3", "JAIL4",
    "JAIL5", "LAB", "MAGDEMO", "MINE1", "MINE2", "MINE3", "MINE4",
    "MINTRO", "POWER1", "POWER2", "SECURITY", "SEWER1", "SPACE",
    "STRIKE", "TRAIN", "WARE1", "WARE2", "WASTE1", "WASTE2", "WASTE3",
    "WASTE4", NULL
};

static int g_modules;
static int g_distinct;
static int g_covered;
static int g_reloc_fail;
static int g_actions_total;
static int g_actions_done;
static int g_actions_empty;
static const u8 *g_img;
static size_t g_img_size;
static bool g_last_full;
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
                                       (int)q2_rd_u16((const u8 *)un[z] + 16),
                                       (int)q2_rd_u16((const u8 *)un[z] + 18),
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
        /*
         * An index is covered when the implementation writes it OR the generic
         * one runs the decoded action, which is what a PARTIAL transcription
         * relies on and what `q2_creature_bind` falls back to. Counting only
         * `impl->method` reported "Tankcomm — 0 of 7 indices" for a creature
         * whose every index acts and whose attack is hand-written: it read as
         * missing work and was the opposite of the truth.
         */
        bool own  = impl && impl->method[idx[i]] != NULL;
        bool acts = own || q2_cre_generic.method[idx[i]] != NULL;

        printf(" %u%s", idx[i], own ? "" : (acts ? "d" : "*"));
        if (!acts)
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
        g_last_full = (n > 0 && got == n);
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

        printf("    port      : %s — %u of %u callbacks written by hand, "
               "%u of %u think indices act (d = decoded)\n",
               impl->name, cb_own, cb_have, n - missing, n);
        if (missing == 0)
            g_covered++;
    } else {
        printf("    port      : decoded actions — %s\n",
               g_last_full ? "every index acts"
                           : "some indices do not decode");
        if (g_last_full)
            g_covered++;
    }
}

int cmd_creatures(const disc *d)
{
    int mi;
    char seen[32][13];
    int nseen = 0;

    printf("Creature modules, decoded from their own code\n");

    for (mi = 0; g_maps[mi]; mi++) {
        char path[160];
        q2_buf b;
        q2_common_file cf;
        const dat_chunk *bin, *rel;
        u32 boff = 0, roff = 0;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", g_maps[mi]);
        if (disc_read_file(d, path, &b) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &b) != Q2_OK) {
            q2_buf_free(&b);
            continue;
        }

        bin = cf.chunk[q2_common_chunk_index("CreAIBin")];
        rel = cf.chunk[q2_common_chunk_index("CreAIRel")];

        while (bin && rel && bin->size > 4 && boff + 16 < bin->size) {
            char nm[13];
            u32 bnext, rnext;
            size_t body;
            u8 *img;
            q2_reloc_stats st;
            int k, dup = 0;

            memcpy(nm, bin->data + boff, 12);
            nm[12] = 0;
            bnext = q2_rd_u32(bin->data + boff + 12);
            rnext = q2_rd_u32(rel->data + roff + 12);

            g_modules++;

            for (k = 0; k < nseen; k++)
                if (strcmp(seen[k], nm) == 0)
                    dup = 1;

            if (!dup) {
                body = (bnext > boff ? bnext : bin->size) - boff - 16;
                img  = (u8 *)malloc(body);
                if (img) {
                    memcpy(img, bin->data + boff + 16, body);
                    if (q2_reloc_apply(img, body, rel->data + roff + 16,
                                       (rnext > roff ? rnext : rel->size)
                                           - roff - 16,
                                       CRE_BASE, &st) == Q2_OK) {
                        q2_creature c;
                        g_img = img; g_img_size = body;
                        if (q2_creature_decode(&c, img, body, CRE_BASE, nm)) {
                            const q2_cre_impl *impl = q2_cre_impl_find(nm);
                            g_distinct++;
                            report(&c, impl);
                        }
                    } else {
                        printf("\n  %-10s  RELOCATION FAILED\n", nm);
                        g_reloc_fail++;
                    }
                    free(img);
                }
                if (nseen < 32) {
                    memcpy(seen[nseen], nm, 13);
                    nseen++;
                }
            }

            if (bnext <= boff || bnext >= bin->size)
                break;
            boff = bnext;
            roff = rnext;
        }

        q2_common_close(&cf);
    }

    printf("\n  %d modules over the disc, %d distinct, %d relocation failures\n",
           g_modules, g_distinct, g_reloc_fail);
    printf("  %d of %d creatures act: animations off the disc, and every think"
           " index they use resolves to a real action\n",
           g_covered, g_distinct);
    printf("  %d of %d moves carry the module's own name for them — a 20-byte"
           " {char[16], u16 first, u16 last} record, matched by frame range\n",
           g_named, g_moves);
    printf("  %d moves the module does not name, and %d name records no decoded"
           " move claims\n", g_unnamed, g_unclaimed);
    printf("  %d of %d think indices across the disc decode to an action;"
           " a ? marks one behind a branch\n", g_actions_done, g_actions_total);
    if (g_actions_empty)
        printf("  %d of the remainder is an EMPTY function on the disc — `jr ra`"
               " and nothing else, which is an answer rather than a gap\n",
               g_actions_empty);
    printf("  a * marks an index with no hand transcription — it runs from the"
           " decoded action instead\n");

    return g_reloc_fail ? 1 : 0;
}
