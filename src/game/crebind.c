#include "crebind.h"

#include "ai.h"           /* q2_vector_length, import +0xB8 / 0x8005C4E8 */

#include <string.h>

/* One bind per class byte, which is how the engine's own table is indexed. */
static q2_cre_bind *g_bind[Q2_CLASS_COUNT];
static void (*g_hurt)(q2_monster *, q2_monster *, s16, int, const s32 *, void *);
static void (*g_beam)(q2_monster *, q2_monster *, s16, void *);
static void *g_contact_user;
static void (*g_explosion)(q2_monster *, const s32 *, s32, void *);
static void *g_explosion_user;
static void (*g_sound_owner)(q2_monster *, u32, const q2_monster *, void *);
static void *g_sound_owner_user;
static void (*g_named_sound)(q2_monster *, const char *, void *);
static void *g_named_sound_user;
static q2_monster *g_roster;
static u32 g_roster_count;
static q2_monster *(*g_spawn_child)(q2_monster *, u8, void *);
static bool (*g_revive)(q2_monster *, q2_monster *, void *);
static void *g_lifecycle_user;
static void (*g_actor_effect)(q2_monster *, u32, u8, void *);
static void (*g_actor_spawn)(q2_monster *, void *);
static void *g_actor_user;

void q2_cre_shared_capture(q2_cre_shared_state *s, const q2_cre_bind *b)
{
    memset(s, 0, sizeof(*s));
    if (!b) return;
    s->present = true; s->flyer_transition = b->flyer_transition;
    s->strider_turn = b->strider_turn; s->strider_explosion_phase = b->strider_explosion_phase;
    s->boss1_explosion_phase = b->boss1_explosion_phase; s->jorg_explosion_phase = b->jorg_explosion_phase;
}
void q2_cre_shared_restore(q2_cre_bind *b, const q2_cre_shared_state *s)
{
    if (!b || !s->present) return;
    b->flyer_transition = s->flyer_transition; b->strider_turn = s->strider_turn;
    b->strider_explosion_phase = s->strider_explosion_phase;
    b->boss1_explosion_phase = s->boss1_explosion_phase; b->jorg_explosion_phase = s->jorg_explosion_phase;
}

void q2_cre_set_actor_hooks(void (*effect)(q2_monster *, u32, u8, void *),
                            void (*spawn)(q2_monster *, void *), void *user)
{ g_actor_effect = effect; g_actor_spawn = spawn; g_actor_user = user; }
void q2_cre_actor_effect(q2_monster *m, u32 index, u8 ticks)
{
    if (!m || index >= sizeof(m->effect)) return;
    m->effect[index] = ticks;
    if (g_actor_effect) g_actor_effect(m, index, ticks, g_actor_user);
}

void q2_cre_set_named_sound_hook(void (*fn)(q2_monster *, const char *, void *), void *user)
{ g_named_sound = fn; g_named_sound_user = user; }
void q2_cre_play_named_sound(q2_monster *m, const char *name)
{ if (g_named_sound) g_named_sound(m, name, g_named_sound_user); }
void q2_cre_set_lifecycle_hooks(q2_monster *monsters, u32 count,
    q2_monster *(*spawn)(q2_monster *, u8, void *),
    bool (*revive)(q2_monster *, q2_monster *, void *), void *user)
{
    g_roster = monsters; g_roster_count = count;
    g_spawn_child = spawn; g_revive = revive; g_lifecycle_user = user;
}
q2_monster *q2_cre_roster(u32 *count) { *count = g_roster_count; return g_roster; }
q2_monster *q2_cre_spawn_child(q2_monster *m, u8 pop_class)
{ return g_spawn_child ? g_spawn_child(m, pop_class, g_lifecycle_user) : NULL; }
bool q2_cre_revive(q2_monster *medic, q2_monster *corpse)
{ return g_revive && g_revive(medic, corpse, g_lifecycle_user); }

q2_class_method q2_cre_method_for(const q2_monster *m, u32 index)
{
    q2_cre_bind *b = m ? m->cre_bind : NULL;
    if (b && b->impl && index < Q2_CLASS_METHOD_COUNT && b->impl->method[index])
        return b->impl->method[index];
    return m ? q2_class_method_get(m->class_id, index) : NULL;
}

