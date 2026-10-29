#include "item.h"

#include "crebind.h"
#include "effect.h"       /* q2_fx_item_materialise — 0x800596B0 */
#include "entitydraw.h"   /* q2_entity_resolve_model — the drop's 0x8006D008 */
#include "levelbin.h"
#include "trig.h"
#include "weapontables.h"

#include <string.h>

/* See q2_item_env in item.h: console globals, so module-wide here too. */
static q2_item_env g_env;

void q2_item_bind_env(const q2_item_env *env)
{
    if (env)
        g_env = *env;
    else
        memset(&g_env, 0, sizeof(g_env));
}

/* ------------------------------------------------------------------------- */
/* The dispatch, transcribed                                                  */
/*                                                                            */
/* One row per effect index. Empty rows are the twelve slots that point at the  */
/* dispatch's failure exit plus indices 0 and 1, which the bounds check         */
/* `(unsigned)(effect - 2) < 55` rejects before the jump.                      */
/* ------------------------------------------------------------------------- */
#define FX_LAST Q2_ITEM_EFFECT_LAST

static const q2_item_fx k_fx[FX_LAST + 1] = {
    /* 0, 1 — below the dispatch's range (0x8003638C bounds check). */
    { 0 }, { 0 },

    /* --- weapons. bit = 1 << (id - 1); the ammo is the weapon's own type. --- */
    /*  2 */ { Q2_ITEMFX_WEAPON, 2,  10,  20, Q2_SND_WEAPON, 0, 0x800363BCu },
    /*  3 */ { Q2_ITEMFX_WEAPON, 3,  10,  20, Q2_SND_WEAPON, 0, 0x8003645Cu },
    /*  4 */ { Q2_ITEMFX_WEAPON, 4,  50, 100, Q2_SND_WEAPON, 0, 0x800364FCu },
    /*  5 */ { Q2_ITEMFX_WEAPON, 5,  50, 100, Q2_SND_WEAPON, 0, 0x8003659Cu },
    /*  6 */ { 0 },                       /* inert: no record names it       */
    /*  7 */ { Q2_ITEMFX_WEAPON, 7,   5,  10, Q2_SND_WEAPON, 0, 0x8003663Cu },
    /*  8 */ { Q2_ITEMFX_WEAPON, 8,   5,  10, Q2_SND_WEAPON, 0, 0x800366ECu },
    /*  9 */ { Q2_ITEMFX_WEAPON, 9,  50, 100, Q2_SND_WEAPON, 0, 0x8003678Cu },
    /* 10 */ { Q2_ITEMFX_WEAPON, 10, 10,  20, Q2_SND_WEAPON, 0, 0x800368CCu },
    /* 11 */ { Q2_ITEMFX_WEAPON, 11, 50, 100, Q2_SND_WEAPON, 0, 0x8003682Cu },

    /* 12..16 — inert. Ionripper P, Plasmagun P, Discharge P, Flame P and
     * Tesla P name these and get nothing; the Xatrix/Rogue weapons were never
     * implemented on this disc. */
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 },

    /* 17 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_NUKE, 0x80037208u },

    /* --- ammo boxes --- */
    /* 18 */ { Q2_ITEMFX_AMMO, Q2_AMMO_SHELLS,   10,  20, Q2_SND_AMMO, 0, 0x8003696Cu },
    /* 19 */ { Q2_ITEMFX_AMMO, Q2_AMMO_BULLETS,  50, 100, Q2_SND_AMMO, 0, 0x800369D4u },
    /* 20 */ { Q2_ITEMFX_AMMO, Q2_AMMO_GRENADES,  5,  10, Q2_SND_AMMO, 0, 0x80036A3Cu },
    /* 21 */ { Q2_ITEMFX_AMMO, Q2_AMMO_ROCKETS,   5,  10, Q2_SND_AMMO, 0, 0x80036AACu },
    /* 22 */ { Q2_ITEMFX_AMMO, Q2_AMMO_CELLS,    50, 100, Q2_SND_AMMO, 0, 0x80036B14u },
    /* 23 */ { Q2_ITEMFX_AMMO, Q2_AMMO_SLUGS,    10,  20, Q2_SND_AMMO, 0, 0x80036BACu },

    /* 24, 25 — inert. Flame Fuel P names 24. */
    { 0 }, { 0 },

    /* --- armour --- */
    /* 26 */ { Q2_ITEMFX_ARMOUR, Q2_ARMOUR_BODY,   0, 0, Q2_SND_ARMOUR, 0, 0x80036C14u },
    /* 27 */ { Q2_ITEMFX_ARMOUR, Q2_ARMOUR_COMBAT, 0, 0, Q2_SND_ARMOUR, 0, 0x80036C8Cu },
    /* 28 */ { Q2_ITEMFX_ARMOUR, Q2_ARMOUR_JACKET, 0, 0, Q2_SND_ARMOUR, 0, 0x80036CF4u },
    /* 29 */ { Q2_ITEMFX_ARMOUR_SHARD, -1, 2, 2, Q2_SND_ARMOUR_SHARD, 0, 0x80036D6Cu },
    /* 30 */ { Q2_ITEMFX_POWER_SHIELD, Q2_AMMO_CELLS, 50, 100, Q2_SND_POWER, 0,
               0x80036DA8u },

    /* 31 — inert. Screen P names it, and the damage path still tests the power
     * screen's bit, so the bit can never be set. */
    { 0 },

    /* --- health --- */
    /* 32 */ { Q2_ITEMFX_MEGA_HEALTH, -1, 100, 100, Q2_SND_MEGA_HEALTH, 0,
               0x80036E20u },
    /* 33 */ { Q2_ITEMFX_ADRENALINE,  -1,   1,   1, Q2_SND_ITEM, 0, 0x80036E64u },
    /* 34 */ { Q2_ITEMFX_HEALTH,      -1,  10,  10, Q2_SND_MEDKIT, 0, 0x80036EA8u },
    /* 35 */ { Q2_ITEMFX_HEALTH,      -1,  25,  25, Q2_SND_LARGE_MEDKIT, 0,
               0x80036EDCu },

    /* 36 — inert. Stimpack P names it: the 2-point health has no handler. */
    { 0 },

    /* --- carry capacity --- */
    /* 37 */ { Q2_ITEMFX_AMMO_PACK,  Q2_AMMO_TIER_PACK,      0, 0, Q2_SND_ITEM, 0,
               0x80036F10u },
    /* 38 */ { Q2_ITEMFX_BANDOLIER,  Q2_AMMO_TIER_BANDOLIER, 0, 0, Q2_SND_ITEM, 0,
               0x8003701Cu },
    /* 39 */ { Q2_ITEMFX_SILENCER,   -1, 30, 30, Q2_SND_ITEM, 0, 0x800370B4u },

    /* --- powerups --- */
    /* 40 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_QUAD,     0, 0, Q2_SND_QUAD,    0,
               0x800370D0u },
    /* 41 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_INVULN,   0, 0, Q2_SND_PROTECT, 0,
               0x8003710Cu },
    /* 42 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_ENVIRO,   0, 0, Q2_SND_ITEM,    0,
               0x80037148u },
    /* 43 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_BREATHER, 0, 0, Q2_SND_ITEM,    0,
               0x80037184u },

    /* --- keys and objectives. One bit each, one handler each. --- */
    /* 44 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_BLUE,           0x800371C0u },
    /* 45 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_RED,            0x800371D8u },
    /* 46 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_PASS,           0x800371F0u },
    /* 47 */ { 0 },
    /* 48 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_HEAD,           0x80037220u },
    /* 49 */ { 0 },
    /* 50 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_PYRAMID_PURPLE, 0x80037238u },
    /* 51 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_PYRAMID_RED,    0x80037250u },
    /* 52 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_DATA_CD,        0x80037268u },
    /* 53 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_DATA_SPINNER,   0x80037280u },
    /* 54 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_GREEN,          0x80037298u },
    /* 55 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_YELLOW,         0x800372B0u },
    /* 56 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_WHITE,          0x800372C8u }
};

const q2_item_fx *q2_item_effect_def(u32 effect)
{
    if (effect > FX_LAST)
        return &k_fx[0];
    return &k_fx[effect];
}

const char *q2_item_effect_name(u32 effect)
{
    static const char *const kind_name[Q2_ITEMFX_KIND_COUNT] = {
        "inert", "weapon", "ammo", "armour", "armour shard", "power shield",
        "health", "mega health", "adrenaline", "bandolier", "ammo pack",
        "silencer", "powerup", "key"
    };
    const q2_item_fx *d = q2_item_effect_def(effect);

    if (d->kind >= Q2_ITEMFX_KIND_COUNT)
        return "?";
    return kind_name[d->kind];
}

/* ------------------------------------------------------------------------- */
/* Helpers the dispatch shares                                                */
/* ------------------------------------------------------------------------- */

