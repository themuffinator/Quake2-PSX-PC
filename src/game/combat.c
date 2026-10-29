#include "combat.h"

#include "playerdeath.h"   /* Q2_PDEATH_GIB_HEALTH: one copy of 0x800397FC */
#include "shock.h"         /* SetShockData, the hit on a player's tail      */
#include "sdkmath.h"
#include "trig.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Mod properties                                                             */
/* ------------------------------------------------------------------------- */
/*
 * The jump table at 0x800ACE1C, sixteen words indexed by mod-1. Each entry
 * lands on either 0x80058348 (`s0 = 1`) or 0x80058350 (`s0 = 0`), and `s0` is
 * the third argument to the armour routine — which reads the `+4` column when
 * it is set and the `+2` column when it is clear. So the flag is "this is
 * energy damage".
 *
 * Anything above 16 misses the table's `sltiu ..., 16` bound and falls through
 * with s0 already zero, so mods 17..21 — bullets among them — are ordinary.
 */
static const u8 k_mod_energy[Q2_MOD_COUNT] = {
    /*  0 */ 0,
    /*  1 */ 1, /*  2 */ 1, /*  3 */ 0, /*  4 */ 1,
    /*  5 */ 1, /*  6 */ 1, /*  7 */ 0, /*  8 */ 0,
    /*  9 */ 0, /* 10 */ 0, /* 11 */ 1, /* 12 */ 1,
    /* 13 */ 0, /* 14 */ 1, /* 15 */ 0, /* 16 */ 1,
    /* 17 */ 0, /* 18 */ 0, /* 19 */ 0, /* 20 */ 0, /* 21 */ 0
};

bool q2_mod_is_energy(s16 mod)
{
    if (mod < 0 || mod >= Q2_MOD_COUNT)
        return false;
    return k_mod_energy[mod] != 0;
}

bool q2_mod_knocks_back(s16 mod)
{
    /* 0x80057ED0..0x80057EE8, in the order the branches test them. */
    return mod == Q2_MOD_ROCKET || mod == Q2_MOD_GRENADE ||
           mod == Q2_MOD_RAIL   || mod == Q2_MOD_BULLET;
}

s16 q2_mod_effect_timer(s16 mod, int *slot)
{
    /* 0x800585A4..0x80058604. Four mods arm a timer, each in its own byte. */
    switch (mod) {
    case Q2_MOD_ENERGY_BOLT: if (slot) *slot = 1; return 3;
    case Q2_MOD_2:           if (slot) *slot = 0; return 15;
    case Q2_MOD_4:           if (slot) *slot = 2; return 30;
    case Q2_MOD_BOLT_HYPER:  if (slot) *slot = 4; return 5;
    default:                 if (slot) *slot = -1; return 0;
    }
}

u8 q2_mod_hit_sound(s16 mod, s16 *volume)
{
    /*
     * 0x80058500 `addiu v1, s3, -1` / 0x80058504 `sltiu v0, v1, 21` /
     * 0x8005851C `lw v0, 0(v1)` — the twenty-one-word jump table at 0x800ACE5C,
     * dumped and decoded rather than guessed. The four arms are:
     *
     *     8005852C  fp = 13, s1 = 4096
     *     80058538  fp = 16, s1 = 2048
     *     80058544  fp = 15, s1 = 4096
     *     80058550  s1 = 0            (and 0x80058554 `beq s1, zero` drops it)
     *
     * The table's twenty-one words, in order for mods 1..21:
     *   2C 44 50 44 38 38 44 50 50 50 50 50 50 50 50 50 50 38 44 44 44
     * which decodes to what the switch below says.
     */
    s16 vol = 0;
    u8  id  = 0;

    switch (mod) {
    case 1:                             /* 0x8005852C */
        id = 13; vol = 4096; break;
    case 3: case 7: case 19: case 20: case 21:   /* 0x80058544 */
        id = 15; vol = 4096; break;
    case 5: case 6: case 18:            /* 0x80058538 */
        id = 16; vol = 2048; break;
    default:                            /* 0x80058550, silent */
        break;
    }

    if (volume)
        *volume = vol;
    return id;
}

/*
 * The two result fields, under exactly the executable's guards.
 *
 * 0x800584D4 `lw v0, 12(s2)` — the target must have a client block; and
 * 0x800584F4 `lh v0, 264(s2)` / 0x800584FC `blez v0, 0x800585A4` — the health
 * read is the one AFTER the store, so this is the sound of SURVIVING a hit.
 *
 * The middle guard, 0x800584E4 `lw v0, 0(v0)` on the client's own entity
 * back-pointer, has no counterpart in this port's actor model and is therefore
 * not reproduced; nothing here can hold a client block whose entity is NULL.
 *
 * And then the call itself, 0x80058558..0x8005859C, which is SetShockData and
 * not a mixer (shock.h):
 *
 *     a0 = (client->window - 0x800D5C30) * 0x1A1F58D1 >> 4
 *     a1 = the id, a2 = the strength, a3 = 0
 *
 * The multiply-and-shift is a divide by 784, GameWindows' stride, so a0 is
 * the VICTIM's own window; `owner` is this port's name for which player an
 * actor is. a3 NULL makes it the non-positional kind — the memset of sp+16 at
 * 0x80058588 zeroes the two radii, which are then never read. An actor that
 * claims a client but is no player's (`owner` -1) has no window, and the
 * request goes nowhere.
 */
static void set_hit_sound(const q2_actor *t, s16 mod, q2_damage_result *out)
{
    if (!t || !out)
        return;
    if (!t->has_client || t->health <= 0)
        return;
    out->hit_sound_id = q2_mod_hit_sound(mod, &out->hit_sound_vol);
    if (out->hit_sound_id == 0) {
        out->hit_sound_vol = 0;
        return;                             /* 0x80058554 `beq s1, zero` */
    }
    q2_shock_request(t->owner, out->hit_sound_id, out->hit_sound_vol,
                     NULL, 0, 0);
}

bool q2_actor_energy_lit(const q2_actor *a)
{
    /* 0x80058650 reads entity+0x2F1, which combat.h maps to effect[1]; the
     * lit path is the `>= 3` arm at 0x80058660. */
    return a && a->effect[1] >= 3;
}

void q2_combat_rules_default(q2_combat_rules *r)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->skill = 1;     /* not the lowest, so monster damage is not halved */
    /*
     * BLAST FORCE's own reset value, 0x80020494/0x80020498 `addiu v1, zero, 64`
     * / `sh v1, 884(v0)`. Zero is not a neutral default here: the scale is
     * 125*(mass+64)>>6, so a memset struct pushes everything exactly half as
     * hard as the disc does. The halfword is never transformed on the way in —
     * `xrefs 0x800B3358` finds only this reset, the menu's store at 0x8001BE38
     * and the single read at 0x80057F84 — so the slider's value IS the field.
     */
    r->knockback_mass = 64;
    /* `cheats` is left at zero by the memset: 0x8001C6CC clears 0x800B29EC
     * before folding the four menu toggles in, so an unconfigured session has
     * no cheat bits set. */
}

/* ------------------------------------------------------------------------- */
/* Actors                                                                     */
/* ------------------------------------------------------------------------- */
void q2_actor_init(q2_actor *a)
{
    int k;

    if (!a)
        return;
    memset(a, 0, sizeof(*a));
    a->owner         = -1;      /* not a player until a caller says so */
    a->entity_kind   = 48;      /* unspecified non-client actor */
    /*
     * "Not a player", as 0x8003DDF8 places every player: 0x8003DE24 `addiu v0,
     * zero, 4` / 0x8003DE34 `sb v0, 222(s1)`. This was -1, the one value the
     * death handler cries out for (0x80039728 `bne s1, -1`) and the frag hook
     * lets through (0x80039774 `slti s1, 4`), and the only instruction that
     * stores -1 in this byte is 0x800396DC, for acid and lava. A seed of -1 was
     * therefore a world kill waiting for anything that read the byte before a
     * hit had written it.
     */
    a->last_attacker = (s8)Q2_MP_NOT_A_PLAYER;
    /*
     * entity+0x94 and entity+0x96, the hit cylinder, at the pair the player
     * and the box-built actors carry. A CREATURE does not keep these:
     * q2_actor_from_monster replaces both with the pair GetCreHitSize took
     * from the creature's model (0x8006DC1C), and a creature with no model
     * bound gets that function's own fallback, 286 / 600, from
     * q2_monster_init. Neither number here is a creature's.
     */
    a->radius = 286;      /* entity+0x94, the live actor's X/Z radius */
    a->height = 572;      /* entity+0x96, from origin-286 to origin+286 */
    for (k = 0; k < 3; k++) {
        a->mins[k] = -286;
        a->maxs[k] =  286;
        /* Native allocator seeds both current and target ambient to 0x40. */
        a->ambient[k] = Q2_ACTOR_AMBIENT_DEFAULT;
    }
}