void q2_cre_set_sound_owner_hook(void (*fn)(q2_monster *, u32, const q2_monster *, void *),
                                 void *user)
{
    g_sound_owner = fn; g_sound_owner_user = user;
}
void q2_cre_play_sound_owner(q2_monster *m, u32 handle, const q2_monster *owner)
{
    extern void (*q2_cre_sound_fn)(q2_monster *, int, void *);
    extern void *q2_cre_sound_user;
    if (g_sound_owner) g_sound_owner(m, handle, owner, g_sound_owner_user);
    else if (q2_cre_sound_fn) q2_cre_sound_fn(m, (int)handle, q2_cre_sound_user);
}

void q2_cre_set_explosion_hook(void (*fn)(q2_monster *, const s32 *, s32, void *),
                                 void *user)
{
    g_explosion = fn; g_explosion_user = user;
}
void q2_cre_explosion(q2_monster *self, const s32 point[3], s32 scale)
{
    if (g_explosion) g_explosion(self, point, scale, g_explosion_user);
}

void q2_cre_set_contact_hooks(
    void (*hurt)(q2_monster *, q2_monster *, s16, int, const s32 *, void *),
    void (*beam)(q2_monster *, q2_monster *, s16, void *), void *user)
{
    g_hurt = hurt; g_beam = beam; g_contact_user = user;
}
void q2_cre_hurt(q2_monster *self, q2_monster *target, s16 damage,
                  int mod, const s32 point[3])
{
    if (g_hurt) g_hurt(self, target, damage, mod, point, g_contact_user);
}
void q2_cre_beam(q2_monster *self, q2_monster *target, s16 vertex)
{
    if (g_beam) g_beam(self, target, vertex, g_contact_user);
}

void q2_cre_bind_reset(void)
{
    memset(g_bind, 0, sizeof(g_bind));
}

q2_cre_bind *q2_cre_bind_for(const q2_monster *m)
{
    if (!m)
        return NULL;
    return m->cre_bind ? m->cre_bind : g_bind[m->class_byte];
}

/*
 * Resolve a move's end callback.
 *
 * The address is the module's, so it is looked up in the two sets the
 * implementation already covers: the monsterinfo callbacks and the method
 * table. Anything else leaves the move looping.
 */
static q2_endfunc resolve_endfunc(const q2_creature *c, const q2_cre_impl *impl,
                                  u32 addr)
{
    u32 i;

    if (!addr)
        return NULL;

    for (i = 0; i < 13; i++)
        if (c->callback[i] == addr)
            return (q2_endfunc)impl->callback[i];

    for (i = 0; i < c->method_count; i++)
        if (c->method[i] == addr)
            return (q2_endfunc)impl->method[i];

    return NULL;
}

/*
 * The endfunc for a move whose own ends in "install this other move".
 *
 * It takes only the entity, so which move to install has to come from the
 * entity: `m->currentmove` is still the move that just finished, and its
 * decoded record carries the address the module's installer would have written.
 */
static void chain_endfunc(q2_monster *m)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b || !b->cre || !m->currentmove)
        return;

    for (i = 0; i < b->move_count && i < b->cre->move_count; i++) {
        if (&b->move[i] != m->currentmove)
            continue;

        {
            u32 target = b->cre->move[i].endfunc_move;
            u32 k;

            if (!target)
                return;

            for (k = 0; k < b->cre->move_count; k++)
                if (b->cre->move[k].addr == target) {
                    q2_cre_set_move(m, b->cre->move[k].first_frame);
                    return;
                }
        }
        return;
    }
}

