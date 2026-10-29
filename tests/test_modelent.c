/*
 * test_modelent.c — model entities: the spawn at 0x8005A778 and the think at
 * 0x8005A5F8.
 *
 * These pin the parts a plausible version gets wrong. All of them are branches
 * or shifts rather than table values, so none of them can be checked by
 * decoding a corpus:
 *
 *   - the clock advances at TWICE the item rate (`sll v1, v1, 1`, 0x8005A618);
 *   - it does NOT wrap against the clip length the way an item's does — it runs
 *     past the end and the entity dies instead;
 *   - the lifetime is clip_length x 10, not clip_length (0x8005A630);
 *   - the light ramp holds at the ceiling before it falls, because the clamp is
 *     applied to `25 * (320 - t)` and that starts at 8000;
 *   - `fade` defaults to Q2_ONE_12 and not to zero, because the lighting setup
 *     MULTIPLIES it by `scale` (0x8006B298) and seeds both with 4096.
 *
 * The model bank is a real one only in `q2psx-inspect modelents`; here the
 * entity is driven directly so the file needs no disc.
 *
 * THE GIBS (0x8007CEB4 and what it calls) are the second half of the file.
 * Their expected values are written out from the disassembly in this file —
 * the jitter, both throws and the draw order — rather than obtained from the
 * functions under test, and every throw is checked against a REPLAY of the
 * shared generator, so a draw added, dropped or reordered anywhere moves the
 * final state and fails. The collision half runs on a one-cell hull built in
 * the on-disc layout, the way test_coll.c builds its three.
 */
#include <stdio.h>
#include <string.h>

#include "collision.h"
#include "effect.h"
#include "entity.h"
#include "item.h"         /* q2_item_shrink_think — 0x8005B358 */
#include "level.h"
#include "modelent.h"
#include "monster.h"
#include "playerdeath.h"
#include "shock.h"
#include "trace.h"        /* q2_move_world — the door the gib meets */
#include "trig.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
static void test_names(void)
{
    puts("the two names, in the order the fourth argument selects them");

    check(strcmp(q2_model_ent_name(Q2_MODEL_ENT_EXPLOSION),
                 "Explosion") == 0, "kind 0 is Explosion (0x800ACDF4)");
    check(strcmp(q2_model_ent_name(Q2_MODEL_ENT_HEXPLOSION),
                 "Hexplosion") == 0, "kind 1 is Hexplosion (0x800ACE00)");
    check(q2_model_ent_name(Q2_MODEL_ENT_KIND_COUNT) == NULL,
          "there is no third");
}

static void test_lifetime(void)
{
    puts("the lifetime is the clip length TIMES TEN (0x8005A630)");

    check_eq(q2_model_ent_lifetime(40), 400,
             "BASE0's Explosion is 40 frames and lives 400 units");
    check_eq(q2_model_ent_lifetime(1), 10, "one frame is ten units");
    check_eq(q2_model_ent_lifetime(0), 0,
             "a model with no clip dies on its first think");
}

static void test_scale_ramp(void)
{
    puts("the light ramp holds full and then falls to nothing (0x8005A65C)");

    /* 25 * (320 - 0) == 8000, clamped to the ceiling. */
    check_eq(q2_model_ent_scale(0), Q2_ONE_12, "full size at t = 0");
    check_eq(q2_model_ent_scale(100), Q2_ONE_12, "still full at t = 100");

    /* It leaves the ceiling when 25 * (320 - t) < 4096, i.e. t > 156.16. */
    check_eq(q2_model_ent_scale(156), Q2_ONE_12, "still full at t = 156");
    check(q2_model_ent_scale(157) < Q2_ONE_12, "and shrinking by t = 157");
    check_eq(q2_model_ent_scale(157), 25 * (320 - 157), "linear once it goes");

    check_eq(q2_model_ent_scale(320), 0, "nothing left at t = 320");
    check_eq(q2_model_ent_scale(400), 0,
             "and the clamp holds it there for the rest of the life");
}

static void test_flash_ramp(void)
{
    puts("the dynamic light's radius ramp is a different multiplier (0x8005A69C)");

    /* 51 * (320 - t) clamped, then * 1300 >> 12. */
    check_eq(q2_model_ent_flash(0), (Q2_ONE_12 * 1300) >> 12,
             "at the ceiling it is 1300");
    check(q2_model_ent_flash(300) < q2_model_ent_flash(280),
          "and it falls as the clock runs");
    check_eq(q2_model_ent_flash(320), 0, "to nothing at t = 320");

    /* The two ramps are NOT the same curve: 51 vs 25 means the light leaves the
     * ceiling later than the model does. */
    check(q2_model_ent_scale(200) < Q2_ONE_12,
          "the model is shrinking at t = 200");
    check_eq(q2_model_ent_flash(200), (Q2_ONE_12 * 1300) >> 12,
             "while the light is still at full radius");
}

static void test_think_emits_retail_light(void)
{
    q2_entity e;
    q2_entity_world w;
    const q2_ent_event *ev;

    puts("the think appends retail's orange-red dynamic light (0x80075C34)");

    memset(&w, 0, sizeof(w));
    w.dt = 6;

    q2_entity_init(&e);
    e.in_use = true;
    e.clip_length = 40;
    e.origin[0] = 101;
    e.origin[1] = 202;
    e.origin[2] = 303;

    q2_model_ent_think(&e, &w);

    check_eq(w.events.count, 1, "one runtime light is raised per live tick");
    ev = &w.events.e[0];
    check_eq(ev->kind, Q2_ENT_EVENT_LIGHT, "the event is a dynamic light");
    check_eq(ev->pos[0], 101, "light x is the model entity origin");
    check_eq(ev->pos[1], 202, "light y is the model entity origin");
    check_eq(ev->pos[2], 303, "light z is the model entity origin");
    check_eq(ev->glow[0], 0xC0, "red comes from 0x800AEAD4");
    check_eq(ev->glow[1], 0x40, "green comes from 0x800AEAD5");
    check_eq(ev->glow[2], 0x31, "blue comes from 0x800AEAD6");
    check_eq(ev->radius, 1300, "the full outer radius is 1300");
    check_eq(ev->inner_radius, 975, "the inner radius is three quarters");
}

static void test_allocator_and_explosion_ambient(void)
{
    q2_entity e;

    puts("allocator and explosion model retain retail's 0x40 ambient floor");

    q2_entity_init(&e);
    check_eq(e.glow[0], Q2_MODEL_ENT_AMBIENT,
             "allocator ambient red is 0x40 (0x800AEB84)");
    check_eq(e.glow[1], Q2_MODEL_ENT_AMBIENT,
             "allocator ambient green is 0x40");
    check_eq(e.glow[2], Q2_MODEL_ENT_AMBIENT,
             "allocator ambient blue is 0x40");
}

/* ------------------------------------------------------------------------- */
static void test_think_clock(void)
{
    q2_entity e;
    q2_entity_world w;

    puts("the clock runs at twice an item's and does not wrap");

    memset(&w, 0, sizeof(w));
    w.dt = 6;

    q2_entity_init(&e);
    e.in_use      = true;
    e.think       = q2_model_ent_think;
    e.clip_length = 40;          /* -> a 400-unit life */

    check_eq(e.fade, Q2_ONE_12,
             "a fresh entity starts at full second intensity (0x8006C1B8)");

    q2_model_ent_think(&e, &w);
    check_eq(e.frame, 12, "one think of dt 6 advances the clock by 12, not 6");

    q2_model_ent_think(&e, &w);
    check_eq(e.frame, 24, "and again");

    /* Run it to just under the end. The clock must NOT wrap at 40. */
    while (e.frame < 380 && e.in_use)
        q2_model_ent_think(&e, &w);

    check(e.in_use, "still alive at t = 380");
    check(e.frame >= 380,
          "the clock ran past the clip length instead of wrapping");
    check_eq(e.fade, 0, "and its lighting has faded to nothing");

    /* And the next few finish it. */
    while (e.in_use && e.frame < 500)
        q2_model_ent_think(&e, &w);

    check(!e.in_use, "removed once the clock passes clip_length * 10");
}

static void test_think_removes_unanimated(void)
{
    q2_entity e;
    q2_entity_world w;

    puts("a model the bank does not animate dies on its first think");

    memset(&w, 0, sizeof(w));
    w.dt = 6;

    q2_entity_init(&e);
    e.in_use      = true;
    e.think       = q2_model_ent_think;
    e.clip_length = 0;

    q2_model_ent_think(&e, &w);
    check(!e.in_use, "clip_length 0 gives a lifetime of 0, so it goes");
}