/* 0x80037DFC: min(max, cur + add), all in 16 bits. */
static s16 clamp_add(s16 max, s16 cur, s16 add)
{
    s16 sum = (s16)(cur + add);
    return sum > max ? max : sum;
}

/*
 * 0x80037E28. Returns 0 only when the weapon is already held AND deathmatch AND
 * weapons-stay — the one case where a second pickup must be refused outright.
 * `selects` is the weapon the engine switches to when the blaster is out.
 */
static bool give_weapon(q2_inventory *inv, int weapon_id, int selects,
                        q2_entity_world *w)
{
    u32 bit;
    bool had;

    if (!inv || weapon_id < 1 || weapon_id > Q2_WEAPON_COUNT)
        return false;

    /* The engine's owned bit is 1 << (id - 1) and its ids are 1-based
     * (weapontables.h); q2_inventory's enum is 0-based, so the shift is the
     * same number and the enum value is id - 1. */
    bit = 1u << (weapon_id - 1);
    had = (inv->weapons & bit) != 0;

    if (had) {
        /* 0x80037E4C: only this combination refuses. */
        if (w && w->deathmatch && w->weapons_stay)
            return false;
        return true;
    }

    inv->weapons |= (u16)bit;

    if (selects < 1 || selects > Q2_WEAPON_COUNT)
        return true;

    /*
     * 0x80037E84: the switch happens only when the blaster is out.
     *
     * `inv->current_weapon` is the inventory's own zero-based record of it, and
     * it is only a record: the console's two halfwords are client+98 and
     * client+102, the port keeps one id for both (statusbar.h), and that id
     * lives in the sim. This byte is written here and by nothing else in the
     * running game, so gating on it would go stale the moment the pad cycled
     * the held weapon — pick up a shotgun, cycle back to the blaster, walk over
     * a railgun, and the console switches while this test says "shotgun".
     */
    if (inv->current_weapon == Q2_WEAPON_BLASTER)
        inv->current_weapon = (s8)(selects - 1);

    /*
     * So the gate that matters is applied by the host, against the weapon it
     * actually holds. Reported UNGATED here for that reason; entity.h's
     * `granted_weapon` says who reads it.
     */
    if (w)
        w->granted_weapon = selects;

    return true;
}

/* The ammo type a weapon id draws on, 1-based in, q2_ammo out. */
static int weapon_ammo_of(int weapon_id)
{
    if (weapon_id < 1 || weapon_id > Q2_WEAPON_COUNT)
        return -1;
    return q2_weapon_ammo[weapon_id - 1];
}

s16 q2_item_armour_amount(q2_inventory *inv, u8 cls)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    const q2_wt_armour *nw, *cw;
    s16 cur_type, keep;
    s32 ratio, add;

    if (!inv || !wt)
        return 0;
    if (cls >= Q2_WT_ARMOUR_CLASSES)
        cls = Q2_WT_ARMOUR_CLASSES - 1;

    /* 0x8003733C: with no armour held, the class and its flag bits are reset
     * first. The mask is the literal the engine ANDs with — it clears the three
     * class bits and everything above bit 18. */
    if (inv->armour == 0) {
        inv->armour_class = 0;
        inv->flags &= 0x00078FFFu;
        if (cls > 0)
            inv->armour_class = cls;
    }

    cur_type = (s16)inv->armour_class;
    if (cur_type >= Q2_WT_ARMOUR_CLASSES)
        cur_type = Q2_WT_ARMOUR_CLASSES - 1;

    nw = &wt->armour[cls];
    cw = &wt->armour[cur_type];

    if (cls < (u8)cur_type) {
        /*
         * 0x8003738C: weaker armour on top of stronger. The ratio is formed in
         * 2-bit fixed point — `(new.normal << 2) / cur.normal` — multiplied by
         * the new class's base count and shifted back. That is PC Quake II's
         * `salvage = new->normal_protection / old->normal_protection` with a
         * float replaced by two bits, which is why the numbers land where id's
         * do without anything being tuned.
         */
        ratio = ((s32)nw->normal_protection << 2) / (s32)cw->normal_protection;
        add   = ratio * (s32)nw->base_count;
        keep  = inv->armour;
        inv->armour = 0;
        return (s16)(keep + (add >> 2));
    }

    if (cls == (u8)cur_type) {
        /* 0x80037490: same class, just the base count. */
        return (s16)nw->base_count;
    }

    /* 0x80037404: stronger armour. The old value converts down and the class
     * changes. */
    ratio = ((s32)cw->normal_protection << 2) / (s32)nw->normal_protection;
    add   = ratio * (s32)inv->armour;
    inv->armour       = 0;
    inv->armour_class = cls;
    return (s16)((s16)nw->base_count + (add >> 2));
}

static s16 armour_cap(const q2_inventory *inv)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    u8 cls;

    if (!inv || !wt)
        return 0;
    cls = inv->armour_class;
    if (cls >= Q2_WT_ARMOUR_CLASSES)
        cls = Q2_WT_ARMOUR_CLASSES - 1;
    return (s16)wt->armour[cls].max_count;
}

static s32 *powerup_slot(q2_inventory *inv, int slot)
{
    switch (slot) {
    case Q2_POWERUP_QUAD:     return &inv->quad_until;
    case Q2_POWERUP_INVULN:   return &inv->invuln_until;
    case Q2_POWERUP_ENVIRO:   return &inv->enviro_until;
    case Q2_POWERUP_BREATHER: return &inv->breather_until;
    default:                  return NULL;
    }
}

static s32 powerup_expiry(const q2_inventory *inv, int slot)
{
    switch (slot) {
    case Q2_POWERUP_QUAD:     return inv->quad_until;
    case Q2_POWERUP_INVULN:   return inv->invuln_until;
    case Q2_POWERUP_ENVIRO:   return inv->enviro_until;
    case Q2_POWERUP_BREATHER: return inv->breather_until;
    default:                  return 0;
    }
}

bool q2_item_powerup_active(const q2_inventory *inv, q2_powerup_slot slot,
                            s32 level_time)
{
    s32 until;

    if (!inv)
        return false;
    until = powerup_expiry(inv, (int)slot);
    if (until == 0)
        return false;

    /* The handlers compare unsigned (`sltu` at 0x800370E0), so the clock is
     * treated as unsigned and a wrap is not special-cased. */
    return (u32)level_time < (u32)until;
}

