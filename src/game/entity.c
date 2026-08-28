#include "entity.h"

#include <stdlib.h>
#include <string.h>

void q2_entity_init(q2_entity *e)
{
    if (!e)
        return;

    memset(e, 0, sizeof(*e));

    /* 0x800587C0: the spawner's first act is to write this word, so a port that
     * starts from zero renders nothing. Bit 0 is cleared again by the item
     * spawner when the record does not carry Q2_ITEM_SPIN (0x80059ACC). */
    e->render_flags = 0x08000001u;

    /* 0x80059AAC: a non-materialising item starts at full light intensity. */
    e->scale = Q2_ONE_12;

    /*
     * AND SO DOES THE SECOND INTENSITY. The allocator at
     * 0x8006C1B8..0x8006C1C0 writes 4096 into BOTH +0xFC and +0xFE, and the
     * lighting setup at 0x8006B298 multiplies the pair. Leaving this at the
     * memset's zero makes the entity black rather than collapsing its model.
     */
    e->fade = Q2_ONE_12;

    /*
     * The allocator's ambient pair — 0x8006C1D8..0x8006C1FC copies the
     * four-byte constant at 0x800AEB84 (40 40 40 00) into both +0x2AC and
     * +0x2B0. The draw reads +0x2AC unconditionally as the GTE back colour;
     * leaving it at the memset's zero makes every newly allocated model lose
     * retail's ambient floor. Individual spawners may replace it (items use
     * 0x30, transient explosions explicitly restore 0x40).
     */
    e->glow[0] = e->glow[1] = e->glow[2] = 0x40;

    e->model_index = -1;      /* not resolved against a bank yet */
    e->model_bank  = NULL;
    e->population_group = -1; /* not a Population place record   */
}

bool q2_entity_visible(const q2_entity *e, u32 player)
{
    if (!e || !e->in_use || e->hidden)
        return false;
    if (player < Q2_MAX_PLAYERS && e->taken[player])
        return false;
    return true;
}

bool q2_entity_bounds_overlap(const q2_entity *a, const q2_entity *b)
{
    int i;

    if (!a || !b)
        return false;

    for (i = 0; i < 3; i++) {
        if (a->bounds_max[i] < b->bounds_min[i])
            return false;
        if (a->bounds_min[i] > b->bounds_max[i])
            return false;
    }
    return true;
}

void q2_entity_set_bounds(q2_entity *e, s32 half)
{
    int i;

    if (!e)
        return;

    /*
     * 0x8005A9DC: the absolute box is `position + mins/maxs`, and the position
     * is `+0x54` — NOT the draw origin at `+0xA4`, which carries the model's own
     * vertical bias and would move the box with the artwork.
     */
    for (i = 0; i < 3; i++) {
        e->bounds_min[i] = e->pos[i] - half;
        e->bounds_max[i] = e->pos[i] + half;
    }
}

/* ------------------------------------------------------------------------- */
void q2_ent_events_clear(q2_ent_events *ev)
{
    if (!ev)
        return;
    ev->count   = 0;
    ev->dropped = 0;
}

