#include "save.h"

#include "mission.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <direct.h>
#  define SAVE_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define SAVE_MKDIR(p) mkdir((p), 0777)
#endif

/* ------------------------------------------------------------------------- */
/* Chunk tags                                                                 */
/*                                                                            */
/* Four characters, low byte first, so a hex dump of the file reads them left  */
/* to right. A reader skips a tag it does not know, which is what lets a later */
/* version add state without invalidating an existing save.                   */
/* ------------------------------------------------------------------------- */
#define TAG(a, b, c, d) \
    ((u32)(u8)(a) | ((u32)(u8)(b) << 8) | ((u32)(u8)(c) << 16) | ((u32)(u8)(d) << 24))

#define TAG_HEAD TAG('H', 'E', 'A', 'D')   /* identity                        */
#define TAG_PLYR TAG('P', 'L', 'Y', 'R')   /* the player, mover state and all */
#define TAG_SIMS TAG('S', 'I', 'M', 'S')   /* the clock and the world's rules */
#define TAG_INVN TAG('I', 'N', 'V', 'N')   /* inventory                       */
#define TAG_CMBT TAG('C', 'M', 'B', 'T')   /* weapon gate and the generators  */
#define TAG_PROJ TAG('P', 'R', 'O', 'J')   /* projectiles in flight           */
#define TAG_EVNT TAG('E', 'V', 'N', 'T')   /* script flags                    */
#define TAG_TRIG TAG('T', 'R', 'I', 'G')   /* trigger residency               */
#define TAG_ENTS TAG('E', 'N', 'T', 'S')   /* per-entity mutable state        */
#define TAG_ITEM TAG('I', 'T', 'E', 'M')   /* group order and stable item keys */
#define TAG_MISN TAG('M', 'I', 'S', 'N')   /* the mission tallies             */
#define TAG_BRKS TAG('B', 'R', 'K', 'S')   /* which panes have been shot      */
#define TAG_MOVR TAG('M', 'O', 'V', 'R')   /* which doors are open, and where */
#define TAG_CRES TAG('C', 'R', 'E', 'S')   /* who is dead and where the rest are */
#define TAG_SETT TAG('S', 'E', 'T', 'T')   /* the menu settings               */

#define SAVE_FILE_HEADER 16                /* magic, version, size, crc       */
#define SETTINGS_MAGIC "Q2CF"
#define SETTINGS_VERSION 1u

/* A corrupt count must not become a gigabyte allocation. These are far above
 * anything a real map produces and far below anything that hurts. */
#define SAVE_MAX_EVENTS   (1u << 20)
#define SAVE_MAX_TRIGGERS (1u << 16)
#define SAVE_MAX_ENTITIES (1u << 16)
#define SAVE_MAX_ITEM_GROUPS (1u << 16)

/* ------------------------------------------------------------------------- */
/* CRC32                                                                      */
/*                                                                            */
/* The chunk sizes already catch truncation. This catches the other half — a   */
/* file that is the right length and wrong in the middle — which for a save is */
/* the failure worth having, because a silently corrupt restore looks like a   */
/* game bug rather than a bad file.                                            */
/* ------------------------------------------------------------------------- */
static u32 crc32_of(const u8 *p, size_t n)
{
    u32 crc = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(s32)(crc & 1u)));
    }
    return ~crc;
}

/* ------------------------------------------------------------------------- */
/* A growable little-endian writer                                            */
/* ------------------------------------------------------------------------- */
typedef struct wbuf {
    u8    *p;
    size_t len, cap;
    bool   bad;      /* an allocation failed; every later write is a no-op */
} wbuf;

static void w_free(wbuf *w)
{
    free(w->p);
    w->p = NULL;
    w->len = w->cap = 0;
}

static bool w_need(wbuf *w, size_t n)
{
    size_t want;
    u8 *bigger;

    if (w->bad)
        return false;
    if (w->len + n <= w->cap)
        return true;

    want = w->cap ? w->cap : 1024;
    while (want < w->len + n)
        want *= 2;

    bigger = (u8 *)realloc(w->p, want);
    if (!bigger) {
        w->bad = true;
        return false;
    }
    w->p   = bigger;
    w->cap = want;
    return true;
}

static void w_bytes(wbuf *w, const void *src, size_t n)
{
    if (!w_need(w, n))
        return;
    memcpy(w->p + w->len, src, n);
    w->len += n;
}

static void w_u8(wbuf *w, u8 v)
{
    if (!w_need(w, 1))
        return;
    w->p[w->len++] = v;
}

static void w_u16(wbuf *w, u16 v)
{
    w_u8(w, (u8)v);
    w_u8(w, (u8)(v >> 8));
}

static void w_u32(wbuf *w, u32 v)
{
    w_u8(w, (u8)v);
    w_u8(w, (u8)(v >> 8));
    w_u8(w, (u8)(v >> 16));
    w_u8(w, (u8)(v >> 24));
}

static void w_s16(wbuf *w, s16 v) { w_u16(w, (u16)v); }
static void w_s32(wbuf *w, s32 v) { w_u32(w, (u32)v); }
static void w_bool(wbuf *w, bool v) { w_u8(w, v ? 1u : 0u); }

/* A fixed-width, NUL-padded string. Fixed width so the field can be read
 * without a length prefix and so a shorter name cannot shift what follows. */
static void w_str(wbuf *w, const char *s, size_t width)
{
    size_t i, n = s ? strlen(s) : 0;

    if (n > width - 1)
        n = width - 1;
    for (i = 0; i < width; i++)
        w_u8(w, i < n ? (u8)s[i] : 0u);
}

static void w_s16v(wbuf *w, const s16 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        w_s16(w, v[i]);
}

static void w_s32v(wbuf *w, const s32 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        w_s32(w, v[i]);
}

/* Open a chunk, returning where its size field sits so w_chunk_end can patch
 * it. Writing the size afterwards rather than counting it first is what keeps
 * every chunk writer a straight run of fields. */
static size_t w_chunk_begin(wbuf *w, u32 tag)
{
    size_t at;
    w_u32(w, tag);
    at = w->len;
    w_u32(w, 0);
    return at;
}

static void w_chunk_end(wbuf *w, size_t size_at)
{
    u32 size;

    if (w->bad || w->len < size_at + 4)
        return;
    size = (u32)(w->len - (size_at + 4));
    w->p[size_at + 0] = (u8)size;
    w->p[size_at + 1] = (u8)(size >> 8);
    w->p[size_at + 2] = (u8)(size >> 16);
    w->p[size_at + 3] = (u8)(size >> 24);
}

/* ------------------------------------------------------------------------- */
/* The matching reader                                                        */
/* ------------------------------------------------------------------------- */
typedef struct rbuf {
    const u8 *p;
    size_t    len, at;
    bool      bad;   /* a read ran off the end; every later read yields zero */
} rbuf;

static bool r_need(rbuf *r, size_t n)
{
    if (r->bad)
        return false;
    if (r->at + n > r->len) {
        r->bad = true;
        return false;
    }
    return true;
}

static u8 r_u8(rbuf *r)
{
    if (!r_need(r, 1))
        return 0;
    return r->p[r->at++];
}

static u16 r_u16(rbuf *r)
{
    u16 lo = r_u8(r);
    return (u16)(lo | ((u16)r_u8(r) << 8));
}

static u32 r_u32(rbuf *r)
{
    u32 v = r_u16(r);
    return v | ((u32)r_u16(r) << 16);
}

static s16 r_s16(rbuf *r) { return (s16)r_u16(r); }
static s32 r_s32(rbuf *r) { return (s32)r_u32(r); }
static bool r_bool(rbuf *r) { return r_u8(r) != 0; }

static void r_str(rbuf *r, char *out, size_t width)
{
    size_t i;

    for (i = 0; i < width; i++) {
        u8 c = r_u8(r);
        if (out && i < width)
            out[i] = (char)c;
    }
    if (out && width)
        out[width - 1] = '\0';
}

static void r_s16v(rbuf *r, s16 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        v[i] = r_s16(r);
}

