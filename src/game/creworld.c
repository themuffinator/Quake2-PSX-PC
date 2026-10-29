#include "random.h"
#include "creworld.h"
#include "modelent.h"

#include "worldscale.h"

#include "levelbin.h"

#include "ai.h"        /* q2_vectoyaw — path_corner_touch turns the creature */
#include "aimove.h"    /* the step hooks: q2_link_entity, the touch stage    */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
static int name_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static s32 module_index(const q2_creature_world *w, const char *name)
{
    u32 i;
    for (i = 0; i < w->mod_count; i++)
        if (name_eq_ci(w->mod[i].name, name))
            return (s32)i;
    return -1;
}

/* ------------------------------------------------------------------------- */
/*
 * Decode one module out of the CreAIBin/CreAIRel pair.
 *
 * The two chunks are parallel lists of `{char name[12]; u32 next}` headers: the
 * body of a module runs from its own header's end to the next header, and its
 * relocation stream is at the same slot of the other chunk. A duplicate name is
 * skipped rather than decoded twice, because a map that spawns three Soldiers
 * carries one module and the registry is keyed by name.
 */
static bool module_take(q2_creature_world *w, const u8 *bin, u32 boff, u32 bnext,
                        u32 bin_size, const u8 *rel, u32 roff, u32 rnext,
                        u32 rel_size)
{
    q2_creature_module *m;
    q2_reloc_stats rs;
    const q2_cre_impl *impl;
    char nm[13];
    size_t body;

    memcpy(nm, bin + boff, 12);
    nm[12] = 0;

    if (module_index(w, nm) >= 0)
        return true;                    /* already have it */
    impl = q2_cre_impl_find(nm);
    /* Generic action fragments do not reproduce control flow. Keep an
     * unsupported module out of the gameplay registry until transcribed. */
    if (!impl || impl == &q2_cre_generic)
        return true;
    if (w->mod_count >= Q2_CREWORLD_MAX_MODULES)
        return false;

    m = &w->mod[w->mod_count];
    memset(m, 0, sizeof(*m));
    memcpy(m->name, nm, 13);

    body = (size_t)((bnext > boff ? bnext : bin_size) - boff -
                    Q2_RELOC_CREAI_PREAMBLE);
    m->image = (u8 *)malloc(body ? body : 1);
    if (!m->image)
        return false;
    m->size = body;
    memcpy(m->image, bin + boff + Q2_RELOC_CREAI_PREAMBLE, body);

    if (q2_reloc_apply(m->image, m->size,
                       rel + roff + Q2_RELOC_CREAI_PREAMBLE,
                       (size_t)((rnext > roff ? rnext : rel_size) - roff -
                                Q2_RELOC_CREAI_PREAMBLE),
                       Q2_CREWORLD_BASE, &rs) != Q2_OK) {
        Q2_WARN("creature module '%s' will not relocate", nm);
        free(m->image);
        m->image = NULL;
        return false;
    }

    if (!q2_creature_decode(&m->cre, m->image, m->size, Q2_CREWORLD_BASE, nm)) {
        Q2_WARN("creature module '%s' will not decode", nm);
        free(m->image);
        m->image = NULL;
        return false;
    }

    /* Keep decoded actions for diagnostics. Gameplay uses the native methods
     * of the registered implementation; unknown modules were excluded above. */
    q2_creature_decode_thinks(&m->cre, m->image, m->size, Q2_CREWORLD_BASE,
                              m->think, Q2_CLASS_METHOD_COUNT);

    m->ready = q2_creature_bind(&m->bind, &m->cre, impl);

    /* Attach after q2_creature_bind, which clears the bind before building it. */
    q2_creature_bind_thinks(&m->bind, m->think, Q2_CLASS_METHOD_COUNT);

    /*
     * And the move names, bound after the same memset for the same reason.
     * These point into `m->image`, which this struct owns, so they stay valid
     * for the module's lifetime.
     */
    q2_creature_move_names(&m->cre, m->image, m->size, m->move_name,
                           Q2_CRE_MAX_MOVES);
    q2_creature_bind_move_names(&m->bind, m->move_name, m->cre.move_count);

    /* And the whole table, frame-indexed — what the draw actually needs. */
    m->frame_name_count = q2_creature_frame_names(&m->cre, m->image, m->size,
                                                  m->frame_name,
                                                  Q2_CRE_MAX_MOVES);
    if (!m->ready) {
        free(m->image);
        m->image = NULL;
        return false;
    }

    w->mod_count++;
    return true;
}

static void modules_load(q2_creature_world *w, const dat_chunk *bin,
                           const dat_chunk *rel)
{
    u32 boff = 0, roff = 0;

    if (!bin || !rel || bin->size <= Q2_RELOC_CREAI_PREAMBLE)
        return;

    while (boff < bin->size && roff < rel->size &&
           bin->size - boff > Q2_RELOC_CREAI_PREAMBLE &&
           rel->size - roff > Q2_RELOC_CREAI_PREAMBLE) {
        u32 bnext = q2_rd_u32(bin->data + boff + 12);
        u32 rnext = q2_rd_u32(rel->data + roff + 12);
        u32 bend = bnext ? bnext : bin->size;
        u32 rend = rnext ? rnext : rel->size;

        if (bend > bin->size || rend > rel->size ||
            bend <= boff + Q2_RELOC_CREAI_PREAMBLE ||
            rend <= roff + Q2_RELOC_CREAI_PREAMBLE ||
            memcmp(bin->data + boff, rel->data + roff, 12) != 0) {
            Q2_WARN("invalid creature binary/relocation pair at %u/%u", boff, roff);
            return;
        }

        module_take(w, bin->data, boff, bend, bin->size,
                    rel->data, roff, rend, rel->size);

        if (bend == bin->size || rend == rel->size)
            break;
        boff = bnext;
        roff = rnext;
    }
}

/*
 * COMMON.DAT donors for the seven implemented creatures. Many maps keep their
 * modules in ZONE*.DAT instead. Load the active zone before borrowing, so its
 * native implementation and module variant take precedence over donors.
 * The census hashes every relocated copy. Campaign copies match; MAGDEMO's
 * Soldier and Flyer differ. A map's own COMMON module wins over a donor.
 */
static const char *const k_module_donors[] = {
    "WASTE4", "POWER1", "BASE0", "COMMAND", "WASTE3", "LAB", "POWER2", NULL
};

/* Jorg owns a second class implementation. Keep its code/data separate from
 * the standalone Rider even though both use class byte 83 and the Rider model. */