/* SetShockData, the spawn's last call: counted, so a spawn that never happens
 * can be shown to shake nothing. */
static int g_shocks;

static void count_shock(void *u, int player, int id, s32 strength,
                        const s32 pos[3], s16 near_r, s16 far_r)
{
    (void)u; (void)player; (void)id; (void)strength; (void)pos;
    (void)near_r; (void)far_r;
    g_shocks++;
}

static void test_spawn_needs_a_bank(void)
{
    q2_entity_set set;
    q2_model_ent_sounds sounds;
    s32 at[3] = { 100, 200, 300 };

    puts("a spawn with no model is no entity at all (0x8005A894)");

    memset(&set, 0, sizeof(set));

    g_shocks = 0;
    q2_model_ent_sounds_clear(&sounds);
    q2_shock_attach(count_shock, NULL);
    check(q2_model_ent_spawn(&set, NULL, Q2_MODEL_ENT_EXPLOSION, at, 0,
                             Q2_MODEL_ENT_SIZE_FULL, &sounds) == NULL,
          "no bank, no entity");
    q2_shock_attach(NULL, NULL);
    check_eq(set.count, 0, "and nothing was taken from the pool");
    check_eq(g_shocks, 0,
             "and no pad shakes: 0x8005AA7C is past 0x8005A894's bail");
    check_eq(sounds.count, 0,
             "and no bang: 0x8005AA54 is past the same bail");

    q2_entity_set_free(&set);
}

/*
 * A ONE-MODEL CASTLIST holding a bodiless `Explosion`: the 64-byte header and
 * nothing behind it, which satisfies every size identity q2_model_get checks
 * with zero verts, parts and faces. Enough for the bind at 0x8005A894 to
 * succeed, which is all the bang waits on.
 */
static u8 g_boom[Q2_MODEL_HEADER_SIZE];

static void boom_wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void boom_wr32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void boom_bank(q2_model_bank *bank)
{
    memset(g_boom, 0, sizeof(g_boom));
    boom_wr16(g_boom + 0x02, 40);                     /* lh(model + 2)      */
    memcpy(g_boom + 0x08, "Explosion", 9);
    boom_wr16(g_boom + 0x1C, 300);                    /* ext2, the height   */
    boom_wr32(g_boom + 0x20, Q2_MODEL_HEADER_SIZE);   /* faces              */
    boom_wr32(g_boom + 0x24, Q2_MODEL_HEADER_SIZE);   /* verts              */
    boom_wr32(g_boom + 0x28, Q2_MODEL_HEADER_SIZE);   /* parts              */

    bank->data  = g_boom;
    bank->size  = sizeof(g_boom);
    bank->count = 1;
}

/* What the queue held when SetShockData ran, so the ORDER of the spawn's last
 * two calls is checked and not just that both happened. */
static const q2_model_ent_sounds *g_watch;
static s64 g_queued_at_shock;

static void order_shock(void *u, int player, int id, s32 strength,
                        const s32 pos[3], s16 near_r, s16 far_r)
{
    count_shock(u, player, id, strength, pos, near_r, far_r);
    g_queued_at_shock = g_watch ? (s64)g_watch->count : -1;
}

static void test_spawn_bangs(void)
{
    q2_entity_set set;
    q2_model_bank bank;
    q2_model_ent_sounds q;
    s32 at[3] = { -1200, 340, 5600 };
    s32 out[3] = { 0, 0, 0 };
    q2_entity *e;

    puts("a spawn that binds bangs where it is, before it shakes (0x8005AA54)");

    memset(&set, 0, sizeof(set));
    boom_bank(&bank);
    q2_model_ent_sounds_clear(&q);

    g_shocks          = 0;
    g_watch           = &q;
    g_queued_at_shock = -1;
    q2_shock_attach(order_shock, NULL);

    e = q2_model_ent_spawn(&set, &bank, Q2_MODEL_ENT_EXPLOSION, at, 0,
                           Q2_MODEL_ENT_SIZE_FULL, &q);
    check(e != NULL, "a bank that carries `Explosion` binds it");
    check_eq(q.count, 1, "and one Sound_Explosion is queued");
    check_eq(g_queued_at_shock, 1,
             "before SetShockData: 0x8005AA54 precedes 0x8005AA7C");
    check(q2_model_ent_sound_next(&q, out) &&
          out[0] == at[0] && out[1] == at[1] && out[2] == at[2],
          "at the point the spawn was handed, which is ent+0x54");
    check(!q2_model_ent_sound_next(&q, out), "and only the one");

    check(q2_model_ent_spawn(&set, &bank, Q2_MODEL_ENT_HEXPLOSION, at, 0,
                             Q2_MODEL_ENT_SIZE_FULL, &q) == NULL,
          "kind 1 binds nothing: no bank carries `Hexplosion`");
    check_eq(q.count, 0, "so it makes no sound either");

    check(q2_model_ent_spawn(&set, &bank, Q2_MODEL_ENT_EXPLOSION, at, 0,
                             Q2_MODEL_ENT_SIZE_GIB, NULL) != NULL,
          "a caller with no queue still gets its entity");
    check_eq(g_shocks, 2, "and both binds shook the pads");

    q2_shock_attach(NULL, NULL);
    g_watch = NULL;
    q2_entity_set_free(&set);
}

static void test_bang_queue(void)
{
    q2_entity_set set;
    q2_model_bank bank;
    q2_model_ent_sounds q;
    s32 at[3], out[3];
    s32 i;
    bool in_order = true;

    puts("the bang queue: oldest first, 24 deep (ProcessSounds' voices)");

    memset(&set, 0, sizeof(set));
    boom_bank(&bank);
    q2_model_ent_sounds_clear(&q);

    for (i = 0; i < Q2_MODEL_ENT_SOUNDS_MAX + 1; i++) {
        at[0] = i * 100;
        at[1] = -i;
        at[2] = 7;
        (void)q2_model_ent_spawn(&set, &bank, Q2_MODEL_ENT_EXPLOSION, at, 0,
                                 Q2_MODEL_ENT_SIZE_FULL, &q);
    }
    check_eq(set.count, Q2_MODEL_ENT_SOUNDS_MAX + 1,
             "every spawn is an entity: the queue never refuses one");
    check_eq(q.count, Q2_MODEL_ENT_SOUNDS_MAX, "the queue holds twenty-four");
    check_eq(q.dropped, 1, "and counts the twenty-fifth it dropped");

    for (i = 0; i < Q2_MODEL_ENT_SOUNDS_MAX; i++)
        if (!q2_model_ent_sound_next(&q, out) || out[0] != i * 100 ||
            out[1] != -i || out[2] != 7)
            in_order = false;
    check(in_order, "drained in the order the spawns happened");
    check(!q2_model_ent_sound_next(&q, NULL), "and then empty");

    q2_model_ent_sounds_clear(&q);
    check_eq(q.dropped, 0, "clearing forgets the drop count too");

    q2_entity_set_free(&set);
}

/* ========================================================================= */
/* GIBS                                                                      */
/* ========================================================================= */

/* Ramps only: the trail and the spray need 2 and 3 to resolve, nothing else. */
static q2_fx_tables g_tab;

static void build_tables(void)
{
    u32 i, c;

    memset(&g_tab, 0, sizeof(g_tab));
    g_tab.loaded = true;
    for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
        g_tab.ramp_id_to_index[i] = (u8)i;
        for (c = 0; c < Q2_FX_RAMP_COLOURS; c++)
            g_tab.ramp[i].colour[c] = (u32)c | ((u32)i << 8);
    }
    g_tab.ramp_index_is_permutation = true;
}

/* A test's whole gib world: a fresh set, a particle pool with the tables, a
 * seeded generator — and attached, which is what the two sites reach. */
typedef struct gib_fixture {
    q2_entity_set   set;
    q2_fx_world     fx;
    q2_rng          rng;
    q2_entity_world ew;
    q2_gib_world    gw;
} gib_fixture;

static void fixture_open(gib_fixture *f, u32 seed, q2_collision *coll)
{
    memset(f, 0, sizeof(*f));
    q2_fx_world_init(&f->fx, &g_tab);
    q2_rng_seed(&f->rng, seed);
    f->ew.dt     = 6;
    f->gw.set    = &f->set;
    f->gw.bank   = NULL;
    f->gw.fx     = &f->fx;
    f->gw.rng    = &f->rng;
    f->gw.coll   = coll;
    q2_gib_attach(&f->gw);
}

