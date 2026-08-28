/*
 * projectile.h — the things a weapon puts into the world.
 *
 * ---------------------------------------------------------------------------
 * Five kinds, five spawners
 * ---------------------------------------------------------------------------
 * Four weapons fire something that travels, and each has its own spawner in the
 * executable. They are reached by class NAME, not by index: the spawner copies
 * a twelve-byte literal to the stack and looks it up (0x8006D008), which is why
 * the names sit in a 12-byte-stride block just before the weapon sound table.
 *
 *     0x800ACB8C  "Grenade2"   0x8004A088   grenade launcher
 *     0x800ACB98  "Grenade3"   0x8004AA6C   hand grenade
 *     0x800ACBA4  "Rocket"     0x8004AF28   rocket launcher
 *     0x800ACBB0  "Spin"       0x8004B678
 *     0x800ACBBC  "BFGBlast"   0x8004BE04   BFG
 *
 * The blaster and hyperblaster share a fifth path, 0x8004D70C, which does not
 * name a class: it allocates a bare entity, gives it a speed of 2560 and eight
 * hull corners read from 0x8009DB1C, and links it. The hyperblaster's bolt sets
 * bit 2 of its flags word and gets those corners rotated into place; the
 * blaster's does not, so its bolt has no orientation. That flag is the only
 * difference between the two projectiles.
 *
 * ---------------------------------------------------------------------------
 * Splash
 * ---------------------------------------------------------------------------
 * Read from the argument pairs at the seven call sites of the radius-damage
 * routine 0x80050810:
 *
 *     grenade   radius 1000   mod 13   0x8004A038, 0x8004A934, 0x8004A9CC
 *     rocket    radius 1300   mod 15   0x8004AE2C
 *     BFG       radius 1300   mod  1   0x8004BBBC, 0x8004BC74
 *
 * The rocket's direct hit is applied separately and first, at full damage with
 * mod 15 (0x8004AE14), and only then does the blast go off — so a direct hit is
 * damage plus splash, exactly as in the lineage.
 *
 * Three different call sites carry the grenade's numbers because a grenade can
 * end three ways: its fuse expiring, touching a target, and being destroyed.
 * All three use the same radius and the same mod.
 *
 * ---------------------------------------------------------------------------
 * The entity list, and what a bolt's numbers mean
 * ---------------------------------------------------------------------------
 * Projectiles live in an 88-byte-record array at 0x800C91C0 which a per-frame
 * sweep at 0x80047C6C walks. Reading that sweep settles two fields the bolt
 * spawner writes that would otherwise have to be guessed at:
 *
 *   +0x1A  is a LIFETIME, not a speed. The sweep subtracts the frame's dt from
 *          it and retires the entity when it reaches zero, so the bolt's 2560
 *          is about eight and a half seconds on the 300 Hz clock.
 *   +0x52  is the VELOCITY, in world units per dt unit. The sweep multiplies it
 *          by the frame's dt to get the move.
 *
 * That means the direction argument the bolt spawner is handed IS its velocity:
 * the blaster's `aim >> 6` gives 64 units per dt and the hyperblaster's
 * `aim >> 7` gives 32, so the hyperblaster's bolt is half the speed of the
 * blaster's. Nothing normalises, and nothing needs to.
 *
 * The grenade and rocket use the shared entity mover instead. Their direction
 * arguments are raw velocities: movement is componentwise `vel * dt / 320`,
 * and gravity on a grenade is capped at the shared mover's +8192 terminal Y.
 * The apparent speeds once assigned to them are timers. Grenade2 copies its
 * 900 argument to +0xF4 and counts it down; Rocket writes 20000 to the same
 * field and counts it down. BFGBlast is different again: it fixed-multiplies
 * the view matrix's forward column by the raw local vector {0,0,768}, with no
 * normalisation, and its private think advances it with a divisor of 64 while
 * its +0xF4 timer starts at 2400.
 *
 * ---------------------------------------------------------------------------
 * What is transcribed and what is modelled
 * ---------------------------------------------------------------------------
 * The velocities, divisors, lifetimes, radii, damage and means of death above
 * are read. Collision is still delegated to the caller's trace, but per-tick
 * integration now follows the console's componentwise integer divides.
 *
 * Grenade3 uses the same record without widening it: while held, `node` is the
 * dedicated Q2_PROJ_NODE_HELD sentinel and `vel[2]` carries the raw charge at
 * entity+0x4C. That preserves the save record byte-for-byte. Release replaces
 * the charge with the rotated velocity and restores the ordinary unknown-cell
 * sentinel before the first move.
 */