void q2_actor_from_monster(q2_actor *a, const q2_monster *m)
{
    int k;
    u8  effect[sizeof(a->effect)];
    u8  ambient[sizeof(a->ambient)];
    s8  killer;

    if (!a || !m)
        return;

    /*
     * TWO ENTITY FIELDS SURVIVE THE REFRESH, because q2_monster holds neither
     * and this runs at the top of every frame.
     *
     * The damage-effect bytes, entity+0x2F0..0x2F5, are armed by the hit
     * (0x800585A4..0x80058604) and run down by the presentation pass
     * (0x8005B880) on LATER ticks. Rebuilding from q2_actor_init zeroed them
     * before the pass could see them, so a creature's damage effect lived for
     * the one tick it was armed in — shot with the blaster, it never lit —
     * unless the caller saved and restored them around this call, as the
     * client's per-frame rebuild had to.
     *
     * And entity+222, the creature's own killer byte. The damage function
     * writes it into the creature when the creature is hurt (0x80057E04 for a
     * player's hit in deathmatch) and reads it back out when the creature is
     * the ATTACKER: 0x80057E54 `lb v0, 222(s5)` / 0x80057E68 `sb a0, 222(s2)`
     * hands it on to whoever the creature hurts. So "who last hurt this
     * creature" has to outlive the frame it was written in.
     */
    memcpy(effect, a->effect, sizeof(effect));
    memcpy(ambient, a->ambient, sizeof(ambient));
    killer = a->last_attacker;
    q2_actor_init(a);
    memcpy(a->effect, effect, sizeof(effect));
    /* And entity+0x2AC, for the same reason: the fade (0x80075E14) walks it a
     * seventh of the way per tick, so it is meaningless unless it outlives the
     * frame it was written in. */
    memcpy(a->ambient, ambient, sizeof(ambient));
    a->last_attacker = killer;

    memcpy(a->origin, q2_cre_origin(m), sizeof(a->origin));
    for (k = 0; k < 3; k++) a->knockback[k] = m->motion.impulse[k];
    a->knocked = (m->motion.flags & 0x4000u) != 0;
    a->health     = m->health;
    a->gib_health = m->gib_health;
    a->has_client = false;
    a->entity_kind = (s16)m->pop_class_id;
    a->is_monster = (m->svflags & Q2_SVF_MONSTER) != 0;
    a->has_enemy  = (m->enemy != NULL);
    a->godmode = (m->flags & Q2_FL_GODMODE) != 0;
    a->no_knockback = (m->flags & Q2_FL_NO_KNOCKBACK) != 0;

    /*
     * THE MOVEMENT HULL AND THE HIT CYLINDER ARE TWO DIFFERENT VOLUMES, and
     * this used to derive the second from the first.
     *
     * SetupBoxDrop (0x80050FA0) builds both from two different arguments: its
     * `a2` pair, always 286/572 for a creature (0x800AECEC), becomes the
     * symmetric movement box at obj+0x18..0x22, and its FIFTH argument becomes
     * obj+0x40/+0x42 — entity+0x94 and entity+0x96 — at 0x80051030 and
     * 0x80051044. The spawner fills that fifth argument from GetCreHitSize
     * (0x8007E50C), which reads the creature's own MODEL header. 0x800544EC
     * reads only the second pair: 0x8005474C `lh v1, 148(v0)` for the radius
     * and 0x80054834 `lh v0, 150(v0)` for the slab.
     *
     * Taking the largest |mins|/|maxs| as the radius and maxs[1]-mins[1] as the
     * height therefore gave every creature in the game the same 286/572
     * cylinder, because q2_monster_init gives every creature the same ±286
     * hull and nothing else writes it. A Soldier (model 124/607) was being shot
     * at through a band 162 units wider than it is on either side, and a Tank
     * Commander (327/918) was unhittable above the 286 the slab stopped at,
     * and Jorg (590/1749) could only be shot in the bottom third — 572 of the
     * 1749 the model actually stands.
     *
     * `mins`/`maxs` still cross, because the actor keeps the source hull for
     * callers that project a box rather than an entity (actor_cylinder_interval
     * falls back to it when `height` is zero, and the sim's body sweep reads
     * it), but they are no longer where the cylinder comes from.
     */
    for (k = 0; k < 3; k++) {
        a->mins[k] = m->mins[k];
        a->maxs[k] = m->maxs[k];
    }
    a->radius = m->hit_radius;   /* entity+0x94 */
    a->height = m->hit_height;   /* entity+0x96 */

    /*
     * AND WHETHER IT CAN BE HURT AT ALL, which never crossed this boundary.
     * `q2_monster.takedamage` is written by `monster_start` and by every
     * module's `die`; without it here the combat side had only `health` to
     * judge by, and health is the wrong question (see `nearest_hit`).
     */
    a->takedamage = m->takedamage;
}

void q2_actor_to_monster(const q2_actor *a, q2_monster *m)
{
    int k;
    if (!a || !m)
        return;
    if (!m->motion.ready) q2_cre_motion_init(m);
    m->health = a->health;
    for (k = 0; k < 3; k++) m->motion.impulse[k] = (s16)a->knockback[k];
    m->motion.flags = (m->motion.flags & ~0x4000u) | (a->knocked ? 0x4000u : 0u);

    /* The damage-effect bytes, entity+0x2F0..0x2F5, for the corpse tick's
     * dissolve gate (0x8007F728 jal 0x8005B2A8, monster.c). The actor is
     * where combat writes them; the monster only reads them. */
    memcpy(m->effect, a->effect, sizeof(m->effect));

    /*
     * IT NO LONGER RAISES `dead` ITSELF, and that is a fix rather than a
     * removal.
     *
     * The console raises the flag inside the creature's own `die`, and every
     * transcribed `*_die` opens with `if (self->dead) return;` — so a sync that
     * set it here handed the die handler a creature that was already dead and
     * the handler returned on its first instruction. The port worked around
     * that by clearing the flag around the call at the one call site, which is
     * a workaround for a bug that only exists because of this line.
     *
     * `q2_monster_damage_reaction` (monster.c) is now the only thing that sets
     * it, which is where T_Damage's own dispatch sets it too.
     */
}

void q2_actor_from_player(q2_actor *a, const q2_inventory *inv,
                          const s32 pos[3])
{
    s8 owner;

    if (!a)
        return;

    /*
     * WHICH PLAYER this is survives the refresh. `q2_actor_init` clears it to
     * -1, and this runs on every hit — so the first bolt that landed on a
     * player erased their identity, and the kill that followed was attributed
     * to the world. A staged pair produced exactly that: "player 1 killed by
     * -1", and because the runtime blames the victim for a world kill, player
     * 1 was docked a frag for being shot.
     */
    /*
     * And so does the one CLIENT-ONLY timer which has no inventory field:
     * `env_next` is client+0x94, the deadline that throttles acid and lava to
     * once per 400 and once per 100 ticks. Clearing it here made that throttle
     * unobservable: a hazard volume calls this every tick, each call reset the
     * deadline it had just armed, and 20 points of lava landed thirty times a
     * second instead of three. Standing in it killed a full-health player
     * inside a fifth of a second, which is fast enough to look like the volume
     * test being wrong rather than the throttle being erased.
     *
     * AND THE ENTITY'S OWN BYTES, which q2_inventory has no copy of either:
     * entity+222 and +223, the credited killer and the mod, and the six
     * damage-effect timers at +0x2F0..0x2F5. The damage function writes +223
     * on every hit (0x80057E84, 0x80057EBC) and +222 on all but two deathmatch
     * arms (0x80057E68, 0x80057EB8), and the death handler reads both back
     * (0x800396C4, 0x800396EC); the timers are armed by the hit (0x800585A4)
     * and run down on later ticks. Re-initialising them here put the killer
     * back to a seed and the mod to 0 on every refresh, and several refreshes
     * are not followed by a hit — q2_sim_fire does one per shot, and the
     * owner's own splash does one per detonation, in range or not. A Soldier's
     * kill (byte 4) followed by the player's rocket bursting far away came out
     * of the refresh as q2_actor_init's old seed, -1, and cried out, and in
     * deathmatch charged a suicide, where the console is silent and scores
     * nothing. A lava death that met a refresh lost its mod 10 the same way.
     * All three are carried as `owner` and `env_next` are.
     *
     * They are NOT re-seeded here on a respawn, and that is the caller's job.
     * 0x8003DDF8 builds the new body from a FRESH entity — 0x8003B250 allocates
     * through 0x8006C098, which clears all 768 bytes (0x8006C18C `jal
     * 0x80089E18` with a1 = 0, a2 = 768), and then 0x8003DE34 stores 4 — so a
     * respawn is `q2_actor_init` followed by this, not this alone.
     *
     * The two protection deadlines are different. They ARE inventory state:
     * client+0xB0 and +0xB4 are `invuln_until` and `enviro_until`, respectively.
     * The damage actor is a projection of that client record, so carrying its
     * stale copies back through this refresh silently made both pickups visual
     * only. Reload them from the inventory below, every damage attempt.
     */
    owner   = a->owner;
    {
        s32 env     = a->env_next;
        s8  killer  = a->last_attacker;
        s16 mod     = a->last_mod;
        /* Carried for the same reason the mod is: the refresh runs on every
         * damage attempt, and a counter that restarted there would look like a
         * fresh hit to the pain read on the next tick. */
        u32 serial  = a->damage_serial;
        u8  effect[sizeof(a->effect)];
        u8  ambient[sizeof(a->ambient)];
        bool existing_player = a->has_client;

        memcpy(effect, a->effect, sizeof(effect));
        memcpy(ambient, a->ambient, sizeof(ambient));
        /* 0x8003B444 replaces the allocator's 0x40 with 0x20 for a new
         * gameplay body. Later refreshes retain any transient hit colour. */
        if (!existing_player)
            memset(ambient, Q2_PLAYER_AMBIENT_DEFAULT, sizeof(ambient));
        q2_actor_init(a);
        a->env_next      = env;
        a->last_attacker = killer;
        a->last_mod      = mod;
        a->damage_serial = serial;
        memcpy(a->effect, effect, sizeof(effect));
        /* entity+0x2AC — see q2_actor_from_monster. */
        memcpy(a->ambient, ambient, sizeof(ambient));
    }
    a->owner = owner;
    if (pos) {
        a->origin[0] = pos[0];
        a->origin[1] = pos[1];
        a->origin[2] = pos[2];
    }
    a->has_client = true;
    a->entity_kind = 47;

    /*
     * A PLAYER IS DAMAGEABLE, and once the sweep honours `takedamage` that has
     * to be said out loud rather than assumed from a non-zero health. id's
     * `PutClientInServer` sets `takedamage = DAMAGE_AIM`; the console's client
     * arm of the damage router reaches T_Damage through the same gate every
     * other entity does.
     */
    a->takedamage = Q2_DAMAGE_AIM;

    if (!inv)
        return;
    a->health       = inv->health;
    a->armour       = inv->armour;
    /*
     * WHICH ARMOUR, not always the weakest. This was a hardcoded 0, and 0 is
     * jacket — so a player wearing body armour absorbed 0.30 of an ordinary hit
     * instead of 0.80, and 0.00 of an energy hit instead of 0.60. Because this
     * runs on EVERY damage attempt the class could never survive one either:
     * there was no path by which the field could hold anything else.
     *
     * `q2_item_armour_amount` (0x8003733C) is what maintains the class, and it
     * is the same index the three-record table at 0x8009C5EC is read with.
     */
    a->armour_class = inv->armour_class;
    a->cells        = inv->ammo[Q2_AMMO_CELLS];
    /*
     * The power items live in the inventory's flag word, and
     * Q2_POWERUP_POWER_ARMOUR (0x18000) is exactly Q2_INV_POWER_SHIELD |
     * Q2_INV_POWER_SCREEN — the pair 0x80057AC4 tests together. Leaving this
     * zero meant q2_combat_power_armour_absorb returned at its first guard, so
     * the shield spent no cells and saved nothing.
     */
    a->powerups     = inv->flags;
    /* 0x800397FC: the player's own gib threshold, which playerdeath.h reads
     * from the same instruction. -100 here was a second, disagreeing copy. */
    a->gib_health   = Q2_PDEATH_GIB_HEALTH;
    a->invuln_until = inv->invuln_until;
    a->protect_until = inv->enviro_until;
}