static void fixture_close(gib_fixture *f)
{
    q2_gib_attach(NULL);
    q2_entity_set_free(&f->set);
}

static u32 live_groups(const q2_fx_world *fx)
{
    u32 i, n = 0;

    for (i = 0; i < fx->group_count; i++)
        if (fx->group[i].life)
            n++;
    return n;
}

/*
 * The throw and the jitter, written out from the disassembly and NOT through
 * the functions under test. C's `/` truncates toward zero, which is what each
 * `bgez; addiu 2^n - 1; sra n` in the original computes — so a version that
 * floors a negative product instead fails against these.
 */
static s32 expect_jitter(s32 r)
{
    return (286 * (r - 16384)) / 16384;               /* 0x8005A458..0x8005A480 */
}

static void expect_meat_vel(s32 yaw, s32 r1, s32 r2, const s16 imp[3],
                            s16 out[3])
{
    s32 speed = 256 + (768 * r1) / 32768;             /* 0x8005A218..0x8005A24C */

    out[0] = (s16)(imp[0] + (q2_cos12(yaw & 0xFFF) * speed) / 4096);
    out[1] = (s16)(imp[1] - 3072 + (-1536 * r2) / 32768);
    out[2] = (s16)(imp[2] + (q2_sin12(yaw & 0xFFF) * speed) / 4096);
}

static void expect_boss_vel(s32 yaw, s32 r1, s32 r2, s16 out[3])
{
    s32 speed = 768 + r1 / 64;                         /* 0x8005AEF8..0x8005AF20 */

    out[0] = (s16)((q2_cos12(yaw & 0xFFF) * speed) / 4096);
    out[1] = (s16)((-2000 * r2) / 32768 - 3000);
    out[2] = (s16)((q2_sin12(yaw & 0xFFF) * speed) / 4096);
}

typedef struct want_chunk {
    s32 yaw;
    s32 pos[3];
    s16 vel[3];
    s32 angles[3];
    s32 spin;
    s32 life;
} want_chunk;

/* One 0x8005A0AC's six draws, in its order, from `r`. */
static void replay_meat_chunk(q2_rng *r, s32 yaw, const s16 imp[3],
                              want_chunk *w)
{
    s32 r1 = q2_rng_next(r), r2 = q2_rng_next(r);

    w->yaw = yaw;
    expect_meat_vel(yaw, r1, r2, imp, w->vel);
    w->angles[0] = q2_rng_next(r);
    w->angles[1] = q2_rng_next(r);
    w->angles[2] = q2_rng_next(r);
    w->spin = (q2_rng_next(r) - 16384) >> 6;
}

/* One 0x8005AD8C's seven. */
static void replay_boss_chunk(q2_rng *r, s32 yaw, s32 base, want_chunk *w)
{
    s32 r1 = q2_rng_next(r), r2 = q2_rng_next(r);

    w->yaw = yaw;
    expect_boss_vel(yaw, r1, r2, w->vel);
    w->angles[0] = q2_rng_next(r);
    w->angles[1] = q2_rng_next(r);
    w->angles[2] = q2_rng_next(r);
    w->spin = (q2_rng_next(r) - 16384) >> 6;
    w->life = base + q2_rng_next(r) / 32;
}

/*
 * ThrowGibs, replayed: the start yaw, then 0x8005B320's forty-five, then per
 * chunk three jitter draws and the chunk's six; the head's six last. Returns
 * the generator as the original would leave it.
 */
static q2_rng replay_throw_meat(u32 seed, const q2_gib_parent *p, s32 count,
                                const s16 imp[3], bool head, want_chunk *out)
{
    q2_rng r;
    s32 start, step, i, k;

    q2_rng_seed(&r, seed);
    start = q2_rng_next(&r);
    step  = 4096 / count;
    for (i = 0; i < 45; i++)
        (void)q2_rng_next(&r);

    for (i = 0; i < count; i++) {
        for (k = 0; k < 3; k++)
            out[i].pos[k] = p->pos[k] + expect_jitter(q2_rng_next(&r));
        replay_meat_chunk(&r, start + i * step, imp, &out[i]);
        out[i].life = 512;
    }
    if (head) {
        for (k = 0; k < 3; k++)
            out[count].pos[k] = p->origin[k];
        replay_meat_chunk(&r, start + count * step, imp, &out[count]);
        out[count].life = 2560;
    }
    return r;
}

static void check_chunk(const q2_entity *e, const want_chunk *w,
                        const char *name, const u8 glow[3], const char *what)
{
    const q2_gib_body *b = q2_gib_body_of(e);
    char line[160];
    int k;

    snprintf(line, sizeof(line), "%s: a live gib with a body", what);
    check(e->in_use && e->think == q2_gib_think && b != NULL, line);
    if (!b)
        return;

    for (k = 0; k < 3; k++) {
        snprintf(line, sizeof(line), "%s: pos[%d] (+0x54) as replayed", what, k);
        check_eq(e->pos[k], w->pos[k], line);
        snprintf(line, sizeof(line), "%s: origin[%d] (+0xA4) is the same point",
                 what, k);
        check_eq(e->origin[k], w->pos[k], line);
        snprintf(line, sizeof(line),
                 "%s: vel[%d] (+0xE0) thrown at yaw %d", what, k, (int)w->yaw);
        check_eq(b->vel[k], w->vel[k], line);
        snprintf(line, sizeof(line), "%s: angle[%d] (+0xE6) is a raw draw",
                 what, k);
        check_eq(e->angles[k], w->angles[k], line);
        snprintf(line, sizeof(line), "%s: glow[%d] is the parent's, not 0x40",
                 what, k);
        check_eq(e->glow[k], glow[k], line);
    }
    snprintf(line, sizeof(line), "%s: +0x44 is (rand() - 16384) >> 6", what);
    check_eq(b->spin, w->spin, line);
    snprintf(line, sizeof(line), "%s: +0xF4 lifetime", what);
    check_eq(e->remove_in, w->life, line);
    snprintf(line, sizeof(line), "%s: named %s", what, name);
    check(strcmp(e->model, name) == 0, line);
    snprintf(line, sizeof(line), "%s: +0x10C is 0x01800000 (lui v0, 0x180)", what);
    check_eq(e->render_flags, 0x01800000u, line);
    snprintf(line, sizeof(line), "%s: +0xA2 is -1 (0x8005A360)", what);
    check_eq(e->node, -1, line);
    snprintf(line, sizeof(line), "%s: a +/-20 box (0x800AEACC)", what);
    check(e->bounds_min[0] == -20 && e->bounds_max[1] == 20 &&
          e->bounds_min[2] == -20, line);
}

/* ------------------------------------------------------------------------- */
static void test_gib_arm_table(void)
{
    static const struct { u16 cls; q2_gib_arm arm; const char *what; } k[] = {
        {  2, Q2_GIB_ARM_BOS2,  "class 2 Boss2 -> explosion + Bos2Gib"     },
        {  8, Q2_GIB_ARM_BOS2,  "class 8 Flyer -> the same arm"            },
        { 11, Q2_GIB_ARM_BOS2,  "class 11 Hover -> the same arm"           },
        { 13, Q2_GIB_ARM_JORG,  "class 13 Jorg -> JorgGib"                 },
        { 14, Q2_GIB_ARM_NONE,  "class 14 Rider -> nothing but the free"   },
        { 17, Q2_GIB_ARM_MEAT,  "class 17 Parasite -> meat, no head"       },
        { 18, Q2_GIB_ARM_CHEST, "class 18 Soldier -> the default"          },
        { 24, Q2_GIB_ARM_BOS1,  "class 24 Boss1 -> Bos1Gib"                },
        { 30, Q2_GIB_ARM_CHEST, "class 30 is word 28: a Chest arm"         },
        { 32, Q2_GIB_ARM_STRID, "class 32 Strider is word 30 (0x800AD6C0)" },
        { 36, Q2_GIB_ARM_CHEST, "class 36 Rider3 is word 34: a Chest arm"  },
        { 37, Q2_GIB_ARM_NONE,  "class 37 RiderStand is word 35: the free" },
        {  0, Q2_GIB_ARM_CHEST, "class 0 wraps past sltiu 36: default"    },
        {  1, Q2_GIB_ARM_CHEST, "class 1 Berserk, likewise"                },
        { 38, Q2_GIB_ARM_CHEST, "class 38 is past the table"               },
        { 39, Q2_GIB_ARM_CHEST, "the player's 39 (0x8003B2B0): default"    },
        { 41, Q2_GIB_ARM_CHEST, "a player skin row 41: default"            },
    };
    u32 i;

    puts("\nthe dispatcher's 36 words at 0x800AD648, indexed class - 2");

    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        check_eq(q2_gib_arm_for_class(k[i].cls, 0), k[i].arm, k[i].what);

    check_eq(q2_gib_arm_for_class(Q2_GIB_CORPSE_CLASS, 13), Q2_GIB_ARM_JORG,
             "a corpse (47) is taken apart as its +0xDA (0x8007CED8)");
    check_eq(q2_gib_arm_for_class(Q2_GIB_CORPSE_CLASS, 17), Q2_GIB_ARM_MEAT,
             "whatever that was");
    check_eq(q2_gib_arm_for_class(18, 13), Q2_GIB_ARM_CHEST,
             "and +0xDA is read ONLY when +0xD2 is 47");
}

