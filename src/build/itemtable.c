#include "itemtable.h"

#include "exe.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* The table as it reads on the catalogued PAL disc                           */
/*                                                                            */
/* Transcribed from 0x8009F5CC, 24 bytes per record, in disc order — which is  */
/* NOT sorted by place id and does contain a duplicate (place id 6 twice).     */
/* Both are kept because q2_item_find is the engine's first-match scan.        */
/* ------------------------------------------------------------------------- */
static const q2_item_def k_defs[Q2_ITEM_COUNT] = {
    {  57,   0, 0x0001, "Q2LOGO",       { 0xFFFF }, 0 },
    {  58,   0, 0x0001, "q2title",      { 0xFFFF }, 0 },
    {  59,   0, 0x0001, "Male2",        { 0xFFFF }, 0 },
    {  60,   0, 0x0001, "Male2red",     { 0xFFFF }, 0 },
    {  61,   0, 0x0001, "Male2purple",  { 0xFFFF }, 0 },
    {  62,   0, 0x0001, "Male2aqua",    { 0xFFFF }, 0 },
    {  63,   0, 0x0001, "Flagred",      { 0xFFFF }, 0 },
    {  64,   0, 0x0001, "Flagblue",     { 0xFFFF }, 0 },
    {   0,  33, 0x0000, "Adrenal P",    { 0xFFFF }, 0 },
    {   1,   0, 0x0000, "Anchead P",    { 0xFFFF }, 0 },
    {   2,  26, 0x0001, "Body P",       { 0xFFFF }, 0 },
    {   3,  27, 0x0001, "Combat P",     { 0xFFFF }, 0 },
    {   4,  28, 0x0001, "Jacket P",     { 0x002E, 0x003A, 0xFFFF }, 2 },
    {   5,  29, 0x0001, "Shard P",      { 0xFFFF }, 0 },
    {   6,  38, 0x0001, "Bandol P",     { 0xFFFF }, 0 },
    {   7,  43, 0x0000, "!Breather P",  { 0xFFFF }, 0 },
    {   8,  42, 0x0001, "Enviro P",     { 0xFFFF }, 0 },
    {  12,  36, 0x0000, "Stimpack P",   { 0xFFFF }, 0 },
    {   9,  34, 0x0000, "Medi P",       { 0xFFFF }, 0 },
    {  10,  35, 0x0000, "Large Medi P", { 0xFFFF }, 0 },  /* fills all 12 bytes */
    {  11,  32, 0x0000, "Mega Medi P",  { 0xFFFF }, 0 },
    {  13,  41, 0x0001, "Invulner P",   { 0xFFFF }, 0 },
    {  14,  37, 0x0001, "Pack P",       { 0xFFFF }, 0 },
    {  15,  31, 0x0001, "Screen P",     { 0xFFFF }, 0 },
    {  16,  30, 0x0001, "Shield P",     { 0xFFFF }, 0 },
    {  17,  40, 0x0001, "Quaddamage P", { 0x000A, 0x0015, 0xFFFF }, 2 },
    {  18,  39, 0x0000, "Silence P",    { 0xFFFF }, 0 },
    {   6,  38, 0x0001, "Bandol P",     { 0xFFFF }, 0 },  /* the duplicate */
    {  19,   0, 0x0000, "Barrel P",     { 0xFFFF }, 0 },
    {  20,   0, 0x0180, "Blackhole P",  { 0xFFFF }, 0 },
    {  23,  19, 0x0000, "Bullets P",    { 0xFFFF }, 0 },
    {  24,  22, 0x0000, "Cells P",      { 0xFFFF }, 0 },
    {  25,  20, 0x0000, "Grenade P",    { 0xFFFF }, 0 },
    {  26,  21, 0x0000, "Rockets P",    { 0xFFFF }, 0 },
    {  27,  18, 0x0000, "Shells P",     { 0xFFFF }, 0 },
    {  28,  23, 0x0000, "Slugs P",      { 0xFFFF }, 0 },
    {  29,  24, 0x0000, "Flame Fuel P", { 0xFFFF }, 0 },  /* fills all 12 bytes */
    {  32,  11, 0x0001, "Bfg P",        { 0x002C, 0x0038, 0xFFFF }, 2 },
    {  33,   5, 0x0001, "Chaingun P",   { 0x0011, 0x0041, 0xFFFF }, 2 },
    {  34,   7, 0x0001, "Glaunch P",    { 0x000C, 0x0045, 0xFFFF }, 2 },
    {  35,   9, 0x0001, "Hyperbl P",    { 0x0010, 0x004B, 0xFFFF }, 2 },
    {  36,   4, 0x0001, "Machgun P",    { 0x0015, 0x0027, 0xFFFF }, 2 },
    {  37,  10, 0x0001, "Railgun P",    { 0x0008, 0x0045, 0xFFFF }, 2 },
    {  38,   8, 0x0001, "Rocketl P",    { 0x005F, 0x002C, 0xFFFF }, 2 },
    {  39,   2, 0x0001, "Shotgun P",    { 0x000D, 0x003F, 0xFFFF }, 2 },
    {  40,   3, 0x0001, "Sshotgun P",   { 0x0012, 0x004C, 0xFFFF }, 2 },
    {  43,  15, 0x0001, "Flame P",      { 0xFFFF }, 0 },
    {  42,  12, 0x0001, "Ionripper P",  { 0xFFFF }, 0 },
    {  41,  13, 0x0001, "Plasmagun P",  { 0xFFFF }, 0 },
    {  30,  16, 0x0001, "Tesla P",      { 0xFFFF }, 0 },
    {  31,   0, 0x0001, "Mines P",      { 0xFFFF }, 0 },
    {  44,  14, 0x0001, "Discharge P",  { 0xFFFF }, 0 },
    {  45,  44, 0x004B, "Bluekey P",    { 0xFFFF }, 0 },
    {  46,  45, 0x0019, "Redkey P",     { 0xFFFF }, 0 },
    {  47,  46, 0x0079, "Pass P",       { 0xFFFF }, 0 },
    {  21,  17, 0x0079, "Nuke P",       { 0xFFFF }, 0 },
    {  49,  48, 0x0009, "Head P",       { 0x0009, 0x001A, 0xFFFF }, 2 },
    {  50,  51, 0x0019, "Pkeyred P",    { 0xFFFF }, 0 },
    {  51,  50, 0x0059, "Pkeypurp P",   { 0xFFFF }, 0 },
    {  52,  52, 0x0079, "Datacd P",     { 0xFFFF }, 0 },
    {  53,  53, 0x0079, "Datasp P",     { 0xFFFF }, 0 },
    {  54,  54, 0x0029, "Greenkey P",   { 0xFFFF }, 0 },
    {  55,  55, 0x0039, "Yellowkey P",  { 0xFFFF }, 0 },
    {  56,  56, 0x0079, "Whitekey P",   { 0xFFFF }, 0 }
};