static void r_s32v(rbuf *r, s32 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        v[i] = r_s32(r);
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                   */
/* ------------------------------------------------------------------------- */
void q2_save_free(q2_save *s)
{
    if (!s)
        return;
    free(s->event_flags);
    free(s->trigger_inside);
    free(s->entities);
    free(s->item_group_order);
    free(s->item_keys);
    free(s->breakables);
    free(s->movers);
    free(s->creatures);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------------- */
/* Capture                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The think pointer, as something a file can hold. Only the two the item path
 * installs are named; anything else is recorded as OTHER, which the restore
 * leaves alone rather than replacing with a guess.
 */
static q2_save_think think_id(q2_think_fn fn)
{
    if (!fn)
        return Q2_SAVE_THINK_NONE;
    if (fn == q2_item_think)
        return Q2_SAVE_THINK_ITEM;
    if (fn == q2_item_shrink_think)
        return Q2_SAVE_THINK_SHRINK;
    return Q2_SAVE_THINK_OTHER;
}

static u8 taken_mask(const q2_entity *e)
{
    u8 mask = 0;
    int i;

    for (i = 0; i < Q2_MAX_PLAYERS && i < 8; i++)
        if (e->taken[i])
            mask |= (u8)(1u << i);
    return mask;
}

q2_result q2_save_capture(q2_save *out, const q2_sim *sim,
                          const q2_inventory *inv,
                          const char *serial, const char *map, s32 zone)
{
    if (!out || !sim || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (serial)
        snprintf(out->serial, sizeof(out->serial), "%s", serial);
    snprintf(out->map, sizeof(out->map), "%s", map);
    out->zone      = zone;
    out->timestamp = (u32)time(NULL);

    /* --- the player, whole ---------------------------------------------- */
    out->player = sim->player[0];

    /* --- the clock and the rules ----------------------------------------- */
    out->level_time          = sim->level_time;
    out->tick_count          = sim->tick_count;
    out->dt_accum            = sim->dt_accum;
    out->dt_per_field        = sim->dt_per_field;
    out->gravity             = sim->gravity;
    out->env_flags           = sim->env_flags;
    out->cheats              = sim->cheats;
    out->current_node        = sim->current_node;
    out->multiplayer         = sim->multiplayer ? 1u : 0u;
    out->full_basis_movement = sim->full_basis_movement ? 1u : 0u;
    out->no_fall_damage      = sim->no_fall_damage ? 1u : 0u;

    /* --- combat ----------------------------------------------------------- */
    out->inventory        = inv ? *inv : sim->combat.inv;
    out->weapon_id        = sim->combat.weapon_id;
    out->next_fire        = sim->combat.next_fire;
    out->kick[0]          = sim->combat.kick[0];
    out->kick[1]          = sim->combat.kick[1];
    out->kick[2]          = sim->combat.kick[2];
    out->chaingun_bullets = sim->combat.chaingun_bullets;
    out->rng_state        = sim->combat.rng.state;
    out->fx_rng_state     = sim->fx_rng.state;

    memcpy(out->proj, sim->combat.projectiles.p, sizeof(out->proj));

    /*
     * --- script state -----------------------------------------------------
     *
     * A save that restores position and inventory but not which triggers have
     * fired drops the player into a level where every door they opened is shut
     * again — so this is copied, not skipped.
     */
    if (sim->events_ready && sim->event_rt.record_count > 0 &&
        sim->event_rt.flags) {
        u32 n = sim->event_rt.record_count;

        out->event_flags = (u8 *)malloc(n);
        if (!out->event_flags) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }

        memcpy(out->event_flags, sim->event_rt.flags, n);
        out->event_count = n;
    }

    /*
     * --- trigger residency -------------------------------------------------
     *
     * A volume fires on ENTRY. Restoring into one with its bit clear re-fires
     * it, so a save made standing on a teleport pad would teleport on load.
     */
    if (sim->trigger_inside && sim->trigger_capacity > 0) {
        u32 n = sim->trigger_capacity;

        out->trigger_inside = (u8 *)malloc(n);
        if (!out->trigger_inside) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }

        memcpy(out->trigger_inside, sim->trigger_inside, n);
        out->trigger_count = n;
    }

    /* --- the breakables ---------------------------------------------------- */
    if (sim->breakable_count > 0) {
        u32 n = sim->breakable_count, i;

        out->breakables = (q2_save_breakable *)calloc(n, sizeof(*out->breakables));
        if (!out->breakables) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }
        out->breakable_count = n;

        for (i = 0; i < n; i++) {
            out->breakables[i].scene_node = sim->breakable[i].scene_node;
            out->breakables[i].health     = sim->breakable[i].health;
            out->breakables[i].broken     = sim->breakable[i].broken ? 1u : 0u;
        }
    }

    /* --- the entity set ---------------------------------------------------- */
    if (sim->entities.count > 0 && sim->entities.ent) {
        u32 n = sim->entities.count;
        u32 i;

        out->entities = (q2_save_entity *)calloc(n, sizeof(q2_save_entity));
        if (!out->entities) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }
        out->entity_count = n;

        for (i = 0; i < n; i++) {
            const q2_entity *e = &sim->entities.ent[i];
            q2_save_entity  *d = &out->entities[i];

            d->place_id   = e->place_id;
            d->in_use     = e->in_use ? 1u : 0u;
            d->hidden     = e->hidden ? 1u : 0u;
            d->taken      = taken_mask(e);
            d->think      = (u8)think_id(e->think);
            d->scale      = e->scale;
            d->health     = e->health;
            d->frame      = e->frame;
            d->spin       = e->angles[1];
            d->remove_in  = e->remove_in;
            d->respawn_at = e->respawn_at;
            d->pos[0]     = e->pos[0];
            d->pos[1]     = e->pos[1];
            d->pos[2]     = e->pos[2];
        }
    }

    /*
     * --- Population group lifetime and stable entity identity ------------
     *
     * The order is not reducible to a bitmap: BASE1 can call ShotgunRoom and
     * LiftRoom in either order, and each appends/reuses slots in that order.
     * The keys are needed for the reuse half — a pickup collected before the
     * call leaves a hole a deferred place can occupy.
     */
    if (sim->item_population_ready && sim->item_population.group_count > 0 &&
        sim->item_group_run && sim->item_group_order) {
        u32 n = sim->item_population.group_count;
        u32 i;

        out->item_state_present = true;
        out->item_population_group_count = n;
        out->item_group_order_count = sim->item_group_order_count;

        if (out->item_group_order_count > n) {
            q2_save_free(out);
            return Q2_ERR_BAD_FORMAT;
        }
        if (out->item_group_order_count) {
            out->item_group_order = (u32 *)malloc(
                (size_t)out->item_group_order_count * sizeof(u32));
            if (!out->item_group_order) {
                q2_save_free(out);
                return Q2_ERR_NO_MEMORY;
            }
            memcpy(out->item_group_order, sim->item_group_order,
                   (size_t)out->item_group_order_count * sizeof(u32));
        }

        out->item_key_count = out->entity_count;
        if (out->item_key_count) {
            out->item_keys = (q2_save_item_key *)calloc(
                out->item_key_count, sizeof(*out->item_keys));
            if (!out->item_keys) {
                q2_save_free(out);
                return Q2_ERR_NO_MEMORY;
            }

            for (i = 0; i < out->item_key_count; i++) {
                out->item_keys[i].group = sim->entities.ent[i].population_group;
                out->item_keys[i].slot  = sim->entities.ent[i].population_slot;
            }
        }
    }

    q2_save_default_label(out, out->label, (u32)sizeof(out->label));
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Apply                                                                      */
/* ------------------------------------------------------------------------- */
static q2_result rebuild_saved_item_roster(const q2_save *s, q2_sim *sim)
{
    q2_sim stage;
    q2_entity_set rebuilt;
    u8 *seen = NULL;
    u8 *used = NULL;
    u32 capacity, i;
    q2_result result = Q2_ERR_BAD_FORMAT;

    if (!s->item_state_present) {
        /* A Population-backed version-5 save without ITEM has lost the exact
         * startup/CREBATCH history needed to identify reused entity slots.
         * Refuse the whole restore; applying ENTS by index would silently put
         * one item's state on another. Utility sims without Population remain
         * valid because they have no such history to preserve. */
        if (sim->item_population_ready &&
            sim->item_population.group_count > 0) {
            Q2_ERROR("version-%d save for a Population map has no ITEM chunk",
                     Q2_SAVE_VERSION);
            return Q2_ERR_BAD_FORMAT;
        }
        return Q2_OK;
    }

    if (!sim->item_population_ready || !sim->item_group_run ||
        !sim->item_group_order ||
        sim->entities.count > SAVE_MAX_ENTITIES ||
        (sim->entities.count && !sim->entities.ent) ||
        s->item_population_group_count != sim->item_population.group_count ||
        s->item_group_order_count > s->item_population_group_count ||
        s->item_key_count != s->entity_count ||
        (s->entity_count && (!s->entities || !s->item_keys)))
        return Q2_ERR_BAD_FORMAT;

    seen = (u8 *)calloc(s->item_population_group_count ?
                        s->item_population_group_count : 1, 1);
    if (!seen)
        return Q2_ERR_NO_MEMORY;

    /* The fresh load's startup selection must be an exact prefix. Everything
     * after it is a CREBATCH first-run, in the order the allocator saw it. */
    if (sim->item_group_order_count > s->item_group_order_count)
        goto done;
    for (i = 0; i < s->item_group_order_count; i++) {
        u32 gi = s->item_group_order[i];

        if (gi >= s->item_population_group_count || seen[gi])
            goto done;
        seen[gi] = 1;
        if (i < sim->item_group_order_count &&
            sim->item_group_order[i] != gi)
            goto done;
    }

    /* Pre-size the staged set for the full logical roster. Runtime activation
     * may reuse holes; the fresh stage has none, so this is deliberately an
     * upper bound and prevents a partial replay on realloc failure. */
    capacity = sim->entities.count;
    for (i = sim->item_group_order_count;
         i < s->item_group_order_count; i++) {
        q2_pop_group g;
        u32 slot;

        if (!q2_pop_get_group(&sim->item_population,
                              s->item_group_order[i], &g))
            goto done;
        for (slot = 0; ; slot++) {
            q2_pop_place place;

            if (!q2_pop_get_place(&sim->item_population, &g, slot, &place))
                break;
            if (!q2_item_find(sim->item_table, (s32)place.id))
                continue;
            if (capacity >= SAVE_MAX_ENTITIES)
                goto done;
            capacity++;
        }
    }

    stage = *sim;
    memset(&stage.entities, 0, sizeof(stage.entities));
    stage.item_group_run = NULL;
    stage.item_group_order = NULL;

    if (capacity) {
        stage.entities.ent = (q2_entity *)calloc(capacity,
                                                  sizeof(q2_entity));
        if (!stage.entities.ent) {
            result = Q2_ERR_NO_MEMORY;
            goto done;
        }
        if (sim->entities.count)
            memcpy(stage.entities.ent, sim->entities.ent,
                   (size_t)sim->entities.count * sizeof(q2_entity));
        stage.entities.count    = sim->entities.count;
        stage.entities.capacity = capacity;
    }

    stage.item_group_run = (u8 *)malloc(
        s->item_population_group_count ? s->item_population_group_count : 1);
    stage.item_group_order = (u32 *)calloc(
        s->item_population_group_count ? s->item_population_group_count : 1,
        sizeof(u32));
    if (!stage.item_group_run || !stage.item_group_order) {
        result = Q2_ERR_NO_MEMORY;
        goto stage_done;
    }
    memcpy(stage.item_group_run, sim->item_group_run,
           s->item_population_group_count);
    if (sim->item_group_order_count)
        memcpy(stage.item_group_order, sim->item_group_order,
               (size_t)sim->item_group_order_count * sizeof(u32));

    for (i = sim->item_group_order_count;
         i < s->item_group_order_count; i++) {
        q2_pop_group g;
        u32 before = stage.item_group_order_count;
        u32 gi = s->item_group_order[i];

        if (!q2_pop_get_group(&stage.item_population, gi, &g))
            goto stage_done;
        (void)q2_sim_activate_item_group(&stage, g.name);
        if (!stage.item_group_run[gi] ||
            stage.item_group_order_count != before + 1 ||
            stage.item_group_order[before] != gi)
            goto stage_done;
    }

    memset(&rebuilt, 0, sizeof(rebuilt));
    if (s->entity_count) {
        rebuilt.ent = (q2_entity *)calloc(s->entity_count,
                                          sizeof(q2_entity));
        used = (u8 *)calloc(stage.entities.count ? stage.entities.count : 1,
                            1);
        if (!rebuilt.ent || !used) {
            result = Q2_ERR_NO_MEMORY;
            goto rebuilt_done;
        }
        rebuilt.count = rebuilt.capacity = s->entity_count;
    }

    for (i = 0; i < s->entity_count; i++) {
        const q2_save_item_key *key = &s->item_keys[i];
        u32 j;

        q2_entity_init(&rebuilt.ent[i]);
        if (key->group >= 0) {
            for (j = 0; j < stage.entities.count; j++) {
                const q2_entity *e = &stage.entities.ent[j];

                if (used[j] || !e->in_use)
                    continue;
                if (e->population_group != key->group ||
                    e->population_slot != key->slot)
                    continue;
                rebuilt.ent[i] = *e;
                used[j] = 1;
                break;
            }
            if (j == stage.entities.count)
                goto rebuilt_done;
        } else if (s->entities[i].in_use) {
            /* Non-Population entities retain the legacy by-index rule. They
             * are not part of CREBATCH reconstruction, but rejecting a changed
             * slot is safer than attaching their state to an item. */
            if (i >= stage.entities.count || used[i] ||
                !stage.entities.ent[i].in_use ||
                stage.entities.ent[i].population_group >= 0)
                goto rebuilt_done;
            rebuilt.ent[i] = stage.entities.ent[i];
            used[i] = 1;
        }

        if (s->entities[i].in_use &&
            rebuilt.ent[i].place_id != s->entities[i].place_id)
            goto rebuilt_done;
    }

    /* All validation happened against private storage. Only now replace the
     * live roster and its two group-lifetime shadows. */
    q2_entity_set_free(&sim->entities);
    sim->entities = rebuilt;
    memset(&rebuilt, 0, sizeof(rebuilt));
    memcpy(sim->item_group_run, stage.item_group_run,
           s->item_population_group_count);
    if (stage.item_group_order_count)
        memcpy(sim->item_group_order, stage.item_group_order,
               (size_t)stage.item_group_order_count * sizeof(u32));
    sim->item_group_order_count = stage.item_group_order_count;
    result = Q2_OK;

rebuilt_done:
    q2_entity_set_free(&rebuilt);
    free(used);
stage_done:
    q2_entity_set_free(&stage.entities);
    free(stage.item_group_run);
    free(stage.item_group_order);
done:
    free(seen);
    return result;
}