static void test_gib_throw_arithmetic(void)
{
    static const s16 zero[3] = { 0, 0, 0 };
    static const s16 imp[3]  = { 100, -50, 25 };
    s16 got[3], again[3], want[3];
    u32 mismatches = 0;
    s32 yaw, r;

    puts("\nthe two throws (0x8005A218.. and 0x8005AEF0..)");

    q2_gib_throw_velocity(Q2_GIB_STYLE_MEAT, 0, 32767, 32767, zero, got);
    check_eq(got[0], 1023, "meat: the fastest draw is 256 + 767 along cos(0)");
    check_eq(got[1], -4607, "meat: the hardest toss is -3072 - 1535, UP");
    check_eq(got[2], 0, "meat: and nothing along sin(0)");

    q2_gib_throw_velocity(Q2_GIB_STYLE_MEAT, 0, 0, 0, zero, got);
    check_eq(got[0], 256, "meat: the slowest is 256");
    check_eq(got[1], -3072, "meat: the gentlest toss is -3072");

    q2_gib_throw_velocity(Q2_GIB_STYLE_MEAT, 1024, 32767, 0, imp, got);
    check_eq(got[0], 100, "meat: a quarter turn puts nothing on x but the impulse");
    check_eq(got[1], -3072 - 50, "meat: the impulse's y rides on the toss");
    check_eq(got[2], 1023 + 25, "meat: and the speed lands on z (sin)");

    q2_gib_throw_velocity(Q2_GIB_STYLE_BOSS, 0, 32767, 32767, zero, got);
    check_eq(got[0], 1279, "boss: 768 + (32767 >> 6)");
    check_eq(got[1], -4999, "boss: -3000 - 1999");
    q2_gib_throw_velocity(Q2_GIB_STYLE_BOSS, 0, 0, 0, imp, got);
    check_eq(got[0], 768, "boss: the slowest is 768");
    check_eq(got[1], -3000, "boss: and the gentlest toss -3000");
    q2_gib_throw_velocity(Q2_GIB_STYLE_BOSS, 777, 12345, 23456, imp, got);
    q2_gib_throw_velocity(Q2_GIB_STYLE_BOSS, 777, 12345, 23456, zero, again);
    check(memcmp(got, again, sizeof(got)) == 0,
          "boss: the impulse changes nothing (a2 dies at 0x8005ADF4)");

    /* Every heading, a spread of draws: the round toward zero on both /4096
     * and the /32768, against C's own truncating divide. */
    for (yaw = 0; yaw < 4096; yaw += 3) {
        for (r = 0; r < 32768; r += 4681) {
            q2_gib_throw_velocity(Q2_GIB_STYLE_MEAT, yaw, r, 32767 - r, imp,
                                  got);
            expect_meat_vel(yaw, r, 32767 - r, imp, want);
            if (memcmp(got, want, sizeof(got)) != 0)
                mismatches++;
            q2_gib_throw_velocity(Q2_GIB_STYLE_BOSS, yaw, r, 32767 - r, imp,
                                  got);
            expect_boss_vel(yaw, r, 32767 - r, want);
            if (memcmp(got, want, sizeof(got)) != 0)
                mismatches++;
        }
    }
    check_eq(mismatches, 0,
             "every heading rounds toward zero, as bgez/addiu/sra does");
}

static void test_gib_throw_meat_five_and_head(void)
{
    gib_fixture f;
    q2_gib_parent p;
    want_chunk want[6];
    q2_rng end;
    s16 imp[3];
    u32 made, i;
    int k;

    puts("\nThrowGibs (0x8005A3D4): five 'Gib meat' on a ring, then the head");

    fixture_open(&f, 0x1234u, NULL);
    q2_gib_parent_init(&p);
    p.placed = true;
    p.pos[0] = 1000;  p.pos[1] = 2000;  p.pos[2] = 3000;
    p.origin[0] = 1000; p.origin[1] = 1714; p.origin[2] = 3000;
    p.glow[0] = 10; p.glow[1] = 20; p.glow[2] = 30;
    p.area = 7;
    imp[0] = 105; imp[1] = -45; imp[2] = 30;

    made = q2_gib_throw_meat(&f.gw, &p, 5, imp, "Chest");
    end  = replay_throw_meat(0x1234u, &p, 5, imp, true, want);

    check_eq(made, 6, "five chunks and the head");
    check_eq(f.set.count, 6, "six entities out of the pool");
    check_eq(f.rng.state, end.state,
             "the generator ends where 1 + 45 + 5 * (3 + 6) + 6 draws leave it");

    for (i = 0; i < 5; i++) {
        char what[64];

        snprintf(what, sizeof(what), "chunk %u (start + %u * 819)", i, i);
        check_chunk(&f.set.ent[i], &want[i], "Gib meat", p.glow, what);
        for (k = 0; k < 3; k++)
            check(f.set.ent[i].pos[k] - p.pos[k] >= -286 &&
                  f.set.ent[i].pos[k] - p.pos[k] <= 285,
                  "and inside the jitter's -286..285 of +0x54");
        check_eq(f.set.ent[i].kind, Q2_ENT_KIND_MODEL,
                 "0x8005A0AC writes no class");
    }
    check_chunk(&f.set.ent[5], &want[5], "Chest", p.glow,
                "the head (start + 5 * 819)");
    check(memcmp(f.set.ent[5].pos, p.origin, sizeof(p.origin)) == 0,
          "the head leaves from +0xA4, unjittered, not from +0x54");

    check_eq(live_groups(&f.fx), 0,
             "the spray's 45 draws were made with no mesh, and drew nothing");
    check(f.set.ent[0].model_index < 0,
          "with no bank the name binds nothing, and the chunk exists anyway "
          "(0x8005A0AC stores +0x10 untested; 0x8005A778 would have bailed)");

    fixture_close(&f);
}

static void test_gib_throw_meat_spacing(void)
{
    static const s32 counts[] = { 1, 3, 7 };
    u32 c;

    puts("\nthe ring's step is 4096 / count for any count");

    for (c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        gib_fixture f;
        q2_gib_parent p;
        want_chunk want[8];
        static const s16 zero[3] = { 0, 0, 0 };
        q2_rng end;
        s32 i, n = counts[c];
        char what[96];

        fixture_open(&f, 0xBEEFu + (u32)n, NULL);
        q2_gib_parent_init(&p);
        p.placed = true;

        check_eq(q2_gib_throw_meat(&f.gw, &p, n, zero, NULL), (s64)n,
                 "a NULL head name throws exactly count chunks");
        end = replay_throw_meat(0xBEEFu + (u32)n, &p, n, zero, false, want);
        snprintf(what, sizeof(what), "count %d: generator as replayed", (int)n);
        check_eq(f.rng.state, end.state, what);

        for (i = 0; i < n; i++) {
            const q2_gib_body *b = q2_gib_body_of(&f.set.ent[i]);

            snprintf(what, sizeof(what), "count %d chunk %d at start + %d * %d",
                     (int)n, (int)i, (int)i, (int)(4096 / n));
            check(b && memcmp(b->vel, want[i].vel, sizeof(want[i].vel)) == 0,
                  what);
        }
        fixture_close(&f);
    }
}