void q2_actor_to_player(const q2_actor *a, q2_inventory *inv)
{
    if (!a || !inv)
        return;
    inv->health = a->health;
    inv->armour = a->armour;
    inv->ammo[Q2_AMMO_CELLS] = a->cells;
}

/* ------------------------------------------------------------------------- */
/* Armour                                                                     */
/* ------------------------------------------------------------------------- */
/* 0x800AEAA4 and 0x800AEAA8, which `bytes 0x800AEAA0 16` shows as
 * `00 00 00 00 | 00 80 20 00 | 80 80 80 00` — the flag byte and the two colour
 * words side by side. The fourth byte of each is padding the packer carries
 * into SetWinFade's colour argument and the draw never reads back. */
const u8 q2_actor_fade_power_rgb[3]  = { 0x00, 0x80, 0x20 };
const u8 q2_actor_fade_armour_rgb[3] = { 0x80, 0x80, 0x80 };

s16 q2_combat_power_armour_absorb(q2_actor *a, s32 damage)
{
    s32 save, cap;

    if (!a)
        return 0;

    /*
     * 0x80057AB0 `sb zero, 1184(gp)` is the FIRST instruction of the function,
     * ahead of every guard — so a hit that finds no shield, no cells or no
     * damage still clears the flag, and CheckArmour that follows it is free to
     * raise its own wash.
     */
    a->armour_fade = Q2_ACTOR_FADE_NONE;

    if (damage == 0 || !a->has_client)
        return 0;
    /* 0x80057AC0: both power items live in one bit pair of the powerup word. */
    if (!(a->powerups & Q2_POWERUP_POWER_ARMOUR))
        return 0;
    if (a->cells == 0)
        return 0;

    /* 0x80057AE0: `(damage * 2) / 3`, signed, truncating. */
    save = (s32)((u32)damage * Q2_POWER_ARMOUR_NUM) / Q2_POWER_ARMOUR_DEN;

    /* 0x80057AF4..0x80057B10 compares the SIGNED HALFWORD of twice the
     * cells. The later guards test zero, not positivity: negative damage
     * can restore cells, and the halfword stores wrap without a clamp. */
    cap = (s16)((u32)(u16)a->cells * 2u);
    if (save >= cap)
        save = cap;
    save = (s16)save;
    if (save == 0)
        return 0;

    /*
     * 0x80057B24 `sb v0, 1184(gp)` with v0 = 1, and 0x80057B80's SetWinFade
     * right behind it: the shield ate something, so the viewport gets the green
     * wash. Both happen BEFORE the cells come off at 0x80057B8C, which is the
     * order kept here even though nothing observes it.
     */
    a->armour_fade = Q2_ACTOR_FADE_POWER;

    /* 0x80057B8C: one cell per two points absorbed, truncating. */
    a->cells = (s16)(a->cells - (save / Q2_POWER_ARMOUR_CELLS));
    return (s16)save;
}

s16 q2_combat_armour_absorb(q2_actor *a, s32 damage, bool energy,
                            bool power_armour_fired,
                            const q2_combat_rules *rules)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    const q2_wt_armour *cls;
    s32 bias, factor, save;

    if (!a || damage == 0 || !a->has_client)
        return 0;
    if (a->armour == 0)
        return 0;
    if (a->armour_class >= Q2_WT_ARMOUR_CLASSES)
        return 0;

    /*
     * 0x80057C10 `lh v0, 0x800B334A` / 0x80057C18 `bne v0, zero, 0x80057C24`,
     * whose delay slot 0x80057C1C loads 2048 and whose fall-through 0x80057C20
     * loads 4095. That halfword is SKILL — it has a getter at 0x8007CCEC, the
     * skill-0 damage halving reads the same address at 0x800582D0, and the
     * place-list walker at 0x8007F538 keys the difficulty filter on it
     * (FORMATS.md §2.7). It is not
     * 0x800AEBCC, the deathmatch word this line used to read: that one is read
     * by the same function at 0x80057DA8 and 0x800580E0 for the killer byte and
     * the knockback cap, and nowhere near the bias.
     *
     * 4095 rounds every non-zero fraction up; 2048 rounds to nearest. So it is
     * EASY that gets the slightly stronger armour, not single player.
     */
    bias = (rules && rules->skill == 0) ? Q2_ARMOUR_BIAS_EASY
                                        : Q2_ARMOUR_BIAS_NORMAL;

    cls    = &t->armour[a->armour_class];
    factor = energy ? cls->energy_protection : cls->normal_protection;

    /* 0x80057C7C..0x80057CAC: low 32-bit product and logical shift, then
     * SIGN-EXTEND the low halfword before comparing with held armour.
     * Negative amounts are not refused: their negative absorption is
     * subtracted from armour, restoring it. Keep the full incoming word;
     * the preceding power stage can leave more than a signed halfword. */
    save = (s16)(((u32)bias + (u32)factor * (u32)damage) >> 12);
    if (save >= a->armour)
        save = a->armour;
    if (save == 0)
        return 0;

    /*
     * 0x80057CB8 `lbu v0, 1184(gp)` / 0x80057CC0 `bne v0, zero, 0x80057D2C`:
     * Armour_Fade gates the GREY WASH AND NOTHING ELSE. The branch lands on the
     * armour subtraction at 0x80057D2C, which is past the join, so a hit the
     * power shield already washed green still takes its armour points — and
     * only one of the two colours is ever on the screen for one hit.
     *
     * The parameter used to be discarded here under a comment saying it "only
     * suppresses the hit sound, not the maths". It does not: the hit sound is
     * chosen at 0x800584D4..0x800585A0 by MOD and never reads gp+1184, and
     * 0x80057CB8 is the only reader of that byte in the image.
     */
    if (!power_armour_fired)
        a->armour_fade = Q2_ACTOR_FADE_ARMOUR;

    a->armour = (s16)(a->armour - save);
    return (s16)save;
}

