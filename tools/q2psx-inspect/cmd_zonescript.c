/*
 * cmd_zonescript — which script does a trigger volume fire, and what names does
 * a zone's own script carry?
 *
 * Two scripts exist per map. COMMON.DAT has an Events chunk and so does every
 * ZONE*.DAT, and only COMMON has the trigger volumes. A zone's carries 2959
 * CALL items, 805 movers and 619 zone gates across the disc, and this port ran
 * none of them — which looked like the largest piece of level behaviour still
 * missing.
 *
 * ** THE PARAGRAPH THAT USED TO BE HERE WAS WRONG. ** It said "the engine never
 * loads a zone's Events chunk", and listed the zone loader's chunks as AreaConx,
 * CastList, CreAIBin, CreAIRel, MapMod, MapNames, Points, Scene, SortData,
 * SpaceLights and the two hulls, concluding `Events` was not among them.
 *
 * `Events` IS among them. The zone loader's own name run is:
 *
 *     CastList  Events  CreAIBin  CreAIRel  SecondaryCol  SecondaryRem
 *     MapNames  SpaceLights  SortData  Scene  MapMod  Points  AreaConx
 *
 * There are two Events LOADERS, not one. COMMON's at 0x8007AC30 stores into
 * gp+372 (0x800AE774); the ZONE's at 0x8007C14C looks the same string up with
 * base *(gp+18856) — the zone file — and stores into gp+376 (0x8007C234). The
 * old note was right that the string has two references and wrong about what the
 * second one does, and that error cost this port most of its rotating geometry:
 * a rotation CALL reads its object slots from gp+376 while STAMPING -1 into
 * gp+372 as it consumes them (0x800285F4 / 0x8002861C), so parsing COMMON alone
 * sees an empty call every time. See openquestions #56.
 *
 * Two counting tests were run before that and BOTH decided nothing, which is
 * worth keeping so neither is repeated: all 834 trigger offsets start a record
 * in COMMON's script *and* in a zone's, and none runs past the end of either.
 * An offset is just a number, and record starts are dense.
 *
 *
 * What is left for this command to do is measure the script that does run:
 * COMMON's, fired by every trigger volume, with the rotators built from the
 * same chunk the engine's global points at.
 */
#include "cmd_zonescript.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "events_rt.h"
#include "level.h"
#include "rotator.h"
#include "scene.h"
#include "entity.h"
#include "population.h"
#include "levelbin.h"
#include "trigger.h"
#include "userfuncs.h"

static const char *const g_maps[] = {
    "BASE0", "BASE1", "BASE2", "BASE3", "BIGGUN", "BOSS1", "BOSS2",
    "CITY1", "CITY2", "CITY3", "COMMAND", "COMPLEX", "CORE", "FACT1",
    "FACT2", "FACT3", "HANGAR1", "HANGAR2", "JAIL2", "JAIL3", "JAIL4",
    "JAIL5", "LAB", "MAGDEMO", "MINE1", "MINE2", "MINE3", "MINE4",
    "MINTRO", "POWER1", "POWER2", "SECURITY", "SEWER1", "SPACE",
    "STRIKE", "TRAIN", "WARE1", "WARE2", "WASTE1", "WASTE2", "WASTE3",
    "WASTE4", NULL
};

/* The client's hook, run here over COMMON's script instead of live play. */
typedef struct live_rot_ctx {
    const q2_userfuncs *uf;
    q2_rotator_set     *set;
    u32                 steps;
    u32                 rot_fired;   /* rotation CALLs the script actually ran */
    u32                 rot_barren;  /* ...of those, ones that turned nothing  */
    u32                 lit_run;     /* light primitives the sweep actually ran */
    /* GLASS calls the trigger sweep reaches, and how many resolve a Scene node
     * to break. A pane the script never calls is not a gap; one it calls and
     * cannot resolve is. */
    u32                 glass_run;
    u32                 glass_node;
    /* LOADMAP: the level-to-level transition, and whether a trigger reaches
     * one. Counted here because "the chunk contains a LOADMAP" and "a player
     * walking the map runs one" are the two different questions, and only the
     * second says level progression is reachable. */
    u32                 loadmap_run;
    u32                 secret_run;   /* INSECRET calls the sweep reaches */
    /* Every primitive the sweep reaches, by kind. The port acts on a handful
     * and the rest are silently nothing; a histogram is the only thing that
     * says WHICH, and it is the list of what is left to build. */
    u32                *prim_run;
    const q2_scene     *scene;
    q2_uf_operands      ops;

    /* Which item offsets turned something, so a call barren under THIS zone can
     * be told apart from one barren under every zone the map ships. */
    const u8           *chunk;
    u8                 *turned;      /* one byte per chunk offset */
    u8                 *seen;
} live_rot_ctx;

static void live_rot_call(void *user, const q2_event_item *item, u8 call_index)
{
    live_rot_ctx *ctx = (live_rot_ctx *)user;

    u32 hit = q2_rotators_call(ctx->set, ctx->uf, item, call_index);
    q2_uf_prim prim = q2_userfuncs_prim(ctx->uf, call_index);

    ctx->steps += hit;

    if (ctx->prim_run && prim >= 0 && prim < Q2_UF_PRIM_COUNT)
        ctx->prim_run[prim]++;

    /*
     * A call that names no object is only a gap if the script ever RUNS it.
     * Count the rotation primitives the trigger sweep actually reaches, and how
     * many of those turned nothing, so "54 calls are empty" can be separated
     * from "54 calls are dead script".
     */
    if (prim == Q2_UF_TIMEDLIGHT || prim == Q2_UF_FLKLIGHT)
        ctx->lit_run++;

    if (prim == Q2_UF_LOADMAP)
        ctx->loadmap_run++;

    if (prim == Q2_UF_INSECRET)
        ctx->secret_run++;

    /*
     * GLASS, resolved the way `q2_sim_breakable_call` resolves it — the same
     * operand rebase and the same Scene lookup — so this counts the panes a
     * player who has walked the whole map would actually shatter, rather than
     * the ones the chunk merely contains.
     */
    if (prim == Q2_UF_GLASS && item->len >= 16 && item->payload) {
        const u8 *pp = q2_uf_operand_at(&ctx->ops, item->payload - 2, 16);
        s16 slot = q2_rd_s16(pp + 4);

        ctx->glass_run++;
        if (slot >= 0 && ctx->scene &&
            (u32)slot < ctx->scene->node_count)
            ctx->glass_node++;
    }

    if (prim == Q2_UF_SIMROT || prim == Q2_UF_SIMROT2 ||
        prim == Q2_UF_ROTHATCH || prim == Q2_UF_ROTBUTTON) {
        ctx->rot_fired++;
        if (!hit)
            ctx->rot_barren++;

        if (ctx->turned && ctx->chunk && item->payload) {
            size_t off = (size_t)(item->payload - ctx->chunk);
            ctx->seen[off] = 1;
            if (hit)
                ctx->turned[off] = 1;
        }
    }
}