static void test_gib_think_runs_the_trail(void)
{
    gib_fixture f;
    q2_gib_parent p;
    static const s16 zero[3] = { 0, 0, 0 };
    u32 i, bad_offsets = 0;

    puts("\nevery chunk's think (0x80059DE0) lays the blood trail");

    fixture_open(&f, 0x5150u, NULL);
    q2_gib_parent_init(&p);
    p.placed = true;
    p.pos[1] = -500;
    (void)q2_gib_throw_meat(&f.gw, &p, 5, zero, "Chest");

    for (i = 0; i < f.set.count; i++) {
        q2_entity *e = &f.set.ent[i];
        q2_gib_body *b = q2_gib_body_of(e);
        q2_rng expect = f.rng;
        s32 before[3], step[3];
        const q2_fx_group *g = NULL;
        u32 j, k;
        char what[96];

        q2_fx_world_clear(&f.fx);
        for (k = 0; k < 3; k++)
            before[k] = e->pos[k];

        q2_gib_think(e, &f.ew);

        for (j = 0; j < 48; j++)
            (void)q2_rng_next(&expect);
        snprintf(what, sizeof(what),
                 "chunk %u: three tumble draws and the trail's 45", i);
        check_eq(f.rng.state, expect.state, what);

        snprintf(what, sizeof(what), "chunk %u: exactly one trail group", i);
        check_eq(live_groups(&f.fx), 1, what);
        for (j = 0; j < f.fx.group_count; j++)
            if (f.fx.group[j].life)
                g = &f.fx.group[j];
        if (!g)
            continue;

        snprintf(what, sizeof(what), "chunk %u: on the blood ramps 2 and 3", i);
        check(g->ramp[0] == q2_fx_ramp_at(&g_tab, 2) &&
              g->ramp[1] == q2_fx_ramp_at(&g_tab, 3), what);
        snprintf(what, sizeof(what), "chunk %u: fifteen quads, life 3", i);
        check(g->count == 15 && g->life == 3, what);
        snprintf(what, sizeof(what),
                 "chunk %u: from +0xA4, just copied from the moved +0x54", i);
        check(memcmp(g->origin, e->origin, sizeof(g->origin)) == 0 &&
              memcmp(e->origin, e->pos, sizeof(e->pos)) == 0, what);

        /* It moved by vel * dt / 320 with gravity already on vel.y. */
        for (k = 0; k < 3; k++) {
            snprintf(what, sizeof(what), "chunk %u: moved vel[%u] * 6 / 320",
                     i, k);
            check_eq(e->pos[k] - before[k], ((s32)b->vel[k] * 6) / 320, what);
            step[k] = b->vel[k] / 15;
        }

        /* offs[i] = i * (vel / 15), stored >> 4 by the second spawner. */
        for (j = 1; j < 15; j++)
            for (k = 0; k < 3; k++)
                if (g->offset[j - 1][k] !=
                        (s16)((s16)(step[k] * (s32)j) >> 4))
                    bad_offsets++;
    }
    check_eq(bad_offsets, 0,
             "every trail is the gib's own line: offs[i] = i * vel/15, >> 4");

    fixture_close(&f);
}

static void test_gib_trail_offsets(void)
{
    gib_fixture f;
    q2_gib_parent p;
    q2_entity *e;
    q2_gib_body *b;
    const q2_fx_group *g = NULL;
    static const s16 zero[3] = { 0, 0, 0 };
    s32 pos[3] = { 0, 0, 0 };
    u32 j;

    puts("\na gib at (150, -300, 75) draws the line (10, -20, 5) per point");

    fixture_open(&f, 77u, NULL);
    q2_gib_parent_init(&p);
    p.placed = true;
    e = q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1, 512,
                         "Gib meat");
    check(e != NULL, "one chunk");
    if (!e) {
        fixture_close(&f);
        return;
    }
    b = q2_gib_body_of(e);
    b->vel[0] = 150; b->vel[1] = -300; b->vel[2] = 75;
    e->render_flags |= Q2_ITEM_TOSS_NO_GRAVITY;   /* hold vel.y still */

    q2_fx_world_clear(&f.fx);
    q2_gib_think(e, &f.ew);

    for (j = 0; j < f.fx.group_count; j++)
        if (f.fx.group[j].life)
            g = &f.fx.group[j];
    check(g != NULL, "the trail group exists");
    if (g) {
        check_eq(g->offset[0][0], 10 >> 4,   "offs[1].x = 10  >> 4");
        check_eq(g->offset[13][0], 140 >> 4, "offs[14].x = 140 >> 4 = 8");
        check_eq(g->offset[13][1], -280 >> 4,
                 "offs[14].y = -280 >> 4 = -18, an arithmetic shift");
        check_eq(g->offset[13][2], 70 >> 4,  "offs[14].z = 70 >> 4 = 4");
        check_eq(g->offset[7][1], -160 >> 4, "offs[8].y = -160 >> 4 = -10");
    }
    check_eq(e->pos[0], 2,  "moved 150 * 6 / 320 = 2 on x");
    check_eq(e->pos[1], -5, "-300 * 6 / 320 = -5 on y, truncated");
    check_eq(e->pos[2], 1,  "75 * 6 / 320 = 1 on z");

    /* Gravity is the sim's word, read live: 0x80046464 `lw v1, 804(gp)`. */
    {
        s32 word = 0;

        f.gw.gravity = &word;
        q2_gib_attach(&f.gw);
        e->render_flags &= ~Q2_ITEM_TOSS_NO_GRAVITY;
        b->vel[1] = -300;
        q2_gib_think(e, &f.ew);
        check_eq(b->vel[1], -300, "a zero gravity word leaves vel.y alone");
        word = 100;
        q2_gib_think(e, &f.ew);
        check_eq(b->vel[1], -300 + 100 * 6,
                 "and a changed word is felt on the next tick: +g * dt");
    }

    fixture_close(&f);
}

static void test_gib_rest_and_expiry(void)
{
    gib_fixture f;
    q2_gib_parent p;
    q2_entity *e;
    q2_gib_body *b;
    static const s16 zero[3] = { 0, 0, 0 };
    s32 pos[3] = { 10, 20, 30 };
    q2_rng before;
    s32 ticks;

    puts("\na gib at rest neither moves nor bleeds, and expiry hands it to "
         "0x8005B358");

    fixture_open(&f, 99u, NULL);
    q2_gib_parent_init(&p);
    p.placed = true;
    e = q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1, 512,
                         "Gib meat");
    check(e != NULL, "one chunk");
    if (!e) {
        fixture_close(&f);
        return;
    }
    b = q2_gib_body_of(e);
    b->move_flags |= Q2_GIB_MOVE_REST;

    q2_fx_world_clear(&f.fx);
    before = f.rng;
    q2_gib_think(e, &f.ew);
    check_eq(live_groups(&f.fx), 0, "+0x98 & 0x20 suppresses the trail");
    check_eq(f.rng.state, before.state,
             "and costs no draws: no tumble, no trail (0x80046BC4, 0x80059E94)");
    check(e->pos[0] == 10 && e->pos[1] == 20 && e->pos[2] == 30,
          "and it does not move");
    check_eq(e->remove_in, 512 - 6, "its clock still runs");

    /* Expiry: once dt reaches what is left. */
    e->remove_in = 6;
    q2_gib_think(e, &f.ew);
    check(e->think == q2_item_shrink_think,
          "at dt >= +0xF4 the think becomes 0x8005B358");
    check_eq(e->remove_in, 1, "and +0xF4 is dt + 1 - dt = 1");

    for (ticks = 0; ticks < 100 && e->in_use; ticks++)
        e->think(e, &f.ew);
    check_eq(ticks, 43, "4096 / (16 * 6) rounds up to 43 ticks of fading");
    check(!e->in_use, "and then it is freed");

    fixture_close(&f);
}

static void test_gib_boss_chunk(void)
{
    gib_fixture f, g2;
    q2_gib_parent p;
    q2_entity *e;
    const q2_gib_body *b;
    s16 v_with[3];
    static const s16 imp[3] = { 1000, 1000, 1000 };
    static const s16 zero[3] = { 0, 0, 0 };
    s32 pos[3] = { 0, 0, 0 };
    want_chunk want;
    q2_rng r;

    puts("\nthe boss twin (0x8005AD8C): class 48, a jittered life, no impulse");

    fixture_open(&f, 4242u, NULL);
    q2_gib_parent_init(&p);
    p.placed = true;
    e = q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_BOSS, 300, imp, pos, -1, 512,
                         "JorgGib3");
    b = e ? q2_gib_body_of(e) : NULL;
    check(e != NULL && b != NULL, "one chunk");
    if (!e || !b) {
        fixture_close(&f);
        return;
    }

    q2_rng_seed(&r, 4242u);
    replay_boss_chunk(&r, 300, 512, &want);
    check_eq(e->kind, Q2_GIB_BOSS_CLASS, "+0xD2 = 48 (0x8005ADD8)");
    check_eq(e->remove_in, want.life, "+0xF4 = 512 + (rand() >> 5)");
    check(e->remove_in >= 512 && e->remove_in <= 1535, "inside 512..1535");
    check(memcmp(b->vel, want.vel, sizeof(want.vel)) == 0,
          "the boss throw, 768 + (r >> 6) and -2000r / 32768 - 3000");
    check(b->vel[1] >= -4999 && b->vel[1] <= -3000, "tossed UP by 3000..4999");
    check_eq(f.rng.state, r.state, "seven draws, the last the life's");
    memcpy(v_with, b->vel, sizeof(v_with));
    fixture_close(&f);

    fixture_open(&g2, 4242u, NULL);
    e = q2_gib_spawn_one(&g2.gw, &p, Q2_GIB_STYLE_BOSS, 300, zero, pos, -1,
                         512, "JorgGib3");
    b = e ? q2_gib_body_of(e) : NULL;
    check(b && memcmp(b->vel, v_with, sizeof(v_with)) == 0,
          "and the same seed with no impulse throws the same chunk");
    fixture_close(&g2);
}