/* ------------------------------------------------------------------------- */
/* Knockback                                                                  */
/* ------------------------------------------------------------------------- */
static s32 isqrt64(s64 v)
{
    s64 lo = 0, hi = 0x7FFFFFFF, best = 0;

    if (v <= 0)
        return 0;
    while (lo <= hi) {
        s64 mid = lo + (hi - lo) / 2;
        if (mid * mid <= v) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (s32)best;
}

static void apply_knockback(q2_actor *attacker, q2_actor *target,
                            s16 damage, const s32 point[3],
                            const q2_combat_rules *rules)
{
    s64 dir[3];
    s32 len;
    s32 scale;
    s32 mass = rules ? rules->knockback_mass : 0;
    bool self = (attacker == target) && target->has_client;
    int i;

    dir[0] = (s64)target->origin[0] - point[0];
    dir[1] = (s64)target->origin[1] - point[1];
    dir[2] = (s64)target->origin[2] - point[2];

    /* 0x80057F28 normalises through 0x8008A588; the port does the same in
     * 1.3.12 so the multiply below keeps its scale. */
    len = isqrt64(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len <= 0)
        return;
    for (i = 0; i < 3; i++)
        dir[i] = (dir[i] * 4096) / len;

    /*
     * 0x80057F80: `125 * (mass + 64) / 64` for an ordinary hit, and
     * 0x80057FA8: `25 * (mass + 64) / 4` when a player hurt themselves.
     * The second is 3.2 times the first — the rocket jump.
     */
    if (self)
        scale = (25 * (mass + 64)) >> 2;
    else
        scale = (125 * (mass + 64)) >> 6;

    for (i = 0; i < 3; i++) {
        /*
         * 0x80057FC8..0x800580C4: `scale * unit[i] * damage / 2400 >> 4`, and
         * NOTHING divides the 1.3.12 scale back out — the unit vector's 4096 is
         * part of the impulse's magnitude. That is why the s16 clamps below are
         * reachable: anything past about 123 points of damage saturates.
         */
        s32 v = (s32)((u32)scale * (u32)dir[i]);
        v = (s32)((u32)v * (u32)(s32)damage) / Q2_KNOCKBACK_DIVISOR;
        v >>= Q2_KNOCKBACK_SHIFT;

        /* 0x800580E8: only the vertical component is floored, and only outside
         * deathmatch. World Y grows downward, so -3072 is a ceiling on how far
         * a blast underfoot can throw you — the single-player rocket jump has a
         * limit the deathmatch one does not. */
        if (i == 1 && !(rules && rules->deathmatch) && v < -3072)
            v = -3072;

        /* Retail writes +32768 on positive overflow, then stores a halfword.
         * This wraps to -32768; saturating to +32767 changes the native hit. */
        if (v >  32767) v =  32768;
        if (v < -32768) v = -32768;

        /* 0x80058188: a living target accumulates, a dead one is overwritten. */
        if (target->health > 0)
            target->knockback[i] = (s16)(target->knockback[i] + v);
        else
            target->knockback[i] = (s16)v;
    }

    if (target->health > 0)
        target->knocked = true;
}

/* ------------------------------------------------------------------------- */
/* The damage function                                                        */
/* ------------------------------------------------------------------------- */
q2_damage_result q2_combat_damage(q2_actor *attacker, q2_actor *target,
                                  s16 damage, s16 mod, const s32 point[3],
                                  const q2_combat_rules *rules)
{
    q2_damage_result out;
    q2_combat_rules local;
    s32 amount = damage;
    s16 saved_power = 0, saved_armour = 0;
    bool was_alive;

    memset(&out, 0, sizeof(out));

    if (!target)
        return out;
    if (!rules) {
        q2_combat_rules_default(&local);
        rules = &local;
    }

    was_alive = target->health > 0;
    target->last_mod = mod;
    /* The same store, counted: 0x80057E84 and 0x80057EBC write entity+0xDF on
     * every path through here, and the live player's think consumes it as its
     * PAIN animation request. See the note on the field. */
    target->damage_serial++;
    target->damage_source = attacker;

    /*
     * Who did it, so a scoring hook has a killer as well as a victim.
     *
     * THE DAMAGE FUNCTION NEVER WRITES -1 HERE. `access 0xDE` lists fourteen
     * instructions touching entity+222 in eight functions, and the only store
     * of a literal -1 is 0x800396DC, inside the death handler and only for mods
     * 9 and 10. What 0x80057D54 writes is an index: the attacker's client index
     * in deathmatch (0x80057E04), the attacker's own +222 byte when it has no
     * client (0x80057E68), or `(attacker - 0x800CBA28) / 768` in single player
     * (0x80057E88..0x80057EB8) — a chain I evaluated rather than assumed, and
     * with a NULL attacker it yields -2797289, whose low byte 0x17 makes the
     * `sb` store 23.
     *
     * So "-1 means the world" was a port convention with no instruction behind
     * it, and it fed a death voice that then cried out for every crusher, every
     * Soldier and every scripted hazard.
     *
     * SINGLE PLAYER is one arm, 0x80057E88..0x80057EB8, and it always stores.
     * The pool slot it computes is not modelled. `access 0xDE` over the whole
     * image finds four reads: this function's three of the ATTACKER's byte
     * (0x80057E54/0x80057E58, the deathmatch copy arm below, and 0x80057F38,
     * the knockback's self test, which `apply_knockback` answers by identity
     * instead) and the death handler's 0x800396EC. In single player that last
     * one only asks whether the byte is -1 (0x80039728; the frag hook sits
     * behind 0x80039764's deathmatch test), and any other value answers it the
     * same way. So a client attacker's `owner` and, for anything else,
     * Q2_MP_NOT_A_PLAYER (4) — what the projectile spawners store for an owner
     * with no client block (0x8004A208, 0x8004ABF4, 0x8004B1C4, 0x8004BF80) —
     * stand in for the slot index and for the 23 a NULL attacker makes.
     *
     * DEATHMATCH has five arms, and two of them store nothing:
     *
     *     80057DB8  bne   s5, zero, 0x80057E04   ; an attacker -> its arms
     *     80057DC0  beq   s0, zero, 0x80057EBC   ; no target client: NO STORE
     *     80057DC8  addiu v1, v1, 31840          ; 0x800C7C60, the client base
     *     80057DCC  subu  v1, s0, v1             ; the TARGET's client block...
     *       ...                                  ; ...times the inverse of 7,
     *     80057E00  sra   v0, v0, 5              ; >> 5: / 224, the client stride
     *     80057E04  lw    v0, 12(s5)             ; the attacker's client block
     *     80057E0C  beq   v0, zero, 0x80057E50   ; none -> the copy arm
     *     80057E48  j     0x80057EB8             ; its index, the same divide
     *     80057E54  lb    v0, 222(s5)            ; the ATTACKER's own byte
     *     80057E5C  beq   v0, v1, 0x80057E6C     ; v1 = 4 (0x80057E50): print,
     *                                            ; and NO STORE
     *     80057E68  sb    a0, 222(s2)            ; otherwise hand it on
     *     80057EB8  sb    v0, 222(s2)
     *
     * s0 is the target's client (0x80057D94), so a NULL attacker credits the
     * player it hurt with their OWN index — the same divide 0x800396E0 uses for
     * the victim. A crusher (0x80051E74) or a scripted hit (0x80027858) that
     * kills in deathmatch therefore reaches 0x80039774 with killer == victim:
     * it passes `slti s1, 4`, the hook is called, and the module charges a
     * suicide. It does not cry out, because the byte is not -1. `owner` is this
     * port's name for a client's index (combat.h); a client actor without one
     * records 4.
     *
     * The copy arm is how a creature's kill is scored: the creature's own byte
     * is whoever last hurt IT (0x80057E04 wrote a player's index there when a
     * player shot it), so a creature that kills player A after player B shot
     * it hands B the frag. A creature nobody has hurt holds 4 here —
     * `q2_actor_init`'s seed — and takes the "can't determine which player hit
     * other player" arm (0x800ACD9C through the hook at 0x800B2FE8, not
     * reproduced), which leaves the victim's byte as it was. INFERRED, and a
     * known difference: the console's creature holds 0 until it is first hurt,
     * because nothing in `access 0xDE` writes a fresh creature's byte and the
     * pool allocator clears the entity (0x8006C18C `jal 0x80089E18`, 768
     * bytes), which would copy a 0 and hand player 0 the frag.
     *
     * Leaving the byte UNWRITTEN only means something because the two
     * refreshes now carry it (q2_actor_from_player, q2_actor_from_monster).
     * `last_mod` has no such arms: 0x80057E84 and 0x80057EBC store +223 on
     * every path, and it was written above.
     */
    if (!rules->deathmatch) {
        target->last_attacker = (attacker && attacker->owner >= 0)
                                    ? attacker->owner
                                    : (s8)Q2_MP_NOT_A_PLAYER;
    } else if (!attacker) {
        if (target->has_client)                         /* 0x80057DC8 */
            target->last_attacker = (target->owner >= 0)
                                        ? target->owner
                                        : (s8)Q2_MP_NOT_A_PLAYER;
        /* else 0x80057DC0: no store. */
    } else if (attacker->has_client) {                  /* 0x80057E14 */
        target->last_attacker = (attacker->owner >= 0)
                                    ? attacker->owner
                                    : (s8)Q2_MP_NOT_A_PLAYER;
    } else if (attacker->last_attacker != (s8)Q2_MP_NOT_A_PLAYER) {
        target->last_attacker = attacker->last_attacker;   /* 0x80057E68 */
    }
    /* else 0x80057E6C..0x80057E84: the print, and no store. */

    /*
     * T_Damage's zero arm at 0x80062940 skips subtraction/death but reaches
     * M_ReactToDamage. Preserve that event even though health did not change.
     * Player zero damage still runs the outer protection/cheat/sound stages:
     * 0x80057EC0 skips only knockback. Negative damage is nonzero and reaches
     * the same signed absorption, subtraction and effect stages as a hit.
     */
    if (amount == 0 && !target->has_client) {
        if (target->takedamage)
            target->damage_reaction_pending = true;
        goto effect_tail;
    }
    /*
     * Knockback comes first and does not care about armour: 0x80057EC0 runs
     * before any absorption. Its only two guards are 0x80057EC0 `beq s4, zero`
     * — s4 is the ORIGINAL damage, copied at 0x80057D7C and not overwritten
     * with 8 until 0x80058354 — and 0x80057EC8 `beq s6, zero`, no point.
     *
     * There is no flags test here. FL_NO_KNOCKBACK used to gate this line; it
     * gates T_Damage's own knockback argument instead, which its single caller
     * passes as zero. See combat.h's note on `no_knockback`.
     */
    if (damage != 0 && point && q2_mod_knocks_back(mod))
        apply_knockback(attacker, target, damage, point, rules);

    /*
     * 0x80062838 `lw v1, 28(s1)` / 0x80062844 `and v1, 0xC0000000` /
     * 0x80062848 `beq v1, zero, 0x80062B54` — T_Damage's first real test,
     * before its own halving (0x80062888), the surprise doubling (0x800628B8),
     * the flags (0x80062914), the health subtraction (0x80062958), pain
     * (0x80062AEC) and die (0x80062A9C).
     *
     * It sits HERE and not at the top of the function because the outer
     * function has already recorded the killer and the impulse by the time
     * control reaches T_Damage: the routing is 0x800582C8 -> 0x8005842C ->
     * 0x800584B4, and the knockback block is 0x80057EC0..0x80058204.
     *
     * And it is guarded on the target having no client, because 0x800582C8
     * `beq s0, zero, 0x8005842C` diverts a client target away from T_Damage
     * entirely — the bit is never consulted for a player.
     *
     * NOT UNIVERSAL, and the port is coarser than the disc here: 0x8005842C
     * `lw v0, 748(s2)` tests the target's entity back-pointer at +0x2EC first,
     * and a client-less target that has none takes its damage at 0x800584C4
     * with no gate at all. This port's actor model has no equivalent of
     * entity+0x2EC on the target side, so the gate is applied to every
     * client-less target. INFERRED that the difference does not matter: nothing
     * here damages a target that has been detached from its entity.
     *
     * The zeroed result is deliberate — 0x80062B54 is the bare epilogue, not a
     * refusal, so `blocked` (which this port uses for invulnerability and the
     * environmental throttle) stays false.
     *
     * AND IT IS NOT A RETURN FROM THIS FUNCTION. T_Damage's epilogue returns to
     * 0x800584BC `j 0x800584D4`, whose client test sends a client-less target
     * on to 0x800585A4 — the effect-timer tail. Every path with a target
     * reaches it except the refusals that branch straight to the epilogue at
     * 0x80058608: acid's two protection tests (0x8005823C, 0x80058250), the
     * two throttles (0x80058264, 0x800582A8) and the general invulnerability
     * test (0x80058318). So a target that cannot be hurt still has its mod's
     * effect byte armed — a bolt stores +0x2F1 = 3 (0x800585E8) whatever
     * T_Damage decided.
     */
    if (!target->has_client && !target->takedamage)
        goto effect_tail;                   /* 0x800584D4 -> 0x800585A4 */

    /*
     * 0x800582C8: at skill 0, a monster hitting a player does half. The test is
     * on the ATTACKER having no client block, which is what makes it "a monster
     * hit you" rather than "you were hurt".
     *
     * AND IT ROUNDS UP, which is the whole of the arithmetic and was the one
     * figure in this file carrying no instruction behind it. The prologue fixes
     * the registers — 0x80057D5C `addu s5, a0` is the attacker, 0x80057D64
     * `addu s2, a1` the target, 0x80057D6C `addu s1, a2` the damage, and
     * 0x80057D94 `lw s0, 12(s2)` the target's client block — and the four
     * guards and the halving read:
     *
     *     800582C8  beq   s0, zero, ...      ; the target has a client
     *     800582D0  lh    v0, 0x800B334A     ; the skill halfword
     *     800582D8  bne   v0, zero, ...      ; only at skill 0
     *     800582E0  lw    v0, 748(s5)        ; attacker+0x2EC, its entity
     *     800582E8  beq   v0, zero, ...
     *     800582F0  lw    v0, 12(s5)         ; the attacker's client block
     *     800582F8  bne   v0, zero, ...      ; only when it has none
     *     800582FC  addiu v0, s1, 1          ; delay slot: damage + 1
     *     80058300  sra   s1, v0, 1          ; damage = (damage + 1) >> 1
     *
     * The +1 is UNCONDITIONAL. A compiler's signed `/2` would have emitted the
     * sign-bit idiom instead — `srl v0, x, 31; addu; sra` — and there is no
     * such term here, so this is `(damage + 1) >> 1` in the source and not a
     * truncating divide. 25 points of monster damage arrive as 13, not 12,
     * which also gives id's "never rounds to nothing" for free.
     *
     * The third guard is the one thing not reproduced: `attacker+0x2EC` is the
     * actor's entity back-pointer (monster.h), zeroed at 0x8007F0DC when a body
     * stops being a creature, so the console additionally refuses the halving
     * for an attacker that has become a corpse. `attacker != NULL` stands in
     * for it; nothing in this port damages anyone from a detached actor.
     */
    if (rules->skill == 0 && target->has_client &&
        attacker && !attacker->has_client)
        amount = (amount + 1) >> 1;

    /*
     * THE ENVIRONMENT SUIT IS NOT GENERAL PROTECTION, and treating it as such
     * made it god mode.
     *
     * 0x80057D94 `lw s0, 12(s2)` is the target's client block and 0x80057DA0
     * `addiu s7, s0, 172` fixes s7, so s7+4 is client+0xB0 (invulnerability)
     * and s7+8 is client+0xB4 (the suit). This function reads client+0xB4 at
     * exactly one instruction — 0x80058230 `lw v0, 8(s7)` — and it is inside
     * the mod-9 arm. The one general protection test, 0x80058304..0x80058318,
     * reads s7+4 and nothing else. Blocking rockets, rails, bullets, melee,
     * crush, falling and lava on client+0xB4 was a fiction. (It is not the
     * word's only reader in the image: the status bar's powerup walk reads
     * client+0xAC/+0xB0/+0xB4 in turn to draw a countdown, the last at
     * 0x80035CB0 `lw a1, 8(v1)` with v1 set to client+0xAC at 0x80035B5C.
     * That one draws; it does not refuse damage.)
     *
     * The dispatch is three instructions — 0x80058208 `beq s3, 9`, 0x80058210
     * `beq s3, 10`, 0x80058218 `j 0x800582C8` — so the two arms below are the
     * whole of what distinguishes an environmental mod, and every other mod
     * reaches the general test directly.
     *
     * (The arms are transcribed AFTER the skill halving because the halving
     * cannot reach them: 0x800582D8 only fires when the attacker has no client
     * and 0x800582E0 requires a live attacker entity, while acid and lava come
     * from 0x8002E4B0/0x8002E524 with none. Retail runs the arms first; the
     * result is identical and this ordering keeps the halving next to its own
     * citation.)
     */
    if (mod == Q2_MOD_ACID && target->has_client) {
        /*
         * 0x80058220..0x80058288, in the executable's own order.
         *   80058230  lw   v0, 8(s7)   / sltu v0, v1, v0 / bne -> epilogue
         *   80058244  lw   v0, 4(s7)   / sltu v0, v1, v0 / bne -> epilogue
         *   80058258  lw   v0, 148(s0) / sltu v0, v0, v1 / beq -> epilogue
         *   80058268  addiu v0, v1, 400, stored by 0x80058278
         *
         * Both protection tests come BEFORE the throttle, so acid does not
         * re-arm client+148 while you are invulnerable.
         *
         * ALL THREE ARE `sltu`, so all three compare UNSIGNED, as statusbar.c
         * already does for the same deadlines. A signed `<` agrees only while
         * both words are below 0x80000000; past it a deadline reads as the
         * distant past, and a clock as before everything.
         *
         * The burn sound: 0x8005826C `lw a0, 40(s0)` takes the
         * client's entity and, when there is one, 0x8005827C `jal 0x8003DEE4`
         * plays the handle at gp+17116 (0x800B28DC) on it through 0x8007270C,
         * on every acid hit the throttle lets through. The world hook below
         * resolves that owner and queues the request, even if armour later
         * absorbs all damage.
         */
        if ((u32)rules->level_time < (u32)target->protect_until) {
            out.blocked = true;
            return out;
        }
        if ((u32)rules->level_time < (u32)target->invuln_until) {
            out.blocked = true;
            return out;
        }
        /* 0x80058260 is `sltu env_next, level_time` and the branch on zero
         * refuses, so the hit is allowed only when env_next < level_time
         * STRICTLY. A hit on the tick where the two are equal is refused; this
         * used to read `level_time < env_next`, one tick the other way. */
        if (!((u32)target->env_next < (u32)rules->level_time)) {
            out.blocked = true;
            return out;
        }
        target->env_next = rules->level_time + Q2_ENV_THROTTLE_ACID;
        if (rules->environment_sound)
            rules->environment_sound(rules->environment_sound_user, target, mod);
    } else if (mod == Q2_MOD_LAVA && target->has_client) {
        /*
         * 0x8005828C..0x800582C4 reads NEITHER timer — the throttle alone,
         * with the same strict, unsigned `sltu` at 0x800582A4 and 100 ticks at
         * 0x800582AC. It also stores the new deadline BEFORE the general
         * invulnerability test below can refuse the damage, which is a real
         * asymmetry with the acid arm and not a transcription slip.
         *
         * 0x800582C0 `jal 0x8003DF0C` bumps
         * the counter at 0x800AE898 and plays the same gp+17116 handle only
         * when `andi 3` leaves zero (0x8003DF20). It starts at 3, so the
         * first admitted hit sounds, then every fourth. The world hook owns
         * this process-wide cadence across players and level loads.
         */
        if (!((u32)target->env_next < (u32)rules->level_time)) {
            out.blocked = true;
            return out;
        }
        target->env_next = rules->level_time + Q2_ENV_THROTTLE_LAVA;
        if (rules->environment_sound)
            rules->environment_sound(rules->environment_sound_user, target, mod);
    }

    /*
     * The one general protection test, for every mod including 9 and 10:
     *   80058304  lw   v0, 0x800AEBAC   ; level time
     *   8005830C  lw   v1, 4(s7)        ; client+0xB0, invulnerability
     *   80058314  sltu v0, v0, v1
     *   80058318  bne  v0, zero, 0x80058608
     * client+0xB4 does not appear here. Unsigned, as the arms above are.
     */
    if (target->has_client &&
        (u32)rules->level_time < (u32)target->invuln_until) {
        out.blocked = true;
        return out;
    }

    /* Armour. Mod 8 is the one class that skips both stages (0x80058358). */
    if (mod != Q2_MOD_NO_ARMOUR) {
        saved_power = q2_combat_power_armour_absorb(target, amount);
        amount -= saved_power;

        saved_armour = q2_combat_armour_absorb(target, amount,
                                               q2_mod_is_energy(mod),
                                               saved_power != 0, rules);
        amount -= saved_armour;
    }

    out.absorbed_power  = saved_power;
    out.absorbed_armour = saved_armour;
    if (!target->has_client)
        target->damage_reaction_pending = true;

    /*
     * NO EXIT WHEN ARMOUR TAKES IT ALL. 0x80058390 `subu s1, s1, v0` runs
     * straight into 0x80058394's cheat test with no branch on s1, so a hit the
     * two stages soak completely still goes through ONE SHOT KILL, the health
     * store, the hit sound and the effect tail. That is reachable, not
     * theoretical: at skill 1 body armour saves (2048 + 3277*d) >> 12, which
     * is all of a 1- or 2-point hit, and at skill 0 all of anything up to 5.
     *
     * An early return here used to skip the four. With the cheat on, the
     * console stores -0 = 0 over a living player's health (0x800583E0) — a
     * kill, and 0x800584FC's `blez` then keeps the hit sound silent — where
     * the port left health alone and played it. And a bolt the armour soaked
     * never lit the player (0x800585E8's +0x2F1 = 3).
     */

    /*
     * Both of these sit INSIDE T_Damage (0x800627F8), after the absorption the
     * caller has already done — so they scale what armour left, not what the
     * weapon started with. It makes no difference to a creature, which has no
     * armour, and it would to a player who ever acquired SVF_MONSTER.
     *
     * The surprise bonus, 0x800628C8-0x80062910: a monster that has not
     * acquired an enemy takes double from an attacker with a client block,
     * while it is still alive. Four conditions, in the original's order.
     */
    if (target->is_monster && !target->has_enemy && was_alive &&
        attacker && attacker->has_client) {
        amount *= 2;
        out.surprised = true;
    }

    /*
     * 0x8006292C: godmode zeroes the damage. The engine's exemption is a
     * dflags bit (0x20) that no caller in this port sets.
     *
     * T_DAMAGE'S, SO NOT A PLAYER'S. The bit is tested at 0x80062924 `andi
     * v0, v1, 0x10` / 0x8006292C `beq`, inside T_Damage, on the flags word
     * 0x80062914 loads, and 0x800582C8 never lets a client target reach
     * T_Damage — so this function never consults it for a player, and it
     * used to exempt one from the health store. Nor is it a return: T_Damage's
     * zero sends it on to 0x80062AAC and its epilogue, and the outer function
     * still reaches the effect tail.
     */
    if (!target->has_client && target->godmode) {
        out.surprised = false;
        goto effect_tail;                   /* 0x800584D4 -> 0x800585A4 */
    }

    /*
     * ONE SHOT KILL, 0x80058394..0x800583F8. The bit is 0x80 of the GAME
     * VARIABLES word at 0x800B29EC, folded in by 0x8001C774 from the menu row.
     *
     *   80058398  lhu   v0, 0x800B29EC
     *   800583A0  andi  v0, v0, 0x80
     *   800583A4  beq   v0, zero, 0x800583EC   ; off -> the ordinary subtract
     *   800583AC  beq   s3, zero, 0x800583EC   ; mod 0 excluded
     *   800583B4  beq   s3, s4, 0x800583EC     ; s4 = 8, set at 0x80058354
     *   800583BC  beq   s3, 19, 0x800583EC
     *   800583C4  beq   s3, 10, 0x800583EC
     *   800583CC  beq   s3, 9,  0x800583EC
     *   800583D4  lh    v0, 264(s2)            ; health, signed
     *   800583DC  bgtz  v0, 0x800583F8
     *   800583E0  subu  v0, zero, s1           ; delay slot: v0 = -damage
     *   800583F8  sh    v0, 264(s2)
     *
     * s1 is the POST-ARMOUR amount — power armour ran at 0x80058364 and armour
     * at 0x80058380 — so this lands at minus what actually got through.
     *
     * IT ONLY EVER HURTS THE PLAYER. The whole block sits past 0x800582C8's
     * `beq s0, zero, 0x8005842C`, which sends every client-less target to
     * T_Damage instead, so the cheat makes any hit fatal TO YOU and does
     * nothing to your shots. `out.taken` stays `amount`: retail does not change
     * what the hit is worth, only where health lands.
     *
     * The neighbouring `& 0x2` arm at 0x800583FC..0x80058428 — health clamped
     * back to 200 whenever the subtraction would leave it non-positive — is
     * DEAD and is deliberately not reproduced. 0x8001C698 is the only writer of
     * 0x800B29EC in the image and it ORs in 0x40, 0x1, 0x20 and 0x80 and
     * nothing else, so bit 0x2 is never set.
     */
    if (target->has_client && (rules->cheats & Q2_CHEAT_ONE_SHOT_KILL) &&
        mod != Q2_MOD_NONE && mod != Q2_MOD_NO_ARMOUR &&
        mod != Q2_MOD_19 && mod != Q2_MOD_LAVA && mod != Q2_MOD_ACID &&
        target->health > 0)
        target->health = (s16)-amount;      /* 0x800583E0 */
    else
        target->health = (s16)(target->health - amount);
    out.taken = (s16)amount;

    set_hit_sound(target, mod, &out);

    if (target->health <= 0) {
        /*
         * `killed` is the TRANSITION and stays behind `was_alive`; the other
         * two are properties of the hit and were wrongly behind it.
         *
         * THE FLOOR RUNS ON EVERY health<=0 OUTCOME, not only the first.
         * 0x800629B4 sits after the subtraction with no already-dead test in
         * front of it, and leaving it inside the transition let a corpse's s16
         * run down without limit: measured, 100 rockets at 300 points take a
         * body to -30040 and 110 take it to **+32496** — the field wraps and
         * the creature is alive again. The console cannot reach that.
         *
         * `gibbed` likewise. The module's own `die` does the real test — the
         * Soldier's at module+0x2324, before its already-dead guard — but this
         * flag is what the client and the effects read, so it has to be true
         * on the hit that takes an already-dead body past `gib_health` and not
         * only on the hit that killed it.
         *
         * AND THE FLOOR IS T_DAMAGE'S, so a player never meets it. 0x800629B4
         * is inside T_Damage (0x800627F8..0x80062B7C), whose one caller
         * (`xrefs 0x800627F8`), 0x800584B4, is on the client-less route; the
         * client arm's store is 0x800583EC `lhu` / 0x800583F4 `subu` /
         * 0x800583F8 `sh`, and nothing between it and the epilogue bounds it
         * from below. A player's health runs as far below zero as the hit
         * takes it, which is also what q2_inventory_apply_damage stores.
         */
        if (was_alive)
            out.killed = true;

        out.gibbed = target->health <= target->gib_health;

        if (!target->has_client && target->health < Q2_HEALTH_FLOOR)
            target->health = (s16)Q2_HEALTH_FLOOR;
    }

effect_tail:
    /*
     * 0x800585A4..0x80058604, which every path reaches bar the refusals that
     * branch to 0x80058608 (see the takedamage gate above): a fully absorbed
     * or zero/negative hit, a target T_Damage
     * turned away, and a godmode body all arm the mod's byte.
     */
    {
        int slot;
        s16 v = q2_mod_effect_timer(mod, &slot);
        if (slot >= 0 && slot < (int)(sizeof(target->effect)))
            target->effect[slot] = (u8)v;
    }

    return out;
}

/* ------------------------------------------------------------------------- */
/* Radius damage                                                              */
/* ------------------------------------------------------------------------- */
s16 q2_combat_splash_at(s16 damage, s32 dist)
{
    s32 loss = (s32)(((s64)dist * Q2_SPLASH_FALLOFF_NUM) >>
                     Q2_SPLASH_FALLOFF_SHIFT);
    s32 v = (s32)damage - loss;
    return v > 0 ? (s16)v : 0;
}

u32 q2_combat_radius_damage(q2_actor *attacker, q2_actor *ignore,
                            const s32 point[3], s16 damage, s16 radius,
                            s16 mod, q2_actor **targets, u32 count,
                            const q2_combat_rules *rules)
{
    return q2_combat_radius_damage_traced(attacker, ignore, point, damage,
                                          radius, mod, targets, count, rules,
                                          NULL, NULL);
}

u32 q2_combat_radius_damage_traced(q2_actor *attacker, q2_actor *ignore,
                                   const s32 point[3], s16 damage, s16 radius,
                                   s16 mod, q2_actor **targets, u32 count,
                                   const q2_combat_rules *rules,
                                   q2_combat_clear_fn clear, void *ctx)
{
    u32 hurt = 0, i;

    if (!point || !targets || damage <= 0 || radius <= 0)
        return 0;

    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 dx, dy, dz, d2;
        s64 reach;
        s32 dist;
        s16 points;

        if (!t || t == ignore)
            continue;

        dx = (s64)t->origin[0] - point[0];
        dy = (s64)t->origin[1] - point[1];
        dz = (s64)t->origin[2] - point[2];
        d2 = dx * dx + dy * dy + dz * dz;

        /* 0x800509AC: the comparison radius is the blast plus the target's own,
         * so a large creature is caught by a blast that misses its centre. */
        reach = (s64)radius + t->radius;
        if (d2 > reach * reach)
            continue;

        dist   = isqrt64(d2);
        points = q2_combat_splash_at(damage, dist);

        /*
         * 0x80050A04 `bne a2, s4, 0x80050A10` / 0x80050A0C `sra s0, s0, 1`:
         * the blast's own owner takes half. s4 is the owner the caller passed
         * in a0 and a2 is the candidate, freshly reloaded at 0x800509FC.
         *
         * The shift sits BETWEEN the subtraction at 0x80050A08 — which is in
         * the branch's delay slot, so it always runs — and the `blez s0` reject
         * at 0x80050A10. So a self-hit that falls off to 1 point halves to zero
         * and is rejected outright, and the order of the two is load-bearing.
         *
         * The owner is swept at all because all six projectile call sites store
         * zero in the exclude-owner slot (0x800508BC/0x800508C4 read it); only
         * the generic helper at 0x80050CC4 passes non-zero.
         */
        if (t == attacker)
            points = (s16)(points >> 1);

        if (points <= 0)
            continue;

        /*
         * 0x80050A24 and 0x80050A3C, both between the falloff and the damage
         * call, both skipping the candidate on a zero: the swept move
         * 0x80044C44 (q2_coll_move) from the blast to the candidate through the
         * hull, then the clip 0x80053974 of the same segment against the
         * 48-slot entity-box table at 0x800CAE10. One callback answers for
         * both; see combat.h. A NULL callback is "everything is visible".
         */
        if (clear && !clear(ctx, point, t->origin))
            continue;

        q2_combat_damage(attacker, t, points, mod, point, rules);
        hurt++;
    }

    return hurt;
}