q2_result q2_save_apply(const q2_save *s, q2_sim *sim, q2_inventory *inv,
                        const char *serial, const char *map)
{
    q2_result rebuild;

    if (!s || !sim)
        return Q2_ERR_INVALID_ARG;

    /* A save from a different release would restore coordinates into a
     * different map's geometry, because the level table and script offsets are
     * per-build. Refusing is the only safe answer. */
    if (serial && s->serial[0] && strcmp(serial, s->serial) != 0) {
        Q2_ERROR("save is for build %s, this disc is %s", s->serial, serial);
        return Q2_ERR_UNSUPPORTED;
    }

    /* The caller loads the map; this only restores into it. Applying a save to
     * the wrong map would put the player inside geometry. */
    if (map && strcmp(map, s->map) != 0) {
        Q2_ERROR("save is for map %s, but %s is loaded", s->map, map);
        return Q2_ERR_INVALID_ARG;
    }

    rebuild = rebuild_saved_item_roster(s, sim);
    if (rebuild != Q2_OK)
        return rebuild;

    /*
     * The entity set is rebuilt by the caller's attach sequence, so its size is
     * a check on whether the same map data came back. A mismatch means the save
     * and the map disagree and applying per-index state would put one item's
     * "collected" flag on another.
     */
    if (s->entity_count && sim->entities.count &&
        s->entity_count != sim->entities.count) {
        Q2_ERROR("save has %u entities, this map spawned %u",
                 s->entity_count, sim->entities.count);
        return Q2_ERR_BAD_FORMAT;
    }

    /*
     * And the per-slot check, run in full BEFORE anything is written.
     *
     * A live slot whose place id does not match is a map whose population has
     * moved under the save. Catching that halfway through the loop would leave
     * the simulation half restored, which is worse than either outcome — so
     * every entity is validated first and the apply is all or nothing.
     */
    if (s->entities && sim->entities.ent) {
        u32 n = s->entity_count;
        u32 i;

        if (n > sim->entities.count)
            n = sim->entities.count;

        for (i = 0; i < n; i++) {
            if (!s->entities[i].in_use)
                continue;   /* the record was cleared when it was collected */
            if (sim->entities.ent[i].place_id != s->entities[i].place_id) {
                Q2_ERROR("save entity %u is place %u, the map spawned %u",
                         i, s->entities[i].place_id,
                         sim->entities.ent[i].place_id);
                return Q2_ERR_BAD_FORMAT;
            }
        }
    }

    /* --- the player ------------------------------------------------------- */
    sim->player[0] = s->player;

    /* --- the clock and the rules ------------------------------------------ */
    sim->level_time          = s->level_time;
    sim->tick_count          = s->tick_count;
    sim->dt_accum            = s->dt_accum;
    sim->gravity             = s->gravity;
    sim->env_flags           = s->env_flags;
    sim->cheats              = s->cheats;
    sim->current_node        = s->current_node;
    sim->multiplayer         = s->multiplayer != 0;
    sim->full_basis_movement = s->full_basis_movement != 0;
    sim->no_fall_damage      = s->no_fall_damage != 0;

    /*
     * dt_per_field is the build's field rate, adjusted by the GAME SPEED
     * slider. A save carries it so a game saved under a modified speed resumes
     * at that speed; zero would stall the clock, so a bad value is ignored
     * rather than honoured.
     */
    if (s->dt_per_field > 0)
        sim->dt_per_field = s->dt_per_field;

    /* --- combat ------------------------------------------------------------ */
    sim->combat.inv              = s->inventory;
    sim->combat.weapon_id        = s->weapon_id;
    sim->combat.next_fire        = s->next_fire;
    sim->combat.kick[0]          = s->kick[0];
    sim->combat.kick[1]          = s->kick[1];
    sim->combat.kick[2]          = s->kick[2];
    sim->combat.chaingun_bullets = s->chaingun_bullets;
    sim->combat.rng.state        = s->rng_state;
    sim->fx_rng.state            = s->fx_rng_state;

    /* The last shot is presentation — a tracer and a sound the client has
     * already drawn — so it is cleared rather than restored. */
    memset(&sim->combat.last_shot, 0, sizeof(sim->combat.last_shot));

    {
        u32 i, live = 0;

        memcpy(sim->combat.projectiles.p, s->proj, sizeof(s->proj));
        for (i = 0; i < Q2_PROJ_MAX; i++)
            if (sim->combat.projectiles.p[i].in_use)
                live++;
        sim->combat.projectiles.live = live;
    }

    if (inv)
        *inv = s->inventory;

    /* --- script state ------------------------------------------------------ */
    if (s->event_flags && sim->events_ready && sim->event_rt.flags) {
        u32 n = s->event_count;
        if (n > sim->event_rt.record_count)
            n = sim->event_rt.record_count;
        memcpy(sim->event_rt.flags, s->event_flags, n);
    }

    /* --- trigger residency -------------------------------------------------- */
    if (sim->trigger_inside && sim->trigger_capacity) {
        memset(sim->trigger_inside, 0, sim->trigger_capacity);
        if (s->trigger_inside) {
            u32 n = s->trigger_count;
            if (n > sim->trigger_capacity)
                n = sim->trigger_capacity;
            memcpy(sim->trigger_inside, s->trigger_inside, n);
        }
    }

    /*
     * --- the breakables --------------------------------------------------
     *
     * Matched by SCENE NODE, not by index: the registry is rebuilt from the map
     * on load and an ordinal is only stable while build order never changes.
     * A pane the file does not mention is left as the map built it, which is
     * what a save written before this chunk existed produces.
     */
    if (s->breakables) {
        u32 i, j;

        for (i = 0; i < s->breakable_count; i++) {
            for (j = 0; j < sim->breakable_count; j++) {
                if (sim->breakable[j].scene_node != s->breakables[i].scene_node)
                    continue;
                sim->breakable[j].health = s->breakables[i].health;
                sim->breakable[j].broken = s->breakables[i].broken != 0;
                break;
            }
        }
    }

    q2_sim_breakables_sync_solidity(sim);

    /* --- the entity set ------------------------------------------------------ */
    if (s->entities && sim->entities.ent) {
        u32 n = s->entity_count;
        u32 i;

        if (n > sim->entities.count)
            n = sim->entities.count;

        for (i = 0; i < n; i++) {
            const q2_save_entity *d = &s->entities[i];
            q2_entity            *e = &sim->entities.ent[i];
            int p;

            if (!d->in_use) {
                /* Collected in single player: the record was cleared and
                 * returned to the free list (0x8006D280). */
                q2_entity_remove(e);
                continue;
            }

            /* The place ids were checked in full above, so by here they line
             * up and nothing in this loop can fail. */
            e->in_use    = true;
            e->hidden    = d->hidden != 0;

            switch ((q2_save_think)d->think) {
            case Q2_SAVE_THINK_NONE:   e->think = NULL;                  break;
            case Q2_SAVE_THINK_ITEM:   e->think = q2_item_think;         break;
            case Q2_SAVE_THINK_SHRINK: e->think = q2_item_shrink_think;  break;
            default: /* OTHER: leave whatever the spawn installed. */    break;
            }

            e->scale     = d->scale;
            e->health    = d->health;
            e->frame     = d->frame;
            e->angles[1] = d->spin;
            e->remove_in = d->remove_in;
            e->respawn_at = d->respawn_at;
            e->pos[0]    = d->pos[0];
            e->pos[1]    = d->pos[1];
            e->pos[2]    = d->pos[2];

            for (p = 0; p < Q2_MAX_PLAYERS && p < 8; p++)
                e->taken[p] = (d->taken & (1u << p)) != 0;

            /* The draw origin follows the position, and the touch box follows
             * the draw origin — both are derived, so they are recomputed rather
             * than stored. */
            e->origin[0] = e->pos[0];
            e->origin[1] = e->pos[1] + Q2_EYE_BASE - e->model_offset;
            e->origin[2] = e->pos[2];
            q2_entity_set_bounds(e, Q2_ITEM_TOUCH_HALF);
        }
    }

    /* One clock: the entity world reads the same level time the sim runs, so
     * it is put back in step rather than left where the rebuild left it. */
    if (sim->entities_ready) {
        sim->ent_world.level_time = sim->level_time;
        sim->ent_world.cheats     = sim->cheats;
        sim->ent_world.deathmatch = sim->multiplayer;
        q2_entity_world_move_player(&sim->ent_world, 0, sim->player[0].pos);
    }

    /*
     * Creatures are NOT restored, because the sim does not own them: a caller
     * registers actors (sim.h). A client that spawns monsters must reload the
     * map's population itself, which is what a fresh zone load already does —
     * so a restored level has its monsters alive again even if the player had
     * killed them. Called out rather than hidden; storing them needs the
     * creature set to live somewhere the save can reach.
     */

    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Mission tallies and settings                                               */
/* ------------------------------------------------------------------------- */
void q2_save_capture_mission(q2_save *s, const struct q2_mission *m)
{
    int i;

    if (!s || !m)
        return;

    s->mission_unit = m->unit;
    for (i = 0; i < Q2_SAVE_MISSION_ROWS && i < Q2_MISSION_ROWS; i++) {
        snprintf(s->mission[i].name, sizeof(s->mission[i].name), "%s",
                 m->row[i].name);
        s->mission[i].secrets       = m->row[i].secrets;
        s->mission[i].secrets_total = m->row[i].secrets_total;
        s->mission[i].kills         = m->row[i].kills;
        s->mission[i].kills_total   = m->row[i].kills_total;
    }
}

void q2_save_apply_mission(const q2_save *s, struct q2_mission *m)
{
    int i;

    if (!s || !m)
        return;

    m->unit = s->mission_unit;
    for (i = 0; i < Q2_SAVE_MISSION_ROWS && i < Q2_MISSION_ROWS; i++)
        q2_mission_set_row(m, i, s->mission[i].name,
                           s->mission[i].secrets, s->mission[i].secrets_total,
                           s->mission[i].kills,   s->mission[i].kills_total);
}

void q2_save_set_settings(q2_save *s, const s16 *values, u32 count)
{
    u32 i;

    if (!s)
        return;

    if (!values || count == 0) {
        s->settings_count = 0;
        return;
    }

    if (count > Q2_SAVE_SETTINGS_MAX)
        count = Q2_SAVE_SETTINGS_MAX;

    for (i = 0; i < count; i++)
        s->settings[i] = values[i];
    s->settings_count = count;
}

u32 q2_save_get_settings(const q2_save *s, s16 *out, u32 count)
{
    u32 i, n;

    if (!s || !out || count == 0)
        return 0;

    n = s->settings_count;
    if (n > count)
        n = count;
    for (i = 0; i < n; i++)
        out[i] = s->settings[i];
    return n;
}

/* ------------------------------------------------------------------------- */
/* Serialising one chunk at a time                                            */
/* ------------------------------------------------------------------------- */
static void write_head(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_HEAD);

    w_str(w, s->serial, Q2_SAVE_SERIAL_LEN);
    w_str(w, s->map,    Q2_SAVE_MAP_LEN);
    w_s32(w, s->zone);
    w_str(w, s->label,  Q2_SAVE_LABEL_LEN);
    w_u32(w, s->timestamp);

    w_chunk_end(w, at);
}