static void test_gib_jorg_ring(void)
{
    static const char *const names[16] = {
        "JorgGib2", "JorgGib3", "JorgGib4", "JorgGib5", "JorgGib6",
        "JorgGib7", "JorgGib8", "JorgGib9", "JorgGib:", "JorgGib2",
        "JorgGib3", "JorgGib4", "JorgGib5", "JorgGib6", "JorgGib7",
        "JorgGib8"
    };
    gib_fixture f;
    q2_gib_parent p;
    q2_gib_report rep;
    q2_rng r;
    s32 start, i, k;
    u32 made;

    puts("\nthe Jorg arm (0x8007D054 -> 0x8005B0AC): '1' + i from i = 1");

    fixture_open(&f, 0x7070u, NULL);
    q2_gib_parent_init(&p);
    p.placed = true;
    p.cls    = 13;
    p.pos[0] = -4000; p.pos[1] = 600; p.pos[2] = 250;
    p.velocity[0] = 900;      /* the impulse, which no boss chunk takes */

    made = q2_gib_destroy(&f.gw, &p, &rep);
    check_eq(rep.arm, Q2_GIB_ARM_JORG, "class 13 takes the Jorg arm");
    check_eq(made, 16, "sixteen chunks");
    check(!rep.explosion, "and no Explosion model on this arm");

    q2_rng_seed(&r, 0x7070u);
    start = q2_rng_next(&r);
    for (i = 0; i < 16 && (u32)i < f.set.count; i++) {
        const q2_entity *e = &f.set.ent[i];
        const q2_gib_body *b = q2_gib_body_of(e);
        want_chunk w;
        char what[96];
        s32 pt[3];

        for (k = 0; k < 3; k++)
            pt[k] = p.pos[k] + expect_jitter(q2_rng_next(&r));
        replay_boss_chunk(&r, start + i * 256, 512, &w);

        snprintf(what, sizeof(what), "piece %d is %s", (int)i, names[i]);
        check(strcmp(e->model, names[i]) == 0, what);
        snprintf(what, sizeof(what),
                 "piece %d thrown at start + %d * 256 (4096 / 16)", (int)i,
                 (int)i);
        check(b && memcmp(b->vel, w.vel, sizeof(w.vel)) == 0, what);
        snprintf(what, sizeof(what), "piece %d jittered off +0x54", (int)i);
        check(memcmp(e->pos, pt, sizeof(pt)) == 0, what);
        snprintf(what, sizeof(what), "piece %d is class 48", (int)i);
        check_eq(e->kind, Q2_GIB_BOSS_CLASS, what);
    }
    check_eq(f.rng.state, r.state,
             "1 + 16 * (3 + 7) draws: the ring has NO spray and NO head");
    check(strcmp(f.set.ent[8].model, "JorgGib:") == 0 &&
          f.set.ent[8].model_index < 0,
          "the ':' piece is a live entity that no bank can bind");

    fixture_close(&f);
}

static void test_gib_dispatch_arms(void)
{
    static const struct {
        u16 cls; u32 chunks; u32 draws; const char *what;
    } k[] = {
        { 18, 6,  1 + 45 + 5 * 9 + 6, "Soldier: five meat and a Chest"   },
        { 17, 5,  1 + 45 + 5 * 9,     "Parasite: five meat and no head"  },
        { 14, 0,  0,                  "Rider: nothing, and not one draw" },
        { 32, 16, 1 + 16 * 10,        "Strider: a sixteen-piece ring"    },
        { 39, 6,  1 + 45 + 5 * 9 + 6, "a player's 39: the default"       },
    };
    u32 i;

    puts("\n0x8007CEB4's arms, end to end");

    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        gib_fixture f;
        q2_gib_parent p;
        q2_rng r;
        u32 n;
        char what[96];

        fixture_open(&f, 1000u + i, NULL);
        q2_gib_parent_init(&p);
        p.placed = true;
        p.cls    = k[i].cls;

        check_eq(q2_gib_destroy(&f.gw, &p, NULL), k[i].chunks, k[i].what);
        q2_rng_seed(&r, 1000u + i);
        for (n = 0; n < k[i].draws; n++)
            (void)q2_rng_next(&r);
        snprintf(what, sizeof(what), "%s — %u draws", k[i].what, k[i].draws);
        check_eq(f.rng.state, r.state, what);
        fixture_close(&f);
    }

    {
        gib_fixture f;
        q2_gib_parent p;
        const q2_gib_body *b;

        fixture_open(&f, 5u, NULL);
        q2_gib_parent_init(&p);
        p.placed = true;
        p.cls = 18;
        p.velocity[0]  = 30000;
        p.knockback[0] = 10000;   /* 40000 wraps as a halfword: -25536 */
        (void)q2_gib_destroy(&f.gw, &p, NULL);
        b = q2_gib_body_of(&f.set.ent[0]);
        check(b && b->vel[0] < 0,
              "the impulse is +0xE0 + +0x2F8 summed as HALFWORDS (0x8007CF3C)");
        fixture_close(&f);
    }
}

/*
 * THE BOSS ARMS' EXPLOSION BANGS TOO. 0x8007CF20 and 0x8007D020 are the same
 * 0x8005A778, so the SoundByCreature inside it runs for a Boss2 coming apart
 * just as for a rocket. The dispatcher reaches the queue through the gib world.
 */
static void test_gib_arm_bangs(void)
{
    static const struct {
        u16 cls; bool sounds; bool explosion; u32 bangs; const char *what;
    } k[] = {
        { 2,  true,  true,  1, "Boss2 (0x8007CF20): one bang, at the body"   },
        { 32, true,  true,  1, "Strider (0x8007D020): one bang"              },
        { 2,  false, true,  0, "with no queue the model is still spawned"     },
        { 13, true,  false, 0, "Jorg's arm calls no spawn, so it is silent"   },
        { 18, true,  false, 0, "a Soldier's Chest arm calls no spawn either"  },
    };
    u32 i;

    puts("\nthe boss arms' Explosion bangs through the gib world (0x8005AA54)");

    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        gib_fixture f;
        q2_gib_parent p;
        q2_gib_report rep;
        q2_model_bank bank;
        q2_model_ent_sounds q;
        s32 out[3] = { 0, 0, 0 };
        char what[128];

        boom_bank(&bank);
        q2_model_ent_sounds_clear(&q);
        fixture_open(&f, 0x2200u + i, NULL);
        f.gw.bank   = &bank;
        f.gw.sounds = k[i].sounds ? &q : NULL;
        q2_gib_attach(&f.gw);

        q2_gib_parent_init(&p);
        p.placed = true;
        p.cls    = k[i].cls;
        p.pos[0] = 2500; p.pos[1] = -80; p.pos[2] = -900 - (s32)i;

        (void)q2_gib_destroy(&f.gw, &p, &rep);
        snprintf(what, sizeof(what), "%s — the model", k[i].what);
        check(rep.explosion == k[i].explosion, what);
        snprintf(what, sizeof(what), "%s — the queue", k[i].what);
        check_eq(q.count, k[i].bangs, what);
        if (k[i].bangs) {
            snprintf(what, sizeof(what), "%s — at +0x54", k[i].what);
            check(q2_model_ent_sound_next(&q, out) && out[0] == p.pos[0] &&
                  out[1] == p.pos[1] && out[2] == p.pos[2], what);
        }
        fixture_close(&f);
    }
}