bool q2_creature_bind(q2_cre_bind *b, const q2_creature *c,
                      const q2_cre_impl *impl)
{
    u32 i;

    if (!b || !c || !impl)
        return false;

    memset(b, 0, sizeof(*b));
    b->cre  = c;
    b->impl = impl;

    for (i = 0; i < c->move_count && i < Q2_CRE_MAX_MOVES; i++) {
        q2_endfunc fn = resolve_endfunc(c, impl, c->move[i].endfunc_addr);

        /*
         * An endfunc that is nothing but an installer is neither a callback nor
         * a think, so `resolve_endfunc` cannot name it and returned NULL — and
         * the move chain it exists to make simply did not happen. The decoder
         * records which move such a function installs; this runs it.
         *
         * The Gunner's hitscan is entirely on the far side of one of these: its
         * fire think lives in move 144-151, which no callback installs and
         * which `M_137_143`'s three-instruction endfunc at `module+0x11D8` is
         * the only route to.
         */
        if (!fn && c->move[i].endfunc_move)
            fn = chain_endfunc;

        q2_creature_mmove(c, &c->move[i], fn, &b->move[i]);
        b->move_count++;
    }

    /* Register the think handlers for every class byte this module serves,
     * exactly as the module's own init does through import +0x118. */
    for (i = 0; i < c->class_count; i++) {
        u32 k;
        /* Jorg registers two DIFFERENT tables. Never install its own
         * methods over the embedded Rider's class. */
        if (c->class_method_table[i] &&
            c->class_method_table[i] != c->method_table_addr) continue;
        for (k = 0; k < Q2_CLASS_METHOD_COUNT; k++) {
            /*
             * A think index the implementation does not write falls back to the
             * generic one, which runs the action the decoder read out of that
             * very function. That is what makes a PARTIAL transcription
             * worthwhile: a creature can have one callback written by hand and
             * keep the decoded actions for everything else, instead of the
             * choice being all thirty-two or none.
             */
            q2_class_method fn = impl->method[k];

            if (!fn && impl != &q2_cre_generic)
                fn = q2_cre_generic.method[k];

            q2_class_method_set(c->class_byte[i], k, fn);
        }
        g_bind[c->class_byte[i]] = b;
    }

    b->ready = true;
    return true;
}

void q2_creature_bind_move_names(q2_cre_bind *b, const char *const *names,
                                 u32 count)
{
    if (!b)
        return;
    b->move_name       = names;
    b->move_name_count = count;
}

void q2_creature_bind_thinks(q2_cre_bind *b, const q2_cre_think *think,
                             u32 count)
{
    if (!b)
        return;
    b->think       = think;
    b->think_count = count;
}

