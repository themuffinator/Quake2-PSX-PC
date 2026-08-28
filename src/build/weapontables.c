#include "weapontables.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/*
 * The transcribed PAL set.
 *
 * Every value here is checkable: `q2psx-inspect weapons <disc>` reads the same
 * fields off the executable and diffs them against this. The point of carrying
 * both is that the game can run without re-reading the disc while the claim
 * stays falsifiable.
 */
static const q2_weapon_tables k_builtin = {
    /* `exe` is deliberately absent. The builtin owns no executable image,
     * and a member omitted from a designated initialiser is zero-filled,
     * which is what the braces here had been reaching for. Naming every
     * other member also stops this table shifting silently if the struct
     * ever gains one. */

    /* ammo_per_shot — 0x8009DB4C, 1-based. The BFG's fifty is what pins the
     * indexing: read 0-based it would land on the railgun. */
    .ammo_per_shot = { 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 50, 0 },

    /* ammo_type — 0x8009DC5C. shells 0, bullets 1, grenades 2, rockets 3,
     * cells 4, slugs 5. The blaster's 0 is never consulted: it costs nothing. */
    .ammo_type = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 4, 5, 4, 0 },

    /* owned_bit — 0x8009DC2C. 1 << (id - 1); slot 12's zero is what makes the
     * original's 1..12 cycle scan reject the phantom slot. */
    .owned_bit = { 0, 0x001, 0x002, 0x004, 0x008, 0x010, 0x020,
         0x040, 0x080, 0x100, 0x200, 0x400, 0 },

    /* fire_fn — 0x8009D704. Slot 0 is 0x8004EB08, which is literally
     * `jr ra; nop`: a do-nothing shot for "no weapon". */
    .fire_fn = { 0x8004EB08u, 0x8004BFBCu, 0x8004C1C0u, 0x8004C488u, 0x8004C744u,
      0x8004CA9Cu, 0x8004EBDCu, 0x8004CE18u, 0x8004D038u, 0x8004D250u,
      0x8004D498u, 0x8004EB10u, 0 },

    /* name — 0x8009DB9C. Four of these fill all twelve bytes with no NUL. */
    .name = { "", "Blaster G", "Shotgun G", "Supershot G", "Machinegun G",
      "Chaingun G", "HandGren G", "GrenLaunch G", "RockLaunch G",
      "HyperBlast G", "Railgun G", "Bfg G", "" },

    /* autoswitch — 0x8009DB7C, 0-terminated. Explosives are absent by design:
     * the BFG, the grenade launcher and hand grenades never auto-select. */
    .autoswitch = { 10, 9, 8, 5, 4, 3, 2, 1, 0, 0, 0, 0 },
    .autoswitch_count = 8,   /* excludes the terminating zero */

    /* muzzle — 0x800AE9C4 onward, stored (right, up, forward) with the middle
     * component already negated as every fire function negates it. Weapons
     * whose offset lives inside their projectile spawner read zero here. */
    .muzzle = {
        {   0,   0,   0 },   /* 0  no weapon                                 */
        {  80,  56, 250 },   /* 1  blaster           0x800AE9C4              */
        {  76,  80, 250 },   /* 2  shotgun           0x800AE9CC              */
        {  96,  72, 250 },   /* 3  super shotgun     0x800AE9DC              */
        {  72,  80, 250 },   /* 4  machinegun        0x800AE9E8              */
        { 104, 128, 250 },   /* 5  chaingun          0x800AE9F0              */
        {   0,   0,   0 },   /* 6  hand grenade      (inside 0x8004AA6C)     */
        {  80,  40, 200 },   /* 7  grenade launcher  0x800AEA00              */
        {  40,  80, 100 },   /* 8  rocket launcher   0x800AEA08              */
        {  90, 140, 250 },   /* 9  hyperblaster      0x800AEA10              */
        {  40,  96, 256 },   /* 10 railgun           0x800AEA18              */
        {   0,   0,   0 },   /* 11 BFG               (inside 0x8004BE04)     */
        {   0,   0,   0 }
    },

    /* armour — 0x8009C5EC. PC Quake II's own six numbers, on all six. */
    .armour = { {  25,  50, 1229,    0 },      /* jacket */
      {  50, 100, 2458, 1229 },      /* combat */
      { 100, 200, 3277, 2458 } },    /* body   */

    /* sound — 0x800ACBC8, 12-byte stride. */
    .sound = { "wep_railgf1a", "wep_noammo",   "wep_grenlb1b", "wep_hgrenc1b",
      "wep_hgrent1a", "wep_shotgr1b", "wep_sshotr1b", "wep_grenlx1a",
      "wep_blastf1a", "wep_shotgf1b", "wep_sshotf1b", "wep_machgf1b",
      "wep_chngnu1a", "wep_chngnl1a", "wep_chngnd1a", "wep_hyprbf1a",
      "wep_hyprbd1a", "wep_rocklf1a", "wep_grenlf1a", "pla_burn1",
      "amb_spark5",   "wep_bfg__f1y" },

    /* bolt_shape — 0x8009DB1C. Eight corners of a 20 x 20 x 100 box, which is
     * the bolt's oriented hull; 0x8004D70C rotates each into the entity. */
    .bolt_shape = { { -10, -10, -50 }, {  10, -10, -50 },
      { -10, -10,  50 }, {  10, -10,  50 },
      { -10,  10, -50 }, {  10,  10, -50 },
      { -10,  10,  50 }, {  10,  10,  50 } }
};