static void test_gib_pool_refusal(void)
{
    gib_fixture f;
    q2_gib_parent p;
    static const s16 zero[3] = { 0, 0, 0 };
    s32 pos[3] = { 0, 0, 0 };
    q2_rng before;
    u32 i, made = 0;

    puts("\nthe forty-ninth gib is refused before it draws (0x8006C098(0))");

    fixture_open(&f, 31u, NULL);
    q2_gib_parent_init(&p);
    p.placed = true;
    for (i = 0; i < Q2_GIB_BODY_MAX; i++)
        if (q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1,
                             512, "Gib meat"))
            made++;
    check_eq(made, Q2_GIB_BODY_MAX, "forty-eight fit");

    before = f.rng;
    check(q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1, 512,
                           "Gib meat") == NULL, "the next is refused");
    check_eq(f.rng.state, before.state, "and costs the generator nothing");
    check_eq(f.set.count, Q2_GIB_BODY_MAX, "or the pool a slot");

    f.set.ent[3].think = q2_item_shrink_think;   /* one has expired */
    check(q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1, 512,
                           "Gib meat") != NULL,
          "a fading gib's body is spare: 0x8005B358 moves nothing");

    fixture_close(&f);
}

/* ------------------------------------------------------------------------- */
/* The collision half, on one hand-built cell                                 */
/* ------------------------------------------------------------------------- */
/*
 * One box, x and z -4000..4000, y -4000..300. +Y is DOWN, so the +Y face at
 * 300 is the floor. Contents 9, so the area byte is visible.
 */
#define CELL_FLOOR 300

static u8 g_cell[4 + 2 * Q2_COLL_NODE_SIZE + 6 * Q2_COLL_PLANE_SIZE];

static void cw16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void cw32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static bool open_cell(q2_collision *out)
{
    static const s32 lo[3] = { -4000, -4000, -4000 };
    static const s32 hi[3] = {  4000, CELL_FLOOR, 4000 };
    static const s16 n[6][3] = {
        {-4096, 0, 0}, {4096, 0, 0},
        {0, -4096, 0}, {0, 4096, 0},
        {0, 0, -4096}, {0, 0, 4096}
    };
    u8 *node, *planes;
    dat_chunk chunk;
    q2_zone_file zf;
    int i, k;

    memset(g_cell, 0, sizeof(g_cell));
    cw16(g_cell + 0, 1);
    cw16(g_cell + 2, 6);
    node   = g_cell + 4;
    planes = node + 2 * Q2_COLL_NODE_SIZE;

    for (k = 0; k < 3; k++) {
        cw32(node + k * 4, (u32)lo[k]);
        cw32(node + 12 + k * 4, (u32)hi[k]);
    }
    cw16(node + 24, 0);
    cw16(node + 26, 0);
    node[32] = 9;
    cw16(node + Q2_COLL_NODE_SIZE + 24, 6);     /* the sentinel */
    cw16(node + Q2_COLL_NODE_SIZE + 26, 0);

    for (i = 0; i < 6; i++) {
        s32 pt[3] = { 0, 0, 0 };

        if (i & 1)
            pt[i / 2] = hi[i / 2] - lo[i / 2];
        for (k = 0; k < 3; k++) {
            cw16(planes + i * 12 + k * 2, (u32)pt[k]);
            cw16(planes + i * 12 + 6 + k * 2, (u32)(u16)n[i][k]);
        }
    }

    memset(&chunk, 0, sizeof(chunk));
    chunk.data = g_cell;
    chunk.size = (u32)sizeof(g_cell);
    memset(&zf, 0, sizeof(zf));
    zf.chunk[Q2_ZONE_PRIMARY_COLL] = &chunk;
    return q2_collision_parse(out, &zf, Q2_COLL_PRIMARY) == Q2_OK;
}

static void test_gib_clip_and_land(void)
{
    q2_collision coll;
    gib_fixture f;
    q2_gib_parent p;
    q2_entity *e;
    q2_gib_body *b;
    static const s16 zero[3] = { 0, 0, 0 };
    s32 pos[3] = { 0, 0, 0 };
    s32 tick, landed = -1, first_contact = -1;
    bool through = false, bounced = false;
    u32 i;

    puts("\nin a hull: the chunk is clipped at spawn and lands on the floor");

    if (!open_cell(&coll)) {
        check(false, "the one-cell hull parses");
        return;
    }

    /* A parent against the +X wall: every jitter past 4000 is clipped. */
    fixture_open(&f, 0xC0FFEEu, &coll);
    q2_gib_parent_init(&p);
    p.placed = true;
    p.cls = 18;
    p.pos[0] = 3900;
    (void)q2_gib_destroy(&f.gw, &p, NULL);
    for (i = 0; i < 5; i++) {
        b = q2_gib_body_of(&f.set.ent[i]);
        check(f.set.ent[i].pos[0] <= 4000,
              "no chunk starts outside the wall (0x8004E920)");
        check(b && b->cell == 0, "and its +0xA0 is the cell it ended in");
        check_eq(f.set.ent[i].surface, 9,
                 "+0x9E is that cell's byte +32 (0x8005A398)");
    }
    fixture_close(&f);

    /*
     * A chunk that starts with no cell (+0xA0 = -1, so +0x9E = 0): its first
     * move finds cell 0, and the trail on that SAME tick is laid in cell 0's
     * area — 0x800463E8 writes +0x9E at 0x80046B0C, inside the toss, before
     * the think's trail reads it; the think's own 0x80054DD4 comes after.
     */
    fixture_open(&f, 0xA5EAu, &coll);
    q2_gib_parent_init(&p);
    p.placed = true;
    e = q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1, 512,
                         "Gib meat");
    check(e != NULL && e->surface == 0, "a chunk with no cell starts in area 0");
    if (e) {
        const q2_fx_group *g = NULL;

        q2_fx_world_clear(&f.fx);
        q2_gib_think(e, &f.ew);
        for (i = 0; i < f.fx.group_count; i++)
            if (f.fx.group[i].life)
                g = &f.fx.group[i];
        check(g != NULL && g->area == 9,
              "and its first trail is already in the area the move found");
    }
    fixture_close(&f);

    /* One long-lived chunk dropped from rest onto the floor. */
    fixture_open(&f, 0xF00Du, &coll);
    q2_gib_parent_init(&p);
    p.placed = true;
    e = q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, 0, 30000,
                         "Gib meat");
    b = e ? q2_gib_body_of(e) : NULL;
    check(b != NULL, "one chunk");
    if (!e || !b) {
        fixture_close(&f);
        return;
    }
    b->vel[0] = b->vel[1] = b->vel[2] = 0;

    for (tick = 0; tick < 1000; tick++) {
        s16 vy_before = b->vel[1];

        q2_gib_think(e, &f.ew);
        if (e->pos[1] > CELL_FLOOR)
            through = true;
        if (first_contact < 0 && vy_before > 0 && b->vel[1] < 0) {
            first_contact = tick;
            bounced = true;
        }
        if (b->move_flags & Q2_GIB_MOVE_REST) {
            landed = tick;
            break;
        }
    }

    check(!through, "it never passes the floor");
    check(bounced, "it rebounds first — 2304/4096 of its speed, reflected");
    check(landed > first_contact && landed >= 0,
          "and comes to rest once a contact is slower than 0xC34FF");
    check_eq(e->pos[1], CELL_FLOOR - 1,
             "one unit off the plane: 0x8005625C's push out of the floor");
    check_eq(e->surface, 9, "+0x9E from the cell the mover ended in");

    {
        s32 at[3];
        q2_rng before = f.rng;

        memcpy(at, e->pos, sizeof(at));
        q2_fx_world_clear(&f.fx);
        q2_gib_think(e, &f.ew);
        check(memcmp(at, e->pos, sizeof(at)) == 0 &&
              live_groups(&f.fx) == 0 && f.rng.state == before.state,
              "at rest: no move, no trail, no draw, from then on");
    }

    fixture_close(&f);
}