void q2_creature_prepare(q2_cre_bind *b, q2_monster *m, u32 class_index)
{
    const q2_creature *c;

    if (!b || !b->ready || !m)
        return;

    c = b->cre;

    if (class_index >= c->class_count)
        class_index = 0;

    m->cre_bind = b;
    if (c->class_count) {
        m->class_byte = c->class_byte[class_index];
        m->class_id   = c->class_byte[class_index];
    }

    if (c->speed_scale > 0)
        m->speed_scale = (u8)c->speed_scale;

    /*
     * A callback the MODULE does not have must stay NULL, even when the
     * implementation offers one.
     *
     * The generic implementation supplies a handler for every slot, because it
     * does not know which creature it is being used for. Installing all of them
     * unconditionally tells the AI things about the creature that are not true,
     * and one of them matters: `M_CheckAttack` picks melee over a missile only
     * when `m->melee` is set (0x8005DB04, and the transcription in ai.c). The
     * Tank Commander's module has no melee callback at all — the census lists
     * stand, idle, walk, run, attack, sight, pain and die — so it was being sent
     * to a melee it does not have, 43 times in a 400-frame capture, where the
     * generic handler found no move to play and it stood there. It never
     * reached a missile attack, which is why it never fired.
     *
     * `c->callback[i]` is the module's own address for that slot, zero when it
     * has none, so the module decides and the implementation only supplies.
     */
#define INSTALL(field, slot)     m->field = c->callback[slot]                    ? (void (*)(q2_monster *))b->impl->callback[slot] : NULL

    INSTALL(stand,   0);
    INSTALL(idle,    1);
    INSTALL(search,  2);
    INSTALL(walk,    3);
    INSTALL(run,     4);
    INSTALL(attack,  6);
    INSTALL(melee,   7);
#undef INSTALL

    /* dodge, sight and checkattack take extra arguments, so they are wired
     * through their own signatures rather than the flat table. */
    /*
     * DODGE — slot 5, and the reason it was never installed is not an
     * oversight in this port. It is an oversight in the ORIGINAL.
     *
     * `cre_soldier.c` used to carry a comment calling the empty slot "A GAP,
     * not a decision", which was the right instinct applied to the wrong side
     * of the boundary. Three of the disc's seven modules install a dodge
     * handler — the Soldier writes `module+0x22F0` to entity+0xF4 in all three
     * of its variant arms (0x801016C0, 0x801017C8, 0x801018CC), and the Gunner
     * and Infantry do the same — so the slot is plainly meant to be live.
     *
     * It is not. Sweeping all 632,832 bytes of `SLES_015.34` for a word load
     * off entity+0xF4 finds **zero** on any base but `sp`, and zero
     * `addiu rX, rY, 0xF4` that could reach it indirectly. Every neighbouring
     * monsterinfo slot has readers and they are easy to find — stand +0xE0 has
     * eleven (0x8005D228, 0x80061BFC, …), run +0xF0 has five, attack +0xF8 has
     * two (0x8005DB64 and 0x8005E298, both inside `M_CheckAttack`), melee +0xFC
     * has seven, sight +0x100 one, checkattack +0x104 two. Dodge has none.
     * id calls it from `check_dodge`, which the weapon fire path invokes when a
     * shot is aimed at a monster; that call site does not exist on this build.
     *
     * So the handler is installed here because the module writes it and a
     * faithful entity carries what the module wrote — and it is deliberately
     * given no caller, because the console has none. A creature on this disc
     * cannot dodge, and that is the disc's behaviour rather than this port's
     * shortfall.
     */
    if (b->impl->callback[5] && c->callback[5])
        m->dodge = (void (*)(q2_monster *, q2_monster *, s32))
                   (void *)b->impl->callback[5];

    if (b->impl->callback[8] && c->callback[8])
        m->sight = (void (*)(q2_monster *, q2_monster *))
                   (void *)b->impl->callback[8];
    if (b->impl->callback[9] && c->callback[9])
        m->checkattack = (bool (*)(q2_monster *))(void *)b->impl->callback[9];
    if (b->impl->callback[10] && c->callback[10])
        m->bigturn = (void (*)(q2_monster *, s32))(void *)b->impl->callback[10];

    /*
     * Slots 11 and 12 — pain and die — which the install list simply did not
     * cover, so both were dead code in every creature on the disc. The module
     * writes them to entity+0xA0 and +0xA4 (the Soldier at 0x80101684 and
     * 0x80101690), and they take extra arguments like sight does.
     */
    if (b->impl->callback[11] && c->callback[11])
        m->pain = (void (*)(q2_monster *, s16))(void *)b->impl->callback[11];
    if (b->impl->callback[12] && c->callback[12])
        m->die = (void (*)(q2_monster *, s16))(void *)b->impl->callback[12];

    m->in_use      = true;
    m->spawnflags |= Q2_SVFLAG_INUSE;
    if (!b->impl->scripted) m->svflags |= Q2_SVF_MONSTER;

    /*
     * AND IT CAN BE HURT, which nothing in this port had ever said.
     *
     * `monster_start` writes DAMAGE_AIM into entity+0x1C bits 30..31 at
     * 0x80061A94 (`and 0x3FFFFFFF` / `or 0x80000000`), beside `svflags |= 4`
     * and `nextthink = level.time + 1`. Only the die and duck handlers were
     * writing the field on this side, so a LIVE creature carried zero — and
     * once the combat sweep started honouring it, as T_Damage does, every
     * living creature on the disc became invulnerable. `tests/test_combat.c`
     * caught it on the first run.
     */
    m->takedamage = b->impl->scripted ? Q2_DAMAGE_NO : Q2_DAMAGE_AIM;
    if (b->impl->scripted) q2_cre_script_callbacks(m);
}