static void read_head(rbuf *r, q2_save *s)
{
    r_str(r, s->serial, Q2_SAVE_SERIAL_LEN);
    r_str(r, s->map,    Q2_SAVE_MAP_LEN);
    s->zone = r_s32(r);
    r_str(r, s->label,  Q2_SAVE_LABEL_LEN);
    s->timestamp = r_u32(r);
}

static void write_move_ent(wbuf *w, const q2_move_ent *e)
{
    w_s32v(w, e->pos, 3);
    w_s16v(w, e->ground_normal, 3);
    w_s16v(w, e->last_normal, 3);
    w_u32(w, e->flags);
    w_s16(w, e->max_slope_ny);
    w_s32(w, e->node);
}

static void read_move_ent(rbuf *r, q2_move_ent *e)
{
    r_s32v(r, e->pos, 3);
    r_s16v(r, e->ground_normal, 3);
    r_s16v(r, e->last_normal, 3);
    e->flags        = r_u32(r);
    e->max_slope_ny = r_s16(r);
    e->node         = r_s32(r);
}

static void write_player(wbuf *w, const q2_save *s)
{
    const q2_player *p = &s->player;
    size_t at = w_chunk_begin(w, TAG_PLYR);

    w_s32v(w, p->pos, 3);
    w_s32v(w, p->vel, 3);
    w_s32(w, p->yaw);
    w_s32(w, p->pitch);
    w_s32(w, p->roll);
    w_s16v(w, p->wish, 3);
    w_s16(w, p->pitch_rate);
    w_s16(w, p->yaw_rate);
    w_s16v(w, p->impulse, 3);
    w_bool(w, p->impulse_armed);
    w_s16v(w, p->frame_delta, 3);
    w_s16(w, p->jump_hold);
    w_s32(w, p->view_height);
    w_bool(w, p->on_ground);
    w_bool(w, p->crouching);
    w_s32(w, p->ground_y);
    w_s16(w, p->fall_value);
    w_s32(w, p->fall_time);
    w_s32(w, p->footstep_time);
    w_s32(w, (s32)p->foot);
    w_s32(w, (s32)p->look_scheme);

    /*
     * The movement frame's remaining state. Every one of these is a deadline or
     * a latch that outlives a tick, so a save without them restores a player who
     * has forgotten which way they were leaning, whether their view was walking
     * itself level, and how long they have left before they can grunt again.
     */
    w_u32(w, p->ent2_flags);
    w_u8(w, p->look_hist);
    w_u8(w, p->recentring);
    w_s16(w, p->autocentre);
    w_s16v(w, p->kick, 3);
    w_s32(w, p->kick_time);
    w_s16v(w, p->hurt_kick, 2);
    w_s32(w, p->pain_time);
    w_s16(w, p->prev_health);
    w_s16(w, p->prev_armour);
    w_s32(w, p->wade);
    w_s32(w, p->water_air);
    w_s32(w, p->water_next);
    w_s32(w, p->splash_time);
    w_bool(w, p->water_voice);

    write_move_ent(w, &p->ent);

    w_chunk_end(w, at);
}