#ifndef Q2PSX_PROJECTILE_H
#define Q2PSX_PROJECTILE_H

#include "combat.h"
#include "q2psx.h"
#include "weapon.h"

/* ------------------------------------------------------------------------- */
typedef enum q2_proj_kind {
    Q2_PROJ_NONE = 0,
    Q2_PROJ_BOLT,          /* blaster and hyperblaster, 0x8004D70C          */
    Q2_PROJ_GRENADE,       /* "Grenade2", 0x8004A088                        */
    Q2_PROJ_HAND_GRENADE,  /* "Grenade3", 0x8004AA6C                        */
    Q2_PROJ_ROCKET,        /* "Rocket",   0x8004AF28                        */
    Q2_PROJ_BFG            /* "BFGBlast", 0x8004BE04                        */
} q2_proj_kind;

/* Splash, as read. Radius is world units. */
#define Q2_SPLASH_RADIUS_GRENADE 1000
#define Q2_SPLASH_RADIUS_ROCKET  1300
#define Q2_SPLASH_RADIUS_BFG     1300

/*
 * A bolt's lifetime, entity +0x1A written by 0x8004D764 and counted down by
 * the sweep at 0x80047D08. READ.
 */
#define Q2_LIFETIME_BOLT     2560

/*
 * Gravity on a thrown grenade. MODELLED: the grenade is handed to the shared
 * entity physics, so it falls at the world's gravity rather than at a constant
 * of its own, and the port therefore takes the simulation's value rather than
 * inventing one. Passed in by the caller.
 */

/*
 * A projectile lights the world around it.
 *
 * The per-frame sweep at `0x80047C6C` calls the engine's dynamic-light append
 * (`0x80075C34`) at `0x80048228` for every live projectile, and it does not
 * compute the light -- it reads a preset out of globals:
 *
 *     0x800AE954   FF 64 4B      RGB (255, 100, 75), a warm orange
 *     0x800AE958   2C 01 20 03   two u16 packed lo | hi<<16: 300 and 800
 *
 * so the colour and both radii are DATA, transcribed here rather than chosen.
 * `0x800482A0` adds a SECOND light behind a `& 0x10` test on a halfword at
 * `s3-54`. Its operands are read and are not guesses:
 *
 *     a1 = 0x800AE954 packed r | g<<8 | b<<16 | a<<24   -- the SAME colour
 *     a2 = 0x800AE960 packed lo | hi<<16                -- 800 and 1600
 *     a3 = 0x800AE95C                                   -- style 0, size_shift 0
 *
 * (`a3` is NOT a radius pair: 0x80075C70 takes its low half as a 3-bit style and
 * its high half as a 2-bit size shift. `a2` carries both radii, sign-extended
 * and squared into the light record at +20 and +24.)
 *
 * so it is a wider, outer halo of the same warm orange: inner 800, outer 1600
 * against the first light's 300 and 800.
 *
 * THE GATE IS READ, and the answer is that it never opens on this path. The
 * halfword is at `s3-54`, and `s3 = record + 88`, so the flag lives at
 * **record+0x22**. `0x8004D7BC` writes it from `s1`, which `0x8004D714` loads as
 * a STACK argument (`lhu s1, 128(sp)`) — so the flags are the spawner's fifth
 * parameter. Its three callers pass:
 *
 *     0x8004C120   11  (0b01011)
 *     0x8004D3EC   14  (0b01110)
 *     0x800620D8   11  (0b01011)
 *
 * **Bit 0x10 is clear in all three.** The second light therefore does not fire
 * for any projectile spawned through this function, and this port is right not
 * to raise it — not because the gate is unknown, but because it is known to be
 * shut. Other writers of a +0x22 halfword exist elsewhere in the image and
 * belong to other structures; this reading is scoped to this spawner.
 */