static bool embedded_rider_load(q2_creature_world *w)
{
    extern const q2_cre_impl q2_cre_jorg_rider;
    s32 ji = module_index(w, "Jorg"), ri = module_index(w, "Rider");
    q2_creature_module *m;
    u32 i, n = 0;
    if (ji < 0 || ri < 0) return true;
    if (w->mod_count >= Q2_CREWORLD_MAX_MODULES) return false;
    m = &w->mod[w->mod_count];
    memset(m, 0, sizeof(*m));
    strcpy(m->name, "JorgRider");
    m->size = w->mod[ji].size;
    m->image = malloc(m->size);
    if (!m->image) return false;
    memcpy(m->image, w->mod[ji].image, m->size);
    if (!q2_creature_decode_entry(&m->cre, m->image, m->size,
            Q2_CREWORLD_BASE, m->name, Q2_CREWORLD_BASE + 0x3AF0, Q2_CLASS_RIDER)) {
        free(m->image); m->image = NULL; return false;
    }
    /* The export-3 table belongs to Jorg's model. Rider's own table supplies
     * the names for its matching frame ranges, not Jorg's overlapping ones. */
    for (i = 0; i < m->cre.move_count; i++) {
        const q2_cre_move *mv = &m->cre.move[i];
        u32 k;
        if (mv->clip_piece) continue;
        for (k = 0; k < w->mod[ri].cre.move_count; k++) {
            const q2_cre_move *r = &w->mod[ri].cre.move[k];
            if (!r->clip_piece && r->first_frame == mv->first_frame &&
                r->last_frame == mv->last_frame) {
                m->cre.move[n] = *mv;
                m->move_name[n++] = w->mod[ri].move_name[k];
                break;
            }
        }
    }
    m->cre.move_count = n;
    m->cre.class_count = 1; m->cre.class_byte[0] = Q2_CLASS_RIDER;
    m->cre.class_method_table[0] = m->cre.method_table_addr;
    m->frame_name_count = w->mod[ri].frame_name_count;
    memcpy(m->frame_name, w->mod[ri].frame_name, sizeof(m->frame_name));
    m->ready = q2_creature_bind(&m->bind, &m->cre, &q2_cre_jorg_rider);
    q2_creature_bind_move_names(&m->bind, m->move_name, n);
    w->mod_count++;
    return m->ready;
}

/* Reserve one stable actor slot per first-stage boss before the client builds
 * model/combat arrays. No live pointer is invalidated when the rider appears. */
static bool reserve_boss_children(q2_creature_world *w)
{
    u32 i, count = w->set.count, extra = 0;
    q2_monster *resized;
    if (module_index(w, "JorgRider") < 0) return true;
    for (i = 0; i < count; i++) if (w->set.monsters[i].class_id == 13) extra++;
    if (!extra) return true;
    resized = realloc(w->set.monsters, (count + extra) * sizeof(*resized));
    if (!resized) return false;
    w->set.monsters = resized; w->set.capacity = count + extra;
    for (i = 0; i < count; i++) {
        q2_monster *m, *parent = &resized[i];
        if (parent->class_id != 13) continue;
        m = &resized[w->set.count++]; q2_monster_init(m);
        m->class_id = 14;
        /* A second namespace of authored group/slot keys keeps this child
         * stable across zone loads, even when another zone has fewer classes
         * available. A roster index is not a persistent identity. */
        m->group = (u16)(w->pop.group_count + parent->group);
        m->population_slot = parent->population_slot; m->owner = parent;
        memcpy(m->pos, parent->pos, sizeof(m->pos));
    }
    return true;
}

static q2_monster *spawn_child(q2_monster *parent, u8 pop_class, void *user)
{
    q2_creature_world *w = user;
    const q2_class_entry *e = q2_class_find(&w->classes, pop_class);
    u32 i;
    if (!e) return NULL;
    for (i = 0; i < w->set.count; i++) {
        q2_monster *m = &w->set.monsters[i];
        if (m->group < w->pop.group_count || m->owner != parent || m->pop_class_id != pop_class ||
            m->population_active) continue;
        memcpy(m->pos, parent->pos, sizeof(m->pos));
        m->angles[2] = 1024;
        m->health = m->max_health = e->health; m->gib_health = e->gib_health;
        m->spawn_pending = false; m->population_active = true;
        q2_creature_spawn(m->cre_bind, m, 0);
        return m;
    }
    return NULL;
}

/* Medic+0x174C creates a fresh creature from the body's original class.
 * Reuse its stable host slot; all renderer/combat/save references retain
 * their identity while the edict state is rebuilt from export 0. */
static bool revive_corpse(q2_monster *medic, q2_monster *corpse, void *user)
{
    q2_creature_world *w = user;
    q2_monster old = *corpse;
    const q2_class_entry *e = q2_class_find(&w->classes, old.pop_class_id);
    q2_monster *enemy = old.oldenemy;
    if (!e || !old.cre_bind || old.cre_bind->impl->scripted) return false;
    /* Medic+0x1994 lowers the replaced actor to its gib threshold, then
     * reinstalls corpse_think. Dispatch that body's destruction while its
     * original posed mesh and combat actor still occupy this stable slot. */
    q2_gib_monster_corpse(corpse);
    q2_monster_init(corpse);
    memcpy(corpse->pos, old.pos, sizeof(corpse->pos));
    corpse->angles[2] = old.angles[2];
    corpse->pop_class_id = old.pop_class_id; corpse->group = old.group;
    corpse->population_slot = old.population_slot; corpse->population_active = true;
    corpse->model_ext2 = old.model_ext2;
    corpse->on_ground = old.on_ground; corpse->ground_height = old.ground_height;
    q2_monster_bind_hit_size(corpse, old.live_hit_height != 0,
                              old.live_hit_radius, old.live_hit_height);
    corpse->health = corpse->max_health = e->health;
    corpse->gib_health = e->gib_health;
    q2_creature_spawn(old.cre_bind, corpse, w->class_variant[old.pop_class_id]);
    corpse->spawnflags &= 0xF803FFFFu;
    corpse->aiflags = 0; corpse->target = corpse->targetname = corpse->combattarget = 0;
    corpse->owner = NULL;
    q2_monster_start_go(corpse);
    if (enemy && enemy->client && q2_ent_inuse(enemy)) {
        corpse->enemy = enemy; q2_found_target(corpse);
    }
    (void)medic; /* Its corpse reference now resolves to the rebuilt edict. */
    return true;
}

/*
 * Load modules from donor maps until every class the table names has one, or
 * the donors run out. Only classes with no module already are wanted, so a map
 * that ships its own is untouched.
 */