static void read_player(rbuf *r, q2_save *s)
{
    q2_player *p = &s->player;

    r_s32v(r, p->pos, 3);
    r_s32v(r, p->vel, 3);
    p->yaw   = r_s32(r);
    p->pitch = r_s32(r);
    p->roll  = r_s32(r);
    r_s16v(r, p->wish, 3);
    p->pitch_rate = r_s16(r);
    p->yaw_rate   = r_s16(r);
    r_s16v(r, p->impulse, 3);
    p->impulse_armed = r_bool(r);
    r_s16v(r, p->frame_delta, 3);
    p->jump_hold   = r_s16(r);
    p->view_height = r_s32(r);
    p->on_ground   = r_bool(r);
    p->crouching   = r_bool(r);
    p->ground_y    = r_s32(r);
    p->fall_value  = r_s16(r);
    p->fall_time   = r_s32(r);
    p->footstep_time = r_s32(r);
    p->foot          = (int)r_s32(r);
    p->look_scheme   = (int)r_s32(r);

    p->ent2_flags = r_u32(r);
    p->look_hist  = r_u8(r);
    p->recentring = r_u8(r) != 0;
    p->autocentre = r_s16(r);
    r_s16v(r, p->kick, 3);
    p->kick_time  = r_s32(r);
    r_s16v(r, p->hurt_kick, 2);
    p->pain_time  = r_s32(r);
    p->prev_health = r_s16(r);
    p->prev_armour = r_s16(r);
    p->wade        = r_s32(r);
    p->water_air   = r_s32(r);
    p->water_next  = r_s32(r);
    p->splash_time = r_s32(r);
    p->water_voice = r_bool(r);

    read_move_ent(r, &p->ent);
}

static void write_sim(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_SIMS);

    w_s32(w, s->level_time);
    w_u32(w, s->tick_count);
    w_s32(w, s->dt_accum);
    w_s32(w, s->dt_per_field);
    w_s32(w, s->gravity);
    w_u32(w, s->env_flags);
    w_u32(w, s->cheats);
    w_s32(w, s->current_node);
    w_u8(w, s->multiplayer);
    w_u8(w, s->full_basis_movement);
    w_u8(w, s->no_fall_damage);

    w_chunk_end(w, at);
}

static void read_sim(rbuf *r, q2_save *s)
{
    s->level_time          = r_s32(r);
    s->tick_count          = r_u32(r);
    s->dt_accum            = r_s32(r);
    s->dt_per_field        = r_s32(r);
    s->gravity             = r_s32(r);
    s->env_flags           = r_u32(r);
    s->cheats              = r_u32(r);
    s->current_node        = r_s32(r);
    s->multiplayer         = r_u8(r);
    s->full_basis_movement = r_u8(r);
    s->no_fall_damage      = r_u8(r);
}

static void write_inventory(wbuf *w, const q2_inventory *v)
{
    int i;

    w_s16(w, v->health);
    w_s16(w, v->health_max);
    w_s16(w, v->armour);
    w_u8(w, v->armour_class);
    for (i = 0; i < Q2_AMMO_COUNT; i++)
        w_s16(w, v->ammo[i]);
    w_u16(w, v->weapons);
    w_u8(w, (u8)v->current_weapon);
    w_u8(w, v->ammo_tier);
    w_u32(w, v->flags);
    w_s16(w, v->silencer_shots);
    w_u8(w, v->last_item);
    w_s32(w, v->item_name_until);
    w_s32(w, v->quad_until);
    w_s32(w, v->invuln_until);
    w_s32(w, v->enviro_until);
    w_s32(w, v->breather_until);
    w_s32(w, v->mega_health_next);
}

static void read_inventory(rbuf *r, q2_inventory *v)
{
    int i;

    v->health       = r_s16(r);
    v->health_max   = r_s16(r);
    v->armour       = r_s16(r);
    v->armour_class = r_u8(r);
    for (i = 0; i < Q2_AMMO_COUNT; i++)
        v->ammo[i] = r_s16(r);
    v->weapons        = r_u16(r);
    v->current_weapon = (s8)r_u8(r);
    v->ammo_tier      = r_u8(r);
    v->flags          = r_u32(r);
    v->silencer_shots = r_s16(r);
    v->last_item      = r_u8(r);
    v->item_name_until = r_s32(r);
    v->quad_until      = r_s32(r);
    v->invuln_until    = r_s32(r);
    v->enviro_until    = r_s32(r);
    v->breather_until  = r_s32(r);
    v->mega_health_next = r_s32(r);
}

static void write_combat(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_CMBT);

    w_s32(w, s->weapon_id);
    w_s32(w, s->next_fire);
    w_s16v(w, s->kick, 3);
    w_s32(w, s->chaingun_bullets);
    w_u32(w, s->rng_state);
    w_u32(w, s->fx_rng_state);

    w_chunk_end(w, at);
}

static void read_combat(rbuf *r, q2_save *s)
{
    s->weapon_id        = r_s32(r);
    s->next_fire        = r_s32(r);
    r_s16v(r, s->kick, 3);
    s->chaingun_bullets = r_s32(r);
    s->rng_state        = r_u32(r);
    s->fx_rng_state     = r_u32(r);
}

static void write_projectiles(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_PROJ);
    u32 i;

    w_u32(w, (u32)Q2_PROJ_MAX);
    for (i = 0; i < Q2_PROJ_MAX; i++) {
        const q2_projectile *p = &s->proj[i];

        w_bool(w, p->in_use);
        w_u8(w, (u8)p->kind);
        w_s32v(w, p->pos, 3);
        w_s32v(w, p->vel, 3);
        w_s16(w, p->damage);
        w_s16(w, p->mod);
        w_s16(w, p->splash_radius);
        w_s32(w, p->expires);
        w_s32(w, p->owner);
        w_bool(w, p->bounced);
        w_s32(w, p->node);
    }

    w_chunk_end(w, at);
}

static void read_projectiles(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    for (i = 0; i < n; i++) {
        q2_projectile scratch;
        q2_projectile *p = (i < Q2_PROJ_MAX) ? &s->proj[i] : &scratch;

        memset(p, 0, sizeof(*p));
        p->in_use = r_bool(r);
        p->kind   = (q2_proj_kind)r_u8(r);
        r_s32v(r, p->pos, 3);
        r_s32v(r, p->vel, 3);
        p->damage        = r_s16(r);
        p->mod           = r_s16(r);
        p->splash_radius = r_s16(r);
        p->expires       = r_s32(r);
        p->owner         = r_s32(r);
        p->bounced       = r_bool(r);
        p->node          = r_s32(r);

        if (r->bad)
            return;
    }
}

static void write_entities(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_ENTS);
    u32 i;

    w_u32(w, s->entity_count);
    for (i = 0; i < s->entity_count; i++) {
        const q2_save_entity *e = &s->entities[i];

        w_u16(w, e->place_id);
        w_u8(w, e->in_use);
        w_u8(w, e->hidden);
        w_u8(w, e->taken);
        w_u8(w, e->think);
        w_s16(w, e->scale);
        w_s16(w, e->health);
        w_s32(w, e->frame);
        w_s32(w, e->spin);
        w_s32(w, e->remove_in);
        w_s32(w, e->respawn_at);
        w_s32v(w, e->pos, 3);
    }

    w_chunk_end(w, at);
}

static bool read_entities(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->entities);
    s->entities = (q2_save_entity *)calloc(n, sizeof(q2_save_entity));
    if (!s->entities)
        return false;
    s->entity_count = n;

    for (i = 0; i < n; i++) {
        q2_save_entity *e = &s->entities[i];

        e->place_id   = r_u16(r);
        e->in_use     = r_u8(r);
        e->hidden     = r_u8(r);
        e->taken      = r_u8(r);
        e->think      = r_u8(r);
        e->scale      = r_s16(r);
        e->health     = r_s16(r);
        e->frame      = r_s32(r);
        e->spin       = r_s32(r);
        e->remove_in  = r_s32(r);
        e->respawn_at = r_s32(r);
        r_s32v(r, e->pos, 3);

        if (r->bad)
            return false;
    }
    return true;
}

static void write_item_state(wbuf *w, const q2_save *s)
{
    size_t at;
    u32 i;

    if (!s->item_state_present)
        return;

    at = w_chunk_begin(w, TAG_ITEM);
    w_u32(w, s->item_population_group_count);
    w_u32(w, s->item_group_order_count);
    for (i = 0; i < s->item_group_order_count; i++)
        w_u32(w, s->item_group_order[i]);

    w_u32(w, s->item_key_count);
    for (i = 0; i < s->item_key_count; i++) {
        w_s32(w, s->item_keys[i].group);
        w_u32(w, s->item_keys[i].slot);
    }
    w_chunk_end(w, at);
}

static bool read_item_state(rbuf *r, q2_save *s)
{
    u32 groups = r_u32(r);
    u32 order_count = r_u32(r);
    u32 i;

    if (r->bad || groups > SAVE_MAX_ITEM_GROUPS || order_count > groups)
        return false;

    free(s->item_group_order);
    s->item_group_order = NULL;
    s->item_group_order_count = 0;
    if (order_count) {
        s->item_group_order = (u32 *)calloc(order_count, sizeof(u32));
        if (!s->item_group_order)
            return false;
        for (i = 0; i < order_count; i++) {
            s->item_group_order[i] = r_u32(r);
            if (r->bad || s->item_group_order[i] >= groups)
                return false;
        }
        s->item_group_order_count = order_count;
    }

    s->item_key_count = r_u32(r);
    if (r->bad || s->item_key_count > SAVE_MAX_ENTITIES)
        return false;

    free(s->item_keys);
    s->item_keys = NULL;
    if (s->item_key_count) {
        s->item_keys = (q2_save_item_key *)calloc(s->item_key_count,
                                                  sizeof(*s->item_keys));
        if (!s->item_keys)
            return false;

        for (i = 0; i < s->item_key_count; i++) {
            s32 group = r_s32(r);
            u32 slot  = r_u32(r);

            if (r->bad || group < -1 ||
                (group >= 0 && (u32)group >= groups))
                return false;
            s->item_keys[i].group = group;
            s->item_keys[i].slot  = slot;
        }
    }
    s->item_population_group_count = groups;
    s->item_state_present = true;
    return true;
}