#define Q2_PROJ_LIGHT2_INNER   800
#define Q2_PROJ_LIGHT2_OUTER  1600

/*
 * The BFG blast lights differently, and it says so itself.
 *
 * `0x8004B2B4` is installed as an entity think at `+0x3C` by the spawner that
 * materialises the string "BFGBlast" (`0x800ACBBC`) beside it, so the identity
 * is read rather than inferred. Its light at `0x8004B8A0` takes RGB from
 * `0x800AE9BC` and radii from `0x800AE9C0`:
 *
 *     800AE9BC   00 FF 00      rgb(0, 255, 0)   -- green
 *     800AE9C0   E8 03 78 05   u16 1000, 1400   -- inner, outer
 *
 * A wide green glow rather than the small warm one every other bolt carries.
 * Each kind's own think adds its own light in the original, so this REPLACES
 * the generic preset for a BFG rather than adding to it — doubling them would
 * light a BFG twice.
 */
#define Q2_PROJ_BFG_LIGHT_R      0
#define Q2_PROJ_BFG_LIGHT_G    255
#define Q2_PROJ_BFG_LIGHT_B      0
#define Q2_PROJ_BFG_LIGHT_INNER 1000
#define Q2_PROJ_BFG_LIGHT_OUTER 1400
#define Q2_PROJ_LIGHT_R       255
#define Q2_PROJ_LIGHT_G       100
#define Q2_PROJ_LIGHT_B        75
#define Q2_PROJ_LIGHT_INNER   300
#define Q2_PROJ_LIGHT_OUTER   800

typedef struct q2_projectile {
    bool in_use;
    q2_proj_kind kind;

    s32  pos[3];
    s32  vel[3];        /* 1.0.12 velocity; held Grenade3 uses vel[2]=charge */
    s16  damage;
    s16  mod;
    s16  splash_radius;

    s32  expires;       /* level tick at which the fuse runs out, 0 = never  */
    s32  owner;         /* index of whoever fired it, -1 for the world       */
    bool bounced;       /* a grenade that has hit something at least once    */

    /*
     * The collision cell this projectile is in, carried across ticks exactly as
     * the mover carries the player's. A rocket that has flown across the map is
     * nowhere near the shooter's cell, so tracing it from the shooter's would
     * ask the hull the wrong question and the rocket would sail through walls.
     * Q2_PROJ_NODE_UNKNOWN means "unknown", which costs one brute-force sweep
     * and self-corrects, the same contract a freshly spawned entity has
     * (0x80044C74). Q2_PROJ_NODE_HELD is Grenade3 state 1: hidden, attached to
     * the owner and not participating in movement or collision.
     */
    s32  node;
} q2_projectile;

#define Q2_PROJ_NODE_UNKNOWN (-1)
#define Q2_PROJ_NODE_HELD    (-2)

#define Q2_PROJ_MAX 32

typedef struct q2_projectiles {
    q2_projectile p[Q2_PROJ_MAX];
    u32 live;
} q2_projectiles;

void q2_projectiles_init(q2_projectiles *list);

/*
 * Launch what a fire result asked for. Does nothing for a hitscan or rail
 * result. Returns the slot used, or -1 when the list is full — which the
 * console also has to cope with, since its entity pool is finite.
 */
