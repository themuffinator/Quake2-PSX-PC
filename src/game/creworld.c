#include "creworld.h"

#include "worldscale.h"

#include "levelbin.h"

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
    char nm[13];
    size_t body;

    memcpy(nm, bin + boff, 12);
    nm[12] = 0;

    if (module_index(w, nm) >= 0)
        return true;                    /* already have it */
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

    /*
     * The actions each think index performs, decoded from the module's own
     * code. Without these a creature animates and does nothing; with them the
     * six modules that have no hand transcription still make their sounds and
     * swing their claws.
     */
    q2_creature_decode_thinks(&m->cre, m->image, m->size, Q2_CREWORLD_BASE,
                              m->think, Q2_CLASS_METHOD_COUNT);

    m->ready = q2_creature_bind(&m->bind, &m->cre, q2_cre_impl_find(nm));

    /*
     * AFTER the bind, not before. `q2_creature_bind` opens with
     * `memset(b, 0, sizeof(*b))`, so installing the think table first handed it
     * over and then wiped it two lines later — `q2_cre_run_think` found
     * `b->think` NULL and returned, every time, for every creature on the disc.
     *
     * The cost was not subtle and was invisible: six of the seven modules run
     * entirely on decoded actions, so none of them made a sound, swung a claw,
     * jumped a frame or set an AI flag. They walked and chased and did nothing,
     * and it looked exactly like the "not transcribed yet" state it was
     * supposed to be an improvement on. Counted on COMMAND: 31 thinks run, 31
     * unbound.
     */
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

static void modules_load(q2_creature_world *w, const q2_common_file *common)
{
    const dat_chunk *bin = common->chunk[q2_common_chunk_index("CreAIBin")];
    const dat_chunk *rel = common->chunk[q2_common_chunk_index("CreAIRel")];
    u32 boff = 0, roff = 0;

    if (!bin || !rel || bin->size <= Q2_RELOC_CREAI_PREAMBLE)
        return;

    while (boff + Q2_RELOC_CREAI_PREAMBLE < bin->size &&
           roff + Q2_RELOC_CREAI_PREAMBLE < rel->size) {
        u32 bnext = q2_rd_u32(bin->data + boff + 12);
        u32 rnext = q2_rd_u32(rel->data + roff + 12);

        module_take(w, bin->data, boff, bnext, bin->size,
                    rel->data, roff, rnext, rel->size);

        if (bnext <= boff || bnext >= bin->size)
            break;
        boff = bnext;
        roff = rnext;
    }
}

/*
 * The maps that ship creature modules, most-carrying first.
 *
 * Eight of the disc's levels ship an EMPTY `CreAIBin` — four bytes — and still
 * place creatures: JAIL2, JAIL3 and JAIL4 have Infantry, SECURITY, WASTE2,
 * BIGGUN, BOSS1 and BOSS2 likewise. Their spawn records name classes that the
 * class table resolves perfectly well; what is missing is the module, so the
 * port used to place nothing at all on them.
 *
 * A module is taken from a donor map when this map ships none. That rests on a
 * module of a given name being the same wherever it appears, which is what the
 * disc-wide census has relied on all along — it finds fifteen module instances
 * and reports seven DISTINCT, deduplicating by name — and which is the same
 * argument that settled `QMULTI.C`, byte-identical on all thirteen arenas.
 * It is an assumption, and it is written down here rather than buried.
 */
static const char *const k_module_donors[] = {
    "WASTE4", "POWER1", "BASE0", "COMMAND", "WASTE3", "LAB", "POWER2", NULL
};

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

        modules_load(w, &cf);
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
q2_result q2_creature_world_load(q2_creature_world *w, const disc *d,
                                 const q2_build_id *id,
                                 const q2_common_file *common,
                                 q2_collision *coll)
{
    q2_population pop;
    u32 i;

    if (!w || !common)
        return Q2_ERR_INVALID_ARG;

    q2_creature_world_free(w);
    memset(w, 0, sizeof(*w));

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

    modules_load(w, common);
    modules_borrow(w, d);
    classes_bind(w);

    if (q2_population_parse(&pop, common) != Q2_OK)
        return Q2_OK;                   /* a map with no population is fine */

    w->pop       = pop;
    w->pop_ready = true;

    if (q2_spawn_from_population(&w->set, &pop, coll, &w->stats) != Q2_OK)
        return Q2_ERR_NO_MEMORY;

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

        q2_creature_spawn(&w->mod[mi].bind, m, w->class_variant[pop_class]);
    }

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