/* Does `offset` name the start of a record in this script? */
static bool offset_is_record(const q2_events *ev, u32 offset)
{
    q2_event_record rec;

    if (!q2_events_first_record(ev, &rec))
        return false;

    do {
        if (rec.offset == offset)
            return true;
    } while (q2_events_next_record(ev, &rec, &rec));

    return false;
}

int cmd_zonescript(const disc *d, const char *only_map)
{
    int mi;
    u32 t_total = 0, t_common = 0, t_zone = 0, t_both = 0, t_neither = 0;
    u32 zone_dirs = 0, zone_records = 0, common_records = 0;
    u32 past_common = 0, past_zone = 0;
    u32 zone_same = 0, zone_diff = 0;
    u32 rot_prim_calls = 0, rot_too_short = 0, rot_no_object = 0,
        rot_usable = 0, rot_zone_rescue = 0, rot_zone_inrange = 0,
        rot_zone_slots = 0, rot_zone_nonneg = 0;
    u32 live_rot_fired = 0, live_rot_barren = 0;
    u32 rot_any_zone = 0, rot_no_zone = 0;
    u32 light_timed = 0, light_flk = 0, live_lit_run = 0;
    /* The breakables, counted the same way the rotators are, because they have
     * the same shape of operand and the same two-buffer rebase applies. */
    u32 brk_calls = 0, brk_short = 0, brk_no_object = 0, brk_usable = 0,
        brk_zone_rescue = 0;
    u32 live_glass_run = 0, live_glass_node = 0;
    u32 loadmap_calls = 0, live_loadmap = 0;
    u32 laser_calls = 0, laser_lit = 0, laser_dark = 0, laser_clamped = 0;
    u32 laser_zone_lit = 0, laser_always = 0;
    u32 misevent_calls = 0, misevent_known = 0, misevent_exe = 0;
    u32 objslot_items = 0, objslot_common = 0, objslot_zone = 0;
    u32 objslot_nowhere = 0;
    u32 misevent_tables = 0, misevent_records = 0;
    u32 laser_kind[6];
    u32 secret_calls = 0, live_secret = 0;
    /*
     * CREBATCH names a Population GROUP. Whether that group claims a zone is
     * the whole question: a group named `Zone<N>` is that zone's own
     * population and the level starts with it standing there, while a group
     * named `ShotgunRoom` or `BerserkHide` is a batch a script summons. If the
     * calls name the second kind, then gating them is right and needs no
     * LevelBin.
     */
    u32 crebatch_calls = 0, crebatch_found = 0, crebatch_zone = 0,
        crebatch_batch = 0;
    /* Which groups a map's own LevelBin names — the initial selection the
     * console makes and this port could only guess at. See levelbin.h. */
    u32 lb_maps = 0, lb_groups = 0, lb_zone = 0, lb_batch = 0;
    u32 lb_sel_calls = 0, lb_sel_ok = 0, lb_sel_zone = 0, lb_sel_batch = 0;
    u32 slot_hit[128], slot_calls[128];
    u32 prim_run[Q2_UF_PRIM_COUNT];
    u32 prim_have[Q2_UF_PRIM_COUNT];
    u32 loadmap_map_ok = 0, loadmap_map_missing = 0,
        loadmap_start_ok = 0, loadmap_late_zone = 0;
    u32 live_built = 0, live_calls = 0, live_steps = 0, live_moved = 0,
        live_turned = 0;
    bool verbose = (only_map != NULL);

    memset(slot_hit, 0, sizeof(slot_hit));
    memset(slot_calls, 0, sizeof(slot_calls));
    memset(laser_kind, 0, sizeof(laser_kind));
    memset(prim_run, 0, sizeof(prim_run));
    memset(prim_have, 0, sizeof(prim_have));

    printf("Which Events chunk does a trigger volume fire?\n\n");

    for (mi = 0; g_maps[mi]; mi++) {
        char path[160];
        q2_buf cbuf;
        q2_common_file cf;
        q2_events cev;
        q2_triggers tg;
        q2_events zev[8];
        q2_buf zbuf[8];
        q2_zone_file zf[8];
        u32 zcount = 0, zi, k;
        bool have_common = false, have_trig = false;

        if (only_map && strcmp(only_map, g_maps[mi]) != 0)
            continue;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", g_maps[mi]);
        if (disc_read_file(d, path, &cbuf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &cbuf) != Q2_OK) {
            q2_buf_free(&cbuf);
            continue;
        }

        have_common = (q2_events_parse_common(&cev, &cf) == Q2_OK);
        have_trig   = (q2_triggers_parse(&tg, &cf) == Q2_OK);

        /* Every zone of this map, so an offset can be tried against each. */
        for (zi = 0; zi < 8; zi++) {
            snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/ZONE%u.DAT",
                     g_maps[mi], zi);
            if (disc_read_file(d, path, &zbuf[zcount]) != Q2_OK)
                continue;
            if (q2_zone_open(&zf[zcount], &zbuf[zcount]) != Q2_OK) {
                q2_buf_free(&zbuf[zcount]);
                continue;
            }
            if (q2_events_parse_zone(&zev[zcount], &zf[zcount]) != Q2_OK) {
                q2_zone_close(&zf[zcount]);
                continue;
            }
            zcount++;
        }

        if (have_common)
            common_records += cev.record_count;

        /*
         * The stronger test: an offset PAST THE END of a chunk cannot be an
         * offset into it, whatever records happen to start where. Record-start
         * membership proved nothing — every offset on the disc starts a record
         * in both scripts — but a chunk's size is not a coincidence.
         */
        if (have_common && have_trig) {
            u32 hi = 0;

            for (k = 0; k < tg.count; k++) {
                q2_trigger tr;

                if (!q2_trigger_get(&tg, k, &tr))
                    continue;
                if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                    continue;
                if (tr.event_offset > hi)
                    hi = tr.event_offset;
            }

            if (hi >= cev.size)
                past_common++;
            for (zi = 0; zi < zcount; zi++)
                if (hi >= zev[zi].size) {
                    past_zone++;
                    break;
                }

            if (verbose)
                printf("  highest trigger offset %u; COMMON events %u bytes, "
                       "zone0 events %u bytes\n",
                       hi, cev.size, zcount ? zev[0].size : 0);
        }

        if (verbose)
            printf("%s: COMMON %u records, %u named; %u zones\n",
                   g_maps[mi], have_common ? cev.record_count : 0,
                   have_common ? cev.dir_count : 0, zcount);

        for (zi = 0; zi < zcount; zi++) {
            zone_records += zev[zi].record_count;
            zone_dirs    += zev[zi].dir_count;

            /*
             * Is a zone's script a COPY of COMMON's? BASE0's is the same 604
             * bytes, the same 23 records and the same two named entries, which
             * would explain why every offset resolves in both at once.
             */
            if (have_common) {
                if (zev[zi].size == cev.size && cev.size &&
                    memcmp(zev[zi].data, cev.data, cev.size) == 0)
                    zone_same++;
                else
                    zone_diff++;
            }

            if (verbose) {
                printf("  ZONE%u: %u records, %u named\n",
                       zi, zev[zi].record_count, zev[zi].dir_count);
                for (k = 0; k < zev[zi].dir_count && k < 24; k++) {
                    q2_event_dir_entry e;
                    if (q2_events_get_dir_entry(&zev[zi], k, &e))
                        printf("    %-13s +%u\n", e.name, e.offset);
                }
            }
        }

        if (verbose && have_common) {
            printf("  COMMON named:\n");
            for (k = 0; k < cev.dir_count && k < 24; k++) {
                q2_event_dir_entry e;
                if (q2_events_get_dir_entry(&cev, k, &e))
                    printf("    %-13s +%u\n", e.name, e.offset);
            }
        }

        /*
         * What the console actually runs: COMMON's script, fired by the
         * trigger volumes. Every volume with an event is fired once, which is
         * a player who has walked the whole map, and the rotators are built
         * from the same chunk the engine's global points at.
         */
        if (have_common && have_trig) {
            q2_userfuncs   uf;
            q2_rotator_set rs;
            q2_scene       gscene;
            u32            best_zone = 0;   /* the zone the probe preferred */
            q2_event_rt    rt;

            /*
             * Coverage, stated rather than left to be inferred: how many CALL
             * items in this map's script name a rotation primitive, against how
             * many rotators get built from them. A SIMROT names up to four
             * objects and builds one rotator each, so `built` is normally the
             * larger; what matters is that no rotation call is skipped.
             */
            {
                q2_event_record rec;
                q2_levelbin_misevent mev[32];
                u32 mev_count = 0;

                {
                    const dat_chunk *mlb = cf.chunk[Q2_COMMON_LEVEL_BIN];

                    if (mlb && mlb->data && mlb->size) {
                        mev_count = q2_levelbin_misevents(mlb->data, mlb->size,
                                                          0, mev, 32);
                        if (mev_count > 32)
                            mev_count = 32;
                        misevent_tables += mev_count ? 1 : 0;
                        misevent_records += mev_count;
                        if (verbose) {
                            u32 mq;
                            for (mq = 0; mq < mev_count; mq++)
                                printf("    %s: LevelBin event '%s'"
                                       " -> module+0x%04X (record +0x%04X)\n",
                                       g_maps[mi], mev[mq].name,
                                       mev[mq].handler, mev[mq].offset);
                        }
                    }
                }

                if (q2_userfuncs_parse(&uf, &cf) == Q2_OK &&
                    q2_events_first_record(&cev, &rec)) {
                    do {
                        u32 it;

                        for (it = 0; it < rec.n_items; it++) {
                            q2_event_item item;
                            q2_uf_call call;

                            if (!q2_events_get_item(&cev, &rec, it, &item))
                                break;
                            if (!item.payload ||
                                (item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                                continue;
                            if (q2_uf_decode_call(&call, &uf, &item) != Q2_OK)
                                continue;

                            {
                                const u8 *pp = item.payload - 2;
                                u32 need = 0;
                                s16 first_obj = -1;

                                if (call.prim >= 0 &&
                                    call.prim < Q2_UF_PRIM_COUNT)
                                    prim_have[call.prim]++;

                                /*
                                 * LASERBEAM, which no trigger reaches, and the
                                 * reason it needs none.
                                 *
                                 * Its exec (0x8002E6C0) tests bit 0 of the
                                 * first endpoint's X, and its constructor
                                 * (0x8002E744) fills that word from the OTHER
                                 * buffer — the zone's script — at load. So the
                                 * enable flag is PER ZONE, and the same beam is
                                 * lit in one room and dark in the next without
                                 * anything firing. JAIL2's corridor grid is
                                 * X=7352 in COMMON and in zone 0, and 7353 in
                                 * zones 1 and 2: the coordinate with the bit
                                 * set, one unit wide of nothing.
                                 *
                                 * Counting COMMON's copy alone therefore reads
                                 * 41 of 72 beams as dark and is simply the
                                 * wrong buffer. This counts per zone.
                                 */
                                /*
                                 * #85, measured rather than reasoned about.
                                 *
                                 * OBJDRAWOFF's constructor (0x8002BD58) STAMPS
                                 * -1 into the working buffer's slot and reads
                                 * the authored Scene node index out of the
                                 * PRISTINE one:
                                 *
                                 *   8002BDA0  addiu v0, zero, -1
                                 *   8002BDA4  sh    v0, 0(s1)   ; working = -1
                                 *   8002BDA8  lh    v0, 0(s0)   ; pristine!
                                 *   8002BDB0  bltz  v0, skip
                                 *
                                 * So -1 in COMMON is not a missing object. It
                                 * is what a slot is SUPPOSED to read there.
                                 * This prints COMMON's value beside every
                                 * zone's, which is the only thing that says
                                 * whether the authored value exists at all.
                                 */
                                if (call.prim == Q2_UF_OBJDRAWOFF ||
                                    call.prim == Q2_UF_BUTTON ||
                                    call.prim == Q2_UF_PLATFORM ||
                                    call.prim == Q2_UF_PISTON ||
                                    call.prim == Q2_UF_LIFT1 ||
                                    call.prim == Q2_UF_CAGELIFT1) {
                                    size_t boff = (size_t)(pp - cev.data);
                                    u32 zq, so, sq;
                                    bool any = false, common_any = false;
                                    /* The slot's own offset, per primitive —
                                     * OBJDRAWOFF's is +4, BUTTON's +12,
                                     * PLATFORM's +20, and reading +4 for all
                                     * three is reading the invert flag and a
                                     * coordinate. PISTON alone among this
                                     * group owns four slots, at +8..+14. */
                                    u32 sl = 4, slot_count = 1;

                                    if (call.prim == Q2_UF_BUTTON)   sl = 12;
                                    if (call.prim == Q2_UF_PLATFORM) sl = 20;
                                    if (call.prim == Q2_UF_LIFT1 ||
                                        call.prim == Q2_UF_CAGELIFT1) sl = 8;
                                    if (call.prim == Q2_UF_PISTON) {
                                        sl = 8;
                                        slot_count = 4;
                                    }

                                    so = sl + 2 * slot_count;
                                    if (item.len < so)
                                        continue;

                                    objslot_items++;
                                    for (sq = 0; sq < slot_count; sq++) {
                                        if (q2_rd_s16(pp + sl + 2 * sq) >= 0)
                                            common_any = true;
                                    }
                                    if (common_any)
                                        objslot_common++;

                                    for (zq = 0; zq < zcount; zq++) {
                                        if (boff + so > zev[zq].size)
                                            continue;
                                        for (sq = 0; sq < slot_count; sq++) {
                                            if (q2_rd_s16(zev[zq].data + boff + sl +
                                                          2 * sq) >= 0)
                                                any = true;
                                        }
                                    }
                                    if (any)
                                        objslot_zone++;
                                    else
                                        objslot_nowhere++;

                                    if (verbose) {
                                        printf("    %s: %-10s slots COMMON",
                                               g_maps[mi],
                                               call.info ? call.info->name
                                                         : "?");
                                        for (sq = 0; sq < slot_count; sq++)
                                            printf(" %d", q2_rd_s16(pp + sl +
                                                                       2 * sq));
                                        for (zq = 0; zq < zcount; zq++) {
                                            if (boff + so > zev[zq].size) {
                                                printf(", zone%u SHORT", zq);
                                                continue;
                                            }
                                            printf(", zone%u", zq);
                                            for (sq = 0; sq < slot_count; sq++)
                                                printf(" %d", q2_rd_s16(
                                                    zev[zq].data + boff + sl +
                                                    2 * sq));
                                        }
                                        printf("\n");
                                    }
                                }

                                if (call.prim == Q2_UF_LASERBEAM &&
                                    item.len == 36) {
                                    size_t boff = (size_t)(pp - cev.data);
                                    s16 st = q2_rd_s16(pp + 34);
                                    u32 zq, lit = 0;

                                    laser_calls++;
                                    if (st >= 0 && st < 6)
                                        laser_kind[st]++;
                                    else
                                        laser_clamped++;

                                    for (zq = 0; zq < zcount; zq++) {
                                        if (boff + 36 > zev[zq].size)
                                            continue;
                                        if (q2_rd_s32(zev[zq].data + boff + 4)
                                            & 1)
                                            lit++;
                                    }

                                    laser_zone_lit += lit;
                                    if (lit)
                                        laser_lit++;
                                    else
                                        laser_dark++;
                                    if (zcount && lit == zcount)
                                        laser_always++;

                                    if (verbose)
                                        printf("    %s: LASERBEAM "
                                               "(%d,%d,%d)->(%d,%d,%d)"
                                               " kind %d, lit in %u of %u "
                                               "zones\n",
                                               g_maps[mi],
                                               q2_rd_s32(pp + 4),
                                               q2_rd_s32(pp + 8),
                                               q2_rd_s32(pp + 12),
                                               q2_rd_s32(pp + 20),
                                               q2_rd_s32(pp + 24),
                                               q2_rd_s32(pp + 28),
                                               (int)st, lit, zcount);
                                }

                                /*
                                 * MISEVENT's key, against the namespace the
                                 * executable actually carries: a three-record
                                 * table of `name[12] + handler` at 0x8009B680,
                                 * NUL-terminated, which 0x800419A0 searches
                                 * with the 12-byte compare at 0x8006DB10. It is
                                 * not a Strings key and never was.
                                 */
                                if (call.prim == Q2_UF_MISEVENT) {
                                    char kn[Q2_UF_NAME_LEN + 1];

                                    misevent_calls++;
                                    if (q2_uf_operand_name(&call, 0, kn) &&
                                        kn[0]) {
                                        u32 mk;
                                        bool found =
                                            q2_misevent_find(kn) != NULL;

                                        if (found)
                                            misevent_exe++;

                                        /*
                                         * And the map's own table, recovered
                                         * from its LevelBin. `load_base` is 0
                                         * because the chunk here is UNRELOCATED
                                         * and its handler words are still the
                                         * module-relative offsets the fixups
                                         * would turn into addresses.
                                         */
                                        for (mk = 0; !found && mk < mev_count;
                                             mk++)
                                            if (strcmp(mev[mk].name, kn) == 0)
                                                found = true;

                                        if (found)
                                            misevent_known++;
                                        else if (verbose) {
                                            const dat_chunk *xl =
                                                cf.chunk[Q2_COMMON_LEVEL_BIN];
                                            u32 at;
                                            bool raw = false;
                                            size_t kl = strlen(kn);

                                            printf("    %s: MISEVENT '%s'"
                                                   " - in NEITHER table\n",
                                                   g_maps[mi], kn);

                                            if (xl && xl->data)
                                                for (at = 0;
                                                     at + 16 <= xl->size; at++)
                                                    if (memcmp(xl->data + at,
                                                               kn, kl) == 0) {
                                                        printf("      module"
                                                               " holds it at"
                                                               " +0x%04X,"
                                                               " +12 word"
                                                               " 0x%08X\n",
                                                               at,
                                                               q2_rd_u32(
                                                                 xl->data + at
                                                                 + 12));
                                                        raw = true;
                                                        break;
                                                    }
                                            if (!raw)
                                                printf("      the module does"
                                                       " not hold it\n");
                                        }
                                    }
                                }

                                if (call.prim == Q2_UF_CREBATCH) {
                                    char gname[Q2_UF_NAME_LEN + 1];

                                    crebatch_calls++;
                                    if (q2_uf_operand_name(&call, 0, gname) &&
                                        gname[0]) {
                                        q2_population pop;
                                        u32 gi;

                                        if (q2_population_parse(&pop, &cf) == Q2_OK) {
                                            for (gi = 0; gi < pop.group_count; gi++) {
                                                q2_pop_group g;

                                                if (!q2_pop_get_group(&pop, gi, &g))
                                                    continue;
                                                if (strcmp(g.name, gname) != 0)
                                                    continue;

                                                crebatch_found++;
                                                if (q2_pop_group_zone(&g) >= 0)
                                                    crebatch_zone++;
                                                else
                                                    crebatch_batch++;
                                                if (verbose)
                                                    printf("    %s: CREBATCH '%s'"
                                                           " -> zone %d\n",
                                                           g_maps[mi], gname,
                                                           q2_pop_group_zone(&g));
                                                break;
                                            }
                                        }
                                    }
                                }

                                if (call.prim == Q2_UF_INSECRET)
                                    secret_calls++;

                                if (call.prim == Q2_UF_LOADMAP) {
                                    char mname[Q2_UF_NAME_LEN + 1];
                                    char sname[Q2_UF_NAME_LEN + 1];

                                    loadmap_calls++;

                                    /*
                                     * Both names, resolved the way the client
                                     * resolves them: the map against the disc,
                                     * and the start position against the
                                     * TARGET map's spawns rather than this
                                     * one's. Getting the second namespace
                                     * wrong is silent - it lands the player at
                                     * the level's own start instead of in the
                                     * doorway - so it is counted rather than
                                     * assumed.
                                     */
                                    if (q2_uf_operand_name(&call, 0, mname) &&
                                        mname[0]) {
                                        char   tp[256];
                                        q2_buf tb;

                                        if (!q2_uf_operand_name(&call, 1, sname))
                                            sname[0] = '\0';

                                        snprintf(tp, sizeof(tp),
                                                 "Q2DATA/LEVELS/%s/COMMON.DAT",
                                                 mname);
                                        if (disc_read_file(d, tp, &tb) == Q2_OK) {
                                            q2_common_file tcf;

                                            loadmap_map_ok++;
                                            if (q2_common_open(&tcf, &tb) == Q2_OK) {
                                                q2_start_pos_list sl;
                                                q2_start_pos sp;

                                                if (sname[0] &&
                                                    q2_start_pos_parse(&sl, &tcf) == Q2_OK &&
                                                    q2_start_pos_find(&sl, sname, &sp)) {
                                                    loadmap_start_ok++;
                                                    if (sp.zone > 0)
                                                        loadmap_late_zone++;
                                                }
                                                q2_common_close(&tcf);
                                            } else {
                                                q2_buf_free(&tb);
                                            }
                                        } else {
                                            loadmap_map_missing++;
                                        }
                                    }
                                }

                                if (call.prim == Q2_UF_TIMEDLIGHT)
                                    light_timed++;
                                else if (call.prim == Q2_UF_FLKLIGHT)
                                    light_flk++;

                                switch (call.prim) {
                                case Q2_UF_SIMROT:
                                case Q2_UF_SIMROT2:
                                    need = 24;
                                    /* ANY of the four, since a negative slot is
                                     * skipped rather than terminating the loop
                                     * (0x80028628 branches to the increment). */
                                    if (item.len >= need) {
                                        u32 sl;

                                        for (sl = 0; sl < 4; sl++) {
                                            s16 nd = q2_rd_s16(pp + 12 + 2 * (s32)sl);

                                            if (nd >= 0) {
                                                first_obj = nd;
                                                break;
                                            }
                                        }

                                        /*
                                         * If COMMON's copy has nothing, ask the
                                         * ZONE's copy at the same offset.
                                         *
                                         * 0x800285CC..0x800285F4 sets up TWO
                                         * cursors: s1 = item + 12 into the chunk
                                         * at gp+372, and s2 = that same offset
                                         * rebased into the chunk at gp+376. The
                                         * loop READS the slot from s2 and stamps
                                         * -1 into s1. So the buffer we parse need
                                         * not be the buffer the operand lives in.
                                         */
                                        if (first_obj < 0) {
                                            u32 off = (u32)(pp - cev.data);
                                            u32 zj;

                                            bool any_in_range = false;

                                            for (zj = 0; zj < zcount; zj++) {
                                                if (off + need > zev[zj].size)
                                                    continue;
                                                any_in_range = true;
                                                for (sl = 0; sl < 4; sl++) {
                                                    s16 q = q2_rd_s16(
                                                        zev[zj].data + off +
                                                        12 + 2 * (s32)sl);
                                                    rot_zone_slots++;
                                                    if (q >= 0)
                                                        rot_zone_nonneg++;
                                                }
                                            }
                                            if (any_in_range)
                                                rot_zone_inrange++;

                                            for (zj = 0; zj < zcount &&
                                                         first_obj < 0; zj++) {
                                                if (off + need > zev[zj].size)
                                                    continue;
                                                for (sl = 0; sl < 4; sl++) {
                                                    s16 nd = q2_rd_s16(
                                                        zev[zj].data + off +
                                                        12 + 2 * (s32)sl);
                                                    if (nd >= 0) {
                                                        first_obj = nd;
                                                        rot_zone_rescue++;
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    break;
                                case Q2_UF_ROTHATCH:
                                    need = 20;
                                    if (item.len >= need)
                                        first_obj = q2_rd_s16(pp + 18);
                                    break;
                                case Q2_UF_ROTBUTTON:
                                    need = 12;
                                    if (item.len >= need)
                                        first_obj = q2_rd_s16(pp + 10);
                                    break;
                                default:
                                    break;
                                }

                                /*
                                 * GLASS and SHOOTTHEN, whose object slot is a
                                 * single s16 at +4 rather than the rotators'
                                 * four at +12. Counted separately because they
                                 * answer a different question: a breaking pane
                                 * is what `q2_sim_debris_burst` is waiting for
                                 * a caller from, and whether it CAN be wired
                                 * turns on whether these slots resolve at all.
                                 *
                                 * The two-buffer rebase (#56) is applied here
                                 * too: an operand the game has already run is
                                 * -1 in COMMON's copy and live in the zone's.
                                 */
                                if (call.prim == Q2_UF_GLASS ||
                                    call.prim == Q2_UF_SHOOTTHEN) {
                                    s16 obj = -1;

                                    brk_calls++;
                                    if (item.len < 6) {
                                        brk_short++;
                                    } else {
                                        obj = q2_rd_s16(pp + 4);
                                        if (obj < 0) {
                                            u32 off = (u32)(pp - cev.data);
                                            u32 zj;

                                            for (zj = 0; zj < zcount &&
                                                         obj < 0; zj++) {
                                                if (off + 6 > zev[zj].size)
                                                    continue;
                                                obj = q2_rd_s16(zev[zj].data +
                                                                off + 4);
                                                if (obj >= 0)
                                                    brk_zone_rescue++;
                                            }
                                        }
                                        if (obj < 0)
                                            brk_no_object++;
                                        else
                                            brk_usable++;
                                    }
                                }

                                if (!need)
                                    continue;

                                rot_prim_calls++;
                                if (item.len < need)
                                    rot_too_short++;
                                else if (first_obj < 0)
                                    rot_no_object++;
                                else
                                    rot_usable++;
                            }
                        }
                    } while (q2_events_next_record(&cev, &rec, &rec));
                }
            }

            memset(&rs, 0, sizeof(rs));
            /*
             * Operands come from the ZONE's Events chunk, at the same offset —
             * `gp+376`, which the zone loader fills at 0x8007C234 after looking
             * "Events" up at 0x8007C14C. Set it before building so the build
             * reads the slots the engine reads. See #56.
             */
            /*
             * Per ZONE, not per map: the engine loads one zone at a time, so the
             * same COMMON script drives different geometry depending on which
             * zone is resident. Take the zone that yields the most rotators —
             * for a map whose zones are byte-identical to COMMON that is any of
             * them, and the count is unchanged.
             */
            /*
             * Sweep EVERY zone, not just the best one. A call that turns nothing
             * with zone 0 resident may turn something with zone 3, and only a
             * call barren under every zone the map ships is genuinely missing.
             */
            /*
             * Which groups does this map's own LevelBin name?
             *
             * That is the initial selection the console makes and #79 could
             * only approximate by a group's name claiming a zone. Reading it
             * needs no interpreter — see levelbin.h.
             */
            {
                const dat_chunk *lb = cf.chunk[Q2_COMMON_LEVEL_BIN];
                q2_population    pop;

                if (lb && lb->data && lb->size &&
                    q2_population_parse(&pop, &cf) == Q2_OK) {
                    u32 gi, named = 0;

                    for (gi = 0; gi < pop.group_count; gi++) {
                        q2_pop_group g;

                        if (!q2_pop_get_group(&pop, gi, &g))
                            continue;
                        if (!q2_levelbin_names_group(lb->data, lb->size, &g))
                            continue;

                        named++;
                        if (q2_pop_group_zone(&g) >= 0)
                            lb_zone++;
                        else
                            lb_batch++;
                        if (verbose)
                            printf("    %s: LevelBin names group '%s'"
                                   " (zone %d)\n",
                                   g_maps[mi], g.name, q2_pop_group_zone(&g));
                    }

                    lb_groups += named;
                    if (named)
                        lb_maps++;

                    /*
                     * And the SELECTIONS, decoded from the call sites rather
                     * than the strings. Every offset must land on a name the
                     * map's Population actually carries — that is the check,
                     * and there is no reason for a wrong decode to pass it.
                     */
                    /*
                     * WHICH SLOT is the selector, swept rather than assumed.
                     * For every offset a module calls with a name argument,
                     * count how many of the names are real groups. The right
                     * slot's are all of them; every other slot's are none.
                     */
                    {
                        s32 so;

                        for (so = 0; so < 512; so += 4) {
                            u32 sl[32];
                            u32 si, got = q2_levelbin_selected_slot(
                                lb->data, lb->size, so, sl, 32);

                            for (si = 0; si < got && si < 32; si++) {
                                char nm[13];

                                memcpy(nm, lb->data + sl[si], 12);
                                nm[12] = 0;

                                for (gi = 0; gi < pop.group_count; gi++) {
                                    q2_pop_group g;

                                    if (!q2_pop_get_group(&pop, gi, &g))
                                        continue;
                                    if (strcmp(g.name, nm) != 0)
                                        continue;
                                    slot_hit[so / 4]++;
                                    break;
                                }
                                slot_calls[so / 4]++;
                            }
                        }
                    }

                    {
                        u32 sel[32];
                        u32 si, got = q2_levelbin_selected(lb->data, lb->size,
                                                          sel, 32);

                        lb_sel_calls += got;
                        for (si = 0; si < got && si < 32; si++) {
                            char nm[13];
                            bool matched = false;

                            memcpy(nm, lb->data + sel[si], 12);
                            nm[12] = 0;

                            for (gi = 0; gi < pop.group_count; gi++) {
                                q2_pop_group g;

                                if (!q2_pop_get_group(&pop, gi, &g))
                                    continue;
                                if (strcmp(g.name, nm) != 0)
                                    continue;
                                matched = true;
                                if (q2_pop_group_zone(&g) >= 0)
                                    lb_sel_zone++;
                                else
                                    lb_sel_batch++;
                                if (verbose)
                                    printf("    %s: LevelBin SELECTS '%s'"
                                           " (zone %d)\n",
                                           g_maps[mi], g.name,
                                           q2_pop_group_zone(&g));
                                break;
                            }

                            if (matched)
                                lb_sel_ok++;
                            else if (verbose)
                                printf("    %s: LevelBin selects '%s'"
                                       " — NO SUCH GROUP\n", g_maps[mi], nm);
                        }
                    }
                }
            }

            if (zcount && cev.size) {
                u8 *any = (u8 *)calloc(cev.size, 1);
                u8 *ran = (u8 *)calloc(cev.size, 1);
                u32 best = 0, bz = 0, zq;

                for (zq = 0; zq < zcount; zq++) {
                    q2_rotator_set probe;
                    q2_userfuncs puf;
                    q2_event_rt prt;

                    memset(&probe, 0, sizeof(probe));
                    q2_rotators_set_operand_source(&probe, cev.data,
                                                   zev[zq].data, zev[zq].size);
                    if (q2_userfuncs_parse(&puf, &cf) == Q2_OK &&
                        q2_rotators_build(&probe, &cev, &puf) == Q2_OK) {
                        if (probe.count > best) {
                            best = probe.count;
                            bz   = zq;
                        }
                        if (any && ran && have_trig &&
                            q2_event_rt_init(&prt, &cev) == Q2_OK) {
                            live_rot_ctx pc;

                            memset(&pc, 0, sizeof(pc));
                            pc.uf     = &puf;
                            pc.set    = &probe;
                            pc.chunk  = cev.data;
                            pc.turned = any;
                            pc.seen   = ran;
                            prt.on_call      = live_rot_call;
                            prt.on_call_user = &pc;

                            for (k = 0; k < tg.count; k++) {
                                q2_trigger tr;
                                if (!q2_trigger_get(&tg, k, &tr))
                                    continue;
                                if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                                    continue;
                                q2_event_rt_trigger(&prt, tr.event_offset);
                            }
                            q2_event_rt_update(&prt);
                        }
                    }
                    q2_rotators_free(&probe);
                }

                if (any && ran) {
                    u32 o;
                    for (o = 0; o < cev.size; o++) {
                        if (!ran[o])
                            continue;
                        if (any[o]) {
                            rot_any_zone++;
                        } else {
                            u32 zq2;

                            rot_no_zone++;
                            printf("  %s: rotation CALL at Events+%u turns "
                                   "nothing under any of its %u zones\n",
                                   g_maps[mi], o, zcount);
                            printf("    COMMON (%u bytes) slots:", cev.size);
                            if (o >= 2 && o + 22 <= cev.size) {
                                const u8 *qq = cev.data + o - 2;
                                int sl2;
                                for (sl2 = 0; sl2 < 4; sl2++)
                                    printf(" %d",
                                           (int)q2_rd_s16(qq + 12 + 2 * sl2));
                            } else {
                                printf(" (offset past the end)");
                            }
                            printf("\n");
                            for (zq2 = 0; zq2 < zcount; zq2++) {
                                printf("    ZONE%u (%u bytes) slots:", zq2,
                                       zev[zq2].size);
                                if (o >= 2 && o + 22 <= zev[zq2].size) {
                                    const u8 *qq = zev[zq2].data + o - 2;
                                    int sl2;
                                    for (sl2 = 0; sl2 < 4; sl2++)
                                        printf(" %d",
                                               (int)q2_rd_s16(qq + 12 + 2 * sl2));
                                } else {
                                    printf(" (offset past the end)");
                                }
                                printf("\n");
                            }
                        }
                    }
                }
                free(any);
                free(ran);

                best_zone = bz;
                q2_rotators_set_operand_source(&rs, cev.data,
                                               zev[bz].data, zev[bz].size);
            }
            if (q2_userfuncs_parse(&uf, &cf) == Q2_OK &&
                q2_rotators_build(&rs, &cev, &uf) == Q2_OK &&
                q2_event_rt_init(&rt, &cev) == Q2_OK) {
                live_rot_ctx ctx;
                u32 t;

                ctx.uf = &uf;
                ctx.set = &rs;
                ctx.steps = 0;
                ctx.rot_fired = 0;
                ctx.rot_barren = 0;
                ctx.lit_run = 0;
                ctx.glass_run  = 0;
                ctx.glass_node = 0;
                ctx.loadmap_run = 0;
                ctx.secret_run  = 0;
                ctx.prim_run    = prim_run;

                /* The best zone's Scene and its Events, so a GLASS slot is
                 * resolved against the same pair the engine holds resident. */
                ctx.scene = NULL;
                ctx.ops.base_a = cev.data;
                ctx.ops.base_b = NULL;
                ctx.ops.b_size = 0;
                if (zcount) {
                    if (q2_scene_parse(&gscene, &zf[best_zone]) == Q2_OK)
                        ctx.scene = &gscene;
                    ctx.ops.base_b = zev[best_zone].data;
                    ctx.ops.b_size = zev[best_zone].size;
                }

                rt.on_call      = live_rot_call;
                rt.on_call_user = &ctx;

                live_built += rs.count;

                for (k = 0; k < tg.count; k++) {
                    q2_trigger tr;

                    if (!q2_trigger_get(&tg, k, &tr))
                        continue;
                    if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                        continue;
                    q2_event_rt_trigger(&rt, tr.event_offset);
                }
                q2_event_rt_update(&rt);

                live_calls += rt.call_count;
                live_steps += ctx.steps;
                live_rot_fired  += ctx.rot_fired;
                live_rot_barren += ctx.rot_barren;
                live_lit_run    += ctx.lit_run;
                live_glass_run  += ctx.glass_run;
                live_loadmap    += ctx.loadmap_run;
                live_secret     += ctx.secret_run;
                live_glass_node += ctx.glass_node;
                for (t = 0; t < 400; t++)
                    live_moved += q2_rotators_tick(&rs, 12);
                for (zi = 0; zi < rs.count; zi++)
                    if (rs.rotators[zi].angle != 0)
                        live_turned++;

                q2_event_rt_free(&rt);
            }
            q2_rotators_free(&rs);
        }

        if (have_common && have_trig) {
            for (k = 0; k < tg.count; k++) {
                q2_trigger tr;
                bool in_c, in_z = false;

                if (!q2_trigger_get(&tg, k, &tr))
                    continue;
                if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                    continue;

                t_total++;
                in_c = offset_is_record(&cev, tr.event_offset);
                for (zi = 0; zi < zcount; zi++)
                    if (offset_is_record(&zev[zi], tr.event_offset))
                        in_z = true;

                if (in_c && in_z) t_both++;
                else if (in_c)    t_common++;
                else if (in_z)    t_zone++;
                else              t_neither++;
            }
        }

        for (zi = 0; zi < zcount; zi++)
            q2_zone_close(&zf[zi]);
        q2_common_close(&cf);
    }

    printf("\n  COMMON records    : %u\n", common_records);
    printf("  zone records      : %u\n", zone_records);
    printf("  zone Events byte-identical to COMMON's: %u of %u\n",
           zone_same, zone_same + zone_diff);
    printf("  zone named entries: %u\n", zone_dirs);
    printf("\n  trigger volumes naming an event : %u\n", t_total);
    printf("    resolves in COMMON only       : %u\n", t_common);
    printf("    resolves in a ZONE only       : %u\n", t_zone);
    printf("    resolves in both              : %u  (says nothing either way)\n",
           t_both);
    printf("    resolves in neither           : %u\n", t_neither);

    printf("\n  maps whose highest trigger offset runs PAST the end of\n");
    printf("    COMMON's Events chunk         : %u\n", past_common);
    printf("    a ZONE's Events chunk         : %u\n", past_zone);

    printf("\n  COMMON's script, fired by every trigger volume — what the\n"
           "  console runs, since the zone loader never looks up \"Events\":\n");
    printf("    rotation CALLs  : %u  in COMMON's scripts, disc-wide\n",
           rot_prim_calls);
    printf("      too short     : %u  (the item cannot hold the operands)\n",
           rot_too_short);
    printf("      no object     : %u  (every slot the call has is -1)\n",
           rot_no_object);
    printf("      usable        : %u\n", rot_usable);
    printf("      empty in COMMON, a ZONE reaches that offset : %u\n",
           rot_zone_inrange);
    printf("      ...and the ZONE has a non-negative slot     : %u\n",
           rot_zone_rescue);
    printf("      zone slots examined %u, non-negative %u (%.1f%%)\n",
           rot_zone_slots, rot_zone_nonneg,
           rot_zone_slots ? 100.0 * rot_zone_nonneg / rot_zone_slots : 0.0);
    printf("    script LIGHT calls in COMMON: %u TIMEDLIGHT, %u FLKLIGHT;"
           " the trigger sweep RUNS %u of them\n",
           light_timed, light_flk, live_lit_run);
    printf("    OBJSLOT-carrying items : %u; a slot usable in "
           "COMMON : %u, in some zone : %u, nowhere : %u\n",
           objslot_items, objslot_common, objslot_zone, objslot_nowhere);
    printf("    MISEVENT keys : %u, resolved : %u (EXE table %u, the map's own %u)\n",
           misevent_calls, misevent_known, misevent_exe,
           misevent_known - misevent_exe);
    printf("      LevelBin MISEVENT tables found : %u, %u records\n",
           misevent_tables, misevent_records);
    printf("    LASERBEAM items : %u - lit in at least one zone : %u, "
           "in none : %u, in every zone : %u\n",
           laser_calls, laser_lit, laser_dark, laser_always);
    printf("      beam-zone pairs lit : %u\n", laser_zone_lit);
    printf("      kind 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u, out of range : %u\n",
           laser_kind[0], laser_kind[1], laser_kind[2], laser_kind[3],
           laser_kind[4], laser_kind[5], laser_clamped);
    printf("    breakable CALLs (GLASS, SHOOTTHEN) : %u\n", brk_calls);
    printf("      too short   : %u\n", brk_short);
    printf("      no object   : %u\n", brk_no_object);
    printf("      usable      : %u  (%u rescued from a ZONE's copy)\n",
           brk_usable, brk_zone_rescue);
    printf("      the trigger sweep RUNS %u, of which resolve a Scene node : %u\n",
           live_glass_run, live_glass_node);
    printf("    LOADMAP calls in COMMON: %u; the trigger sweep RUNS %u\n",
           loadmap_calls, live_loadmap);
    printf("      target map is on the disc      : %u  (missing %u)\n",
           loadmap_map_ok, loadmap_map_missing);
    printf("      start position resolves THERE  : %u, of which land past "
           "zone 0 : %u\n", loadmap_start_ok, loadmap_late_zone);
    printf("    INSECRET calls in COMMON: %u; the trigger sweep RUNS %u\n",
           secret_calls, live_secret);
    printf("    CREBATCH calls in COMMON: %u, naming a group that exists: %u\n",
           crebatch_calls, crebatch_found);
    printf("      the group claims a ZONE (the level's own population) : %u\n",
           crebatch_zone);
    printf("      the group claims none (a batch a script summons)     : %u\n",
           crebatch_batch);
    printf("    the map's own LevelBin NAMES a group on %u maps, %u groups\n",
           lb_maps, lb_groups);
    printf("      of those, claiming a zone : %u, claiming none : %u\n",
           lb_zone, lb_batch);
    printf("    LevelBin SELECT call sites decoded : %u, naming a real "
           "group : %u\n", lb_sel_calls, lb_sel_ok);
    printf("      the group claims a zone : %u, claims none : %u\n",
           lb_sel_zone, lb_sel_batch);
    printf("    which engine slot takes a GROUP NAME (offset: hits/calls)\n");
    {
        int so;
        for (so = 0; so < 128; so++)
            if (slot_calls[so])
                printf("      +%-4d  %4u / %-4u\n",
                       so * 4, slot_hit[so], slot_calls[so]);
    }

    /*
     * Every primitive a player who has walked the whole disc would run, by
     * kind. This is the list of what a script can ask the game to do, ordered
     * by how often it asks — and therefore the list of what is left to build,
     * measured rather than guessed at.
     */
    printf("\n  what a trigger volume ASKS FOR, disc-wide\n");
    printf("    %-14s %8s %8s\n", "primitive", "in COMMON", "run");
    {
        int pi;
        for (pi = 0; pi < Q2_UF_PRIM_COUNT; pi++) {
            const q2_uf_prim_info *pinfo;

            if (!prim_have[pi] && !prim_run[pi])
                continue;
            pinfo = q2_uf_info((q2_uf_prim)pi);
            printf("    %-14s %8u %8u\n",
                   pinfo ? pinfo->name : "?", prim_have[pi], prim_run[pi]);
        }
    }
    printf("    rotation CALLs the script RUNS : %u, of which turn nothing : %u\n",
           live_rot_fired, live_rot_barren);
    printf("    distinct rotation CALL sites the script reaches : %u\n",
           rot_any_zone + rot_no_zone);
    printf("      turn something under SOME zone : %u\n", rot_any_zone);
    printf("      barren under EVERY zone        : %u\n", rot_no_zone);
    printf("    rotators built  : %u  (one per object slot each call names)\n",
           live_built);
    printf("    CALL items run  : %u\n", live_calls);
    printf("    rotation steps  : %u\n", live_steps);
    printf("    tick-moves      : %u\n", live_moved);
    printf("    rotators turned : %u\n", live_turned);
        printf("\n  Record-start membership decides NOTHING: every offset"
               " starts a record in both.\n");

    return 0;
}