/* ------------------------------------------------------------------------- */
/* Tracing                                                                    */
/* ------------------------------------------------------------------------- */
s64 q2_combat_ray_dist_sq(const s32 origin[3], const s32 dir[3],
                          const s32 point[3], s64 *out_along)
{
    s64 vx, vy, vz;
    s64 len2, dot, along;
    s64 cx, cy, cz;

    if (out_along)
        *out_along = 0;
    if (!origin || !dir || !point)
        return 0;

    vx = (s64)point[0] - origin[0];
    vy = (s64)point[1] - origin[1];
    vz = (s64)point[2] - origin[2];

    len2 = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] + (s64)dir[2] * dir[2];
    if (len2 <= 0)
        return vx * vx + vy * vy + vz * vz;

    dot = (s64)dir[0] * vx + (s64)dir[1] * vy + (s64)dir[2] * vz;

    /* Fraction along the ray, 1.0.12 — 4096 is the far end. Nothing is
     * normalised because the direction's LENGTH is the weapon's range. */
    along = (dot * 4096) / len2;
    if (out_along)
        *out_along = along;

    cx = vx - ((s64)dir[0] * along) / 4096;
    cy = vy - ((s64)dir[1] * along) / 4096;
    cz = vz - ((s64)dir[2] * along) / 4096;

    return cx * cx + cy * cy + cz * cz;
}