/*
 * The twelve dispatch slots that point at the failure exit, as effect indices
 * rather than table offsets. Read off the jump table at 0x800AC30C; the loader
 * re-derives them from the disc and `q2psx-inspect items` compares.
 *
 * 6 and 25 are gaps no record names; 14 and 47/49 likewise. The other eight are
 * named by real records — see itemtable.h.
 */
static const u8 k_inert_effects[] = { 6, 12, 13, 14, 15, 16, 24, 25, 31, 36, 47, 49 };

/* 0x800AC240, 12-byte stride. The loader at 0x800374BC stores them into
 * gp+17032… in this order, which is what fixes the order. */
static const char *const k_sound_names[11] = {
    "itm_pkup",      /* generic                           */
    "wep_noammo",    /* weapon pickup — yes, this one     */
    "msc_am_pkup",   /* ammo                              */
    "itm_m_health",  /* mega health                       */
    "itm_l_health",  /* large medkit                      */
    "itm_n_health",  /* medkit                            */
    "msc_ar1_pkup",  /* armour                            */
    "msc_ar2_pkup",  /* armour shard                      */
    "msc_power1",    /* power shield / cells while shielded */
    "itm_damage3",   /* quad damage                       */
    "itm_protect"    /* invulnerability                   */
};

/*
 * 0x800AC144, chased through: 57 pointers, one per effect index, and the array
 * the pickup caption's `%s` comes out of. Transcribed rather than derived from
 * the model names, because they are not the same words — record `Sshotgun P`
 * reads out as "Super Shotgun" and `Medi P` as "Health".
 *
 * Three effects share the string "Health" (34, 35 and the inert 36) and two
 * share "Tesla" (16 and 25); in the executable those are the same pointer
 * twice, so the duplication is the table's, not this transcription's.
 */