static void write_breakables(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_BRKS);
    u32 i;

    w_u32(w, s->breakable_count);
    for (i = 0; i < s->breakable_count; i++) {
        w_s32(w, s->breakables[i].scene_node);
        w_s16(w, s->breakables[i].health);
        w_u8(w, s->breakables[i].broken);
    }

    w_chunk_end(w, at);
}

static bool read_breakables(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->breakables);
    s->breakables = (q2_save_breakable *)calloc(n, sizeof(*s->breakables));
    if (!s->breakables)
        return false;
    s->breakable_count = n;

    for (i = 0; i < n; i++) {
        s->breakables[i].scene_node = r_s32(r);
        s->breakables[i].health     = r_s16(r);
        s->breakables[i].broken     = r_u8(r);
        if (r->bad)
            return false;
    }
    return true;
}

static void write_movers(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_MOVR);
    u32 i;

    w_u32(w, s->mover_count);
    for (i = 0; i < s->mover_count; i++) {
        const q2_save_mover *m = &s->movers[i];

        w_u32(w, m->item_offset);
        w_u8(w, m->seq);
        w_u8(w, m->state);
        w_u8(w, m->saved_state);
        w_u8(w, m->block_timer);
        w_u8(w, m->triggered);
        w_u8(w, m->announced);
        w_u16(w, m->delay_timer);
        w_u16(w, m->wait_timer);
        w_s32(w, m->offset);
    }

    w_chunk_end(w, at);
}

static bool read_movers(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->movers);
    s->movers = (q2_save_mover *)calloc(n, sizeof(*s->movers));
    if (!s->movers)
        return false;
    s->mover_count = n;

    for (i = 0; i < n; i++) {
        q2_save_mover *m = &s->movers[i];

        m->item_offset = r_u32(r);
        m->seq         = r_u8(r);
        m->state       = r_u8(r);
        m->saved_state = r_u8(r);
        m->block_timer = r_u8(r);
        m->triggered   = r_u8(r);
        m->announced   = r_u8(r);
        m->delay_timer = r_u16(r);
        m->wait_timer  = r_u16(r);
        m->offset      = r_s32(r);
        if (r->bad)
            return false;
    }
    return true;
}

/* Which occurrence of `item_offset` mover `index` is. A MOVER_C double door is
 * two leaves from one item, so the offset alone does not identify a leaf. */
static u8 mover_seq(const q2_mover_set *set, u32 index)
{
    u32 i, n = 0;

    for (i = 0; i < index && i < set->count; i++)
        if (set->movers[i].item_offset == set->movers[index].item_offset)
            n++;
    return (u8)(n > 255 ? 255 : n);
}

void q2_save_capture_movers(q2_save *s, const q2_mover_set *set)
{
    u32 i;

    if (!s || !set || set->count == 0 || !set->movers)
        return;

    free(s->movers);
    s->movers = (q2_save_mover *)calloc(set->count, sizeof(*s->movers));
    if (!s->movers)
        return;
    s->mover_count = set->count;

    for (i = 0; i < set->count; i++) {
        const q2_mover *m = &set->movers[i];
        q2_save_mover  *d = &s->movers[i];

        d->item_offset = m->item_offset;
        d->seq         = mover_seq(set, i);
        d->state       = m->state;
        d->saved_state = m->saved_state;
        d->block_timer = m->block_timer;
        d->triggered   = m->triggered;
        d->announced   = m->announced;
        d->delay_timer = m->delay_timer;
        d->wait_timer  = m->wait_timer;
        d->offset      = m->offset;
    }
}

void q2_save_apply_movers(const q2_save *s, q2_mover_set *set)
{
    u32 i, j;

    if (!s || !s->movers || !set || !set->movers)
        return;

    for (i = 0; i < s->mover_count; i++) {
        const q2_save_mover *d = &s->movers[i];

        for (j = 0; j < set->count; j++) {
            q2_mover *m = &set->movers[j];

            if (m->item_offset != d->item_offset)
                continue;
            if (mover_seq(set, j) != d->seq)
                continue;

            m->state       = d->state;
            m->saved_state = d->saved_state;
            m->block_timer = d->block_timer;
            m->triggered   = d->triggered;
            m->announced   = d->announced;
            m->delay_timer = d->delay_timer;
            m->wait_timer  = d->wait_timer;
            m->offset      = d->offset;
            break;
        }
    }
}

static void write_creatures(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_CRES);
    u32 i;

    w_u32(w, s->creature_count);
    for (i = 0; i < s->creature_count; i++) {
        const q2_save_creature *c = &s->creatures[i];

        w_u8(w, c->in_use);
        w_u8(w, c->dead);
        w_s16(w, c->health);
        w_s16(w, c->frame);
        w_s16v(w, c->angles, 3);
        w_s32v(w, c->pos, 3);
    }

    w_chunk_end(w, at);
}

static bool read_creatures(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->creatures);
    s->creatures = (q2_save_creature *)calloc(n, sizeof(*s->creatures));
    if (!s->creatures)
        return false;
    s->creature_count = n;

    for (i = 0; i < n; i++) {
        q2_save_creature *c = &s->creatures[i];

        c->in_use = r_u8(r);
        c->dead   = r_u8(r);
        c->health = r_s16(r);
        c->frame  = r_s16(r);
        r_s16v(r, c->angles, 3);
        r_s32v(r, c->pos, 3);
        if (r->bad)
            return false;
    }
    return true;
}

void q2_save_capture_creatures(q2_save *s, const q2_monster_set *set)
{
    u32 i;

    if (!s || !set || set->count == 0 || !set->monsters)
        return;

    free(s->creatures);
    s->creatures = (q2_save_creature *)calloc(set->count, sizeof(*s->creatures));
    if (!s->creatures)
        return;
    s->creature_count = set->count;

    for (i = 0; i < set->count; i++) {
        const q2_monster *m = &set->monsters[i];
        q2_save_creature *d = &s->creatures[i];
        int k;

        d->in_use = m->in_use ? 1u : 0u;
        d->dead   = m->dead ? 1u : 0u;
        d->health = m->health;
        d->frame  = m->frame;
        for (k = 0; k < 3; k++) {
            d->angles[k] = m->angles[k];
            d->pos[k]    = m->pos[k];
        }
    }
}

void q2_save_apply_creatures(const q2_save *s, q2_monster_set *set)
{
    u32 i;

    if (!s || !s->creatures || !set || !set->monsters)
        return;

    /*
     * The count must match. The set is rebuilt from the map's spawn records in
     * a fixed order, so a different count means a different population — a
     * save from another zone, or from a build whose spawn pass differs — and
     * restoring by index into that would put one creature's health on another.
     */
    if (s->creature_count != set->count)
        return;

    for (i = 0; i < set->count; i++) {
        const q2_save_creature *d = &s->creatures[i];
        q2_monster             *m = &set->monsters[i];
        int k;

        m->in_use = d->in_use != 0;
        m->dead   = d->dead != 0;
        m->health = d->health;
        m->frame  = d->frame;
        for (k = 0; k < 3; k++) {
            m->angles[k] = d->angles[k];
            m->pos[k]    = d->pos[k];
        }
    }
}

static void write_mission(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_MISN);
    int i;

    w_s32(w, s->mission_unit);
    w_u32(w, (u32)Q2_SAVE_MISSION_ROWS);
    for (i = 0; i < Q2_SAVE_MISSION_ROWS; i++) {
        w_str(w, s->mission[i].name, Q2_SAVE_MISSION_NAME + 1);
        w_u8(w, s->mission[i].secrets);
        w_u8(w, s->mission[i].secrets_total);
        w_u8(w, s->mission[i].kills);
        w_u8(w, s->mission[i].kills_total);
    }

    w_chunk_end(w, at);
}

static void read_mission(rbuf *r, q2_save *s)
{
    u32 n, i;

    s->mission_unit = r_s32(r);
    n = r_u32(r);

    for (i = 0; i < n; i++) {
        q2_save_level_stats scratch;
        q2_save_level_stats *row =
            (i < Q2_SAVE_MISSION_ROWS) ? &s->mission[i] : &scratch;

        r_str(r, row->name, Q2_SAVE_MISSION_NAME + 1);
        row->secrets       = r_u8(r);
        row->secrets_total = r_u8(r);
        row->kills         = r_u8(r);
        row->kills_total   = r_u8(r);

        if (r->bad)
            return;
    }
}

static void write_settings(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_SETT);
    u32 i;

    w_u32(w, s->settings_count);
    for (i = 0; i < s->settings_count; i++)
        w_s16(w, s->settings[i]);

    w_chunk_end(w, at);
}

static void read_settings(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    s->settings_count = (n > Q2_SAVE_SETTINGS_MAX) ? Q2_SAVE_SETTINGS_MAX : n;
    for (i = 0; i < n; i++) {
        s16 v = r_s16(r);
        if (i < Q2_SAVE_SETTINGS_MAX)
            s->settings[i] = v;
        if (r->bad)
            return;
    }
}

/* A run of bytes with a leading count — the event flags and the trigger
 * residency are both exactly that, so they share one pair. */
static void write_bytes_chunk(wbuf *w, u32 tag, const u8 *data, u32 count)
{
    size_t at = w_chunk_begin(w, tag);

    w_u32(w, count);
    if (count && data)
        w_bytes(w, data, count);

    w_chunk_end(w, at);
}