/*
 * Where a shot stopped considering a target. "It missed" has five causes and
 * only one of them is aim; counting them apart is the difference between fixing
 * the right thing and guessing three times, which is what this cost.
 */
q2_combat_scan_stats q2_combat_scan;
q2_combat_scan_stats q2_combat_scan_by[Q2_COMBAT_SCAN_SLOTS];
int                  q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;

/* Both the total and the shooter's own slot, so neither has to be derived. */
#define SCAN_BUMP(field)                                                          do {                                                                              q2_combat_scan.field++;                                                       if (q2_combat_scan_who >= 0 &&                                                    q2_combat_scan_who < Q2_COMBAT_SCAN_SLOTS)                                    q2_combat_scan_by[q2_combat_scan_who].field++;                        } while (0)

static s64 trace_isqrt(s64 v)
{
    s64 lo = 0, hi = 3037000499LL, best = 0;

    if (v <= 0)
        return 0;
    if (hi > v)
        hi = v;
    while (lo <= hi) {
        s64 mid = lo + (hi - lo) / 2;
        if (mid == 0 || mid <= v / mid) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

/*
 * Normal retail inputs stay well inside this exact-integer envelope. A bullet
 * direction is the 4096 aim scaled by four (plus sub-4096 spread), rail is the
 * unscaled aim, and a target which survived the broad box can be only one
 * segment length plus its 286/429-unit radius away. Projectile tick segments
 * are shorter still. Keeping each horizontal term <= 30000 bounds cross^2,
 * radius^2*a, the discriminant and the fixed-root numerator below to s64.
 *
 * The public host API nevertheless accepts arbitrary s32 coordinates. Those
 * use the long-double arm instead of invoking signed-overflow UB; that arm is
 * not reached by retail-scale gameplay.
 */
#define Q2_TRACE_EXACT_TERM_MAX 30000

static bool trace_term_is_exact(s64 v)
{
    if (v < 0)
        v = -v;
    return v <= Q2_TRACE_EXACT_TERM_MAX;
}

static s64 trace_root_from_long_double(long double v)
{
    if (v >= 2147483647.0L)
        return 2147483647;
    if (v <= -2147483647.0L)
        return -2147483647;
    return (s64)v;               /* C truncates toward zero, as 0x800B11D8 */
}

/* 0x800546B0..0x80054704 rejects a centre whose dot along the sweep is not
 * positive. Only the sign is needed, so do not route arbitrary host s32 input
 * through q2_combat_ray_dist_sq's dot*4096 fraction. */
static bool actor_centre_is_ahead(const s32 origin[3], const s32 dir[3],
                                  const q2_actor *t)
{
    return q2_combat_centre_is_ahead(origin, dir, t->origin);
}

bool q2_combat_centre_is_ahead(const s32 origin[3], const s32 dir[3],
                               const s32 centre[3])
{
    s64 rel[3];
    int k;
    bool exact = true;

    for (k = 0; k < 3; k++) {
        rel[k] = (s64)centre[k] - origin[k];
        if (!trace_term_is_exact(rel[k]) || !trace_term_is_exact(dir[k]))
            exact = false;
    }
    if (exact) {
        s64 dot = (s64)dir[0] * rel[0] + (s64)dir[1] * rel[1] +
                  (s64)dir[2] * rel[2];
        return dot > 0;
    }
    return (long double)dir[0] * rel[0] +
           (long double)dir[1] * rel[1] +
           (long double)dir[2] * rel[2] > 0.0L;
}

/*
 * The literal narrow phase in 0x800544EC.
 *
 * X/Z solve a quadratic against entity+0x94, producing the two entry/exit
 * roots in 1.0.12. Y is not part of that distance: it produces its own slab
 * interval, and the function intersects the two. That distinction is visible
 * at a box corner and after the corpse volume is made wider and shorter.
 */
/*
 * The same narrow phase with the actor unpacked, so a caller that is clipping a
 * MOVE rather than a shot can share it — see `q2_combat_cylinder_interval` in
 * combat.h. trace.h's rule about five copies of a slab test being how they
 * drift apart applies here exactly.
 */
static bool cylinder_interval(const s32 origin[3], const s32 dir[3],
                              const s32 centre[3], s32 radius_in, s16 height,
                              s64 *out_enter, s64 *out_exit);

bool q2_combat_cylinder_interval(const s32 origin[3], const s32 dir[3],
                                 const s32 centre[3], s32 radius, s16 height,
                                 s64 *out_enter, s64 *out_exit)
{
    return cylinder_interval(origin, dir, centre, radius, height,
                             out_enter, out_exit);
}

static bool actor_cylinder_interval(const s32 origin[3], const s32 dir[3],
                                    const q2_actor *t, s32 fallback_radius,
                                    s64 *out_enter, s64 *out_exit)
{
    s16 height;

    if (!origin || !dir || !t)
        return false;

    /* The Y slab's height is entity+0x96, and an actor built from a box rather
     * than from a live entity has it as the box instead. Resolved here so the
     * unpacked primitive below takes one number. */
    height = t->height > 0 ? t->height : (s16)(t->maxs[1] - t->mins[1]);

    return cylinder_interval(origin, dir, t->origin,
                             t->radius > 0 ? t->radius : fallback_radius,
                             height, out_enter, out_exit);
}

static bool cylinder_interval(const s32 origin[3], const s32 dir[3],
                              const s32 centre[3], s32 radius_in, s16 height,
                              s64 *out_enter, s64 *out_exit)
{
    /* `disc` would shadow the disc type; this is a quadratic discriminant. */
    s64 ox, oz, a, b, discriminant;
    s64 h_enter, h_exit, v_enter, v_exit;
    s64 radius;
    s64 ymin, ymax;

    if (!origin || !dir || !centre)
        return false;

    radius = radius_in;
    if (radius <= 0)
        return false;

    /* 0x800545F4..0x8005467C: the cheap swept-box rejection precedes the
     * quadratic. Besides saving the solve, this defines the degenerate
     * vertical-ray case where the horizontal quadratic has a == 0. */
    {
        s64 end[3];
        s64 xmin, xmax, zmin, zmax;

        end[0] = (s64)origin[0] + dir[0];
        end[1] = (s64)origin[1] + dir[1];
        end[2] = (s64)origin[2] + dir[2];
        xmin = origin[0] < end[0] ? origin[0] : end[0];
        xmax = origin[0] > end[0] ? origin[0] : end[0];
        zmin = origin[2] < end[2] ? origin[2] : end[2];
        zmax = origin[2] > end[2] ? origin[2] : end[2];
        if (xmax < (s64)centre[0] - radius ||
            xmin > (s64)centre[0] + radius ||
            zmax < (s64)centre[2] - radius ||
            zmin > (s64)centre[2] + radius)
            return false;
    }

    ox = (s64)origin[0] - centre[0];
    oz = (s64)origin[2] - centre[2];

    if (dir[0] == 0 && dir[2] == 0) {
        /* The retail solver explicitly returns 0..4096 here. The broad box
         * above is therefore the exact horizontal test for a vertical ray. */
        h_enter = -(s64)0x7FFFFFFF;
        h_exit  =  (s64)0x7FFFFFFF;
    } else if (trace_term_is_exact(dir[0]) &&
               trace_term_is_exact(dir[2]) &&
               trace_term_is_exact(ox) && trace_term_is_exact(oz) &&
               trace_term_is_exact(radius)) {
        s64 cross;
        s64 root;
        s64 denom;

        a = (s64)dir[0] * dir[0] + (s64)dir[2] * dir[2];
        b = 2 * ((s64)dir[0] * ox + (s64)dir[2] * oz);

        cross = (s64)dir[0] * oz - (s64)dir[2] * ox;
        /* b^2 - 4ac == 4(r^2*a - cross^2). This equivalent form avoids
         * overflowing on the two large, nearly cancelling b^2/4ac terms. */
        /* Test the unscaled discriminant first. A legal host actor can have a
         * broad-box-overlapping but very off-axis centre; multiplying that
         * negative value by four before rejecting it can overflow s64 even
         * though all retail-scale hits remain exact. The positive arm is
         * bounded by Q2_TRACE_EXACT_TERM_MAX and is safe to scale. */
        discriminant = radius * radius * a - cross * cross;

        /* 0x80054768 uses a strict comparison for the tangent case. */
        if (discriminant <= 0)
            return false;
        discriminant *= 4;

        root  = trace_isqrt(discriminant);
        denom = 2 * a;
        /* 0x800B11D8 receives +/-2048 and divides by a, i.e. these
         * (-b +/- sqrt(d)) * 4096 / (2a) roots, truncated toward zero. */
        h_enter = ((-b - root) * 4096) / denom;
        h_exit  = ((-b + root) * 4096) / denom;
    } else {
        long double la = (long double)dir[0] * dir[0] +
                         (long double)dir[2] * dir[2];
        long double lb = 2.0L * ((long double)dir[0] * ox +
                                 (long double)dir[2] * oz);
        long double lc = (long double)ox * ox +
                         (long double)oz * oz -
                         (long double)radius * radius;
        long double ld = lb * lb - 4.0L * la * lc;
        long double root;

        if (ld <= 0.0L)
            return false;
        root = sqrtl(ld);
        h_enter = trace_root_from_long_double(
            (-lb - root) * 4096.0L / (2.0L * la));
        h_exit = trace_root_from_long_double(
            (-lb + root) * 4096.0L / (2.0L * la));
    }
    if (h_enter > h_exit)
        return false;

    /* 0x80054834..0x8005483C builds exactly these two endpoints: the lower
     * face is always origin.y + 286, and entity+0x96 reaches upward from it. */
    ymax = (s64)centre[1] + 286;
    ymin = ymax - height;

    if (dir[1] > 0) {
        v_enter = ((ymin - origin[1]) * 4096) / dir[1];
        v_exit  = ((ymax - origin[1]) * 4096) / dir[1];
    } else if (dir[1] < 0) {
        s64 denom = -(s64)dir[1];
        v_enter = (((s64)origin[1] - ymax) * 4096) / denom;
        v_exit  = (((s64)origin[1] - ymin) * 4096) / denom;
    } else {
        if ((s64)origin[1] < ymin || (s64)origin[1] > ymax)
            return false;
        v_enter = -(s64)0x7FFFFFFF;
        v_exit  =  (s64)0x7FFFFFFF;
    }

    if (v_enter > h_enter)
        h_enter = v_enter;
    if (v_exit < h_exit)
        h_exit = v_exit;
    if (h_enter > h_exit)
        return false;

    if (out_enter)
        *out_enter = h_enter;
    if (out_exit)
        *out_exit = h_exit;
    return true;
}

/* Shared scan: the nearest actor whose cylinder the segment crosses within
 * the fraction the world allows. Returns its index, or -1. */
static s32 nearest_hit(const s32 origin[3], const s32 dir[3],
                       s32 world_fraction, s32 fallback_radius,
                       q2_actor **targets, u32 count, u32 skip_mask_index,
                       s64 *out_along)
{
    s32 best = -1;
    s64 best_along = 0;
    u32 i;

    for (i = 0; i < count; i++) {
        q2_actor *t = targets[i];
        s64 along = 0, leave = 0;

        SCAN_BUMP(tested);
        if (!t || i == skip_mask_index) {
            SCAN_BUMP(skipped);
            continue;
        }
        /*
         * NOT `health <= 0`. HEALTH IS NOT HOW THE CONSOLE DECIDES WHAT CAN BE
         * SHOT — `takedamage` is, and that is the whole reason a corpse can be
         * blown apart.
         *
         * The console's own entity sweep (0x800544EC, called from the hitscan
         * at 0x8004891C) filters on identity, bbox overlap, being ahead, the
         * perpendicular radius, the fraction band and the vertical slab — and
         * on NOTHING else. No health, no svflags, no solid. `findradius`
         * (0x8005FA28) filters on solid and inuse, again not health. What
         * rejects a shot is T_Damage's own first test, five instructions in:
         * `lw v1, 28(targ)` / `and v1, 0xC0000000` / `beq v1, zero, epilogue`
         * at 0x80062838..0x80062848.
         *
         * And a creature is DAMAGE_AIM from `monster_start` (0x80061A94) and
         * downgraded to DAMAGE_YES by every module's own `die` on its normal
         * death arm — the Soldier at module+0x23B4, the Gunner at
         * module+0x1780 — which is exactly so that the body stays shootable.
         * Filtering on health here threw that away and made the corpse
         * untouchable, which is why nothing could ever be gibbed.
         *
         * This older query still filters on `takedamage`. That is NOT
         * equivalent to rejecting damage after native collision: an immune
         * body can stop a shot and shield another candidate. The independent
         * contact fixtures measure this and the old arithmetic differences.
         * q2_combat_trace_actors implements the measured query, and every
         * live shot now goes through it (q2_combat_hitscan). This one is
         * kept for the tests that pin its geometry; the AI's body clip still
         * uses its cylinder arithmetic (aiworld.c), an open audit item.
         */
        if (!t->takedamage) {
            SCAN_BUMP(dead);
            continue;
        }

        if (!actor_centre_is_ahead(origin, dir, t)) {
            SCAN_BUMP(behind);
            continue;
        }

        if (!actor_cylinder_interval(origin, dir, t, fallback_radius,
                                     &along, &leave) || leave <= 0 ||
            along > 4096) {
            SCAN_BUMP(off_axis);
            continue;
        }
        if (along < 0)
            along = 0;
        if (along > world_fraction) {
            SCAN_BUMP(beyond_world);
            continue;
        }

        SCAN_BUMP(hit);

        if (best < 0 || along < best_along) {
            best = (s32)i;
            best_along = along;
        }
    }

    if (out_along)
        *out_along = best_along;
    return best;
}

q2_damage_result q2_combat_melee(q2_actor *attacker, q2_actor *target,
                                 s16 damage, const s32 point[3],
                                 const q2_combat_rules *rules)
{
    q2_damage_result out;

    memset(&out, 0, sizeof(out));
    if (!attacker || !target)
        return out;

    /* 0x800612E4 passes sp+88, the contact built by AngleVectors/VectorMA.
     * Mod 7 has no damage-function knockback; fire_hit adds its own kick. */
    return q2_combat_damage(attacker, target, damage, Q2_MOD_MELEE,
                            point, rules);
}

/* 0x800B11D8 retains a wrapping 32-bit accumulator while producing at most
 * sixteen fractional bits. Exact division can finish early. A plain 64-bit
 * multiply/divide misses its signed overflow for long exit roots. */
static s32 native_root_fraction(s32 numerator, s32 denominator)
{
    u32 remainder = numerator < 0 ? 0u - (u32)numerator : (u32)numerator;
    u32 divisor = denominator < 0 ? 0u - (u32)denominator : (u32)denominator;
    u32 accumulator = 0, shift = 0;
    s32 result;
    if (!remainder || !divisor) return 0;
    for (;;) {
        if ((s32)remainder >= (s32)divisor) {
            accumulator += (remainder / divisor) * 2048u;
            remainder %= divisor;
            if (!remainder) break;
        }
        accumulator <<= 1;
        remainder <<= 1;
        if (++shift == 16) break;
    }
    result = (s32)accumulator >> shift;
    return (numerator < 0) != (denominator < 0) ? (s32)(0u - (u32)result) : result;
}

/* 0x80056084 reduces its signed 64-bit discriminant before taking the
 * engine's twice-refined SDK root. Replacing this with exact sqrt changes
 * accepted fractions, especially when a nearer candidate shortens the ray. */
static bool native_cylinder_roots(s32 a, s32 b, s32 c, s32 *first, s32 *last)
{
    u64 discriminant = (u64)((s64)b * b) - ((u64)((s64)a * c) << 2);
    u32 shift = 0, root;
    s32 low, high;
    if ((s64)discriminant < 0) return false;
    while (discriminant > 0x7FFFFFFFu) { discriminant >>= 2; shift++; }
    root = q2_sdk_sqrt_refined((u32)discriminant) << shift;
    low = (s32)(0u - (u32)b - root);
    high = (s32)(root - (u32)b);
    low = native_root_fraction(low, a);
    high = native_root_fraction(high, a);
    *first = a > 0 ? low : high;
    *last = a > 0 ? high : low;
    return true;
}

s32 q2_combat_trace_actors(const s32 from[3], const s32 to[3],
                            const q2_actor *ignore, q2_actor *const *targets,
                            u32 count, s32 out[3])
{
    s32 point[3], delta[3], length, best = -1;
    u32 squared = 0, i;
    int k;
    if (!from || !to) return -1;
    for (k = 0; k < 3; k++) {
        point[k] = to[k];
        delta[k] = (s32)((u32)to[k] - (u32)from[k]);
        squared += (u32)delta[k] * (u32)delta[k];
    }
    length = (s32)q2_sdk_sqrt_refined(squared);
    for (i = 0; targets && i < count; i++) {
        const q2_actor *actor = targets[i];
        s32 radius, low[3], high[3], rel[3], dot, cross;
        s32 enter = 0, leave = 4096, y_enter = 0, y_leave = 4096;
        u32 dot_bits = 0;
        bool overlaps = true;
        if (!actor || actor == ignore) continue;
        radius = (s16)actor->radius;
        low[0] = (s32)((u32)actor->origin[0] - (u32)radius);
        high[0] = (s32)((u32)actor->origin[0] + (u32)radius);
        low[2] = (s32)((u32)actor->origin[2] - (u32)radius);
        high[2] = (s32)((u32)actor->origin[2] + (u32)radius);
        high[1] = (s32)((u32)actor->origin[1] + 286u);
        low[1] = (s32)((u32)high[1] - (u32)(s32)actor->height);
        for (k = 0; k < 3; k++) {
            s32 minimum = from[k] < point[k] ? from[k] : point[k];
            s32 maximum = from[k] > point[k] ? from[k] : point[k];
            if (maximum < low[k] || minimum > high[k]) overlaps = false;
            rel[k] = (s32)((u32)from[k] - (u32)actor->origin[k]);
            dot_bits += (u32)delta[k] * (u32)rel[k];
        }
        if (!overlaps) continue;
        dot = (s32)dot_bits;
        if (dot >= 0) continue;
        cross = (s32)((u32)rel[2] * ((u32)point[0] - (u32)actor->origin[0]) -
                      (u32)rel[0] * ((u32)point[2] - (u32)actor->origin[2]));
        if (cross < 0) cross = (s32)(0u - (u32)cross);
        if (cross >= (s32)((u32)radius * (u32)length)) continue;
        {
            s32 a = (s32)((u32)delta[0] * (u32)delta[0] +
                          (u32)delta[2] * (u32)delta[2]);
            s32 b = (s32)(2u * ((u32)delta[0] * (u32)rel[0] +
                                (u32)delta[2] * (u32)rel[2]));
            s32 c = (s32)((u32)rel[0] * (u32)rel[0] +
                          (u32)rel[2] * (u32)rel[2] - (u32)radius * (u32)radius);
            if (a && !native_cylinder_roots(a, b, c, &enter, &leave)) continue;
        }
        if (enter < 0 && leave >= 0) enter = 0;
        if ((u32)enter >= 4096u) continue;
        if (delta[1]) {
            s32 near_y = delta[1] > 0 ? low[1] : high[1];
            s32 far_y = delta[1] > 0 ? high[1] : low[1];
            s32 numerator = (s32)(((u32)near_y - (u32)from[1]) << 12);
            y_enter = (s32)((s64)numerator / delta[1]);
            numerator = (s32)(((u32)far_y - (u32)from[1]) << 12);
            y_leave = (s32)((s64)numerator / delta[1]);
        } else if (from[1] < low[1] || from[1] > high[1]) continue;
        if (enter > y_enter) y_enter = enter;
        if (leave < y_leave) y_leave = leave;
        if (y_leave < y_enter || y_enter > 4096) continue;
        best = (s32)i;
        if (!out) return best;
        squared = 0;
        for (k = 0; k < 3; k++) {
            s32 product = (s32)((u32)delta[k] * (u32)y_enter);
            delta[k] = product / 4096;
            point[k] = (s32)((u32)from[k] + (u32)delta[k]);
            squared += (u32)delta[k] * (u32)delta[k];
        }
        length = (s32)q2_sdk_sqrt_refined(squared);
    }
    if (out) memcpy(out, point, sizeof(point));
    return best;
}

s32 q2_combat_find_target(const s32 eye[3], const s32 angles[3],
                          const q2_actor *ignore, q2_actor *const *targets,
                          u32 count, q2_aim_visible_fn visible, void *user,
                          s16 direction[3], s16 *pitch)
{
    s16 matrix[3][3];
    s32 local_y = 0, ray[3], end[3], point[3], normal[3];
    int attempt, k;
    if (!eye || !angles || !direction || !pitch) return -1;
    q2_rotation_euler(matrix, angles[0], angles[1], angles[2]);
    for (attempt = 0; attempt < 11; attempt++) {
        for (k = 0; k < 3; k++) {
            ray[k] = ((s32)matrix[k][1] * local_y +
                      (s32)matrix[k][2] * 8192) >> 12;
            if (k == 1) ray[k] = -ray[k];
            end[k] = (s32)((u32)eye[k] + (u32)ray[k]);
            /* The no-target direction is half the unnormalised first ray.
             * Native narrows to a signed halfword BEFORE truncating / 2. */
            if (!attempt) direction[k] = (s16)((s16)ray[k] / 2);
        }
        {
            s32 hit = q2_combat_trace_actors(eye, end, ignore, targets, count, point);
            if (hit >= 0 && (!visible || visible(user, eye, point, attempt))) {
                *pitch = (s16)q2_ratan2(-local_y, 8192);
                q2_sdk_vector_normal(ray, normal);
                for (k = 0; k < 3; k++) direction[k] = (s16)normal[k];
                return hit;
            }
        }
        local_y = -local_y;
        if (!(attempt & 1)) local_y += 512;
    }
    *pitch = 0;
    return -1;
}

s32 q2_combat_nearest_on_segment(const s32 origin[3], const s32 dir[3],
                                 s32 fallback_radius, q2_actor **targets,
                                 u32 count)
{
    if (!origin || !dir || !targets)
        return -1;
    return nearest_hit(origin, dir, 4096, fallback_radius, targets, count,
                       (u32)-1, NULL);
}

s32 q2_combat_nearest_hit(const s32 origin[3], const s32 dir[3],
                          s32 world_fraction, s32 fallback_radius,
                          q2_actor **targets, u32 count, s64 *out_along)
{
    if (out_along)
        *out_along = 0;
    if (!origin || !dir || !targets)
        return -1;
    if (world_fraction <= 0 || world_fraction > 4096)
        world_fraction = (world_fraction <= 0) ? 0 : 4096;
    return nearest_hit(origin, dir, world_fraction, fallback_radius, targets,
                       count, (u32)-1, out_along);
}

s32 q2_combat_hitscan(q2_actor *attacker, const s32 origin[3],
                      const s32 dir[3], s16 damage, s16 mod,
                      s32 world_fraction, q2_actor **targets, u32 count,
                      const q2_combat_rules *rules, q2_damage_result *out,
                      s32 point[3])
{
    s32 stop[3], at[3], idx;
    int k;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!origin || !dir)
        return -1;
    if (world_fraction <= 0 || world_fraction > 4096)
        world_fraction = (world_fraction <= 0) ? 0 : 4096;

    for (k = 0; k < 3; k++)
        stop[k] = origin[k] + (s32)(((s64)dir[k] * world_fraction) >> 12);

    idx = q2_combat_trace_actors(origin, stop, attacker, targets,
                                 targets ? count : 0, at);
    if (point)
        memcpy(point, at, sizeof(at));
    if (idx < 0)
        return -1;

    {
        q2_damage_result r = q2_combat_damage(attacker, targets[idx], damage,
                                              mod, at, rules);
        if (out)
            *out = r;
    }
    return idx;
}

s32 q2_combat_fire_bullet(q2_actor *attacker, const s32 origin[3],
                          const s32 dir[3], s16 damage, s32 world_fraction,
                          s32 fallback_radius, q2_actor **targets, u32 count,
                          const q2_combat_rules *rules,
                          q2_damage_result *out)
{
    (void)fallback_radius;
    return q2_combat_hitscan(attacker, origin, dir, damage, Q2_MOD_BULLET,
                             world_fraction, targets, count, rules, out,
                             NULL);
}

u32 q2_combat_fire_rail(q2_actor *attacker, const s32 origin[3],
                        const s32 dir[3], s16 damage, s32 world_fraction,
                        s32 fallback_radius, q2_actor **targets, u32 count,
                        const q2_combat_rules *rules)
{
    (void)fallback_radius;
    return q2_combat_hitscan(attacker, origin, dir, damage, Q2_MOD_RAIL,
                             world_fraction, targets, count, rules, NULL,
                             NULL) >= 0 ? 1u : 0u;
}