const q2_weapon_tables *q2_weapon_tables_builtin(void)
{
    return &k_builtin;
}

/* ------------------------------------------------------------------------- */
static void copy_name(char *dst, const u8 *src, size_t n)
{
    memcpy(dst, src, n);
    dst[n] = '\0';
}

q2_result q2_weapon_tables_load(q2_weapon_tables *out, const disc *d,
                                const q2_build_id *id)
{
    q2_result r;
    const u8 *p;
    u32 i;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("weapon table locations are unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }
    if (!id->exe_name[0])
        return Q2_ERR_NOT_FOUND;

    r = q2_exe_load(&out->exe, d, id->exe_name);
    if (r != Q2_OK)
        return r;

    /* The five parallel arrays. Each is materialised as base-minus-one in the
     * code, so element `i` sits at base + 4*i and slot 0 is real storage the
     * engine can and does read. */
    for (i = 0; i < Q2_WT_SLOTS; i++) {
        u32 w;

        if (!q2_exe_u32(&out->exe, Q2_WT_ADDR_AMMO_PER_SHOT + 4 * i, &w))
            goto bad;
        out->ammo_per_shot[i] = (s32)w;

        if (!q2_exe_u32(&out->exe, Q2_WT_ADDR_AMMO_TYPE + 4 * i, &w))
            goto bad;
        out->ammo_type[i] = (s32)w;

        if (!q2_exe_u32(&out->exe, Q2_WT_ADDR_OWNED_BIT + 4 * i, &w))
            goto bad;
        out->owned_bit[i] = w;

        if (!q2_exe_u32(&out->exe, Q2_WT_ADDR_FIRE_FN + 4 * i, &w))
            goto bad;
        out->fire_fn[i] = w;

        p = q2_exe_ptr(&out->exe, Q2_WT_ADDR_NAMES + Q2_WT_NAME_LEN * i,
                       Q2_WT_NAME_LEN);
        if (!p) goto bad;
        copy_name(out->name[i], p, Q2_WT_NAME_LEN);
    }

    /* The name array is char[11][12] with no terminator entry — only four
     * bytes of padding separate it from the owned-bit array — so slot 0 and
     * slot 12 read padding rather than names. Blank them so a caller printing
     * `name[0]` gets nothing rather than four bytes of the previous table. */
    out->name[0][0] = '\0';
    out->name[Q2_WT_SLOTS - 1][0] = '\0';

    /* Auto-switch: read until the terminating zero. */
    out->autoswitch_count = 0;
    for (i = 0; i < Q2_WT_AUTOSWITCH_MAX; i++) {
        u32 w;
        if (!q2_exe_u32(&out->exe, Q2_WT_ADDR_AUTOSWITCH + 4 * i, &w))
            goto bad;
        if (w == 0)
            break;
        out->autoswitch[out->autoswitch_count++] = (s32)w;
    }

    /* Muzzle offsets are not one array: each fire function materialises its own
     * address. Transcribe which weapon reads which, then read the values. */
    {
        static const u32 k_muzzle_addr[Q2_WT_SLOTS] = {
            0,
            0x800AE9C4u, 0x800AE9CCu, 0x800AE9DCu, 0x800AE9E8u,
            0x800AE9F0u, 0,           0x800AEA00u, 0x800AEA08u,
            0x800AEA10u, 0x800AEA18u, 0,           0
        };

        for (i = 0; i < Q2_WT_SLOTS; i++) {
            s16 v[3];
            int k;

            if (!k_muzzle_addr[i])
                continue;
            for (k = 0; k < 3; k++)
                if (!q2_exe_s16(&out->exe, k_muzzle_addr[i] + 2 * (u32)k, &v[k]))
                    goto bad;

            /* Stored exactly as the disc holds them: (right, DOWN, forward).
             * The fire function negates the middle component into the rotation
             * and negates the rotated Y again coming out (0x8004C01C and
             * 0x8004C04C), so the two cancel — see weapontables.h. */
            out->muzzle[i][0] = v[0];
            out->muzzle[i][1] = v[1];
            out->muzzle[i][2] = v[2];
        }
    }

    for (i = 0; i < Q2_WT_ARMOUR_CLASSES; i++) {
        u32 base = Q2_WT_ADDR_ARMOUR + 6 * i;
        u8  b, m;
        s16 n, e;

        if (!q2_exe_u8 (&out->exe, base + 0, &b) ||
            !q2_exe_u8 (&out->exe, base + 1, &m) ||
            !q2_exe_s16(&out->exe, base + 2, &n) ||
            !q2_exe_s16(&out->exe, base + 4, &e))
            goto bad;

        out->armour[i].base_count        = b;
        out->armour[i].max_count         = m;
        out->armour[i].normal_protection = n;
        out->armour[i].energy_protection = e;
    }

    for (i = 0; i < Q2_WT_SOUND_COUNT; i++) {
        p = q2_exe_ptr(&out->exe, Q2_WT_ADDR_SOUNDS + Q2_WT_NAME_LEN * i,
                       Q2_WT_NAME_LEN);
        if (!p) goto bad;
        copy_name(out->sound[i], p, Q2_WT_NAME_LEN);
    }

    for (i = 0; i < Q2_WT_BOLT_POINTS; i++) {
        int k;
        for (k = 0; k < 3; k++)
            if (!q2_exe_s16(&out->exe,
                            Q2_WT_ADDR_BOLT_SHAPE + 6 * i + 2 * (u32)k,
                            &out->bolt_shape[i][k]))
                goto bad;
    }

    return Q2_OK;

bad:
    q2_weapon_tables_free(out);
    return Q2_ERR_BAD_FORMAT;
}

void q2_weapon_tables_free(q2_weapon_tables *t)
{
    if (!t)
        return;
    q2_exe_free(&t->exe);
    memset(t, 0, sizeof(*t));
}

/* ------------------------------------------------------------------------- */
static void note(q2_wt_report_fn f, void *user, u32 *bad,
                 const char *what, long expect, long got)
{
    if (expect == got)
        return;
    (*bad)++;
    if (f)
        f(user, what, expect, got);
}

u32 q2_weapon_tables_diff(const q2_weapon_tables *a, const q2_weapon_tables *b,
                          q2_wt_report_fn report, void *user)
{
    u32 bad = 0, i;
    char label[64];

    if (!a || !b)
        return 0;

    /*
     * Only ids 1..11 are compared for the arrays that are eleven elements long.
     * Their 1-based bases sit one element BEFORE the storage, so index 0 and
     * index 12 read the neighbouring table rather than a weapon — see the
     * adjacency note in the header, which is a finding rather than a caveat.
     *
     * The two exceptions are compared over their full range: the owned-bit
     * array's phantom twelfth slot really is zero on disc (it is the ammo-type
     * array's own base word), which is what makes the cycle scan reject it, and
     * the fire-function array's slot 0 really is a stub.
     */
    for (i = 1; i <= Q2_WT_WEAPON_COUNT; i++) {
        snprintf(label, sizeof(label), "ammo_per_shot[%u]", i);
        note(report, user, &bad, label, b->ammo_per_shot[i], a->ammo_per_shot[i]);
        snprintf(label, sizeof(label), "ammo_type[%u]", i);
        note(report, user, &bad, label, b->ammo_type[i], a->ammo_type[i]);
        snprintf(label, sizeof(label), "muzzle[%u]", i);
        note(report, user, &bad, label, b->muzzle[i][0], a->muzzle[i][0]);
        note(report, user, &bad, label, b->muzzle[i][1], a->muzzle[i][1]);
        note(report, user, &bad, label, b->muzzle[i][2], a->muzzle[i][2]);

        if (strcmp(a->name[i], b->name[i]) != 0) {
            bad++;
            if (report) {
                snprintf(label, sizeof(label), "name[%u] '%s'", i, a->name[i]);
                report(user, label, 0, 1);
            }
        }
    }

    for (i = 0; i < Q2_WT_SLOTS; i++) {
        snprintf(label, sizeof(label), "owned_bit[%u]", i);
        note(report, user, &bad, label, (long)b->owned_bit[i],
             (long)a->owned_bit[i]);
    }
    for (i = 0; i <= Q2_WT_WEAPON_COUNT; i++) {
        snprintf(label, sizeof(label), "fire_fn[%u]", i);
        note(report, user, &bad, label, (long)b->fire_fn[i],
             (long)a->fire_fn[i]);
    }

    note(report, user, &bad, "autoswitch_count",
         (long)b->autoswitch_count, (long)a->autoswitch_count);
    for (i = 0; i < Q2_WT_AUTOSWITCH_MAX; i++) {
        snprintf(label, sizeof(label), "autoswitch[%u]", i);
        note(report, user, &bad, label, b->autoswitch[i], a->autoswitch[i]);
    }

    for (i = 0; i < Q2_WT_ARMOUR_CLASSES; i++) {
        snprintf(label, sizeof(label), "armour[%u].base", i);
        note(report, user, &bad, label, b->armour[i].base_count,
             a->armour[i].base_count);
        snprintf(label, sizeof(label), "armour[%u].max", i);
        note(report, user, &bad, label, b->armour[i].max_count,
             a->armour[i].max_count);
        snprintf(label, sizeof(label), "armour[%u].normal", i);
        note(report, user, &bad, label, b->armour[i].normal_protection,
             a->armour[i].normal_protection);
        snprintf(label, sizeof(label), "armour[%u].energy", i);
        note(report, user, &bad, label, b->armour[i].energy_protection,
             a->armour[i].energy_protection);
    }

    for (i = 0; i < Q2_WT_SOUND_COUNT; i++) {
        if (strcmp(a->sound[i], b->sound[i]) != 0) {
            bad++;
            if (report) {
                snprintf(label, sizeof(label), "sound[%u] '%s'", i, a->sound[i]);
                report(user, label, 0, 1);
            }
        }
    }

    for (i = 0; i < Q2_WT_BOLT_POINTS; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            snprintf(label, sizeof(label), "bolt_shape[%u][%d]", i, k);
            note(report, user, &bad, label, b->bolt_shape[i][k],
                 a->bolt_shape[i][k]);
        }
    }

    return bad;
}