static bool read_bytes_chunk(rbuf *r, u8 **out, u32 *out_count, u32 limit)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > limit || !r_need(r, n))
        return false;

    free(*out);
    *out = (u8 *)malloc(n);
    if (!*out)
        return false;

    for (i = 0; i < n; i++)
        (*out)[i] = r_u8(r);
    *out_count = n;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */
static q2_result build_body(const q2_save *s, wbuf *w)
{
    write_head(w, s);
    write_player(w, s);
    write_sim(w, s);

    {
        size_t at = w_chunk_begin(w, TAG_INVN);
        write_inventory(w, &s->inventory);
        w_chunk_end(w, at);
    }

    write_combat(w, s);
    write_projectiles(w, s);
    write_bytes_chunk(w, TAG_EVNT, s->event_flags, s->event_count);
    write_bytes_chunk(w, TAG_TRIG, s->trigger_inside, s->trigger_count);
    write_entities(w, s);
    write_item_state(w, s);
    write_breakables(w, s);
    write_movers(w, s);
    write_creatures(w, s);
    write_mission(w, s);
    write_settings(w, s);

    return w->bad ? Q2_ERR_NO_MEMORY : Q2_OK;
}

q2_result q2_save_write(const q2_save *s, const char *path)
{
    wbuf w;
    FILE *f;
    u8 header[SAVE_FILE_HEADER];
    u32 crc;
    q2_result rc;

    if (!s || !path)
        return Q2_ERR_INVALID_ARG;

    memset(&w, 0, sizeof(w));
    rc = build_body(s, &w);
    if (rc != Q2_OK) {
        w_free(&w);
        return rc;
    }

    crc = crc32_of(w.p, w.len);

    memcpy(header, Q2_SAVE_MAGIC, 4);
    header[4]  = (u8)Q2_SAVE_VERSION;
    header[5]  = header[6] = header[7] = 0;
    header[8]  = (u8)w.len;
    header[9]  = (u8)(w.len >> 8);
    header[10] = (u8)(w.len >> 16);
    header[11] = (u8)(w.len >> 24);
    header[12] = (u8)crc;
    header[13] = (u8)(crc >> 8);
    header[14] = (u8)(crc >> 16);
    header[15] = (u8)(crc >> 24);

    f = fopen(path, "wb");
    if (!f) {
        w_free(&w);
        return Q2_ERR_IO;
    }

    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        (w.len && fwrite(w.p, 1, w.len, f) != w.len)) {
        fclose(f);
        w_free(&w);
        return Q2_ERR_IO;
    }

    fclose(f);
    w_free(&w);
    return Q2_OK;
}

/* Read the whole file into memory. Saves are tens of kilobytes; streaming them
 * would buy nothing and would make the CRC a second pass. */
static q2_result slurp(const char *path, u8 **out, size_t *out_size)
{
    FILE *f;
    long size;
    u8 *data;

    f = fopen(path, "rb");
    if (!f)
        return Q2_ERR_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return Q2_ERR_IO;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return Q2_ERR_IO;
    }

    data = (u8 *)malloc((size_t)size ? (size_t)size : 1);
    if (!data) {
        fclose(f);
        return Q2_ERR_NO_MEMORY;
    }

    if (size && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return Q2_ERR_IO;
    }

    fclose(f);
    *out      = data;
    *out_size = (size_t)size;
    return Q2_OK;
}

/*
 * Validate the fixed header and hand back the body. Shared by the full read and
 * the header-only listing, so a slot that lists cleanly is a slot that loads.
 */
static q2_result open_body(const u8 *data, size_t size, u32 *out_version,
                           const u8 **out_body, size_t *out_body_size)
{
    u32 version, body_size, crc;

    if (size < SAVE_FILE_HEADER)
        return Q2_ERR_BAD_FORMAT;
    if (memcmp(data, Q2_SAVE_MAGIC, 4) != 0)
        return Q2_ERR_BAD_FORMAT;

    version   = q2_rd_u32(data + 4);
    body_size = q2_rd_u32(data + 8);
    crc       = q2_rd_u32(data + 12);

    if (out_version)
        *out_version = version;

    if (version != Q2_SAVE_VERSION) {
        Q2_ERROR("save is version %u, this build reads version %d",
                 version, Q2_SAVE_VERSION);
        return Q2_ERR_UNSUPPORTED;
    }

    if (body_size > size - SAVE_FILE_HEADER)
        return Q2_ERR_BAD_FORMAT;

    if (crc32_of(data + SAVE_FILE_HEADER, body_size) != crc) {
        Q2_ERROR("save is corrupt: checksum mismatch");
        return Q2_ERR_BAD_FORMAT;
    }

    *out_body      = data + SAVE_FILE_HEADER;
    *out_body_size = body_size;
    return Q2_OK;
}

/*
 * Walk the chunks. Each is `tag, size, payload`, and a payload is read through
 * its own cursor so a chunk that is longer than its reader expects is skipped
 * cleanly rather than desynchronising everything after it — which is the whole
 * point of sizing them.
 */