void q2_creature_spawn(q2_cre_bind *b, q2_monster *m, u32 class_index)
{
    if (!b || !b->ready || !m) return;
    q2_creature_prepare(b, m, class_index);
    m->spawn_initialized = true;
    q2_cre_motion_init(m);

    /*
     * AND THE MODULE'S OWN SPAWN FUNCTION, WHICH HAD NO CALLER AT ALL.
     *
     * `q2_cre_impl.spawn` has been declared in crebind.h since the binder was
     * written, with a comment saying it is "for anything the module's spawn
     * function does beyond the callbacks". Nothing ever ran it —
     * `grep -rn 'impl->spawn' src/` returned nothing — and four of the seven
     * transcriptions now supply one.
     *
     * The Arachner's is the one that shows: its module ends export 0 by
     * writing `currentmove = Stand`, and without that a placed Arachner starts
     * with a NULL currentmove, which `q2_M_MoveFrame` returns on immediately.
     * It stood inert until an AI callback happened to install something.
     *
     * Run after the class byte, the scale, the mass and the callbacks,
     * because that is the order the loader uses: 0x8007E68C and 0x8007E698
     * write health and gib_health from the class row and only then does
     * 0x8007E6AC call the module's export 0, which is free to overwrite them.
     */
    if (b->impl && b->impl->spawn)
        b->impl->spawn(m);

    /*
     * THE DENOMINATOR, and it is the console's own rather than a headcount.
     *
     * `monster_start` increments `level.total_monsters` at 0x80061A64 for every
     * creature it starts that is not a good guy — the same exclusion `Killed`
     * applies to the kill counter. The pair moves together, so "kills 3/9" is
     * two numbers the original keeps rather than two scans of a live array.
     *
     * AFTER THE MODULE'S SPAWN, not before it, because the test reads a bit the
     * module writes. monster_start is reached through the start wrapper the
     * module's spawn calls (monster.h), so its 0x80061A48 `lw v0, 220(s0)` /
     * 0x80061A50 `andi v0, v0, 0x100` / 0x80061A54 `bne` sees aiflags as the
     * module has left them at that call. The Insane raises AI_GOOD_GUY at
     * 0x80100904 (`ori v1, v1, 0x100`, stored at 0x80100908) and only then
     * calls its wrapper, at 0x80100994 or 0x801009AC — so on the console an
     * Insane is never counted. Counted before the hook, every Insane on LAB
     * was. Counting after the whole hook rather than at the wrapper call
     * differs only for a module that changes AI_GOOD_GUY after that call; the
     * Insane does not (its tail is the crucified override, 0x801009EC on), and
     * the other six were not read for it — INFERRED.
     */
    if (!b->impl->scripted && !(m->aiflags & Q2_AI_GOOD_GUY))
        q2_level_state.total_monsters++;
    if (g_actor_spawn) g_actor_spawn(m, g_actor_user);
}

const q2_mmove *q2_cre_find_move(const q2_monster *m, s32 first_frame)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b)
        return NULL;

    /*
     * SKIP THE CLIP PIECES. `b->move[]` carries both the module's own mmove
     * records and the ranges the decoder cut out of them at the model's clip
     * boundaries (creature.h). Only the first kind is a move a creature can be
     * put into; a piece has no endfunc and a truncated range, and installing
     * one freezes the creature.
     *
     * They stay in the array rather than being filtered out of it because
     * `chain_endfunc` above and the client's move-name lookup both index
     * `b->move[]` and `b->cre->move[]` in lockstep.
     */
    for (i = 0; i < b->move_count; i++) {
        if (b->cre && i < b->cre->move_count && b->cre->move[i].clip_piece)
            continue;
        if (b->move[i].first_frame == first_frame)
            return &b->move[i];
    }

    return NULL;
}

bool q2_cre_set_move(q2_monster *m, s32 first_frame)
{
    const q2_mmove *mv = q2_cre_find_move(m, first_frame);

    if (!mv || !m)
        return false;

    m->currentmove = mv;
    return true;
}

/*
 * By module address, for the two creatures whose first frames collide.
 *
 * `q2_cre_bind` keeps its runtime moves parallel to the decoded ones, so the
 * decoded record's `image_offset` — which is the module-relative address the
 * callback's `lui`/`addiu` pair materialised — identifies a move exactly where
 * a frame number cannot. See the note in crebind.h for which two.
 */
const q2_mmove *q2_cre_find_move_at(const q2_monster *m, u32 module_addr)
{
    q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b || !b->cre)
        return NULL;

    for (i = 0; i < b->move_count && i < b->cre->move_count; i++) {
        /*
         * Clip pieces are skipped for the same reason `q2_cre_find_move`
         * skips them: a piece is a copy of its parent record and carries the
         * parent's address, so installing one freezes the creature on a
         * fragment of its own animation.
         */
        if (b->cre->move[i].clip_piece)
            continue;
        if (b->cre->move[i].addr == module_addr)
            return &b->move[i];
    }

    return NULL;
}