static void test_gib_door(void)
{
    gib_fixture f;
    q2_gib_parent p;
    q2_entity *e;
    q2_gib_body *b;
    q2_move_target door;
    q2_move_world ents;
    static const s16 zero[3] = { 0, 0, 0 };
    s32 pos[3] = { 0, 0, 0 };

    puts("\na door in the way (0x80053974, tried before the hull)");

    memset(&door, 0, sizeof(door));
    door.min[0] = 100;  door.max[0] = 200;
    door.min[1] = -1000; door.max[1] = 1000;
    door.min[2] = -1000; door.max[2] = 1000;
    memcpy(door.env_min, door.min, sizeof(door.min));
    memcpy(door.env_max, door.max, sizeof(door.max));
    door.kind   = Q2_MOVE_KIND_ENTITY;
    door.active = true;
    memset(&ents, 0, sizeof(ents));
    ents.targets     = &door;
    ents.count       = 1;
    ents.half_extent = Q2_SWEEP_HALF_EXTENT;

    fixture_open(&f, 0xD00Au, NULL);
    f.gw.ents = &ents;
    q2_gib_attach(&f.gw);
    q2_gib_parent_init(&p);
    p.placed = true;
    e = q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1, 512,
                         "Gib meat");
    b = e ? q2_gib_body_of(e) : NULL;
    check(b != NULL, "one chunk");
    if (!e || !b) {
        fixture_close(&f);
        return;
    }
    e->render_flags |= Q2_ITEM_TOSS_NO_GRAVITY;
    b->vel[0] = 3200; b->vel[1] = 0; b->vel[2] = 0;

    q2_gib_think(e, &f.ew);
    check_eq(e->pos[0], 60, "the first step, 3200 * 6 / 320, is clear");
    q2_gib_think(e, &f.ew);
    check_eq(e->pos[0], 39,
             "the second meets the door at 100: pulled back the whole step "
             "(0x80046650) and one unit off its face");
    /* SquareRoot0(3200^2) is 3197, not 3200 (0x8008A7E8); 3197 * 2304 >> 12
     * is 1798, and VectorNormal of (-1, 0, 0) is exactly -4096. */
    check_eq(b->vel[0], -1798, "and thrown back at 2304/4096 of the SDK root of 3200");
    check(!(b->move_flags & Q2_GIB_MOVE_REST), "too fast to settle there");
    fixture_close(&f);

    /* No entity list: the door is not there for it. */
    fixture_open(&f, 0xD00Au, NULL);
    e = q2_gib_spawn_one(&f.gw, &p, Q2_GIB_STYLE_MEAT, 0, zero, pos, -1, 512,
                         "Gib meat");
    b = e ? q2_gib_body_of(e) : NULL;
    if (e && b) {
        e->render_flags |= Q2_ITEM_TOSS_NO_GRAVITY;
        b->vel[0] = 3200; b->vel[1] = 0; b->vel[2] = 0;
        q2_gib_think(e, &f.ew);
        q2_gib_think(e, &f.ew);
    }
    check(e && e->pos[0] == 120, "without `ents` the gib goes through");
    fixture_close(&f);
}

/* ------------------------------------------------------------------------- */
/* The two sites                                                              */
/* ------------------------------------------------------------------------- */
static void test_gib_monster_site(void)
{
    gib_fixture f;
    q2_monster m;
    q2_gib_stats was = q2_gib_counters;
    u32 i;

    puts("\nthe creature corpse handler hands the body over (0x8007F764)");

    fixture_open(&f, 18u, NULL);

    /*
     * The ordinary case: killed straight past gib_health. The module's gib
     * arm has already raised `gibbed` (cre_soldier.c) and thrown nothing; the
     * body detached; this is its first corpse tick.
     */
    memset(&m, 0, sizeof(m));
    m.in_use           = true;
    m.dead             = true;
    m.gibbed           = true;
    m.corpse           = true;
    m.class_id         = Q2_CLASS_CORPSE;
    m.corpse_was_class = 87;
    m.pop_class_id     = 18;
    m.takedamage       = Q2_DAMAGE_YES;
    m.health           = -45;
    m.gib_health       = -30;
    m.pos[0] = 640; m.pos[1] = -200; m.pos[2] = 1280;

    check(q2_monster_corpse_tick(&m), "the body is destroyed this tick");
    check_eq(f.set.count, 6,
             "and comes apart as five Gib meat and a Chest, although the "
             "module's arm had already marked it gibbed");
    for (i = 0; i < 5 && i < f.set.count; i++)
        check(f.set.ent[i].pos[0] - 640 >= -286 &&
              f.set.ent[i].pos[0] - 640 <= 285,
              "around the body's own position");
    check(f.set.count == 6 && strcmp(f.set.ent[5].model, "Chest") == 0 &&
          f.set.ent[5].pos[1] == -200,
          "the Chest from where the body lay");
    check(!m.in_use, "the record is freed, as 0x8007D140 frees the actor");
    check_eq(m.takedamage, Q2_DAMAGE_NO, "and can no longer be hit");

    check(!q2_monster_corpse_tick(&m), "and it is destroyed only once");
    check_eq(f.set.count, 6, "throwing nothing more");
    check_eq(q2_gib_counters.destroyed - was.destroyed, 1,
             "the run report counts one body");
    check_eq(q2_gib_counters.chunks - was.chunks, 6, "and six chunks");
    check_eq(q2_gib_counters.unbound - was.unbound, 6,
             "none of which a NULL bank could bind");

    fixture_close(&f);

    /* Nothing attached: still destroyed, nothing thrown, nothing crashes. */
    memset(&m, 0, sizeof(m));
    m.in_use = true; m.corpse = true; m.pop_class_id = 18;
    m.health = -45;  m.gib_health = -30;
    check(q2_monster_corpse_tick(&m), "with no gib world the body still goes");
}

static s32 g_placed_at[3];
static int g_described;

static void place_player(void *user, q2_gib_victim kind, const void *who,
                         q2_gib_parent *parent)
{
    int k;

    (void)who;
    g_described++;
    if (kind != Q2_GIB_VICTIM_PLAYER || user != (void *)g_placed_at)
        return;
    parent->placed = true;
    for (k = 0; k < 3; k++) {
        parent->pos[k]    = g_placed_at[k];
        parent->origin[k] = g_placed_at[k];
    }
}

static void test_gib_player_site(void)
{
    gib_fixture f;
    q2_player_death d;
    q2_player_death_event ev;

    puts("\nthe player's corpse think hands the body over (0x80039578)");

    fixture_open(&f, 39u, NULL);
    g_placed_at[0] = 7000; g_placed_at[1] = -300; g_placed_at[2] = -7000;
    g_described = 0;
    f.gw.describe = place_player;
    f.gw.user     = g_placed_at;
    q2_gib_attach(&f.gw);

    q2_player_death_init(&d);
    memset(&ev, 0, sizeof(ev));
    q2_player_die(&d, -1, 9, 0, false, false, &ev);
    check_eq(d.stage, Q2_PDEATH_DYING, "the body is down");

    check(!q2_player_death_tick(&d, -41, 6, false, 0),
          "past -40 the body is gone");
    check_eq(d.stage, Q2_PDEATH_GIBBED, "GIBBED");
    check_eq(g_described, 1, "the owner was asked where the body is");
    check_eq(f.set.count, 6, "and the default arm threw five and a Chest");
    check(f.set.count == 6 && f.set.ent[5].pos[0] == 7000 &&
          f.set.ent[5].pos[2] == -7000,
          "from where the owner said the body was");

    fixture_close(&f);

    /* An owner that cannot place the body: the stage still changes and
     * nothing is thrown from nowhere. */
    fixture_open(&f, 40u, NULL);
    q2_player_death_init(&d);
    memset(&ev, 0, sizeof(ev));
    q2_player_die(&d, -1, 9, 0, false, false, &ev);
    check(!q2_player_death_tick(&d, -41, 6, false, 0), "gone");
    check_eq(d.stage, Q2_PDEATH_GIBBED, "GIBBED");
    check_eq(f.set.count, 0, "and no gib without a position");
    fixture_close(&f);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    puts("model entities - 0x8005A778 and 0x8005A5F8\n");

    test_names();
    test_lifetime();
    test_scale_ramp();
    test_flash_ramp();
    test_think_emits_retail_light();
    test_allocator_and_explosion_ambient();
    test_think_clock();
    test_think_removes_unanimated();
    test_spawn_needs_a_bank();
    test_spawn_bangs();
    test_bang_queue();

    puts("\ngibs - 0x8007CEB4, 0x8005A3D4, 0x8005A0AC, 0x8005AD8C, "
         "0x8005B0AC, 0x80059DE0");
    build_tables();
    test_gib_arm_table();
    test_gib_throw_arithmetic();
    test_gib_throw_meat_five_and_head();
    test_gib_throw_meat_spacing();
    test_gib_think_runs_the_trail();
    test_gib_trail_offsets();
    test_gib_rest_and_expiry();
    test_gib_boss_chunk();
    test_gib_jorg_ring();
    test_gib_dispatch_arms();
    test_gib_arm_bangs();
    test_gib_pool_refusal();
    test_gib_clip_and_land();
    test_gib_door();
    test_gib_monster_site();
    test_gib_player_site();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