static const char *const k_names[Q2_ITEM_NAME_COUNT] = {
    /*  0 */ "",
    /*  1 */ "Blaster",
    /*  2 */ "Shotgun",
    /*  3 */ "Super Shotgun",
    /*  4 */ "Machinegun",
    /*  5 */ "Chaingun",
    /*  6 */ "Grenades",
    /*  7 */ "Grenade Launcher",
    /*  8 */ "Rocket Launcher",
    /*  9 */ "HyperBlaster",
    /* 10 */ "Railgun",
    /* 11 */ "BFG10K",
    /* 12 */ "IonRipper",
    /* 13 */ "Plasma Beam",
    /* 14 */ "Discharger",
    /* 15 */ "FlameGun",
    /* 16 */ "Tesla",
    /* 17 */ "A-M Bomb",
    /* 18 */ "Shells",
    /* 19 */ "Bullets",
    /* 20 */ "Grenades",
    /* 21 */ "Rockets",
    /* 22 */ "Cells",
    /* 23 */ "Slugs",
    /* 24 */ "Fuel",
    /* 25 */ "Tesla",
    /* 26 */ "Body Armour",
    /* 27 */ "Combat Armour",
    /* 28 */ "Jacket Armour",
    /* 29 */ "Armour Shard",
    /* 30 */ "Power Shield",
    /* 31 */ "Power Screen",
    /* 32 */ "Mega Health",
    /* 33 */ "Adrenaline",
    /* 34 */ "Health",
    /* 35 */ "Health",
    /* 36 */ "Health",
    /* 37 */ "Ammo Pack",
    /* 38 */ "Bandolier",
    /* 39 */ "Silencer",
    /* 40 */ "Quad Damage",
    /* 41 */ "Invulnerability",
    /* 42 */ "Environment Suit",
    /* 43 */ "Rebreather",
    /* 44 */ "Blue Key",
    /* 45 */ "Red Key",
    /* 46 */ "Security Pass",
    /* 47 */ "AntiMatter Core",
    /* 48 */ "Commander's Head",
    /* 49 */ "Power Cube",
    /* 50 */ "Purple Pyramid Key",
    /* 51 */ "Red Pyramid Key",
    /* 52 */ "Data CD",
    /* 53 */ "Data Spinner",
    /* 54 */ "Green Key",
    /* 55 */ "Yellow Key",
    /* 56 */ "White Key"
};

/* ------------------------------------------------------------------------- */
static void fill_builtin(q2_item_table *t)
{
    u32 i;

    memset(t, 0, sizeof(*t));
    memcpy(t->def, k_defs, sizeof(k_defs));
    t->count = Q2_ITEM_COUNT;

    t->dispatch_default = 0x800372E8u;
    for (i = 0; i < Q2_ITEM_EFFECT_COUNT; i++)
        t->dispatch[i] = 1;                 /* live, address not transcribed */
    for (i = 0; i < sizeof(k_inert_effects); i++) {
        u32 e = k_inert_effects[i];
        if (e >= Q2_ITEM_EFFECT_FIRST && e <= Q2_ITEM_EFFECT_LAST)
            t->dispatch[e - Q2_ITEM_EFFECT_FIRST] = t->dispatch_default;
    }

    for (i = 0; i < 11; i++) {
        strncpy(t->sound[i], k_sound_names[i], Q2_ITEM_MODEL_LEN);
        t->sound[i][Q2_ITEM_MODEL_LEN] = '\0';
    }

    for (i = 0; i < Q2_ITEM_NAME_COUNT; i++) {
        strncpy(t->name[i], k_names[i], Q2_ITEM_NAME_LEN);
        t->name[i][Q2_ITEM_NAME_LEN] = '\0';
    }
}

const q2_item_table *q2_item_table_builtin(void)
{
    static q2_item_table t;
    static bool built = false;

    if (!built) {
        fill_builtin(&t);
        built = true;
    }
    return &t;
}