static void modules_borrow(q2_creature_world *w, const disc *d)
{
    u32 i, k;

    if (!d)
        return;

    for (k = 0; k_module_donors[k]; k++) {
        char path[160];
        q2_buf buf;
        q2_common_file cf;
        bool wanted = false;

        /* Stop as soon as every named creature class resolves. */
        for (i = 0; i < w->classes.count; i++) {
            const q2_class_entry *e = &w->classes.entries[i];
            if (!e->is_player && e->name[0] &&
                e->id < Q2_MONSTER_CLASS_COUNT &&
                module_index(w, e->name) < 0) {
                wanted = true;
                break;
            }
        }
        if (!wanted)
            return;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT",
                 k_module_donors[k]);
        if (disc_read_file(d, path, &buf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        modules_load(w, cf.chunk[Q2_COMMON_CRE_AI_BIN], cf.chunk[Q2_COMMON_CRE_AI_REL]);
        q2_common_close(&cf);
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Bind every class id in the table to the module that serves it.
 *
 * The variant is the id's ordinal among the table entries sharing its name —
 * see the header for why that is the reading and what it rests on.
 */
static void classes_bind(q2_creature_world *w)
{
    u32 i;

    for (i = 0; i < Q2_MONSTER_CLASS_COUNT; i++) {
        w->class_module[i]  = -1;
        w->class_variant[i] = 0;
    }

    if (!w->classes_ready)
        return;

    for (i = 0; i < w->classes.count; i++) {
        const q2_class_entry *e = &w->classes.entries[i];
        s32 mi;
        u32 j, ordinal = 0;

        if (e->is_player || !e->name[0] || e->id >= Q2_MONSTER_CLASS_COUNT)
            continue;

        mi = module_index(w, e->name);
        if (mi < 0)
            continue;

        /* How many entries of this name came before this one. */
        for (j = 0; j < i; j++)
            if (!w->classes.entries[j].is_player &&
                name_eq_ci(w->classes.entries[j].name, e->name))
                ordinal++;

        if (ordinal >= w->mod[mi].cre.class_count)
            ordinal = w->mod[mi].cre.class_count ?
                      w->mod[mi].cre.class_count - 1 : 0;

        w->class_module[e->id]  = (s8)mi;
        w->class_variant[e->id] = (u8)ordinal;
        q2_monster_set_register(&w->set, e->id);
    }
}

/* ------------------------------------------------------------------------- */
/* Path corners — the spawner at 0x8007F390 and the two routines that use them */
/* ------------------------------------------------------------------------- */
/*
 * 0x8007F390 walks the PathCorner group's 24-byte records and turns each into
 * an entity. Its only caller is 0x800575A4, reached from 0x80057588 where the
 * literal "PathCorner" at 0x800ACD90 selects the group by NAME — the same test
 * q2_pop_group_is_path makes here.
 *
 * The one thing to get right is that the origin is copied VERBATIM
 * (0x8007F404-0x8007F418 are three plain `lw`/`sw` pairs). A creature record
 * gets the Q2_EYE_BASE lift, the 30-unit nudge and the drop-to-floor sweep; a
 * corner gets none of them, and applying them would move the point the
 * creature walks to and the yaw it turns to when it starts.
 */
static q2_result corners_spawn(q2_creature_world *w, const q2_population *pop)
{
    u32 gi, slot, n = 0;

    /* Counted first so the array is allocated once: q2_pick_target hands out
     * pointers into it and monster_start_go keeps them across ticks. */
    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;
        q2_pop_path  node;

        if (!q2_pop_get_group(pop, gi, &g) || !q2_pop_group_is_path(&g))
            continue;
        for (slot = 0; q2_pop_get_path(pop, &g, slot, &node); slot++)
            n++;
    }

    if (!n)
        return Q2_OK;

    w->corner = (q2_monster *)calloc(n, sizeof(q2_monster));
    if (!w->corner)
        return Q2_ERR_NO_MEMORY;

    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;
        q2_pop_path  node;

        if (!q2_pop_get_group(pop, gi, &g) || !q2_pop_group_is_path(&g))
            continue;

        for (slot = 0; q2_pop_get_path(pop, &g, slot, &node); slot++) {
            q2_monster *c = &w->corner[w->corner_count++];

            q2_monster_init(c);

            c->pos[0] = node.x;
            c->pos[1] = node.y;
            c->pos[2] = node.z;

            c->class_id   = Q2_CLASS_PATH_CORNER;   /* 0x8005F870 */
            c->targetname = (s16)node.targetname;   /* 0x8007F3E8 */
            c->target     = (s16)node.target;       /* 0x8007F3F4 */

            /* The two flag writes into entity+0x1C, in the order the spawner
             * makes them: byte 0 is the corner's wait (0x8007F400), bits
             * 18..26 the map's nine authored bits (0x8007F430-0x8007F448). */
            c->spawnflags = (c->spawnflags & ~0xFFu) | (node.wait & 0xFFu);
            c->spawnflags = (c->spawnflags & 0xF803FFFFu)
                          | (((u32)node.flags & Q2_POP_SPAWN_FLAGS_MASK)
                             << Q2_POP_SPAWN_FLAGS_SHIFT);

            /* 0x8005F884 raises the in-use bit the resolver tests at
             * 0x8005F74C. The port's own latch goes up with it. */
            c->spawnflags |= Q2_SVFLAG_INUSE;
            c->in_use      = true;
        }
    }

    return Q2_OK;
}

/*
 * G_PickTarget — 0x8005F708. Walks the entity list (here: the corner array),
 * requires the 0x20000000 in-use bit at +0x1C, matches entity+0x18 against the
 * wanted value, collects at most eight (0x8005F77C-0x8005F78C) and picks one
 * at random (0x8005F79C `jal` rand, then `div` by the count).
 *
 * `blez s1` at 0x8005F724 is the `targetname <= 0` guard, which q2_pick_target
 * already makes. The `a1 = 4` the two call sites pass is never read.
 *
 * The rand() draw is made even for a single match, because the original makes
 * it — `rand() % 1` is zero but the draw still moves the sequence, and every
 * other consumer of that sequence is reproduced tick for tick.
 */
static q2_monster *creworld_pick_target(s16 targetname, void *user)
{
    q2_creature_world *w = (q2_creature_world *)user;
    q2_monster *found[8];
    u32 n = 0, i;

    if (!w)
        return NULL;

    for (i = 0; i < w->corner_count && n < 8; i++) {
        q2_monster *c = &w->corner[i];

        if (q2_ent_inuse(c) && c->targetname == targetname)
            found[n++] = c;
    }

    if (!n)
        return NULL;

    return found[(u32)q2_random_next() % n];
}

/*
 * path_corner_touch — 0x8005F1F8, reached from G_TouchTriggers (0x8005F4A8) at
 * 0x8005F598. The dispatch is a SPECIAL CASE, not a touch pointer: 0x8005F584
 * compares the touched entity's class byte against 114 and calls this
 * directly, which is why 0x8007F390 never installs one.
 */
static void corner_touch(q2_monster *m, q2_monster *corner)
{
    q2_monster *next;
    s32 wait;

    if (m->movetarget != corner)        /* 0x8005F218 */
        return;
    if (m->enemy)                       /* 0x8005F228 */
        return;

    next = corner->target ? q2_pick_target(corner->target) : NULL;

    /*
     * A corner carrying the map's first authored flag bit TELEPORTS the
     * creature to the next one and then hands it the one after that —
     * 0x8005F254 (`srl 18; andi 1`) through 0x8005F2E8.
     *
     * The Y correction is 0x8005CBF0, which returns the entity's hull mins.y
     * and a constant -96 for a path corner (0x8005CC14), so the creature lands
     * with its own hull where the corner's notional one is.
     *
     * Transcribed rather than observed: whether any corner on this disc sets
     * the bit has not been measured, so this arm may never run here.
     */
    if (next && ((next->spawnflags >> Q2_POP_SPAWN_FLAGS_SHIFT) & 1)) {
        const s32 corner_mins_y = -Q2_CORNER_ABS_EXTENT;   /* 0x8005CC14 */

        m->pos[0] = next->pos[0];
        m->pos[1] = next->pos[1] - corner_mins_y + m->mins[1];
        m->pos[2] = next->pos[2];

        next = next->target ? q2_pick_target(next->target) : NULL;
        q2_link_entity(m, 1);                   /* 0x8005F2E4 */
    }

    m->movetarget = next;               /* 0x8005F2EC */
    m->goalentity = next;               /* 0x8005F2F0 */

    /* The corner's own wait, byte 0 of its spawnflags word — 0x8005F2F4. */
    wait = (s32)(corner->spawnflags & 0xFFu);

    if (wait) {
        m->pausetime = q2_level_state.time + wait;
        if (m->stand)
            m->stand(m);
    } else if (!m->movetarget) {
        /* The chain ends here: stand for longer than the level lasts. */
        m->pausetime = q2_level_state.time + Q2_PAUSE_FOREVER;
        if (m->stand)
            m->stand(m);
    } else {
        s32 v[3];

        /*
         * 0x8005F360 calls 0x8005C980 here, which is link_entity's INVERSE —
         * it copies the render object's position back onto the entity. This
         * port has no separate render object (the draw reads the monster), so
         * there is nothing to copy back and the call has no counterpart.
         */
        v[0] = m->goalentity->pos[0] - m->pos[0];
        v[1] = m->goalentity->pos[1] - m->pos[1];
        v[2] = m->goalentity->pos[2] - m->pos[2];
        m->ideal_yaw = (s16)q2_vectoyaw(v);  /* 0x8005F3AC */
    }
}

/*
 * G_TouchTriggers — 0x8005F4A8, called from SV_movestep at 0x80060300 and from
 * SV_NewChaseDir at 0x80060508/0x80060520, which is why this is installed as
 * the step's touch hook rather than run once a tick: a creature that reaches a
 * corner picks up the next one on the same step, not on the next one.
 *
 * DEVIATION: the original asks the area tree for the AREA_TRIGGERS entities
 * the mover's box overlaps (0x8005F554, a3 = 2). This port has no area tree,
 * so the corner array is scanned instead. There are at most a couple of dozen
 * corners in a map, so the scan is not the interesting part.
 *
 * WHAT IS INTERESTING IS THE CORNER'S OWN BOX, and this used to treat it as a
 * point. BoxEdicts (Q_SHARED.C 0x8005C7C0) does not read a candidate's stored
 * bounds: at 0x8005C81C it calls CalcAbsFromCreBox (Q_SHARED.C 0x8005CB00) on
 * the CANDIDATE first and only then runs VectBoxOverlap (COLL.C 0x800552D8) at
 * 0x8005C828. CalcAbsFromCreBox opens with
 *
 *     8005CB00  lbu   v1, 35(a0)          the object's class byte
 *     8005CB04  addiu v0, zero, 114       PathCorner
 *     8005CB08  beq   v1, v0, 0x8005CB20
 *
 * and the 0x8005CB20 arm writes origin - 96 into absmin on all three axes
 * (0x8005CB28, 0x8005CB38, 0x8005CB48) and origin + 96 into absmax
 * (0x8005CB58, 0x8005CB68, 0x8005CB78). Everything else takes 0x8005CB84,
 * which copies the edict's linked +0x78..+0x8C. So a corner IS given a hull —
 * a 192-unit cube, made up on the spot every time it is asked for rather than
 * stored — and the corner really is in the trigger set the filter accepts
 * (0x8007F41C..0x8007F42C sets solid = SOLID_TRIGGER; 0x8005C840 tests it).
 *
 * The acceptance window is therefore the creature's hull grown by 96 on every
 * axis: 382 per axis, not 286. VectBoxOverlap rejects only on a STRICT `slt`
 * (0x800552E4 and its five siblings), so boxes that merely touch overlap.
 *
 * monster.c already had the right reading of 0x8005CCFC's corner short-circuit
 * while the comment here had the wrong one; the tree no longer disagrees with
 * itself.
 */
bool q2_creature_corner_overlaps(const q2_monster *m, const q2_monster *corner)
{
    int axis;

    if (!m || !corner)
        return false;

    for (axis = 0; axis < 3; axis++) {
        /* CalcAbsFromCreBox's corner arm: origin ∓ 96 on every axis, 0x8005CB28
         * and 0x8005CB58. The corner has no stored hull to read. */
        s32 cmin = corner->pos[axis] - Q2_CORNER_ABS_EXTENT;
        s32 cmax = corner->pos[axis] + Q2_CORNER_ABS_EXTENT;

        /*
         * VectBoxOverlap, 0x800552D8. Every one of its six tests is a strict
         * `slt` (0x800552E4, 0x800552FC, 0x80055314, 0x80055334, 0x8005534C,
         * 0x80055364), so boxes whose faces coincide still overlap and the
         * window is closed at both ends.
         *
         * The mover's own box comes from the same function's general arm,
         * which copies the linked edict's +0x78..+0x8C — origin + mins and
         * origin + maxs, which is what this builds inline.
         */
        if (cmax < m->pos[axis] + m->mins[axis]
            || cmin > m->pos[axis] + m->maxs[axis])
            return false;
    }

    return true;
}

static void creworld_touch(q2_monster *m, void *user)
{
    q2_creature_world *w = (q2_creature_world *)user;
    u32 i;

    if (!w || !m || !m->movetarget || m->enemy)
        return;

    for (i = 0; i < w->corner_count; i++) {
        q2_monster *c = &w->corner[i];

        if (!q2_ent_inuse(c) || c->class_id != Q2_CLASS_PATH_CORNER)
            continue;

        if (!q2_creature_corner_overlaps(m, c))
            continue;

        /*
         * EVERY OVERLAPPING CORNER, NOT THE FIRST. BoxEdicts hands back the
         * whole list and 0x8005F568..0x8005F5CC walks all of it: 0x8005F5A0's
         * `j 0x8005F5C4` lands on the loop's own `addiu s0, s0, 1`, so a
         * path_corner_touch is followed by the next candidate rather than by a
         * return. This used to stop after one, which was invisible while the
         * acceptance window was a point and is not now that it is a 382-unit
         * box: two corners can sit inside one creature's grown hull, and
         * stopping on the first would let a corner that is not the movetarget
         * shadow the one that is. path_corner_touch's own `bne` at 0x8005F218
         * is what makes visiting all of them safe.
         */
        corner_touch(m, c);
    }
}

/* ------------------------------------------------------------------------- */
q2_result q2_creature_world_load(q2_creature_world *w, const disc *d,
                                 const q2_build_id *id,
                                 const q2_common_file *common,
                                 const q2_zone_file *zone,
                                 q2_collision *coll, const q2_move_world *world)
{
    q2_population pop;
    u32 i;

    if (!w || !common)
        return Q2_ERR_INVALID_ARG;

    q2_creature_world_free(w);
    memset(w, 0, sizeof(*w));
    w->placement_coll = coll;
    w->placement_world = world;

    /*
     * The method table and the level clock are global — the engine's are too —
     * so a second level must not inherit the first's. Resetting here rather
     * than in the caller keeps the two loads that matter next to each other.
     */
    q2_cre_bind_reset();
    q2_class_table_reset();
    q2_level_reset();

    w->classes_ready = (q2_class_table_load(&w->classes, d, id) == Q2_OK);
    if (!w->classes_ready)
        Q2_WARN("no entity class table for this build — no creatures");

    modules_load(w, common->chunk[Q2_COMMON_CRE_AI_BIN], common->chunk[Q2_COMMON_CRE_AI_REL]);
    if (zone)
        modules_load(w, zone->chunk[Q2_ZONE_CRE_AI_BIN], zone->chunk[Q2_ZONE_CRE_AI_REL]);
    modules_borrow(w, d);
    if (!embedded_rider_load(w)) return Q2_ERR_NO_MEMORY;
    classes_bind(w);

    if (q2_population_parse(&pop, common) != Q2_OK)
        return Q2_OK;                   /* a map with no population is fine */

    w->pop       = pop;
    w->pop_ready = true;

    if (q2_spawn_from_population(&w->set, &pop, coll, world, &w->stats) != Q2_OK)
        return Q2_ERR_NO_MEMORY;
    if (!reserve_boss_children(w)) return Q2_ERR_NO_MEMORY;

    /*
     * THE PATROL ROUTES, AND THE RESOLVER THAT REACHES THEM.
     *
     * monster_start_go (0x80061BA4) calls G_PickTarget on the creature's
     * `target` and walks it at whatever comes back if that is a path corner.
     * Nothing in this tree had ever installed a resolver, so every lookup
     * failed and the `if (!t)` arm ran instead: target cleared, pausetime set
     * to forever, stand(). 174 of the disc's 651 placed creature records name
     * a corner, so about a quarter of the game's creatures stood where they
     * were dropped until they saw the player.
     *
     * The two land together on purpose: without the corners the resolver has
     * nothing to find, and without the resolver the corners are unreachable.
     */
    if (corners_spawn(w, &pop) != Q2_OK)
        return Q2_ERR_NO_MEMORY;

    q2_ai_set_pick_target(creworld_pick_target, w);

    /*
     * And the step's touch stage, which is what advances a creature from one
     * corner to the next. q2_link_entity updates the physical actor's target
     * directly; no additional world-specific link callback is needed.
     */
    q2_ai_set_link_hooks(NULL, creworld_touch, w);

    /*
     * Now that the set exists, give every creature its module: the class byte,
     * the speed scale, the mass and the thirteen callbacks. Health comes from
     * the class table, which is per ID and not per module — it is what makes a
     * 20-health Soldier different from a 40-health one.
     */
    if (w->set.count) {
        w->model_name = (char (*)[13])calloc(w->set.count, 13);
        if (!w->model_name)
            return Q2_ERR_NO_MEMORY;
    }

    for (i = 0; i < w->set.count; i++) {
        q2_monster *m = &w->set.monsters[i];
        u32 pop_class = m->class_id;      /* before q2_creature_spawn eats it */
        s32 mi = w->class_module[pop_class];
        const q2_class_entry *e;

        if (m->group >= w->pop.group_count) mi = module_index(w, "JorgRider");

        if (mi < 0)
            continue;

        e = q2_class_find(&w->classes, pop_class);
        if (e && w->model_name)
            memcpy(w->model_name[i], e->name, sizeof(e->name) < 13
                                              ? sizeof(e->name) : 13);

        /* Kept before the spawn eats `class_id`: a module that shares one
         * method table across several class bytes dispatches on the ROW, and
         * the row is the only thing that says which variant this is. */
        m->pop_class_id = (u8)pop_class;
        if (e) m->class_id = m->class_byte = e->class_byte;

        /*
         * THE CLASS ROW FIRST, THE MODULE SECOND — which is the order the
         * loader uses and the reverse of what this did.
         *
         * 0x8007E68C and 0x8007E698 write health and gib_health out of the
         * class descriptor record, and only THEN does 0x8007E6AC call the
         * module's export 0. So a module whose spawn function sets its own
         * health overrides the row, not the other way round.
         *
         * It did not matter while `q2_cre_impl.spawn` had no caller. It does
         * now: the hook runs inside `q2_creature_spawn`, so leaving the row
         * assignment below it would let the table quietly overwrite whatever
         * the module had just decided.
         */
        if (e && e->health > 0) {
            m->health     = e->health;
            m->max_health = e->health;
            m->gib_health = (s16)(e->gib_health ? e->gib_health
                                                : -e->health / 2);
        }

        q2_creature_prepare(&w->mod[mi].bind, m, w->class_variant[pop_class]);
        if (m->group >= w->pop.group_count) {
            m->in_use = false; m->spawn_pending = true; m->population_active = false;
            m->next_think = 0;
        }
    }

    q2_cre_set_lifecycle_hooks(w->set.monsters, w->set.count, spawn_child, revive_corpse, w);
    w->ready = true;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/*
 * The sight client's position, and it is the player's ENTITY ORIGIN.
 *
 * `q2_visible` (ai.c, transcribing 0x8005B950) forms each end as
 * `pos + view_height`, and 0x8005B950 adds +0x4C to the entity's ORIGIN. The
 * caller used to pass the player's EYE — already feet - Q2_VIEW_STAND — so the
 * view height was added a second time and the player end of every sight line
 * sat 400 units above the eye, in the ceiling.
 *
 * The sight entity's own view height is therefore set so that origin +
 * view_height lands back on the console's eye: with the origin at
 * feet - Q2_EYE_BASE and the eye at feet - Q2_VIEW_STAND, that is
 * -(Q2_VIEW_STAND - Q2_EYE_BASE) = -290.
 */
static void sight_place(q2_creature_world *w, const s32 origin[3])
{
    if (!origin)
        return;
    w->sight.pos[0] = origin[0];
    w->sight.pos[1] = origin[1];
    w->sight.pos[2] = origin[2];
}

/*
 * AND THE PLAYER'S HEALTH, WHICH THE STAND-IN USED TO HOLD AT 100 FOR EVER.
 *
 * The AI asks this entity whether its enemy is still worth shooting, in two
 * places that are both transcriptions:
 *
 *   ai_checkattack's "he's dead Jim" (ai.c) clears `enemy` and falls back to
 *   the old enemy, the move target or a stand — `enemy->health <= 0`
 *   q2_M_CheckAttack (0x8005D8EC) takes the dead-enemy arm on the same test
 *
 * Neither can ever fire against a constant. `q2_creature_world_wake` set the
 * stand-in's health to 100 and the tick refreshed only the position, so every
 * creature on every map believed the player was at full health from the
 * moment the zone loaded until it was torn down. A player who died went on
 * being shot where they fell: SECURITY, `--demo --watch`, reported health
 * -14 at frame 560 and -48 by 620, still dropping.
 *
 * max_health is left where the spawn put it. Nothing in the AI reads it, and
 * it is the stand-in's own field rather than a copy of the player's.
 */
static void sight_health(q2_creature_world *w, s16 health)
{
    w->sight.health = health;
}

void q2_creature_world_wake(q2_creature_world *w, const s32 player_origin[3],
                            s16 player_health)
{
    if (!w)
        return;

    q2_monster_init(&w->sight);
    /*
     * Both halves of "in use". `q2_ent_inuse` is `in_use && (spawnflags &
     * INUSE)` — the port's own bool and the engine's bit — and FindTarget runs
     * it on the sight client before anything else. Setting only the bool made
     * every creature on every map look straight through the player.
     */
    w->sight.in_use      = true;
    w->sight.spawnflags |= Q2_SVFLAG_INUSE;
    w->sight.client      = true;     /* entity+0x0C != NULL: this is a player */
    w->sight.health      = player_health;
    w->sight.max_health  = 100;
    w->sight.hit_radius = 286;
    w->sight.hit_height = 572;
    /* The eye the console's `visible` reconstructs from the origin: the
     * origin is feet - 286 and the eye is feet - 576, so -290. */
    w->sight.view_height = -(s16)(Q2_VIEW_STAND - Q2_EYE_BASE);
    sight_place(w, player_origin);

    /*
     * The two noise entities, as 0x80062C30..0x80062C64 builds them. Neither
     * is noisy yet: nothing points at them until the first noise, and their
     * zero health is what ai_checkattack reads, through a null object pointer
     * on the console (0x8005DE48), once a noise has gone stale.
     */
    {
        int k;
        for (k = 0; k < 2; k++) {
            q2_monster *n = &w->noise[k];
            q2_monster_init(n);
            n->in_use      = true;
            n->spawnflags |= Q2_SVFLAG_INUSE;
            n->class_id    = Q2_CLASS_PLAYER_NOISE;
            n->owner       = &w->sight;
            n->svflags     = 1;
            n->client      = false;
            n->health      = 0;
        }
    }

    q2_monster_set_wake(&w->set, &w->sight);
}

void q2_creature_world_actor_tick(q2_creature_world *w)
{
    u32 i;
    if (!w || !w->ready) return;
    for (i = 0; i < w->set.count; i++) {
        q2_monster *m = &w->set.monsters[i];
        if (m->in_use && m->corpse && m->think == q2_cre_medic_revive)
            q2_cre_medic_revive(m);
    }
}

u32 q2_creature_world_tick(q2_creature_world *w, const s32 player_origin[3],
                           s16 player_health)
{
    if (!w || !w->ready)
        return 0;

    sight_place(w, player_origin);
    sight_health(w, player_health);
    return q2_monster_set_tick(&w->set);
}

/*
 * PlayerNoise — 0x80062B80, and without it two of FindTarget's three alert
 * arms can never fire.
 *
 * `q2_find_target` reads `sound_entity`/`sound_entity_framenum` (0x800E46EC and
 * 0x800E46F0) and `sound2_entity`/`sound2_entity_framenum` (0x800E46F4, F8).
 * 0x80062C68's `sltiu v0, s4, 2` sends types 0 and 1 to the first pair and
 * type 2 to the second, which an ambush creature ignores; each pair points at
 * its own noise entity, moved to `where` (0x80062CA0..0x80062CC0).
 *
 * The port used to point both at the sight client, the live player. A
 * creature that heard a shot then hunted the player rather than the noise,
 * woke the others with a sighting, and could not see the player it was hunting
 * because FindTarget's "already got one" returned first. See the note on
 * `noise` in creworld.h.
 *
 * The console's early outs: none at all in multiplayer (0x800AEBCC), none for
 * a NOTARGET player (0x80062C14), and the silencer's quiet shots (type 1,
 * 0x80062BC8). No shipped map places a silencer, and the callers in this port
 * raise noise only in single player.
 */
void q2_creature_world_player_noise(q2_creature_world *w, const s32 where[3],
                                    int type)
{
    q2_monster *n;

    if (!w || !w->ready || !where)
        return;
    if (w->sight.flags & Q2_FL_NOTARGET)
        return;

    n = &w->noise[type < 2 ? 0 : 1];
    if (type < 2) {
        q2_level_state.sound_entity          = n;
        q2_level_state.sound_entity_framenum = q2_level_state.framenum;
    } else {
        q2_level_state.sound2_entity          = n;
        q2_level_state.sound2_entity_framenum = q2_level_state.framenum;
    }
    n->pos[0] = where[0];
    n->pos[1] = where[1];
    n->pos[2] = where[2];

    /*
     * AND THE STAMP THAT KEEPS THE NOISE ALIVE. 0x80062CC8 loads level.time
     * from 0x800E46DC and 0x80062CD4 `sw v0, 208(a0)` writes it to the noise
     * entity's +0xD0 — teleport_time. ai_checkattack's AI_SOUND_TARGET arm
     * (src/game/ai.c) measures staleness against exactly that stamp.
     *
     * Nothing in this tree had ever written the field, so from level.time 51
     * onward `time - teleport_time > 50` was true on the creature's very first
     * check and the sound target was dropped before the investigate behaviour
     * — walk to the noise, stop and look at 768 units, hold fire — could run
     * for even one tick. A creature that heard a shot opened fire instead.
     */
    n->teleport_time = q2_level_state.time;
}

/* Case-insensitive substring, over a name that is not NUL-terminated in the
 * module image beyond 16 bytes. */
static bool name_has_word(const char *name, const char *word)
{
    u32 i, j;

    for (i = 0; i < 16 && name[i]; i++) {
        for (j = 0; word[j]; j++) {
            char a = name[i + j];
            char b = word[j];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (a != b)
                break;
        }
        if (!word[j])
            return true;
    }

    return false;
}

/*
 * The name record covering `frame` for this creature, out of its own module's
 * table — which is how the engine reaches an animation, per frame rather than
 * per move. NULL when the module names nothing there.
 */
const q2_cre_frame_name *q2_creature_world_frame_name(
        const q2_creature_world *w, const q2_monster *m, s32 frame)
{
    s32 mi;

    if (!w || !m)
        return NULL;

    mi = w->class_module[m->pop_class_id];
    if (mi < 0 || (u32)mi >= Q2_CREWORLD_MAX_MODULES)
        return NULL;

    return q2_creature_frame_name_at(w->mod[mi].frame_name,
                                     w->mod[mi].frame_name_count, frame);
}

const char *q2_creature_world_model_name(const q2_creature_world *w,
                                         const q2_monster *m)
{
    size_t i;

    if (!w || !m || !w->model_name || !w->set.monsters)
        return NULL;
    if (m < w->set.monsters || m >= w->set.monsters + w->set.count)
        return NULL;

    i = (size_t)(m - w->set.monsters);
    return w->model_name[i][0] ? w->model_name[i] : NULL;
}

/*
 * The first frame of the module's death animation, or -1.
 *
 * A creature's module names its own moves — a 20-byte {char[16], u16 first,
 * u16 last} record matched to a move by frame range — and every module on the
 * disc that carries names has at least one whose name says death. That name is
 * the only thing that identifies it: the moves are a flat list in frame order
 * with nothing marking their role.
 *
 * This exists because a killed creature FROZE. The body was still drawn — the
 * draw loop only checks `in_use` — but `q2_monster_set_tick` skipped anything
 * with `dead` set, so it stood in whatever pose the shot caught it in, mid
 * stride, forever. T_Damage ends by calling the entity's `die` at entity+0xA4
 * (0x80062A9C); this is the part of what a module's die does that can be
 * reconstructed from the module's own data rather than from its code.
 */
s32 q2_creature_world_death_frame(const q2_creature_world *w,
                                  const q2_monster *m)
{
    static const char *const k_words[] = { "death", "die", "dead", NULL };
    const q2_creature_module *mod = NULL;
    const char *names[Q2_CRE_MAX_MOVES];
    u32 i, k, named;

    if (!w || !m)
        return -1;

    /* `class_id` is the module`s class BYTE after spawning, not the Population
     * id — see the header. That byte is what identifies the module. */
    for (i = 0; i < w->mod_count; i++) {
        u32 j;

        if (!w->mod[i].ready)
            continue;
        for (j = 0; j < w->mod[i].cre.class_count; j++)
            if (w->mod[i].cre.class_byte[j] == m->class_id) {
                mod = &w->mod[i];
                break;
            }
        if (mod)
            break;
    }

    if (!mod)
        return -1;

    /* Cast for MSVC: it reads an array of pointers-to-const reaching a
     * void * parameter as discarding const (C4090). This array is a
     * local, and zeroing it is exactly what is meant. */
    memset((void *)names, 0, sizeof(names));
    named = q2_creature_move_names(&mod->cre, mod->image, mod->size, names,
                                   (u32)(sizeof(names) / sizeof(names[0])));
    if (!named)
        return -1;

    for (i = 0; i < mod->cre.move_count; i++) {
        if (!names[i])
            continue;
        for (k = 0; k_words[k]; k++)
            if (name_has_word(names[i], k_words[k]))
                return mod->cre.move[i].first_frame;
    }

    return -1;
}

/* Which module owns this creature's class. */
static const q2_creature_module *module_for(const q2_creature_world *w,
                                            const q2_monster *m)
{
    u32 i, j;

    if (!w || !m)
        return NULL;

    if (m->cre_bind) {
        for (i = 0; i < w->mod_count; i++)
            if (&w->mod[i].bind == m->cre_bind) return &w->mod[i];
        return NULL;
    }

    for (i = 0; i < w->mod_count; i++) {
        if (!w->mod[i].ready)
            continue;
        for (j = 0; j < w->mod[i].cre.class_count; j++)
            if (w->mod[i].cre.class_byte[j] == m->class_id)
                return &w->mod[i];
    }

    return NULL;
}

static bool module_sound_requests(q2_creature_module *mod,
    bool (*bank_has)(const char *, void *), void *user,
    q2_sfx_request *(*lookup)(const char *, void *))
{
    u32 i, fault;
    if (!mod->sounds_ready) {
        if (q2_creature_init_sound_requests(mod->image, mod->size, Q2_CREWORLD_BASE,
                bank_has, user, lookup, mod->sound_bind, 128, &mod->sound_count, &fault) != Q2_OK ||
            mod->sound_count > 128) {
            Q2_WARN("%s sound requests failed at %08X", mod->name, fault);
            return false;
        }
        for (i = 0; i < mod->sound_count; i++) {
            const q2_cre_sound_bind *b = &mod->sound_bind[i];
            if (!b->request_id || b->request_id > 128) return false;
            mod->sound_request[b->request_id - 1] = b->request;
        }
        mod->sounds_ready = true;
    }
    return true;
}

void q2_creature_world_init_sound_requests(q2_creature_world *w,
    bool (*bank_has)(const char *, void *), void *user,
    q2_sfx_request *(*lookup)(const char *, void *))
{
    u32 i;
    for (i = 0; i < w->mod_count; i++)
        if (w->mod[i].ready) module_sound_requests(&w->mod[i], bank_has, user, lookup);
}

q2_sfx_request *q2_creature_world_sound_request(q2_creature_world *w,
    const q2_monster *m, u32 addr, const char *registered,
    bool (*bank_has)(const char *, void *), void *user,
    q2_sfx_request *(*lookup)(const char *, void *), const char **name)
{
    const q2_creature_module *found = module_for(w, m);
    q2_creature_module *mod;
    u32 i;
    if (name) *name = NULL;
    if (!found) return NULL;
    mod = &w->mod[found - w->mod];
    if (!module_sound_requests(mod, bank_has, user, lookup)) return NULL;
    if (addr < Q2_CREWORLD_BASE && registered) {
        q2_cre_sound_bind trace[128];
        u32 n = q2_creature_sound_bindings(mod->image, mod->size, Q2_CREWORLD_BASE,
                                           trace, 128);
        for (i = 0; i < n && i < 128; i++)
            if (!strcmp(trace[i].name, registered)) { addr = trace[i].addr; break; }
    }
    for (i = 0; i < mod->sound_count; i++) {
        const q2_cre_sound_bind *b = &mod->sound_bind[i];
        if (b->addr != addr) continue;
        if (name) *name = b->name;
        if (!b->cloned && lookup) return lookup(b->name, user);
        return &mod->sound_request[b->request_id - 1];
    }
    return NULL;
}

const char *q2_creature_world_sound_for_addr(const q2_creature_world *w,
                                             const q2_monster *m, u32 addr)
{
    return q2_creature_world_sound_resolve(w, m, addr, NULL, NULL);
}

const char *q2_creature_world_sound_resolve(const q2_creature_world *w,
                                            const q2_monster *m, u32 addr,
                                            bool (*bank_has)(const char *, void *),
                                            void *user)
{
    static q2_cre_sound_bind binds[128];
    const q2_creature_module *mod = module_for(w, m);
    u32 n, fault;
    if (!mod) return NULL;
    if (q2_creature_init_sounds(mod->image, mod->size, Q2_CREWORLD_BASE,
            bank_has, user, binds, Q2PSX_ARRAY_COUNT(binds), &n, &fault) != Q2_OK) {
        Q2_WARN("%s sound initializer failed at %08X", mod->name, fault);
        return NULL;
    }
    if (n > Q2PSX_ARRAY_COUNT(binds)) return NULL;
    return q2_creature_sound_for_addr(binds, n, addr);
}

const char *q2_creature_world_sound_resolve_name(const q2_creature_world *w,
        const q2_monster *m, const char *registered,
        bool (*bank_has)(const char *, void *), void *user)
{
    q2_cre_sound_bind binds[128];
    const q2_creature_module *mod = module_for(w, m);
    u32 i, n;
    if (!mod || !registered) return NULL;
    n = q2_creature_sound_bindings(mod->image, mod->size, Q2_CREWORLD_BASE,
                                  binds, Q2PSX_ARRAY_COUNT(binds));
    if (n > Q2PSX_ARRAY_COUNT(binds)) return NULL;
    for (i = 0; i < n; i++) if (!strcmp(binds[i].name, registered))
        return q2_creature_world_sound_resolve(w, m, binds[i].addr, bank_has, user);
    return NULL;
}

const char *q2_creature_world_sound_name(const q2_creature_world *w,
                                         const q2_monster *m, u32 index)
{
    static const char *names[24];
    const q2_creature_module *mod = NULL;
    u32 i, n;

    if (!w || !m)
        return NULL;

    for (i = 0; i < w->mod_count; i++) {
        u32 j;

        if (!w->mod[i].ready)
            continue;
        for (j = 0; j < w->mod[i].cre.class_count; j++)
            if (w->mod[i].cre.class_byte[j] == m->class_id) {
                mod = &w->mod[i];
                break;
            }
        if (mod)
            break;
    }

    if (!mod)
        return NULL;

    /* Cast for MSVC: it reads an array of pointers-to-const reaching a
     * void * parameter as discarding const (C4090). This array is a
     * local, and zeroing it is exactly what is meant. */
    memset((void *)names, 0, sizeof(names));
    n = q2_creature_sound_names(&mod->cre, mod->image, mod->size, names,
                                (u32)(sizeof(names) / sizeof(names[0])));

    return (index < n) ? names[index] : NULL;
}

u32 q2_creature_world_summon(q2_creature_world *w, const char *group)
{
    u32 gi, i, woke = 0;

    if (!w || !w->pop_ready || !group || !group[0])
        return 0;

    for (gi = 0; gi < w->pop.group_count; gi++) {
        q2_pop_group g;

        if (!q2_pop_get_group(&w->pop, gi, &g))
            continue;
        if (strcmp(g.name, group) != 0)
            continue;

        for (i = 0; i < w->set.count; i++) {
            q2_monster *m = &w->set.monsters[i];

            if (m->group != gi || !m->spawn_pending)
                continue;
            /* Retail creates this record at CREBATCH time. The port keeps a
             * dormant placeholder, whose load-time floor can be stale after a
             * lift moves or a hatch disappears. Recompute from the authored
             * record, never by dropping the already-dropped position again.
             * Resumed actors and revived bodies retain their runtime origin. */
            if (!m->in_use && !m->dead && !m->spawn_initialized && w->placement_coll) {
                q2_pop_spawn rec;
                s32 origin[3];
                if (!q2_pop_get_spawn(&w->pop, &g, m->population_slot, &rec) ||
                    !q2_spawn_place(&rec, w->placement_coll, w->placement_world, origin))
                    continue;
                memcpy(m->pos, origin, sizeof(m->pos));
            }
            m->spawn_pending = false;
            if (m->in_use || m->dead)
                continue;
            m->in_use = true;
            m->population_active = true;
            /*
             * A held batch missed `q2_monster_set_wake`: that sweep quite
             * correctly skips records whose in-use latch is clear. Merely
             * raising the latch here therefore produced a model whose module
             * had been bound, but whose generic monster start never ran — no
             * stand move, no q2_M_MoveFrame think, and no next-think time.
             * BASE1's adjacent LiftRoom Soldier and Infantry are the visible
             * instance: CREBATCH made them appear and they remained inert.
             *
             * Retail does not pre-spawn and hold the group. Its selected-group
             * pass creates the entity at this point and reaches
             * monster_start_go as part of that spawn. Calling the same start
             * here closes the seam introduced by the port's dormant-record
             * representation without inventing a second wake path.
             */
            q2_monster_start_go(m);
            woke++;
        }
        break;
    }

    return woke;
}

static u32 hold_groups(q2_creature_world *w, int resident_zone,
                        const u8 *levelbin, u32 levelbin_size,
                        const u8 *groups, u32 group_count)
{
    u32 i, held = 0;
    u32 sel[32];
    u32 sel_count = 0;

    if (!w || !w->pop_ready)
        return 0;

    if (levelbin && levelbin_size)
        sel_count = q2_levelbin_selected(levelbin, levelbin_size, sel, 32);

    for (i = 0; i < w->set.count; i++) {
        q2_monster *m = &w->set.monsters[i];
        q2_pop_group g;
        int zone;

        if (!m->in_use)
            continue;
        if (!q2_pop_get_group(&w->pop, m->group, &g))
            continue;

        zone = q2_pop_group_zone(&g);

        if (groups) {
            if (m->group < group_count && groups[m->group] &&
                (zone < 0 || zone == resident_zone))
                continue;
            m->in_use = false;
            m->spawn_pending = true;
            held++;
            continue;
        }

        /*
         * The module's own answer first: a group its init SELECTS is one the
         * level starts with, whatever the group is called. The client resolves
         * conditional calls and supplies their shared item/creature selection
         * through hold_selected; this scan is the standalone fallback.
         */
        if (sel_count) {
            u32 k;
            bool selected = false;

            for (k = 0; k < sel_count; k++) {
                char nm[13];

                if (sel[k] + 12 > levelbin_size)
                    continue;
                memcpy(nm, levelbin + sel[k], 12);
                nm[12] = 0;
                if (strcmp(nm, g.name) == 0) {
                    selected = true;
                    break;
                }
            }

            if (selected && (zone < 0 || zone == resident_zone))
                continue;
        }

        /* Ownership is not selection: e.g. JAIL4's Zone1Key is an ambush,
         * while Zone1 is the ordinary population. Both belong to zone 1. */
        if (zone == resident_zone && q2_pop_group_is_zone(&g))
            continue;

        m->in_use = false;
        /* CREBATCH selects by exact name, without a zone-name test. Only the
         * earlier geometric/module/skill filters decide eligibility here:
         * overlapping zones can legitimately summon a differently named group. */
        m->spawn_pending = true;
        held++;
    }

    return held;
}

u32 q2_creature_world_hold_batches(q2_creature_world *w, int resident_zone,
                                   const u8 *levelbin, u32 levelbin_size)
{
    return hold_groups(w, resident_zone, levelbin, levelbin_size, NULL, 0);
}

u32 q2_creature_world_hold_selected(q2_creature_world *w, int resident_zone,
                                     const u8 *selected, u32 count)
{
    if (!selected)
        return 0;
    return hold_groups(w, resident_zone, NULL, 0, selected, count);
}

void q2_creature_world_free(q2_creature_world *w)
{
    u32 i;

    if (!w)
        return;

    q2_cre_set_lifecycle_hooks(NULL, 0, NULL, NULL, NULL);

    for (i = 0; i < w->mod_count; i++) {
        free(w->mod[i].image);
        w->mod[i].image = NULL;
    }
    w->mod_count = 0;

    free(w->model_name);
    w->model_name = NULL;

    /* The resolver and the touch stage both point into this world; drop them
     * before the storage goes, or monster_start_go on the NEXT level resolves
     * into freed memory. */
    q2_ai_set_pick_target(NULL, NULL);
    q2_ai_set_link_hooks(NULL, NULL, NULL);

    free(w->corner);
    w->corner       = NULL;
    w->corner_count = 0;

    q2_monster_set_free(&w->set);

    if (w->classes_ready) {
        q2_class_table_free(&w->classes);
        w->classes_ready = false;
    }

    /* The AI keeps a pointer to the sight client across ticks; this set is
     * going away, so it must not be left pointing into it. */
    if (q2_level_state.sight_client == &w->sight)
        q2_level_state.sight_client = NULL;

    w->ready = false;
    w->placement_coll = NULL;
    w->placement_world = NULL;
}