s32 q2_projectile_launch(q2_projectiles *list, const q2_fire_result_v2 *fire,
                         s32 owner, s32 now);

/* Grenade3 state 1, 0x8004A414..0x8004A470. The index query returns -1 when
 * this owner has no live held grenade. `cook_dt` is added as 6*dt to the raw
 * charge after copying the view weapon's world position. */
s32 q2_projectile_hand_held_index(const q2_projectiles *list, s32 owner);
s32 q2_projectile_hand_charge(const q2_projectiles *list, s32 owner);
bool q2_projectile_hand_update(q2_projectiles *list, s32 owner,
                               const s32 attached_pos[3], s32 cook_dt);

/* Grenade3 state 2 -> 3. `raw_dir` is the already-rotated shared-mover vector;
 * release converts it to the list's common fixed representation and makes the
 * entity visible/moving. */
bool q2_projectile_hand_release(q2_projectiles *list, s32 owner,
                                const s32 origin[3], const s32 raw_dir[3]);

/*
 * What a projectile did on one tick. The caller supplies the trace and applies
 * the outcome, so this module never needs to know about collision hulls.
 */
typedef struct q2_proj_step {
    s32  from[3];
    s32  to[3];         /* where it wants to be                              */
    bool expired;       /* the fuse or safety lifetime ran out this tick     */
} q2_proj_step;

/*
 * Advance one projectile by the tick's `dt` and report where it wants to move.
 * Apply gravity to the grenades only, in the caller's own units.
 *
 * `dt` IS REQUIRED and is the same dt the sim's own tick ran on. A projectile's
 * velocity is per dt UNIT, exactly as the original's is (0x80047D40 forms the
 * destination as `pos += vel * dt` against the frame delta at 0x800B2DB4), so
 * dropping it does not slow a bolt by a rounding error — it slows it by the
 * whole tick length, twelve to twenty times. Zero or negative falls back to
 * Q2_DT_NOMINAL rather than freezing every projectile in the air.
 *
 * The caller then traces `from`..`to`, and either commits the move or calls
 * q2_projectile_impact.
 */
void q2_projectile_step(q2_projectiles *list, u32 index, s32 gravity, s32 dt,
                        s32 now, q2_proj_step *out);

/* Commit a move that met nothing. */
void q2_projectile_commit(q2_projectiles *list, u32 index, const s32 to[3]);

/*
 * Resolve an impact at `point`.
 *
 * `hit` is the actor struck, or NULL for the world. Grenades bounce off the
 * world — reflected about `normal` and losing half their speed, which is
 * MODELLED, not read — and detonate on anything else. Everything else
 * detonates on contact.
 *
 * Returns true when the projectile was consumed.
 */
bool q2_projectile_impact(q2_projectiles *list, u32 index,
                          const s32 point[3], const s32 normal[3],
                          q2_actor *attacker, q2_actor *hit,
                          q2_actor **targets, u32 target_count,
                          const q2_combat_rules *rules);

/* How much of its speed a grenade keeps when it bounces. MODELLED. */
#define Q2_GRENADE_BOUNCE_NUM 1
#define Q2_GRENADE_BOUNCE_DEN 2

/* Detonate a projectile where it stands, applying its splash. Used by the fuse
 * path and by a caller that wants to clear the world. */
u32 q2_projectile_detonate(q2_projectiles *list, u32 index,
                           q2_actor *attacker, q2_actor **targets, u32 count,
                           const q2_combat_rules *rules);

/*
 * Remove a projectile whose safety lifetime elapsed, without applying splash
 * damage. The rocket at 0x8004ADB0 and BFG blast at 0x8004B8E4 both call the
 * entity free path when +0xF4 reaches zero; only the grenade fuse detonates.
 * Bolts use the same quiet expiry. Returns false for an invalid or free slot.
 */
bool q2_projectile_expire(q2_projectiles *list, u32 index);

#endif /* Q2PSX_PROJECTILE_H */