static q2_result read_body(q2_save *out, const u8 *body, size_t body_size)
{
    rbuf top;

    memset(&top, 0, sizeof(top));
    top.p   = body;
    top.len = body_size;

    while (top.at + 8 <= top.len) {
        u32 tag  = r_u32(&top);
        u32 size = r_u32(&top);
        rbuf c;

        if (top.bad || size > top.len - top.at)
            return Q2_ERR_BAD_FORMAT;

        memset(&c, 0, sizeof(c));
        c.p   = body + top.at;
        c.len = size;

        switch (tag) {
        case TAG_HEAD: read_head(&c, out);    break;
        case TAG_PLYR: read_player(&c, out);  break;
        case TAG_SIMS: read_sim(&c, out);     break;
        case TAG_INVN: read_inventory(&c, &out->inventory); break;
        case TAG_CMBT: read_combat(&c, out);  break;
        case TAG_PROJ: read_projectiles(&c, out); break;
        case TAG_MISN: read_mission(&c, out); break;
        case TAG_SETT: read_settings(&c, out); break;

        case TAG_EVNT:
            if (!read_bytes_chunk(&c, &out->event_flags, &out->event_count,
                                  SAVE_MAX_EVENTS))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_TRIG:
            if (!read_bytes_chunk(&c, &out->trigger_inside,
                                  &out->trigger_count, SAVE_MAX_TRIGGERS))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_ENTS:
            if (!read_entities(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_ITEM:
            if (!read_item_state(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_BRKS:
            if (!read_breakables(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_MOVR:
            if (!read_movers(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_CRES:
            if (!read_creatures(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        default:
            /* An unknown chunk from a later version. Skipping it is the
             * contract that makes the format extensible. */
            break;
        }

        if (c.bad)
            return Q2_ERR_BAD_FORMAT;

        top.at += size;
    }

    if (top.at != top.len)
        return Q2_ERR_BAD_FORMAT;

    return Q2_OK;
}

q2_result q2_save_read(q2_save *out, const char *path)
{
    u8 *data = NULL;
    size_t size = 0;
    const u8 *body;
    size_t body_size;
    q2_result rc;

    if (!out || !path)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    rc = slurp(path, &data, &size);
    if (rc != Q2_OK)
        return rc;

    rc = open_body(data, size, NULL, &body, &body_size);
    if (rc != Q2_OK) {
        free(data);
        return rc;
    }

    rc = read_body(out, body, body_size);
    free(data);

    if (rc != Q2_OK)
        q2_save_free(out);

    return rc;
}

q2_result q2_save_read_info(q2_save_info *out, const char *path)
{
    u8 *data = NULL;
    size_t size = 0;
    const u8 *body;
    size_t body_size;
    u32 version = 0;
    q2_result rc;
    rbuf top;

    if (!out || !path)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    rc = slurp(path, &data, &size);
    if (rc != Q2_OK)
        return rc;

    rc = open_body(data, size, &version, &body, &body_size);
    out->version = version;
    if (rc != Q2_OK) {
        free(data);
        return rc;
    }

    /*
     * Only the chunks the row needs: identity, the clock, and enough of the
     * player's condition to say what state the save is in. Everything else is
     * skipped, so listing four slots reads four headers rather than four games.
     */
    memset(&top, 0, sizeof(top));
    top.p   = body;
    top.len = body_size;

    while (top.at + 8 <= top.len) {
        u32 tag  = r_u32(&top);
        u32 csize = r_u32(&top);
        rbuf c;

        if (top.bad || csize > top.len - top.at)
            break;

        memset(&c, 0, sizeof(c));
        c.p   = body + top.at;
        c.len = csize;

        if (tag == TAG_HEAD) {
            q2_save scratch;
            memset(&scratch, 0, sizeof(scratch));
            read_head(&c, &scratch);
            memcpy(out->serial, scratch.serial, sizeof(out->serial));
            memcpy(out->map,    scratch.map,    sizeof(out->map));
            memcpy(out->label,  scratch.label,  sizeof(out->label));
            out->zone      = scratch.zone;
            out->timestamp = scratch.timestamp;
        } else if (tag == TAG_SIMS) {
            out->level_time = r_s32(&c);
        } else if (tag == TAG_INVN) {
            q2_inventory inv;
            memset(&inv, 0, sizeof(inv));
            read_inventory(&c, &inv);
            out->health = inv.health;
        } else if (tag == TAG_CMBT) {
            out->weapon_id = r_s32(&c);
        }

        top.at += csize;
    }

    free(data);
    out->used = true;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Where saves live                                                           */
/* ------------------------------------------------------------------------- */
static char g_save_dir[512];
static bool g_save_dir_set;

void q2_save_set_dir(const char *dir)
{
    if (!dir || !*dir) {
        g_save_dir[0]  = '\0';
        g_save_dir_set = false;
        return;
    }
    snprintf(g_save_dir, sizeof(g_save_dir), "%s", dir);
    g_save_dir_set = true;
}

const char *q2_save_dir(void)
{
    const char *env;

    if (g_save_dir_set)
        return g_save_dir;

#if defined(_WIN32)
    env = getenv("APPDATA");
    if (env && *env) {
        snprintf(g_save_dir, sizeof(g_save_dir), "%s/Q2PSX-PC/saves", env);
        g_save_dir_set = true;
        return g_save_dir;
    }
#else
    env = getenv("XDG_DATA_HOME");
    if (env && *env) {
        snprintf(g_save_dir, sizeof(g_save_dir), "%s/q2psx-pc/saves", env);
        g_save_dir_set = true;
        return g_save_dir;
    }
    env = getenv("HOME");
    if (env && *env) {
        snprintf(g_save_dir, sizeof(g_save_dir),
                 "%s/.local/share/q2psx-pc/saves", env);
        g_save_dir_set = true;
        return g_save_dir;
    }
#endif

    /* Nothing resolved — a bare relative directory beside the executable is
     * better than refusing to save at all. */
    snprintf(g_save_dir, sizeof(g_save_dir), "%s", "saves");
    g_save_dir_set = true;
    return g_save_dir;
}

/* Create every component of `path`. Only called on the way to a write, so a
 * session that never saves leaves nothing behind. */
static bool ensure_dir(const char *path)
{
    char work[512];
    size_t i, n;

    snprintf(work, sizeof(work), "%s", path);
    n = strlen(work);
    if (!n)
        return false;

    for (i = 1; i <= n; i++) {
        char c = work[i];

        if (c != '/' && c != '\\' && c != '\0')
            continue;

        work[i] = '\0';
        /* A drive root ("C:") is not a directory anyone creates. */
        if (!(i == 2 && work[1] == ':'))
            (void)SAVE_MKDIR(work);
        work[i] = c;
    }

    return true;
}

q2_result q2_save_slot_path(int slot, char *out, u32 out_size)
{
    if (!out || out_size == 0)
        return Q2_ERR_INVALID_ARG;
    if (slot < 0 || slot >= Q2_SAVE_SLOTS)
        return Q2_ERR_RANGE;

    snprintf(out, out_size, "%s/slot%d.q2s", q2_save_dir(), slot);
    return Q2_OK;
}

q2_result q2_save_slot_write(const q2_save *s, int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;

    ensure_dir(q2_save_dir());
    return q2_save_write(s, path);
}

q2_result q2_save_slot_read(q2_save *out, int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return q2_save_read(out, path);
}

q2_result q2_save_slot_info(q2_save_info *out, int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return q2_save_read_info(out, path);
}

q2_result q2_save_slot_delete(int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return (remove(path) == 0) ? Q2_OK : Q2_ERR_NOT_FOUND;
}

u32 q2_save_slots_scan(q2_save_info *out, u32 count)
{
    u32 i, used = 0;

    if (!out)
        return 0;
    if (count > Q2_SAVE_SLOTS)
        count = Q2_SAVE_SLOTS;

    for (i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        /*
         * A slot that fails to parse comes back unused rather than as an error:
         * the screen has four rows either way, and a row that cannot be loaded
         * should not be selectable.
         */
        if (q2_save_slot_info(&out[i], (int)i) == Q2_OK && out[i].used)
            used++;
        else
            out[i].used = false;
    }

    return used;
}

/* ------------------------------------------------------------------------- */
/* Row text                                                                   */
/* ------------------------------------------------------------------------- */
const char *q2_save_slot_row(const q2_save_info *info, int slot,
                             char *out, u32 out_size)
{
    if (!out || out_size == 0)
        return "";

    /*
     * An empty slot is the EMPTY STRING, not "EMPTY". The selection bar tests
     * the label against the empty string, so an unfilled row draws nothing and
     * gets no bar (memcard.h) — writing a word there would give it both.
     */
    if (!info || !info->used) {
        out[0] = '\0';
        return out;
    }

    if (info->label[0])
        snprintf(out, out_size, "%d %s", slot + 1, info->label);
    else
        snprintf(out, out_size, "%d %s", slot + 1, info->map);

    return out;
}

/* ------------------------------------------------------------------------- */
/* Multiplayer settings slots                                                */

q2_result q2_settings_slot_path(int slot, char *out, u32 out_size)
{
    if (!out || out_size == 0)
        return Q2_ERR_INVALID_ARG;
    if (slot < 0 || slot >= Q2_SAVE_SLOTS)
        return Q2_ERR_RANGE;

    snprintf(out, out_size, "%s/settings%d.q2c", q2_save_dir(), slot);
    return Q2_OK;
}

q2_result q2_settings_slot_write(const q2_settings_blob *settings, int slot)
{
    u8 file[SAVE_FILE_HEADER + 4 + Q2_SETTINGS_VALUE_MAX * 2];
    u8 *body = file + SAVE_FILE_HEADER;
    u32 body_size, crc, i;
    char path[512];
    FILE *f;
    q2_result rc;

    if (!settings)
        return Q2_ERR_INVALID_ARG;
    if (settings->count > Q2_SETTINGS_VALUE_MAX)
        return Q2_ERR_RANGE;
    rc = q2_settings_slot_path(slot, path, (u32)sizeof(path));
    if (rc != Q2_OK)
        return rc;

    body_size = 4 + settings->count * 2;
    q2_wr_u32(body, settings->count);
    for (i = 0; i < settings->count; i++)
        q2_wr_u16(body + 4 + i * 2, (u16)settings->value[i]);
    crc = crc32_of(body, body_size);

    memcpy(file, SETTINGS_MAGIC, 4);
    q2_wr_u32(file + 4, SETTINGS_VERSION);
    q2_wr_u32(file + 8, body_size);
    q2_wr_u32(file + 12, crc);

    ensure_dir(q2_save_dir());
    f = fopen(path, "wb");
    if (!f)
        return Q2_ERR_IO;
    if (fwrite(file, 1, SAVE_FILE_HEADER + body_size, f) !=
        SAVE_FILE_HEADER + body_size) {
        fclose(f);
        return Q2_ERR_IO;
    }
    fclose(f);
    return Q2_OK;
}

q2_result q2_settings_slot_read(q2_settings_blob *out, int slot)
{
    u8 *data = NULL;
    size_t size = 0;
    u32 version, body_size, crc, count, i;
    char path[512];
    q2_result rc;

    if (!out)
        return Q2_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    rc = q2_settings_slot_path(slot, path, (u32)sizeof(path));
    if (rc != Q2_OK)
        return rc;
    rc = slurp(path, &data, &size);
    if (rc != Q2_OK)
        return rc;

    if (size < SAVE_FILE_HEADER || memcmp(data, SETTINGS_MAGIC, 4) != 0) {
        rc = Q2_ERR_BAD_FORMAT;
        goto done;
    }
    version   = q2_rd_u32(data + 4);
    body_size = q2_rd_u32(data + 8);
    crc       = q2_rd_u32(data + 12);
    if (version != SETTINGS_VERSION) {
        rc = Q2_ERR_UNSUPPORTED;
        goto done;
    }
    if (body_size > size - SAVE_FILE_HEADER || body_size < 4 ||
        crc32_of(data + SAVE_FILE_HEADER, body_size) != crc) {
        rc = Q2_ERR_BAD_FORMAT;
        goto done;
    }

    count = q2_rd_u32(data + SAVE_FILE_HEADER);
    if (count > Q2_SETTINGS_VALUE_MAX || body_size != 4 + count * 2) {
        rc = Q2_ERR_BAD_FORMAT;
        goto done;
    }
    out->count = count;
    for (i = 0; i < count; i++)
        out->value[i] = (s16)q2_rd_u16(data + SAVE_FILE_HEADER + 4 + i * 2);
    rc = Q2_OK;

done:
    free(data);
    if (rc != Q2_OK)
        memset(out, 0, sizeof(*out));
    return rc;
}

q2_result q2_settings_slot_delete(int slot)
{
    char path[512];
    q2_result rc = q2_settings_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return (remove(path) == 0) ? Q2_OK : Q2_ERR_NOT_FOUND;
}

u32 q2_settings_slots_scan(bool *used, u32 count)
{
    u32 i, found = 0;

    if (!used)
        return 0;
    if (count > Q2_SAVE_SLOTS)
        count = Q2_SAVE_SLOTS;
    for (i = 0; i < count; i++) {
        q2_settings_blob probe;
        used[i] = (q2_settings_slot_read(&probe, (int)i) == Q2_OK);
        if (used[i])
            found++;
    }
    return found;
}

const char *q2_save_default_label(const q2_save *s, char *out, u32 out_size)
{
    s32 seconds, minutes;

    if (!out || out_size == 0)
        return "";
    if (!s) {
        out[0] = '\0';
        return out;
    }

    /* 300 ticks to the second — the level clock every duration in the game is
     * expressed in (sim.h). */
    seconds = s->level_time / 300;
    minutes = seconds / 60;
    seconds %= 60;

    snprintf(out, out_size, "%s %d  %d:%02d",
             s->map[0] ? s->map : "?", (int)s->zone,
             (int)minutes, (int)seconds);
    return out;
}