bool q2_cre_set_move_at(q2_monster *m, u32 module_addr)
{
    const q2_mmove *mv = q2_cre_find_move_at(m, module_addr);

    if (!mv || !m)
        return false;

    m->currentmove = mv;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Sound fallbacks — see q2_cre_sound_resolve in crebind.h                    */
/* ------------------------------------------------------------------------- */
const char *q2_cre_sound_resolve(const q2_monster *m, const char *registered,
                                 bool (*bank_has)(const char *name, void *user),
                                 void *user)
{
    const q2_cre_bind *b = q2_cre_bind_for(m);
    const char *module = NULL;

    /* The decoded header name is the module's own; the implementation's name
     * is what q2_cre_impl_find matched it with, so the two agree whenever a
     * transcription is bound. */
    if (b && b->cre && b->cre->name[0])
        module = b->cre->name;
    else if (b && b->impl)
        module = b->impl->name;

    return q2_creature_sound_fallback(module, registered, bank_has, user);
}

/* ------------------------------------------------------------------------- */
/* The shot hook — see q2_cre_shot in crebind.h                               */
/* ------------------------------------------------------------------------- */
static void (*g_shot_fn)(q2_monster *m, const q2_cre_shot *shot, void *user);
static void  *g_shot_user;

void q2_cre_set_shot_hook(void (*fn)(q2_monster *m, const q2_cre_shot *shot,
                                     void *user),
                          void *user)
{
    g_shot_fn   = fn;
    g_shot_user = user;
}

/* The muzzle hook — see q2_cre_muzzle in crebind.h. */
static bool (*g_muzzle_fn)(const q2_monster *m, s16 vert_a, s16 vert_b,
                           s32 out[3], void *user);
static void  *g_muzzle_user;

void q2_cre_set_muzzle_hook(bool (*fn)(const q2_monster *m, s16 vert_a,
                                       s16 vert_b, s32 out[3], void *user),
                            void *user)
{
    g_muzzle_fn   = fn;
    g_muzzle_user = user;
}

void q2_cre_muzzle(const q2_monster *m, s16 vert_a, s16 vert_b, s32 out[3])
{
    if (!m || !out)
        return;
    if (g_muzzle_fn && g_muzzle_fn(m, vert_a, vert_b, out, g_muzzle_user))
        return;
    out[0] = m->pos[0];
    out[1] = m->pos[1];
    out[2] = m->pos[2];
}

/* The attachment hooks — see q2_cre_attach_blend in crebind.h. */
static bool (*g_attach_blend_fn)(const q2_monster *m, s16 vert_a, s16 vert_b,
                                 q2_cre_attach *out, void *user);
static bool (*g_attach_place_fn)(const q2_monster *m, const q2_cre_attach *a,
                                 s32 out[3], void *user);
static void  *g_attach_user;

void q2_cre_set_attach_hooks(
    bool (*blend)(const q2_monster *m, s16 vert_a, s16 vert_b,
                  q2_cre_attach *out, void *user),
    bool (*place)(const q2_monster *m, const q2_cre_attach *a, s32 out[3],
                  void *user),
    void *user)
{
    g_attach_blend_fn = blend;
    g_attach_place_fn = place;
    g_attach_user     = user;
}

bool q2_cre_attach_blend(const q2_monster *m, s16 vert_a, s16 vert_b,
                         q2_cre_attach *out)
{
    if (!m || !out || !g_attach_blend_fn)
        return false;
    return g_attach_blend_fn(m, vert_a, vert_b, out, g_attach_user);
}

void q2_cre_attach_place(const q2_monster *m, const q2_cre_attach *a,
                         s32 out[3])
{
    if (!m || !out)
        return;
    if (a && g_attach_place_fn && g_attach_place_fn(m, a, out, g_attach_user))
        return;
    out[0] = m->pos[0];
    out[1] = m->pos[1];
    out[2] = m->pos[2];
}

/*
 * The melee hook itself lives beside the sound hook in cre_soldier.c, which is
 * where the first transcribed creature put it; this is the engine side of it.
 */
extern void (*q2_cre_melee_fn)(q2_monster *m, const s32 aim[3],
                               s32 damage, s32 kick, void *user);
extern void  *q2_cre_melee_user;

bool q2_cre_melee_contact(const q2_monster *m, const s32 aim[3], s32 point[3])
{
    s32 reach[3], forward[3], right[3], up[3], distance, side;
    int k;
    if (!m || !m->enemy || !aim || !point) return false;
    for (k = 0; k < 3; k++) reach[k] = m->enemy->pos[k] - m->pos[k];
    distance = q2_vector_length(reach);
    if (distance > aim[0]) return false;
    side = aim[2];
    if (side > m->mins[0] && side < m->maxs[0])
        distance -= m->enemy->maxs[0];
    else
        side = side < 0 ? m->enemy->mins[0] : m->enemy->maxs[0];
    q2_angle_vectors(m->angles, forward, right, up);
    q2_vector_ma(m->pos, distance, forward, point);
    q2_vector_ma(point, side, right, point);
    q2_vector_ma(point, -aim[1], up, point);
    return true;
}

void q2_cre_melee_impulse(q2_monster *victim, const s32 point[3], s32 kick,
                          s16 impulse[3])
{
    s32 dir[3];
    const s32 zero[3] = { 0, 0, 0 };
    int k;
    if (!victim || !point || !impulse) return;
    /* GetAbsBounds + half the local extent, then hit -> centre. */
    for (k = 0; k < 3; k++)
        dir[k] = victim->pos[k] + victim->mins[k] +
                   (victim->maxs[k] - victim->mins[k]) / 2 - point[k];
    q2_vector_normalize(dir);
    q2_vector_ma(zero, kick, dir, victim->velocity);
    /* Vertical velocity is scaled in place; X/Z are scaled only at the
     * halfword accumulator. The upward cap applies at every difficulty. */
    victim->velocity[1] *= 16;
    if (victim->velocity[1] < -3072) victim->velocity[1] = -3072;
    impulse[0] = (s16)(victim->velocity[0] * 16);
    impulse[1] = (s16)victim->velocity[1];
    impulse[2] = (s16)(victim->velocity[2] * 16);
    if (victim->velocity[1] < 0) victim->on_ground = false;
}

void q2_cre_fire_hit(q2_monster *m, const s32 aim[3], s32 damage, s32 kick)
{
    s32 reach[3];

    if (!m || !aim)
        return;

    q2_cre_actions.melee_calls++;

    if (!q2_cre_melee_fn) {
        q2_cre_actions.melee_no_hook++;
        return;
    }

    /*
     * No enemy, no swing. The console would fault here rather than decline —
     * 0x80061144 is `lw v0, 188(s3)` followed by `lw v0, 0(v0)` with no test —
     * so this is a port guard, not a transcribed one; the modules only reach a
     * melee frame with an enemy. Counted rather than silent so that a module
     * which does get here without one is visible instead of merely harmless.
     */
    if (!m->enemy) {
        q2_cre_actions.melee_no_enemy++;
        return;
    }

    /* 0x80061144..0x8006119C. See the header for the four instructions. */
    reach[0] = m->enemy->pos[0] - m->pos[0];
    reach[1] = m->enemy->pos[1] - m->pos[1];
    reach[2] = m->enemy->pos[2] - m->pos[2];
    if (q2_vector_length(reach) > aim[0]) {
        q2_cre_actions.melee_short++;
        return;
    }

    q2_cre_actions.melee_sent++;
    q2_cre_melee_fn(m, aim, damage, kick, q2_cre_melee_user);
}

void q2_cre_fire_shot(q2_monster *m, const q2_cre_shot *shot)
{
    if (!m || !shot)
        return;

    q2_cre_actions.fire_calls++;

    if (!g_shot_fn) {
        q2_cre_actions.fire_no_hook++;
        return;
    }
    /*
     * THE ENEMY-ALIVE GUARDS BELONG TO THE REFIRE CALLBACKS, NOT TO THE SHOT,
     * and putting them here made every creature decline shots the console
     * takes.
     *
     * Read off the Tank Commander's machine gun, `moddisasm COMMAND 0x80100F90`
     * then 0x80101074: the fire think loads the enemy at 0x80101074 `lw a3,
     * 188(s2)` and 0x8010107C `beq a3, zero, 0x801010F4` jumps to a branch that
     * writes a literal zero into sp+32 — the PITCH — and then falls straight
     * through to the yaw computation at 0x801010F8 and the fire call at
     * 0x80101160. It fires with no enemy, aimed flat. Nowhere in the think is
     * obj+0x108, the health, loaded at all; the only enemy field it reads
     * besides the origin is +0x4C, the view height, at 0x801010C0.
     *
     * The health test lives one level up, in the refire callbacks:
     * `moddisasm COMMAND 0x8010136C` opens `lh v0, 264(v0)` — 0x108 — then
     * `blez v0, 0x801013BC`, the arm that ENDS the burst, and 0x8010144C is the
     * same. So the console's rule is "finish the burst you started, and decide
     * at the refire whether to start another", not "check before every round".
     *
     * The two cases are still counted, as OBSERVATIONS rather than as gates,
     * so the census can say how many shots went out at nothing — in
     * `shot_no_enemy` and `shot_dead_enemy`, NOT in `fire_no_enemy` and
     * `fire_dead_enemy`. Those two stay the decoded path's REFUSALS
     * (cre_actions.c), which with `fire_sent` and `fire_no_hook` partition
     * `fire_calls`; a shot sent from here counted in them as well would be
     * counted twice by any line that adds them up.
     *
     * This is not a Tank fix. The port's own files had recorded the same
     * departure for two more creatures — cre_infantry.c above
     * k_infantry_machinegun, whose death burst at module 0x80101038 never
     * touches the enemy, and cre_gunner.c above gunner_grenade — and both now
     * record that it is gone. Where a module's refire really does have the
     * guard the port already transcribes it: tankcomm_refire_rocket and
     * tankcomm_reattack_blaster (cre_tankcomm.c) for the two callbacks above,
     * and soldier_attack1_refire1, soldier_attack1_refire2,
     * soldier_attack2_refire1, soldier_attack2_refire2 and
     * soldier_attack6_refire (cre_soldier.c, module+0x1B5C, +0x1C18, +0x1CF0,
     * +0x1DAC and +0x1FDC).
     */
    if (!m->enemy)
        q2_cre_actions.shot_no_enemy++;
    else if (m->enemy->health <= 0)
        q2_cre_actions.shot_dead_enemy++;

    q2_cre_actions.fire_sent++;
    g_shot_fn(m, shot, g_shot_user);
}

/* ------------------------------------------------------------------------- */
/* The registry of built-in implementations                                   */
/* ------------------------------------------------------------------------- */
extern const q2_cre_impl q2_cre_soldier;
extern const q2_cre_impl q2_cre_tankcomm;
extern const q2_cre_impl q2_cre_gunner;
extern const q2_cre_impl q2_cre_infantry;
extern const q2_cre_impl q2_cre_arachner;
extern const q2_cre_impl q2_cre_berserk;
extern const q2_cre_impl q2_cre_insane;
extern const q2_cre_impl q2_cre_flyer;
extern const q2_cre_impl q2_cre_gladiator;
extern const q2_cre_impl q2_cre_hover;
extern const q2_cre_impl q2_cre_parasite;
extern const q2_cre_impl q2_cre_ironmaiden;
extern const q2_cre_impl q2_cre_strider;
extern const q2_cre_impl q2_cre_boss1;
extern const q2_cre_impl q2_cre_boss2;
extern const q2_cre_impl q2_cre_jorg;
extern const q2_cre_impl q2_cre_rider;
extern const q2_cre_impl q2_cre_medic;
extern const q2_cre_impl q2_cre_riderstand;
extern const q2_cre_impl q2_cre_rider2;
extern const q2_cre_impl q2_cre_rider3;
extern const q2_cre_impl q2_cre_deadcomm;

static const q2_cre_impl *const g_impls[] = {
    &q2_cre_soldier,
    &q2_cre_tankcomm,
    &q2_cre_gunner,
    &q2_cre_infantry,
    &q2_cre_arachner,
    &q2_cre_berserk,
    &q2_cre_insane,
    &q2_cre_flyer,
    &q2_cre_gladiator,
    &q2_cre_hover,
    &q2_cre_parasite,
    &q2_cre_ironmaiden,
    &q2_cre_strider,
    &q2_cre_boss1,
    &q2_cre_boss2,
    &q2_cre_jorg,
    &q2_cre_rider,
    &q2_cre_medic,
    &q2_cre_riderstand,
    &q2_cre_rider2,
    &q2_cre_rider3,
    &q2_cre_deadcomm,
    NULL
};

const q2_cre_impl *const *q2_cre_impls(void)
{
    return g_impls;
}

const q2_cre_impl *q2_cre_impl_find(const char *module_name)
{
    u32 i;

    if (!module_name)
        return NULL;

    for (i = 0; g_impls[i]; i++)
        if (g_impls[i]->name && strcmp(g_impls[i]->name, module_name) == 0)
            return g_impls[i];

    /*
     * No hand-written implementation. The generic one gives the creature its
     * decoded animation/action fragments and shared AI. This is a development
     * fallback, not a complete implementation: branch conditions, projectile
     * arguments and special behavior still need native code.
     */
    return &q2_cre_generic;
}