void q2_creature_world_wake(q2_creature_world *w, const s32 player_origin[3])
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
    w->sight.health      = 100;
    w->sight.max_health  = 100;
    /* The eye the console's `visible` reconstructs from the origin: the
     * origin is feet - 286 and the eye is feet - 576, so -290. */
    w->sight.view_height = -(s16)(Q2_VIEW_STAND - Q2_EYE_BASE);
    sight_place(w, player_origin);

    q2_monster_set_wake(&w->set, &w->sight);
}

u32 q2_creature_world_tick(q2_creature_world *w, const s32 player_origin[3])
{
    if (!w || !w->ready)
        return 0;

    sight_place(w, player_origin);
    return q2_monster_set_tick(&w->set);
}

/*
 * PlayerNoise — 0x80062C74, and without it two of FindTarget's three alert arms
 * can never fire.
 *
 * `q2_find_target` reads `sound_entity`/`sound_entity_framenum` (0x800E46EC and
 * 0x800E46F0) and `sound2_entity`/`sound2_entity_framenum` (0x800E46F4, F8)
 * exactly as the original does — and NOTHING in this port ever wrote them. The
 * only alerting that worked was `sight_client`, which needs a creature to
 * already be looking the right way. So a player could empty a magazine in a
 * corridor and wake nobody: measured at 126 shots fired, 0 creatures hunting.
 *
 * The original spawns a noise ENTITY and keeps it; this port has one entity it
 * can point at — the sight client, which already stands where the player is —
 * so the pair is written with that. What matters to FindTarget is the position
 * and the framenum, and both are right.
 *
 * 0x80062C80's own split: a noise type under 2 is WEAPON noise and goes to the
 * first pair; anything else is the player's own and goes to the second. The
 * second is additionally suppressed for an ambush creature, which is the whole
 * reason the engine keeps them apart.
 */
void q2_creature_world_player_noise(q2_creature_world *w, bool weapon)
{
    if (!w || !w->ready)
        return;

    if (weapon) {
        q2_level_state.sound_entity          = &w->sight;
        q2_level_state.sound_entity_framenum = q2_level_state.framenum;
    } else {
        q2_level_state.sound2_entity          = &w->sight;
        q2_level_state.sound2_entity_framenum = q2_level_state.framenum;
    }
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

    memset(names, 0, sizeof(names));
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

    for (i = 0; i < w->mod_count; i++) {
        if (!w->mod[i].ready)
            continue;
        for (j = 0; j < w->mod[i].cre.class_count; j++)
            if (w->mod[i].cre.class_byte[j] == m->class_id)
                return &w->mod[i];
    }

    return NULL;
}

const char *q2_creature_world_sound_for_addr(const q2_creature_world *w,
                                             const q2_monster *m, u32 addr)
{
    static q2_cre_sound_bind binds[48];
    const q2_creature_module *mod = module_for(w, m);
    u32 n;

    if (!mod)
        return NULL;

    /*
     * `Q2_CREWORLD_BASE` is where the loader relocates a module, and the
     * registrations store absolute addresses — so the decode and the play
     * site's `addr` are already in the same space and nothing has to be
     * rebased.
     */
    n = q2_creature_sound_bindings(mod->image, mod->size, Q2_CREWORLD_BASE,
                                   binds,
                                   (u32)(sizeof(binds) / sizeof(binds[0])));

    return q2_creature_sound_for_addr(binds, n, addr);
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

    memset(names, 0, sizeof(names));
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

            if (m->group != gi || m->in_use || m->dead)
                continue;
            m->in_use = true;

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

u32 q2_creature_world_hold_batches(q2_creature_world *w, int resident_zone,
                                   const u8 *levelbin, u32 levelbin_size)
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

        /*
         * The module's own answer first: a group its init SELECTS is one the
         * level starts with, whatever the group is called. That is what makes
         * JAIL3's `Jail4Return` reachable at all — the naming rule below would
         * hold it back for ever, since no CREBATCH names it either.
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

        /* A group named after THIS zone is the level's own population. One
         * named after another zone is that zone's business. Anything else is a
         * batch and waits to be called for. */
        if (zone == resident_zone)
            continue;

        m->in_use = false;
        held++;
    }

    return held;
}

void q2_creature_world_free(q2_creature_world *w)
{
    u32 i;

    if (!w)
        return;

    for (i = 0; i < w->mod_count; i++) {
        free(w->mod[i].image);
        w->mod[i].image = NULL;
    }
    w->mod_count = 0;

    free(w->model_name);
    w->model_name = NULL;

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
}