void q2_item_mega_health_tick(q2_inventory *inv, s32 level_time)
{
    if (!inv)
        return;
    if (!(inv->flags & Q2_INV_MEGA_HEALTH))
        return;

    if (inv->mega_health_next == 0) {
        inv->mega_health_next = level_time + Q2_ITEM_MEGA_DECAY_TICKS;
        return;
    }
    if ((u32)level_time < (u32)inv->mega_health_next)
        return;

    inv->mega_health_next = level_time + Q2_ITEM_MEGA_DECAY_TICKS;

    if (inv->health > inv->health_max) {
        inv->health--;
    } else {
        inv->flags &= ~Q2_INV_MEGA_HEALTH;
        inv->mega_health_next = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* The touch dispatch                                                         */
/* ------------------------------------------------------------------------- */
q2_touch_result q2_item_touch(u32 effect, q2_inventory *inv, const s32 pos[3],
                             q2_entity_world *w)
{
    const q2_item_fx *d;
    bool took = false;       /* s3 */
    bool weaponish = false;  /* !s6 */
    bool infinite_ammo;
    s16 amount;

    if (!inv || !w)
        return Q2_TOUCH_NOTHING;

    /* 0x8003638C: `(unsigned)(effect - 2) < 55`, so 0 and 1 fall out here. */
    if (effect < Q2_ITEM_EFFECT_FIRST || effect > FX_LAST)
        return Q2_TOUCH_NOTHING;

    /* An effect index whose dispatch slot is the failure exit does nothing, and
     * the table says which those are rather than this code assuming it. */
    if (!q2_item_effect_is_live(w->items, effect))
        return Q2_TOUCH_NOTHING;

    d = q2_item_effect_def(effect);
    if (d->kind == Q2_ITEMFX_NONE)
        return Q2_TOUCH_NOTHING;

    infinite_ammo = (w->cheats & Q2_CHEAT_INFINITE_AMMO) != 0;
    amount = w->deathmatch ? d->amount_dm : d->amount;

    switch (d->kind) {

    case Q2_ITEMFX_WEAPON: {
        int ammo = weapon_ammo_of(d->subject);
        s16 cap  = ammo >= 0 ? q2_inventory_ammo_max(inv, (q2_ammo)ammo) : 0;
        int bit_weapon = d->subject;

        /* 0x800363BC: with INFINITE AMMO on, the weapon grants capacity. */
        if (infinite_ammo)
            amount = cap;

        /*
         * The grenade launcher grants the HAND GRENADE first (bit 0x20, i.e.
         * weapon 6) and the launcher second (0x8003668C / 0x800366D0), and both
         * calls name weapon 7 as the one to switch to. So one pickup yields two
         * weapons — reproduced rather than simplified, because a player who
         * picks up a launcher in the original can throw grenades.
         */
        if (d->subject == Q2_WEAPON_GRENADE_LAUNCHER + 1)
            bit_weapon = Q2_WEAPON_HAND_GRENADE + 1;

        if (!give_weapon(inv, bit_weapon, d->subject, w))
            return Q2_TOUCH_NOTHING;

        took = true;

        if (ammo >= 0)
            inv->ammo[ammo] = clamp_add(cap, inv->ammo[ammo], amount);

        weaponish = true;

        if (d->subject == Q2_WEAPON_GRENADE_LAUNCHER + 1)
            give_weapon(inv, d->subject, d->subject, w);
        break;
    }

    case Q2_ITEMFX_AMMO: {
        q2_ammo type = (q2_ammo)d->subject;
        s16 cap = q2_inventory_ammo_max(inv, type);

        /*
         * 0x80036B14: cells while a power item is held and the player has none
         * plays the power sound as well — the shield coming back to life.
         */
        if (type == Q2_AMMO_CELLS &&
            (inv->flags & Q2_INV_POWER_SHIELD) && inv->ammo[type] == 0)
            q2_ent_sound_at(&w->events, Q2_SND_POWER, pos);

        /* 0x8003698C: full means the box stays where it is. */
        if (inv->ammo[type] >= cap)
            return Q2_TOUCH_NOTHING;

        took = true;
        inv->ammo[type] = clamp_add(cap, inv->ammo[type], amount);

        /* 0x80036A90: the grenade box is also the hand grenade. */
        if (type == Q2_AMMO_GRENADES)
            give_weapon(inv, Q2_WEAPON_HAND_GRENADE + 1,
                        Q2_WEAPON_HAND_GRENADE + 1, w);
        break;
    }

    case Q2_ITEMFX_ARMOUR: {
        u8  cls = (u8)d->subject;
        s16 add, cap;
        u32 test;

        add = q2_item_armour_amount(inv, cls);

        /* 0x80036C28 / 0x80036CA0 / 0x80036D08: the test widens as the class
         * weakens, so a stronger bit already up suppresses the weaker flag. */
        test = cls == Q2_ARMOUR_BODY   ? Q2_INV_ARMOUR_BODY
             : cls == Q2_ARMOUR_COMBAT ? (Q2_INV_ARMOUR_BODY | Q2_INV_ARMOUR_COMBAT)
                                       : Q2_INV_ARMOUR_MASK;
        if ((inv->flags & test) == 0)
            inv->flags |= cls == Q2_ARMOUR_BODY   ? Q2_INV_ARMOUR_BODY
                        : cls == Q2_ARMOUR_COMBAT ? Q2_INV_ARMOUR_COMBAT
                                                  : Q2_INV_ARMOUR_JACKET;

        /* The cap is read AFTER the conversion, because the conversion may have
         * changed the class. */
        cap = armour_cap(inv);

        /* Body and jacket refuse a full player; combat does not. */
        if (cls != Q2_ARMOUR_COMBAT && inv->armour >= cap)
            return Q2_TOUCH_NOTHING;

        took = true;
        inv->armour = clamp_add(cap, inv->armour, add);
        break;
    }

    case Q2_ITEMFX_ARMOUR_SHARD:
        /* 0x80036D74: a shard with no armour held makes it jacket armour, and
         * its own cap is the literal 999 rather than the class maximum. */
        if ((inv->flags & Q2_INV_ARMOUR_MASK) == 0)
            inv->flags |= Q2_INV_ARMOUR_JACKET;
        took = true;
        inv->armour = clamp_add(999, inv->armour, amount);
        break;

    case Q2_ITEMFX_POWER_SHIELD: {
        s16 cap = q2_inventory_ammo_max(inv, Q2_AMMO_CELLS);

        /* 0x80036DB4: the bit goes up whether or not the cells fit. */
        inv->flags |= Q2_INV_POWER_SHIELD;
        took = true;
        if (inv->ammo[Q2_AMMO_CELLS] < cap)
            inv->ammo[Q2_AMMO_CELLS] =
                clamp_add(cap, inv->ammo[Q2_AMMO_CELLS], amount);
        break;
    }

    case Q2_ITEMFX_HEALTH:
        /* 0x80036EB4: the cap is the player's own max, and being at it refuses
         * the pickup so it can be come back for. */
        if (inv->health >= inv->health_max)
            return Q2_TOUCH_NOTHING;
        took = true;
        inv->health = clamp_add(inv->health_max, inv->health, amount);
        break;

    case Q2_ITEMFX_MEGA_HEALTH:
        /* 0x80036E20: 200 and 100 are immediates, not the player's max — so
         * mega health always applies and always overheals to 200. */
        inv->flags |= Q2_INV_MEGA_HEALTH;
        took = true;
        inv->health = clamp_add(200, inv->health, amount);
        inv->mega_health_next = w->level_time + Q2_ITEM_MEGA_DECAY_TICKS;
        break;

    case Q2_ITEMFX_ADRENALINE:
        /* 0x80036E64: max health rises by one and health is topped up to it. */
        inv->health_max = (s16)(inv->health_max + amount);
        took = true;
        if (inv->health < inv->health_max)
            inv->health = clamp_add(inv->health_max, inv->health,
                                    inv->health_max);
        break;

    case Q2_ITEMFX_BANDOLIER:
        /* 0x8003701C: a pack already held is not downgraded. Then shells and
         * bullets top up at the NEW tier's capacity. */
        if (inv->ammo_tier != Q2_AMMO_TIER_PACK)
            inv->ammo_tier = Q2_AMMO_TIER_BANDOLIER;
        took = true;
        inv->ammo[Q2_AMMO_SHELLS] =
            clamp_add(q2_inventory_ammo_max(inv, Q2_AMMO_SHELLS),
                      inv->ammo[Q2_AMMO_SHELLS], w->deathmatch ? 20 : 10);
        inv->ammo[Q2_AMMO_BULLETS] =
            clamp_add(q2_inventory_ammo_max(inv, Q2_AMMO_BULLETS),
                      inv->ammo[Q2_AMMO_BULLETS], w->deathmatch ? 100 : 50);
        break;

    case Q2_ITEMFX_AMMO_PACK: {
        /* 0x80036F10: the tier is set to the pack unconditionally, and then all
         * six types top up — each by its own amount, at the pack capacities. */
        static const s16 sp[Q2_AMMO_COUNT] = { 10, 50,  5,  5, 50, 10 };
        static const s16 dm[Q2_AMMO_COUNT] = { 20, 100, 10, 10, 100, 20 };
        int i;

        inv->ammo_tier = Q2_AMMO_TIER_PACK;
        took = true;
        for (i = 0; i < Q2_AMMO_COUNT; i++)
            inv->ammo[i] = clamp_add(q2_inventory_ammo_max(inv, (q2_ammo)i),
                                     inv->ammo[i],
                                     w->deathmatch ? dm[i] : sp[i]);
        break;
    }

    case Q2_ITEMFX_SILENCER:
        /* 0x800370BC: shots, not seconds, and no cap. */
        inv->silencer_shots = (s16)(inv->silencer_shots + amount);
        took = true;
        break;

    case Q2_ITEMFX_POWERUP: {
        s32 *slot = powerup_slot(inv, d->subject);

        if (!slot)
            return Q2_TOUCH_NOTHING;

        /* 0x800370D0: extend from whichever is later, then add 30 seconds. */
        if ((u32)w->level_time < (u32)*slot)
            *slot = *slot + Q2_ITEM_POWERUP_TICKS;
        else
            *slot = w->level_time + Q2_ITEM_POWERUP_TICKS;
        took = true;
        break;
    }

    case Q2_ITEMFX_KEY:
        /* 0x800371C0 and the eleven like it: one OR, never refused. */
        inv->flags |= d->key_bit;
        took = true;
        break;

    case Q2_ITEMFX_NONE:
    default:
        return Q2_TOUCH_NOTHING;
    }

    /* 0x800372E0: the sound plays at the PLAYER's position, not the item's. */
    q2_ent_sound_at(&w->events, (q2_ent_sound)d->sound, pos);

    if (!took)
        return Q2_TOUCH_NOTHING;

    /* 0x800372F0: the HUD's caption. */
    inv->last_item       = (u8)effect;
    inv->item_name_until = w->level_time + Q2_ITEM_CAPTION_TICKS;

    return weaponish ? Q2_TOUCH_WEAPON : Q2_TOUCH_COLLECTED;
}

/* ------------------------------------------------------------------------- */
/* What the HUD shows for it — 0x800359C0                                     */
/* ------------------------------------------------------------------------- */
bool q2_item_pickup_caption(q2_inventory *inv, s32 level_time,
                            const q2_item_table *table,
                            u8 *icon, const char **name)
{
    if (!inv)
        return false;

    /* 0x80035A28: nothing collected since the last one expired, and the whole
     * sub-draw returns without touching its field. */
    if (inv->last_item == 0)
        return false;

    /*
     * 0x80035A40: `sltu deadline, now`, so the caption survives the tick it
     * expires ON and dies the tick after. Unsigned, as the console's is, which
     * is also what keeps a wrapped clock from pinning it forever.
     */
    if ((u32)inv->item_name_until < (u32)level_time)
        inv->last_item = 0;

    /* 0x80035A50 reloads it, so the expiry frame draws index 0 — the blank
     * rect and the empty name — rather than skipping. */
    if (icon)
        *icon = inv->last_item;
    if (name)
        *name = q2_item_display_name(table, inv->last_item);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Spawning                                                                   */
/* ------------------------------------------------------------------------- */
static q2_entity *item_spawn(q2_entity_set *set, const q2_pop_place *place,
                         const q2_item_table *table, s16 model_offset,
                         q2_collision *coll, bool *outside)
{
    const q2_item_def *def;
    q2_entity *e;
    u32 i;
    s32 pos[3], node;

    if (outside) *outside = false;

    if (!set || !place)
        return NULL;

    if (!table)
        table = q2_item_table_builtin();

    /* 0x800599F0: the scan, and a place id it does not name spawns nothing. */
    def = q2_item_find(table, (s32)place->id);
    if (!def)
        return NULL;

    pos[0] = place->x; pos[1] = place->y - Q2_ITEM_SPAWN_LIFT; pos[2] = place->z;
    if (!q2_entity_drop_to_floor(coll, NULL, pos,
           (def->flags & Q2_ITEM_NO_DROP) ? 0 : Q2_ENTITY_DROP_DIST, &node)) {
        /* 0x800588C0 rejects a placement outside the resident hull. Do not
         * retain an unlinked pickup or misreport this as an allocation error. */
        if (outside) *outside = true;
        return NULL;
    }

    e = q2_entity_alloc(set);
    if (!e)
        return NULL;

    e->think    = q2_item_think;
    e->kind     = Q2_ENT_KIND_ITEM;      /* 0x80059A60 writes 46 */
    e->place_id = place->id;
    e->flags    = def->flags;
    e->effect   = def->effect;
    e->def      = def;

    /* 0x80058944..0x80058970 copies "000" (30 30 30 00 at
     * 0x800AEABC) into the entity's current and target ambient colours. The
     * current triplet is what the entity draw feeds to the GTE back colour. */
    e->glow[0] = e->glow[1] = e->glow[2] = 0x30;

    memcpy(e->model, def->model, sizeof(e->model));
    for (i = 0; i < Q2_ITEM_SHADOW_VERTEX_MAX; i++)
        e->shadow_vertex[i] = def->shadow_vertex[i];
    e->shadow_vertex_count = def->shadow_vertex_count;

    /*
     * 0x80051054 copies the place record straight in, then 0x80051068 and
     * 0x800510A4 raise y by 286 and 30. +Y is down, so both subtract.
     *
     * AND THEN IT DROPS. The note here used to say the engine "follows this
     * with a hull query that drops the entity onto the surface below
     * (0x80044F54)" and that the part was not reproduced. Two things were
     * wrong with that: 0x80044F54 is `q2_coll_find_node`, which locates the
     * cell holding a point and clips nothing, and the drop is not a detail —
     * it is a 1024-unit downward SWEEP, and the authored Y is a hint rather
     * than a position. Without it every item in the game hangs where the
     * record put it.
     *
     * The distance is the item's own: `sll a3, a3, 10` at 0x80059A50 unless
     * the table's 0x100 flag is set, which is the no-drop flag Quake II's own
     * items carry. See q2_entity_drop_to_floor.
     */
    memcpy(e->pos, pos, sizeof(pos));
    e->node = node;

    /* 0x80058930: the low twelve bits are the heading. The list walker has
     * already consumed the difficulty exclusions above it. */
    e->angles[1] = (s32)Q2_POP_PLACE_ANGLE(place->angle_flags);

    /*
     * 0x800588F0: the draw origin is the position lowered by 286 and raised
     * again by the model's own bias. Net against the place record it is
     * `place.y - 30 - model_offset`, the 286s cancelling.
     */
    e->model_offset = model_offset;
    e->origin[0]    = e->pos[0];
    e->origin[1]    = e->pos[1] + Q2_EYE_BASE - model_offset;
    e->origin[2]    = e->pos[2];

    /* 0x80058908: the spawn origin keeps the same lowering without the bias. */
    e->spawn_origin[0] = e->pos[0];
    e->spawn_origin[1] = e->pos[1] + Q2_EYE_BASE;
    e->spawn_origin[2] = e->pos[2];

    /* The box is about the POSITION, not the draw origin (0x8005A9DC). */
    q2_entity_set_bounds(e, Q2_ITEM_TOUCH_HALF);

    /* 0x80059A98: a materialising item starts at nothing. */
    e->scale = (def->flags & Q2_ITEM_MATERIALISE) ? 0 : (s16)Q2_ONE_12;

    /* 0x80059AB4: the countdown a dropped item would use. Set at spawn on every
     * item, but only read when Q2_ITEM_TIMED is up. */
    e->remove_in = Q2_ITEM_DROP_LIFE;

    /* 0x80059AC4: bit 1 of the render word comes off unless the item spins. */
    if (!(def->flags & Q2_ITEM_SPIN))
        e->render_flags &= ~2u;

    /* The engine gets the clip length from the CastList node. Without the bank
     * open there is nothing to wrap against, and the frame simply holds — which
     * is what Q2_ITEM_NO_ANIM does anyway. */
    e->clip_length = 0;

    return e;
}

q2_entity *q2_item_spawn(q2_entity_set *set, const q2_pop_place *place,
                         const q2_item_table *table, s16 model_offset,
                         q2_collision *coll)
{
    return item_spawn(set, place, table, model_offset, coll, NULL);
}

static q2_result spawn_group_places(q2_entity_set *set,
                                    const q2_population *pop,
                                    const q2_pop_group *g,
                                    u32 group_index,
                                    s32 skill,
                                    const q2_item_table *table,
                                    q2_collision *coll,
                                    q2_item_spawn_stats *stats)
{
    u32 slot;

    for (slot = 0; ; slot++) {
        q2_pop_place place;
        q2_entity *e;
        bool outside;

        if (!q2_pop_get_place(pop, g, slot, &place))
            break;

        stats->places++;

        /* 0x8007F54C..0x8007F5D8: filter the authored list before calling
         * 0x800599DC. Bit 12 is intentionally absent — no retail reader uses
         * it — and a negative skill is the offline all-record census. */
        if (skill >= 0 &&
            !q2_pop_place_allows_skill(place.angle_flags, skill)) {
            stats->skill_filtered++;
            continue;
        }

        e = item_spawn(set, &place, table, 0, coll, &outside);
        if (!e) {
            if (outside)
                stats->no_placement++;
            else if (q2_item_find(table, (s32)place.id))
                stats->no_memory++;
            else
                stats->no_def++;
            continue;
        }

        e->population_group = (s32)group_index;
        e->population_slot  = slot;

        stats->spawned++;
        if (e->effect == 0)
            stats->scenery++;
        else if (!q2_item_effect_is_live(table, e->effect))
            stats->inert++;
    }

    return stats->no_memory ? Q2_ERR_NO_MEMORY : Q2_OK;
}

q2_result q2_item_spawn_zone(q2_entity_set *set, const q2_population *pop,
                             int zone, const u8 *levelbin, u32 levelbin_size,
                             const q2_item_table *table,
                             q2_collision *coll,
                             u8 *group_run,
                             q2_item_spawn_stats *stats)
{
    q2_item_spawn_stats local;
    u32 selected[32];
    u32 selected_count = 0;
    u32 gi;

    if (!set || !pop)
        return Q2_ERR_INVALID_ARG;

    memset(&local, 0, sizeof(local));

    if (!table)
        table = q2_item_table_builtin();

    /* The same bounded decode used by q2_creature_world_hold_batches. The
     * disc-wide census finds at most a small handful per module; 32 also keeps
     * malformed input from turning selection into an allocation surface. */
    if (zone >= 0 && levelbin && levelbin_size)
        selected_count = q2_levelbin_selected(levelbin, levelbin_size,
                                               selected, 32);

    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;
        int claims;
        bool selected_here = false;

        if (!q2_pop_get_group(pop, gi, &g))
            continue;

        /* A group owned by another resident zone never crosses the streaming
         * boundary, even if a static LevelBin scan sees a conditional select
         * for returning to that zone. */
        claims = q2_pop_group_zone(&g);
        if (zone >= 0 && claims >= 0 && claims != zone) {
            local.other_zone++;
            continue;
        }

        /* Only the exact Zone<N> group is resident startup population.
         * Zone0Room2, Zone1Key and similar suffixes are separately selected
         * CREBATCH groups on the disc, just like unzoned ambushes. */
        if (zone >= 0 && !q2_pop_group_is_zone(&g)) {
            u32 si;
            u32 n = selected_count < 32 ? selected_count : 32;

            for (si = 0; si < n; si++) {
                if (selected[si] + 12 > levelbin_size)
                    continue;
                if (memcmp(levelbin + selected[si], g.name, 12) == 0) {
                    selected_here = true;
                    break;
                }
            }

            if (!selected_here) {
                local.not_selected++;
                continue;
            }
        }

        /* The retail spawn pass sets group flag bit 1 so neither another
         * selector nor a later CREBATCH can run the same list twice. Mark it
         * before walking the list for the same one-shot behaviour even if an
         * allocation failure makes this pass partial. */
        if (group_run) {
            if (group_run[gi])
                continue;
            group_run[gi] = 1;
        }

        /* Every group can carry a place list, the path group included: the
         * spawn and place lists are independent. */
        (void)spawn_group_places(set, pop, &g, gi,
                                 zone < 0 ? -1 : q2_cre_skill(),
                                 table, coll, &local);
    }

    if (stats)
        *stats = local;

    return local.no_memory ? Q2_ERR_NO_MEMORY : Q2_OK;
}

q2_result q2_item_spawn_group(q2_entity_set *set,
                              const q2_population *pop,
                              const char *group,
                              const q2_item_table *table,
                              q2_collision *coll,
                              u8 *group_run,
                              q2_item_spawn_stats *stats)
{
    q2_item_spawn_stats local;
    u32 gi;

    if (!set || !pop || !group || !group[0])
        return Q2_ERR_INVALID_ARG;

    memset(&local, 0, sizeof(local));
    if (!table)
        table = q2_item_table_builtin();

    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;

        if (!q2_pop_get_group(pop, gi, &g))
            continue;
        if (strcmp(g.name, group) != 0)
            continue;

        if (group_run) {
            if (group_run[gi]) {
                if (stats)
                    *stats = local;
                return Q2_OK;
            }
            group_run[gi] = 1;
        }

        (void)spawn_group_places(set, pop, &g, gi, q2_cre_skill(),
                                 table, coll, &local);
        if (stats)
            *stats = local;
        return local.no_memory ? Q2_ERR_NO_MEMORY : Q2_OK;
    }

    if (stats)
        *stats = local;
    return Q2_ERR_NOT_FOUND;
}

q2_result q2_item_spawn_all(q2_entity_set *set, const q2_population *pop,
                            const q2_item_table *table,
                            q2_item_spawn_stats *stats)
{
    /* A census, not a placement: no hull, so no drop. */
    return q2_item_spawn_zone(set, pop, -1, NULL, 0, table, NULL, NULL,
                              stats);
}

/* ------------------------------------------------------------------------- */
/* The think                                                                  */
/* ------------------------------------------------------------------------- */
s32 q2_item_glow_pulse(s32 level_time)
{
    /*
     * 0x80059550: `table[(time << 6) & 0x3FC0]` reads a 256-entry signed table
     * with a 2-byte stride, i.e. entry `time & 0xFF`, then `(v + 4096) >> 4`.
     * The port's sine is indexed on the 4096-step circle, so the same 256 steps
     * are `(time & 0xFF) * 16`.
     */
    s32 v = q2_sin12((level_time & 0xFF) * 16);
    return (v + Q2_ONE_12) >> 4;
}

void q2_item_shrink_think(q2_entity *e, q2_entity_world *w)
{
    if (!e || !w)
        return;

    /* 0x8005B368: scale falls by 16 per tick and the entity goes when it hits
     * zero. */
    e->scale = (s16)(e->scale - Q2_ITEM_SHRINK_RATE * w->dt);
    if (e->scale <= 0)
        q2_entity_remove(e);
}

/* The collected path: 0x800598CC (freed or respawned) and 0x8005998C (kept). */
static void item_collected(q2_entity *e, q2_entity_world *w, u32 player,
                           bool keep)
{
    u32 i;

    /* 0x8005B6C0: the pickup particle burst, in both branches. */
    q2_ent_burst_at(&w->events, e->origin, e->glow, e->model_index, e->model_bank);

    /*
     * THE KEPT BRANCH TOUCHES NOTHING ELSE. 0x8005998C is two instructions —
     * the burst and a jump to the epilogue — so a weapon left standing by
     * weapons-stay stays VISIBLE, which is the whole point of the rule: the
     * next player has to be able to see the gun they are allowed to take.
     *
     * This used to hide it before the early-out, so the item vanished for
     * everybody and the rule accomplished nothing but leaving a live entity
     * where a freed one belonged. Nothing marks it taken either, and it does
     * not need to: a second touch runs `give_weapon` again, is refused, and
     * returns Q2_TOUCH_NOTHING.
     */
    if (keep)
        return;

    e->hidden = true;

    if (w->deathmatch && !(e->flags & Q2_ITEM_TIMED)) {
        /* 0x800598FC: mega health waits a great deal longer than anything
         * else, and it is selected by the effect index rather than a flag. */
        e->respawn_at = (e->effect == 32) ? Q2_ITEM_RESPAWN_MEGA
                                          : Q2_ITEM_RESPAWN_TICKS;
        for (i = 0; i < Q2_MAX_PLAYERS; i++)
            e->taken[i] = true;
        return;
    }

    (void)player;
    q2_entity_remove(e);
}

void q2_item_think(q2_entity *e, q2_entity_world *w)
{
    u32 flags, p;

    if (!e || !w)
        return;

    flags = e->flags;

    /*
     * 0x80059360: in deathmatch a taken item counts its respawn down and does
     * nothing else. Outside deathmatch this block is skipped entirely, which is
     * why a collected item in single player is freed rather than hidden.
     */
    if (w->deathmatch && e->taken[0]) {
        e->respawn_at -= w->dt;
        if (e->respawn_at > 0)
            return;
        if (w->player_count == 0)
            return;
        for (p = 0; p < Q2_MAX_PLAYERS; p++)
            e->taken[p] = false;
        e->hidden = false;
        return;
    }

    if (flags == 0)
        goto touch_sweep;      /* 0x800593EC */

    /* 0x800593F4: the frame advances unless the record says not to. */
    if (!(flags & Q2_ITEM_NO_ANIM) && e->clip_length > 0) {
        e->frame += w->dt;
        while (e->frame >= e->clip_length)
            e->frame -= e->clip_length;
    }

    /* 0x8005945C: the spin, and the matrix rebuild it implies. */
    if (flags & Q2_ITEM_SPIN) {
        e->angles[1] = (s32)((s16)(e->angles[1] - Q2_ITEM_SPIN_RATE * w->dt));
    }

    /* 0x80059488: materialising. */
    if (flags & Q2_ITEM_MATERIALISE) {
        if (e->scale != (s16)Q2_ONE_12) {
            if (e->scale == 0)
                q2_ent_sound_entity(&w->events, Q2_SND_TELEPORT, e);

            e->scale = (s16)(e->scale + Q2_ITEM_SCALE_RATE * w->dt);
            if (e->scale > (s16)Q2_ONE_12) {
                e->scale = (s16)Q2_ONE_12;
                /* 0x800595DC: full size resets the tint to neutral. */
                e->glow[0] = e->glow[1] = e->glow[2] = 127;
            }

            /*
             * 0x80059608..0x800596B0: fifteen `(rand() - 16384) >> 9` triples
             * into the block at 0x800D4B24, then ONE group of them spawned at
             * the draw origin (`addiu a0, s2, 164`), in the ramp the glow bits
             * pick out of the flags word `s3` (0x80059648), on the area byte
             * +0x9E (`lbu v0, 158(s2)` at 0x800596A8). effect.h has the rest.
             *
             * The draws come off the item world's own generator in the order
             * they always did — 45, x/y/z per sparkle — so the stream every
             * later draw sees is unchanged by the burst now being SPAWNED rather
             * than thrown away. With no pool bound the same 45 are still drawn
             * and discarded: q2_fx_item_materialise refuses a NULL pool before
             * its first draw, and the stream must not depend on whether a
             * caller happens to have a presentation layer.
             */
            if (g_env.fx) {
                (void)q2_fx_item_materialise(g_env.fx, &w->rng, e->origin,
                                             e->surface, flags);
            } else {
                int i;
                for (i = 0; i < Q2_ITEM_SPARKLES * 3; i++)
                    (void)q2_rng_next(&w->rng);
            }
        }
    }

    /* 0x800596B8: the glow light. */
    if (flags & Q2_ITEM_GLOW) {
        s32 pulse = q2_item_glow_pulse(w->level_time);
        s32 lit[3];

        e->glow[0] = e->glow[1] = e->glow[2] = 0;
        if (flags & Q2_ITEM_GLOW_R) e->glow[0] = (u8)(pulse >> 2);
        if (flags & Q2_ITEM_GLOW_G) e->glow[1] = (u8)(pulse >> 2);
        if (flags & Q2_ITEM_GLOW_B) e->glow[2] = (u8)(pulse >> 2);

        lit[0] = e->origin[0] + Q2_ITEM_LIGHT_OFFSET_X;
        lit[1] = e->origin[1] + Q2_ITEM_LIGHT_OFFSET_Y;
        lit[2] = e->origin[2];
        q2_ent_light_at(&w->events, lit, e->glow, 0,
                        pulse + Q2_ITEM_GLOW_RADIUS_BIAS);
    }

    /* 0x800597C8: a dropped item's life. */
    if (flags & Q2_ITEM_TIMED) {
        e->remove_in -= w->dt;
        if (e->remove_in <= 0) {
            e->remove_in = Q2_ITEM_SHRINK_RESET;
            e->think     = q2_item_shrink_think;
            e->scale     = (s16)Q2_ONE_12;
        }
    }

touch_sweep:
    /* 0x80059810: every player, in slot order. */
    for (p = 0; p < w->player_count && p < Q2_MAX_PLAYERS; p++) {
        q2_entity_player *pl = &w->player[p];
        q2_touch_result r;
        bool keep;

        if (!pl->present || !pl->inv)
            continue;
        if (pl->inv->health <= 0)              /* 0x8005983C */
            continue;
        if (!q2_entity_bounds_overlap(e, &pl->ent))
            continue;

        w->granted_weapon = 0;
        r = q2_item_touch(e->effect, pl->inv, pl->pos, w);

        /*
         * The console's 0x80037E84 store, which this layer reports rather than
         * performs — see entity.h. Before the weapons-stay fold below, because
         * on the disc it happens inside 0x80036348, i.e. before 0x80059878 ever
         * looks at the result.
         */
        if (w->granted_weapon && w->weapon_grant)
            w->weapon_grant(w->weapon_grant_user, p, w->granted_weapon);

        /*
         * 0x80059878: a weapon-style pickup survives only when the item is not
         * a dropped one, weapons-stay is on and this is deathmatch. Everything
         * else folds back to an ordinary collection.
         */
        keep = false;
        if (r == Q2_TOUCH_WEAPON) {
            if (!(e->flags & Q2_ITEM_TIMED) && w->weapons_stay && w->deathmatch)
                keep = true;
        } else if (r == Q2_TOUCH_NOTHING) {
            continue;                          /* try the next player */
        }

        item_collected(e, w, p, keep);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* A creature's death drop — 0x8002085C and 0x80020C48                        */
/* ------------------------------------------------------------------------- */
/*
 * The flight state the console keeps at +0xE0..+0xE4, which q2_entity has no
 * field for (see Q2_ITEM_TOSS_MAX). Keyed by the set and the SLOT, because
 * q2_entity_alloc may move the whole array and never moves an index.
 */
typedef struct item_toss {
    const q2_entity_set *set;
    u32                  slot;
    s16                  vel[3];      /* +0xE0 x, +0xE2 y, +0xE4 z */
    bool                 used;
} item_toss;

static item_toss g_toss[Q2_ITEM_TOSS_MAX];

static bool toss_is(const item_toss *t, const q2_entity *e)
{
    return t->used && t->set && t->set->ent && t->slot < t->set->count &&
           &t->set->ent[t->slot] == e;
}

static item_toss *toss_find(const q2_entity *e)
{
    u32 i;

    if (!e)
        return NULL;
    for (i = 0; i < Q2_ITEM_TOSS_MAX; i++)
        if (toss_is(&g_toss[i], e))
            return &g_toss[i];
    return NULL;
}

/*
 * A record for (set, slot): the one that slot already had, else a free one,
 * else one whose entity is no longer an in-flight drop — its slot was freed or
 * reused by something that is not flying. NULL only when every record belongs
 * to a drop that really is in the air.
 */
static item_toss *toss_claim(const q2_entity_set *set, u32 slot)
{
    item_toss *free_rec = NULL;
    u32 i;

    for (i = 0; i < Q2_ITEM_TOSS_MAX; i++) {
        item_toss *t = &g_toss[i];

        if (t->used && t->set == set && t->slot == slot)
            return t;
        if (!free_rec && !t->used)
            free_rec = t;
    }
    if (free_rec)
        return free_rec;

    for (i = 0; i < Q2_ITEM_TOSS_MAX; i++) {
        item_toss *t = &g_toss[i];
        const q2_entity *owner;

        if (!t->set || !t->set->ent || t->slot >= t->set->count)
            return t;
        owner = &t->set->ent[t->slot];
        if (!owner->in_use || owner->think != q2_item_drop_think)
            return t;
    }
    return NULL;
}

void q2_item_drop_reset(void)
{
    memset(g_toss, 0, sizeof(g_toss));
}

u32 q2_item_drop_in_flight(void)
{
    u32 i, n = 0;

    for (i = 0; i < Q2_ITEM_TOSS_MAX; i++) {
        const item_toss *t = &g_toss[i];

        if (t->used && t->set && t->set->ent && t->slot < t->set->count &&
            t->set->ent[t->slot].in_use &&
            t->set->ent[t->slot].think == q2_item_drop_think)
            n++;
    }
    return n;
}

bool q2_item_drop_velocity(const q2_entity *e, s16 out[3])
{
    const item_toss *t = toss_find(e);

    if (!t || !out)
        return false;
    out[0] = t->vel[0];
    out[1] = t->vel[1];
    out[2] = t->vel[2];
    return true;
}

bool q2_item_drop_restore(const q2_entity_set *set, u32 slot, const s16 vel[3])
{
    item_toss *t;
    if (!set || slot >= set->count || !vel) return false;
    t = toss_claim(set, slot);
    if (!t) return false;
    t->set = set; t->slot = slot; t->used = true;
    memcpy(t->vel, vel, sizeof(t->vel));
    return true;
}

/* The draw origin every frame after the first: +0xA4 = +0x54, y lowered by
 * 286 and raised by the bias (0x80020CC0..0x80020CF0, and the identical tail
 * at 0x80020D20..0x80020D4C). */
static void drop_origin(q2_entity *e)
{
    e->origin[0] = e->pos[0];
    e->origin[1] = e->pos[1] + Q2_EYE_BASE - e->model_offset;
    e->origin[2] = e->pos[2];
}

q2_entity *q2_item_drop_spawn(q2_entity_set *set, const q2_item_table *table,
                              const struct q2_model_bank *const *banks,
                              u32 bank_count, u8 item_id, u16 heading,
                              const s32 pos[3], s32 cell, int (*rand15)(void))
{
    const q2_item_def *def;
    item_toss *t;
    q2_entity *e;
    u32 i, slot;
    s32 r, speed, kick;
    s16 ext2;
    s32 c, s;

    if (!set || !pos || !rand15)
        return NULL;

    if (!table)
        table = q2_item_table_builtin();

    /* 0x80020888 `jal 0x8005B5D8` — the same place-id scan the placed spawner
     * uses — and 0x80020894 `beq s2, zero`: no record, nothing spawns. */
    def = q2_item_find(table, (s32)item_id);
    if (!def)
        return NULL;

    /*
     * The port's one refusal, taken BEFORE the allocation so it costs no
     * entity and no random number: nowhere to keep the velocity. See
     * Q2_ITEM_TOSS_MAX. The claim is by slot, which is only known after the
     * allocation, so this asks whether ANY record could be had.
     */
    if (!toss_claim(NULL, (u32)-1))
        return NULL;

    /* 0x8002089C `jal 0x8006C098` with a0 = 1, and 0x800208A8: out of
     * entities, nothing spawns. */
    e = q2_entity_alloc(set);
    if (!e)
        return NULL;
    slot = (u32)(e - set->ent);

    /*
     * 0x8006C098 clears the record (0x8006C18C `jal 0x80089E18`, 768 bytes)
     * and writes nothing into +0x10C, and this spawner does not either. So the
     * placed spawner's 0x08000001 (0x800587C0), which q2_entity_init stands
     * for, is not a drop's: no floor shadow and no shadow link (0x80046B4C).
     */
    e->render_flags = 0;

    e->think = q2_item_drop_think;          /* 0x800208B8: 0x80020C48        */
    e->kind  = Q2_ENT_KIND_ITEM;            /* 0x800208C4: 46                */
    e->def   = def;
    e->place_id = item_id; /* Native identity for rebinding the item table after a zone load. */

    memcpy(e->model, def->model, sizeof(e->model));
    /* 0x80020954 `sw v1, 4(v0)` — the record's +16 list becomes the model
     * wrapper's shadow vertices, as the placed spawner does at 0x80059AC0. */
    for (i = 0; i < Q2_ITEM_SHADOW_VERTEX_MAX; i++)
        e->shadow_vertex[i] = def->shadow_vertex[i];
    e->shadow_vertex_count = def->shadow_vertex_count;

    /*
     * 0x80020944 `jal 0x8006D008` looks the name up; 0x80020960 `bne v0,
     * zero` — found, carry on; otherwise 0x80020968 frees the entity and
     * nothing spawns. The resolve also supplies the bias 0x8006D100 reads
     * (`lh model[+0x1C]`, stored at +0xF8 by 0x80020980) and the clip length
     * the item think will wrap the frame against.
     */
    if (bank_count && banks) {
        bool found = false;

        for (i = 0; i < bank_count && !found; i++)
            found = q2_entity_resolve_model(e, banks[i]);
        if (!found) {
            q2_entity_remove(e);
            return NULL;
        }
    }
    ext2 = e->model_offset;

    /*
     * THE POSITION IS THE RECORD'S, EXACTLY. +0xA4 at 0x80020984..0x80020998
     * and +0x54 at 0x800209B8..0x800209CC, both straight from the three words
     * the queue copied off the creature. No lift, no sweep.
     *
     * And the SPAWN-FRAME draw origin: `addiu v0, v0, -286` (0x800209A4) then
     * `subu v0, v0, v1` with v1 the bias (0x800209AC). +Y is down, so this is
     * 286 + bias ABOVE the position, where the think's every later write is
     * 286 below it. Reproduced as read.
     */
    for (i = 0; i < 3; i++) {
        e->pos[i]    = pos[i];
        e->origin[i] = pos[i];
    }
    e->origin[1] = pos[1] - Q2_EYE_BASE - ext2;

    /* 0x800209D0..0x800209FC: both ambient triplets from 0x800AE718, "@@@" —
     * 0x40 each, the allocator's own value written again. */
    e->glow[0] = e->glow[1] = e->glow[2] = 0x40;

    /*
     * THE TOSS. 0x80020A00 draws the speed and 0x80020A58 the kick, in that
     * order. `r * 3 << 8` and `>> 15` is `r * 768 / 32768`, whose `bgez`
     * rounding arm is never taken for a BIOS rand(); `-r * 3 << 9` then
     * `+32767` and `>> 15` IS taken, and truncates toward zero.
     */
    r     = rand15() & 0x7FFF;
    speed = Q2_ITEM_DROP_SPEED_BASE + (r * Q2_ITEM_DROP_SPEED_SPAN) / 32768;

    /*
     * 0x80020A24..0x80020A38: the entry at 0x800A5430 + (heading & 0xFFF) * 4
     * is {sine, cosine}, and the SECOND halfword (`lh v0, 2(s3)`) goes to
     * +0xE0, x; the first (`lh v0, 0(s3)`, 0x80020A9C) to +0xE4, z. Each is
     * `* speed` then the compiler's truncating divide by 4096 (the `bgez` /
     * `+4095` / `sra 12` triple). trig.h is that table, 4096 of 4096.
     */
    c = q2_cos12((s32)(heading & 0xFFFu));
    s = q2_sin12((s32)(heading & 0xFFFu));

    r    = rand15() & 0x7FFF;
    kick = Q2_ITEM_DROP_KICK_BASE - (r * Q2_ITEM_DROP_KICK_SPAN) / 32768;

    t = toss_claim(set, slot);
    if (!t) {
        /* Cannot happen: a record was available above and nothing between
         * the check and here claims one. Belt and braces. */
        q2_entity_remove(e);
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->used   = true;
    t->set    = set;
    t->slot   = slot;
    t->vel[0] = (s16)((c * speed) / 4096);
    /* 0x80020A84..0x80020A94: the kick is taken as a halfword (`sll 16` /
     * `sra 16`) and HALVED by the signed divide (`srl 31` / `addu` / `sra 1`),
     * so -3072..-4607 becomes -1536..-2303: upward. */
    t->vel[1] = (s16)((s32)(s16)kick / 2);
    t->vel[2] = (s16)((s * speed) / 4096);

    /* 0x80020AC8 `jal 0x80089E18` (a2 = 6) — the angles, zeroed, and the
     * matrix built from them at 0x80020AD4. The item never faces anywhere. */
    e->angles[0] = e->angles[1] = e->angles[2] = 0;

    e->remove_in = Q2_ITEM_DROP_TOSS_LIFE;  /* 0x80020AE0 */

    /*
     * 0x80020AE8 `sh s4, 162(s1)` — the cell — and 0x80020AF4 +0xA0 = -1.
     * Then +0x9E is that cell's byte +32 in SecondaryCol's node table
     * (0x800C8FEC, `lbu 32` at 0x80020B0C). The console indexes with whatever
     * the cell is; the port reads it only for a real one.
     */
    e->node = cell;
    if (cell >= 0 && g_env.hull) {
        q2_coll_node cn;

        if (q2_collision_get_node(g_env.hull, (u32)cell, &cn))
            e->surface = cn.contents;
    }

    /*
     * 0x80020B18..0x80020B44: item 21 keeps the record's word (`lh`), every
     * other id takes it with Q2_ITEM_TIMED (`lhu`, `ori 4`, sign-extended).
     * The flags fit in fifteen bits, so the sign never shows.
     */
    if (item_id == Q2_ITEM_DROP_KEEPS_FLAGS)
        e->flags = (u32)(s32)(s16)def->flags;
    else
        e->flags = (u32)(s32)(s16)(def->flags | Q2_ITEM_TIMED);

    /* 0x80020B50 +0x50 = 0, which the allocator already did; 0x80020B54 the
     * touch-dispatch index. */
    e->effect = def->effect;

    /*
     * 0x80020B4C..0x80020C1C: mins (-256, ext2 - 512, -256) and maxs (256,
     * ext2, 256), each built as a halfword triple on the stack, and the
     * absolute box +0x78..+0x8C = position + each. Set here and nowhere else.
     */
    e->bounds_min[0] = pos[0] - Q2_ITEM_DROP_BOX_HALF;
    e->bounds_min[1] = pos[1] + (s16)(ext2 - Q2_ITEM_DROP_BOX_DEPTH);
    e->bounds_min[2] = pos[2] - Q2_ITEM_DROP_BOX_HALF;
    e->bounds_max[0] = pos[0] + Q2_ITEM_DROP_BOX_HALF;
    e->bounds_max[1] = pos[1] + ext2;
    e->bounds_max[2] = pos[2] + Q2_ITEM_DROP_BOX_HALF;

    e->field90 = Q2_ITEM_DROP_FIELD90;      /* 0x80020BB8 */

    return e;
}

/*
 * 0x800463E8 as 0x80046DDC calls it — SecondaryCol, the +0xA2 cell, and every
 * argument zero: no out-pointer, no rebound, no trigger flag. Returns whether
 * anything was touched, the mover's `s3`.
 *
 * What is NOT here, and why each is unobservable for a drop:
 *   - the two contact-normal slots at +0x60 / +0x66 and the ground bit
 *     +0x98 & 0x20. Their one reader on this path is the shadow link at
 *     0x80046B60, which a drop never reaches (render flags 0; see the spawn);
 *   - the per-player copy of +0x9E into +0x119 + 100n (0x80046B28): the port
 *     keeps no per-player block on an entity;
 *   - the rebound (0x800466C0..0x80046A5C): with a2 = 0 its speed term is
 *     zero, so the reflected, normalised direction it computes is multiplied
 *     by nothing and the velocity comes out (0, 0, 0). That product is kept.
 */
static bool toss_move(q2_entity *e, s16 vel[3], s32 dt)
{
    s32 start[3], dest[3], end[3];
    s16 delta[3];
    s16 normal[3] = { 0, 0, 0 };
    s32 gravity = g_env.gravity ? *g_env.gravity : Q2_GRAVITY;
    bool contact = false, have_normal = false;
    int k;

    for (k = 0; k < 3; k++)
        start[k] = e->pos[k];

    /* 0x80046450..0x800464A0: `lhu`, add `[0x800AE924] * [0x800B2DB4]`, `sh`
     * — a halfword add — then `slti 8193` on the sign-extended result. */
    if (!(e->render_flags & Q2_ITEM_TOSS_NO_GRAVITY)) {
        s16 vy = (s16)(u16)((u32)(u16)vel[1] + (u32)(gravity * dt));

        if (vy > Q2_ITEM_TOSS_TERMINAL)
            vy = Q2_ITEM_TOSS_TERMINAL;
        vel[1] = vy;
    }

    /* 0x800464D4..0x800465B8: the step, truncated, stored as a halfword and
     * added back sign-extended. */
    for (k = 0; k < 3; k++) {
        delta[k] = (s16)(((s32)vel[k] * dt) / Q2_ITEM_TOSS_STEP_DIV);
        dest[k]  = start[k] + delta[k];
    }

    /*
     * 0x800465BC `jal 0x80053974` — the ENTITY boxes first, a bare point
     * against every active one, nearest hit kept (it shrinks the segment as it
     * goes). A hit is kind 2 (0x80053AEC), which is a contact (0x800465E4),
     * and the destination it wrote back is then pulled back by the whole step
     * (0x80046650..0x8004667C `subu`). Both as read.
     */
    if (g_env.ents) {
        q2_move_seg_hit hit;

        if (q2_move_clip_segment(g_env.ents, start, dest, NULL, &hit)) {
            contact     = true;
            have_normal = true;
            for (k = 0; k < 3; k++) {
                normal[k] = hit.normal[k];
                dest[k]   = hit.pos[k] - delta[k];
            }
        }
    }

    /* 0x80046694 `jal 0x80044C44` through SecondaryCol from the +0xA2 cell. A
     * stop is a contact, and its plane's normal replaces the entity's
     * (0x800466A4..0x800466B4). */
    for (k = 0; k < 3; k++)
        end[k] = dest[k];
    if (g_env.hull) {
        s32 node = e->node;

        if (!q2_coll_move(g_env.hull, start, dest, e->node, end, &node)) {
            q2_coll_plane pl;

            contact = true;
            if (g_env.hull->hit_plane_index >= 0 &&
                q2_collision_get_plane(g_env.hull,
                                       (u32)g_env.hull->hit_plane_index,
                                       &pl)) {
                normal[0]   = pl.nx;
                normal[1]   = pl.ny;
                normal[2]   = pl.nz;
                have_normal = true;
            }
        }

        /* 0x80046ACC: +0xA2 takes the move's last cell, and 0x80046AF8 +0x9E
         * that cell's byte +32 — contact or not. */
        e->node = node;
        if (node >= 0) {
            q2_coll_node cn;

            if (q2_collision_get_node(g_env.hull, (u32)node, &cn))
                e->surface = cn.contents;
        }
    }

    if (!contact) {
        /* 0x80046AB4: the destination, as the trace left it. */
        for (k = 0; k < 3; k++)
            e->pos[k] = dest[k];
        return false;
    }

    /* 0x8004670C `jal 0x8005625C` — one unit back along the dominant axis of
     * the contact normal, subtracted from where the trace stopped
     * (0x800467D8..0x80046820), which becomes +0x54 at 0x800469D8. */
    if (have_normal) {
        s16 push[3];

        q2_move_push_vector(normal, push);
        for (k = 0; k < 3; k++)
            end[k] -= push[k];
    }
    for (k = 0; k < 3; k++)
        e->pos[k] = end[k];

    /* 0x80046A24..0x80046A64: `(unit * ((speed * 0) >> 12)) >> 12` per axis. */
    vel[0] = vel[1] = vel[2] = 0;
    return true;
}

/* 0x80020CB8..0x80020D1C — it has come to rest. */
static void drop_land(q2_entity *e)
{
    /* +0x50 = 1 (0x80020CBC): no field on the port's record, and nothing on
     * the item think's path reads it. Named rather than dropped silently. */
    drop_origin(e);

    /* 0x80020CF4 `andi v0, a0, 0x8`: an OBJECTIVE item keeps its word; any
     * other takes TIMED (again — the spawn already gave it to all but 21). */
    if (!(e->flags & Q2_ITEM_OBJECTIVE))
        e->flags |= Q2_ITEM_TIMED;

    /* 0x80020CFC: 8700 sits in the branch's DELAY SLOT, so both arms store
     * it at 0x80020D0C. */
    e->remove_in = Q2_ITEM_DROP_LANDED_LIFE;

    /* 0x80020D10..0x80020D1C: from now on it is an ordinary item — spin,
     * glow, the TIMED countdown and the touch sweep. */
    e->think = q2_item_think;
}

void q2_item_drop_think(q2_entity *e, q2_entity_world *w)
{
    item_toss *t;
    bool contact;

    if (!e || !w)
        return;

    /*
     * No record: the flight state was forgotten under a live drop (a reset
     * that did not free the set). There is no velocity to fly with, so it
     * rests where it is — the one outcome that cannot strand it in the air.
     */
    t = toss_find(e);
    if (!t) {
        drop_land(e);
        return;
    }

    /* 0x80020C64 `jal 0x80046DDC`, a1..a3 and the fifth argument all zero. */
    contact = toss_move(e, t->vel, w->dt);

    /*
     * 0x80020C6C `beq v0, zero` and 0x80020C74..0x80020CB4: a contact, and
     * the squared speed at most 0xC34FF, lands it. The squares are `mult` /
     * `mflo` and the sum `addu`, 32 bits with wrap, compared SIGNED (`slt`).
     */
    if (contact) {
        u32 v2 = (u32)((s32)t->vel[0] * t->vel[0]) +
                 (u32)((s32)t->vel[1] * t->vel[1]) +
                 (u32)((s32)t->vel[2] * t->vel[2]);

        if (!((s32)Q2_ITEM_DROP_REST_SPEED2 < (s32)v2)) {
            t->used = false;
            drop_land(e);
            return;
        }
    }

    drop_origin(e);                         /* 0x80020D20..0x80020D4C */
}