q2_result q2_item_table_load(q2_item_table *out, const disc *d,
                             const q2_build_id *id)
{
    q2_exe exe;
    q2_result r;
    u32 addr, i;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("item table location is unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }

    r = q2_exe_load(&exe, d, id->exe_name);
    if (r != Q2_OK)
        return r;

    addr = Q2_ITEMTABLE_ADDR_SLES01534;

    /* Walk to the terminator exactly as the spawner does rather than trusting a
     * record count: a build whose table grew would then read long instead of
     * silently truncating. */
    for (i = 0; i < Q2_ITEM_COUNT; i++) {
        const u8 *rec = q2_exe_ptr(&exe, addr + i * Q2_ITEM_RECORD_SIZE,
                                   Q2_ITEM_RECORD_SIZE);
        q2_item_def *e = &out->def[i];
        u32 k;

        if (!rec)
            break;
        if ((s8)rec[0] == -1)
            break;                          /* the 0xFF terminator */

        e->place_id = (s8)rec[0];
        e->effect   = rec[1];
        e->flags    = q2_rd_u16(rec + 0x02);

        /* Two names use all twelve bytes with no NUL, so the copy has to be
         * bounded by the field and terminated here. */
        memcpy(e->model, rec + 0x04, Q2_ITEM_MODEL_LEN);
        e->model[Q2_ITEM_MODEL_LEN] = '\0';

        for (k = 0; k < Q2_ITEM_SHADOW_VERTEX_MAX; k++)
            e->shadow_vertex[k] = q2_rd_u16(rec + 0x10 + k * 2);

        e->shadow_vertex_count = 0;
        for (k = 0; k < Q2_ITEM_SHADOW_VERTEX_MAX; k++) {
            if (e->shadow_vertex[k] == 0xFFFFu)
                break;
            e->shadow_vertex_count++;
        }
    }
    out->count = i;

    /* The dispatch, so "inert" is read rather than asserted. */
    {
        u32 base = Q2_ITEM_DISPATCH_SLES01534;
        u32 counts[Q2_ITEM_EFFECT_COUNT], best = 0, best_n = 0, n;

        memset(counts, 0, sizeof(counts));
        for (i = 0; i < Q2_ITEM_EFFECT_COUNT; i++) {
            u32 v = 0;
            if (!q2_exe_u32(&exe, base + i * 4, &v))
                v = 0;
            out->dispatch[i] = v;
        }
        /* The failure exit is whichever target the most slots share — twelve of
         * them against one apiece for the real handlers. */
        for (i = 0; i < Q2_ITEM_EFFECT_COUNT; i++) {
            u32 j;
            n = 0;
            for (j = 0; j < Q2_ITEM_EFFECT_COUNT; j++)
                if (out->dispatch[j] == out->dispatch[i])
                    n++;
            if (n > best_n) { best_n = n; best = out->dispatch[i]; }
        }
        out->dispatch_default = best_n > 1 ? best : 0;
    }

    for (i = 0; i < 11; i++) {
        const u8 *p = q2_exe_ptr(&exe, Q2_ITEM_SOUNDNAMES_SLES01534 + i * 12, 12);
        if (!p)
            break;
        memcpy(out->sound[i], p, 12);
        out->sound[i][Q2_ITEM_MODEL_LEN] = '\0';
    }

    /*
     * The display names — a table of POINTERS, so each one has to be chased.
     * Unlike the sound names above there is no fixed stride to copy: the
     * strings are ordinary NUL-terminated data scattered across two runs
     * (0x800ABEB4… and 0x800AE818…), and several entries share a pointer.
     */
    for (i = 0; i < Q2_ITEM_NAME_COUNT; i++) {
        u32 ptr = 0, k;

        if (!q2_exe_u32(&exe, Q2_ITEM_NAMES_SLES01534 + i * 4, &ptr) || !ptr)
            continue;

        /* Byte at a time and bounded by the FIELD, so a pointer that lands
         * outside the segment stops rather than walking off the image, and a
         * build whose names grew truncates here and is caught by the diff. */
        for (k = 0; k < Q2_ITEM_NAME_LEN; k++) {
            const u8 *ch = q2_exe_ptr(&exe, ptr + k, 1);

            if (!ch || *ch == 0)
                break;
            out->name[i][k] = (char)*ch;
        }
        out->name[i][k] = '\0';
    }

    q2_exe_free(&exe);
    return Q2_OK;
}

const char *q2_item_display_name(const q2_item_table *t, u32 effect)
{
    if (effect >= Q2_ITEM_NAME_COUNT)
        return "";
    if (t && t->count)
        return t->name[effect];
    return k_names[effect];
}

const q2_item_def *q2_item_find(const q2_item_table *t, s32 place_id)
{
    u32 i;

    if (!t)
        return NULL;

    /* The engine compares with `lb`, so the id is signed and only the low byte
     * of a place record's halfword can ever match. */
    if (place_id < -128 || place_id > 127)
        return NULL;

    for (i = 0; i < t->count; i++)
        if (t->def[i].place_id == (s8)place_id)
            return &t->def[i];
    return NULL;
}

bool q2_item_effect_is_live(const q2_item_table *t, u32 effect)
{
    if (effect < Q2_ITEM_EFFECT_FIRST || effect > Q2_ITEM_EFFECT_LAST)
        return false;

    if (t && t->dispatch_default) {
        u32 slot = t->dispatch[effect - Q2_ITEM_EFFECT_FIRST];
        return slot != 0 && slot != t->dispatch_default;
    }

    {
        u32 i;
        for (i = 0; i < sizeof(k_inert_effects); i++)
            if (k_inert_effects[i] == effect)
                return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
u32 q2_item_table_diff(const q2_item_table *a, const q2_item_table *b,
                       q2_item_report_fn report, void *user)
{
    u32 bad = 0, i;
    char what[96];

#define CMP(label, x, y)                                                      \
    do {                                                                      \
        if ((long)(x) != (long)(y)) {                                         \
            bad++;                                                            \
            if (report) report(user, (label), (long)(y), (long)(x));          \
        }                                                                     \
    } while (0)

    if (!a || !b)
        return 1;

    CMP("record count", a->count, b->count);

    for (i = 0; i < a->count && i < b->count; i++) {
        const q2_item_def *x = &a->def[i], *y = &b->def[i];

        snprintf(what, sizeof(what), "def[%u].place_id", i);
        CMP(what, x->place_id, y->place_id);
        snprintf(what, sizeof(what), "def[%u].effect", i);
        CMP(what, x->effect, y->effect);
        snprintf(what, sizeof(what), "def[%u].flags", i);
        CMP(what, x->flags, y->flags);
        snprintf(what, sizeof(what), "def[%u].shadow_vertex_count", i);
        CMP(what, x->shadow_vertex_count, y->shadow_vertex_count);

        if (memcmp(x->model, y->model, Q2_ITEM_MODEL_LEN) != 0) {
            bad++;
            if (report) {
                snprintf(what, sizeof(what), "def[%u].model \"%s\" != \"%s\"",
                         i, x->model, y->model);
                report(user, what, 0, 0);
            }
        }
        {
            u32 k;
            for (k = 0; k < x->shadow_vertex_count; k++) {
                snprintf(what, sizeof(what), "def[%u].shadow_vertex[%u]", i, k);
                CMP(what, x->shadow_vertex[k], y->shadow_vertex[k]);
            }
        }
    }

    for (i = 0; i < Q2_ITEM_EFFECT_COUNT; i++) {
        bool live_a = q2_item_effect_is_live(a, i + Q2_ITEM_EFFECT_FIRST);
        bool live_b = q2_item_effect_is_live(b, i + Q2_ITEM_EFFECT_FIRST);
        if (live_a != live_b) {
            snprintf(what, sizeof(what), "effect %u live",
                     i + Q2_ITEM_EFFECT_FIRST);
            CMP(what, live_a, live_b);
        }
    }

    for (i = 0; i < 11; i++) {
        if (strncmp(a->sound[i], b->sound[i], Q2_ITEM_MODEL_LEN) != 0) {
            bad++;
            if (report) {
                snprintf(what, sizeof(what), "sound[%u] \"%s\" != \"%s\"",
                         i, a->sound[i], b->sound[i]);
                report(user, what, 0, 0);
            }
        }
    }

    for (i = 0; i < Q2_ITEM_NAME_COUNT; i++) {
        if (strncmp(a->name[i], b->name[i], Q2_ITEM_NAME_LEN) != 0) {
            bad++;
            if (report) {
                snprintf(what, sizeof(what), "name[%u] \"%s\" != \"%s\"",
                         i, a->name[i], b->name[i]);
                report(user, what, 0, 0);
            }
        }
    }

#undef CMP
    return bad;
}