static q2_ent_event *event_push(q2_ent_events *ev)
{
    q2_ent_event *e;

    if (!ev)
        return NULL;
    if (ev->count >= Q2_ENT_EVENTS_MAX) {
        ev->dropped++;
        return NULL;
    }

    e = &ev->e[ev->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void event_fill(q2_ent_event *e, const s32 pos[3], const u8 glow[3])
{
    if (pos) {
        e->pos[0] = pos[0];
        e->pos[1] = pos[1];
        e->pos[2] = pos[2];
    }
    if (glow) {
        e->glow[0] = glow[0];
        e->glow[1] = glow[1];
        e->glow[2] = glow[2];
    }
}

void q2_ent_sound_at(q2_ent_events *ev, q2_ent_sound which, const s32 pos[3])
{
    q2_ent_event *e = event_push(ev);

    if (!e)
        return;

    e->kind  = Q2_ENT_EVENT_SOUND;
    e->sound = (u8)which;
    event_fill(e, pos, NULL);
}

void q2_ent_light_at(q2_ent_events *ev, const s32 pos[3], const u8 glow[3],
                     s32 inner_radius, s32 radius)
{
    q2_ent_event *e = event_push(ev);

    if (!e)
        return;

    e->kind         = Q2_ENT_EVENT_LIGHT;
    e->radius       = radius;
    e->inner_radius = inner_radius;
    event_fill(e, pos, glow);
}

void q2_ent_burst_at(q2_ent_events *ev, const s32 pos[3], const u8 glow[3],
                     s32 model_index)
{
    q2_ent_event *e = event_push(ev);

    if (!e)
        return;

    e->kind        = Q2_ENT_EVENT_BURST;
    e->model_index = model_index;
    event_fill(e, pos, glow);
}

/* ------------------------------------------------------------------------- */
/* The shared placement drop — see entity.h                                   */
/* ------------------------------------------------------------------------- */
bool q2_entity_drop_to_floor(q2_collision *coll, s32 pos[3], s32 dist,
                             s32 *out_node)
{
    s32 dest[3], end[3];
    s32 node = -1;

    if (out_node)
        *out_node = -1;
    if (!pos)
        return false;

    /* No hull to drop against, or a caller that asked for no drop: the
     * position stands. That is the item flag's arm, not a failure. */
    if (!coll || dist <= 0)
        return true;

    dest[0] = pos[0];
    dest[1] = pos[1] + dist;      /* +Y is down */
    dest[2] = pos[2];

    /*
     * `q2_coll_move` returns false when the move was STOPPED, which is the
     * answer this probe wants — a drop exists to be stopped by a floor. What
     * it cannot place at all is reported by `node < 0`.
     */
    if (!q2_coll_move(coll, pos, dest, -1, end, &node) && node < 0)
        return false;

    /*
     * A COMPLETED move means the sweep never met anything in `dist` units.
     * collision.h warns that a completed move returns `end` verbatim, which
     * here is 1024 units below the authored floor — so taking it would bury
     * the entity rather than place it. The original's caller treats an
     * unplaced entity as a failure; a completed drop is the same situation
     * seen from the other side, and the position is left where it was.
     */
    if (end[1] < dest[1])
        pos[1] = end[1];

    if (out_node)
        *out_node = node;
    return true;
}

/* ------------------------------------------------------------------------- */
void q2_entity_world_init(q2_entity_world *w)
{
    if (!w)
        return;

    memset(w, 0, sizeof(*w));
    q2_rng_seed(&w->rng, 1);
}

void q2_entity_world_add_player(q2_entity_world *w, u32 slot,
                                q2_inventory *inv, const s32 pos[3])
{
    q2_entity_player *p;

    if (!w || slot >= Q2_MAX_PLAYERS)
        return;

    p = &w->player[slot];
    memset(p, 0, sizeof(*p));
    q2_entity_init(&p->ent);

    p->present = true;
    p->inv     = inv;
    p->ent.in_use = true;

    if (slot + 1 > w->player_count)
        w->player_count = slot + 1;

    q2_entity_world_move_player(w, slot, pos);
}

void q2_entity_world_move_player(q2_entity_world *w, u32 slot,
                                 const s32 pos[3])
{
    q2_entity_player *p;
    int i;

    if (!w || slot >= Q2_MAX_PLAYERS)
        return;

    p = &w->player[slot];
    if (!p->present)
        return;

    if (pos)
        for (i = 0; i < 3; i++)
            p->pos[i] = pos[i];

    /*
     * The engine overlaps the item's +0x78 box against the PLAYER's +0x78 box,
     * and both are built the same way: a 286-unit cube about the entity's own
     * +0x54. For the player that field is the ORIGIN, Q2_EYE_BASE above the
     * feet — and +Y points down, so the origin has the SMALLER y.
     */
    p->ent.pos[0] = p->pos[0];
    p->ent.pos[1] = p->pos[1] - Q2_EYE_BASE;
    p->ent.pos[2] = p->pos[2];

    p->ent.origin[0] = p->ent.pos[0];
    p->ent.origin[1] = p->ent.pos[1];
    p->ent.origin[2] = p->ent.pos[2];

    q2_entity_set_bounds(&p->ent, Q2_SWEEP_HALF_EXTENT);
}

/* ------------------------------------------------------------------------- */
void q2_entity_set_free(q2_entity_set *set)
{
    if (!set)
        return;
    free(set->ent);
    memset(set, 0, sizeof(*set));
}

q2_entity *q2_entity_alloc(q2_entity_set *set)
{
    u32 i;

    if (!set)
        return NULL;

    /* Reuse a dead slot first. The engine's allocator (0x8006C098) hands back
     * freed records too, and reusing them keeps indices stable for anything
     * holding one — the draw list especially. */
    for (i = 0; i < set->count; i++) {
        if (!set->ent[i].in_use) {
            q2_entity_init(&set->ent[i]);
            set->ent[i].in_use = true;
            return &set->ent[i];
        }
    }

    if (set->count >= set->capacity) {
        u32 want = set->capacity ? set->capacity * 2 : 64;
        q2_entity *bigger =
            (q2_entity *)realloc(set->ent, (size_t)want * sizeof(q2_entity));
        if (!bigger)
            return NULL;
        set->ent      = bigger;
        set->capacity = want;
    }

    {
        q2_entity *e = &set->ent[set->count++];
        q2_entity_init(e);
        e->in_use = true;
        return e;
    }
}

void q2_entity_remove(q2_entity *e)
{
    /* 0x8006D280: the engine clears the record and returns it to the free list.
     * `in_use` false is that, and the slot is reused by the next alloc. */
    if (!e)
        return;
    q2_entity_init(e);
    e->in_use = false;
}

u32 q2_entity_run(q2_entity_set *set, q2_entity_world *w)
{
    u32 i, ran = 0, n;

    if (!set || !w)
        return 0;

    /* The clock advances once, before the sweep. */
    w->level_time += w->dt;

    /* Snapshot the count: a think may allocate (a dropped item, a projectile)
     * and the engine's sweep does not run what this tick created. */
    n = set->count;

    for (i = 0; i < n; i++) {
        q2_entity *e = &set->ent[i];

        if (!e->in_use || !e->think)
            continue;

        e->think(e, w);
        ran++;
    }

    return ran;
}
