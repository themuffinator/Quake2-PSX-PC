#include "cmd_ai.h"

#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "aimove.h"
#include "exe.h"
#include "monster.h"
#include "trig.h"

/*
 * cmd_ai — check the reconstructed creature AI against the executable.
 *
 * Every constant in `src/game/ai.[ch]`, `aimove.[ch]` and `monster.[ch]` was
 * read out of `SLES_015.34`, so each one can be read back and compared rather
 * than trusted. That is the point of this command: the port's AI is either the
 * disc's or it is not, and the difference should be a number on a line rather
 * than an argument.
 *
 * Three kinds of check:
 *
 *   word   a whole instruction at an address must be exactly what the
 *          reconstruction assumed it was
 *   imm    the 16-bit immediate of an instruction must equal a port constant
 *   ptr    a lui/addiu pair must materialise a particular address
 */
static int g_checks;
static int g_bad;

static void ok(const char *what)
{
    (void)what;
    g_checks++;
}

static void bad(const char *what, u32 addr, long want, long got)
{
    g_checks++;
    g_bad++;
    printf("  MISMATCH  %-46s @%08X  want %ld, got %ld\n",
           what, addr, want, got);
}

/* The 16-bit immediate of an I-format instruction, sign-extended. */
static bool imm_at(const q2_exe *e, u32 addr, s32 *out)
{
    u32 w;
    if (!q2_exe_u32(e, addr, &w))
        return false;
    *out = (s32)(s16)(w & 0xFFFF);
    return true;
}

static bool immu_at(const q2_exe *e, u32 addr, u32 *out)
{
    u32 w;
    if (!q2_exe_u32(e, addr, &w))
        return false;
    *out = w & 0xFFFF;
    return true;
}

static void check_word(const q2_exe *e, u32 addr, u32 want, const char *what)
{
    u32 got;
    if (!q2_exe_u32(e, addr, &got)) { bad(what, addr, (long)want, -1); return; }
    if (got != want) bad(what, addr, (long)want, (long)got);
    else             ok(what);
}

static void check_imm(const q2_exe *e, u32 addr, s32 want, const char *what)
{
    s32 got;
    if (!imm_at(e, addr, &got)) { bad(what, addr, want, -1); return; }
    if (got != want) bad(what, addr, want, got);
    else             ok(what);
}

static void check_immu(const q2_exe *e, u32 addr, u32 want, const char *what)
{
    u32 got;
    if (!immu_at(e, addr, &got)) { bad(what, addr, (long)want, -1); return; }
    if (got != want) bad(what, addr, (long)want, (long)got);
    else             ok(what);
}

/* A `lui rX, hi` at `hi_addr` followed by `ori/addiu rX, rX, lo` at lo_addr. */
static void check_split(const q2_exe *e, u32 hi_addr, u32 lo_addr, u32 want,
                        bool signed_lo, const char *what)
{
    u32 hi, lo, got;

    if (!immu_at(e, hi_addr, &hi) || !immu_at(e, lo_addr, &lo)) {
        bad(what, hi_addr, (long)want, -1);
        return;
    }

    got = signed_lo ? (u32)((hi << 16) + (s32)(s16)lo)
                    : ((hi << 16) | lo);

    if (got != want) bad(what, hi_addr, (long)want, (long)got);
    else             ok(what);
}

int cmd_ai(const disc *d)
{
    q2_exe e;
    q2_result r;

    memset(&e, 0, sizeof(e));

    r = q2_exe_load(&e, d, NULL);
    if (r != Q2_OK) {
        printf("cannot load the executable\n");
        return 1;
    }

    printf("creature AI, checked against the executable\n\n");

    /* --------------------------------------------------------------------- */
    printf("the shared verb table (built at 0x80061D98)\n");
    /* 0x800D561C[1..5] = stand, walk, run, charge, move. Each is a lui/addiu
     * pair; the port's table order must match slot for slot. */
    check_split(&e, 0x80061DA4, 0x80061DA8, 0x8005CDA8, true,
                "verb 1 is ai_stand");
    check_split(&e, 0x80061DB0, 0x80061DB4, 0x8005ED6C, true,
                "verb 2 is ai_walk");
    check_split(&e, 0x80061DBC, 0x80061DC0, 0x8005E350, true,
                "verb 3 is ai_run");
    check_split(&e, 0x80061DC8, 0x80061DCC, 0x8005EE84, true,
                "verb 4 is ai_charge");
    check_split(&e, 0x80061DD4, 0x80061DD8, 0x8005ED44, true,
                "verb 5 is ai_move");
    /* Slot 0 really is zero, and there really is no turn verb. */
    check_word(&e, 0x80061D9C, 0xAC60561C, "verb 0 is stored as zero");

    /* --------------------------------------------------------------------- */
    printf("\nthe class method table (built at 0x80061D10)\n");
    check_imm(&e, 0x80061D18, 8 - 1,
              "the shared verb table has 8 slots");
    check_imm(&e, 0x80061D40, Q2_CLASS_COUNT - 1,
              "the class table has 256 slots");
    check_imm(&e, 0x80061D68, Q2_CLASS_METHOD_COUNT - 1,
              "each class has 32 methods");
    check_imm(&e, 0x800618D8, Q2_CLASS_VERB_BASE * 4,
              "local verbs start 26 methods in");
    check_immu(&e, 0x800618C8, 0x7F,
               "the local verb index masks off bit 7");
    check_immu(&e, 0x800618A8, Q2_AI_LOCAL_FLAG,
               "bit 7 of the ai byte selects a local verb");

    /* --------------------------------------------------------------------- */
    printf("\nM_MoveFrame (0x8006175C)\n");
    /* frame = frames + index*3, as a shift-and-add. The `sll v0, s2, 1`
     * followed by `addu v0, v0, s2` is the three-byte stride. */
    check_word(&e, 0x80061890, 0x00121040, "frame stride is index<<1 ...");
    check_word(&e, 0x80061894, 0x8E230008, "... (the frames pointer load)");
    check_word(&e, 0x80061898, 0x00521021, "... plus index, i.e. 3 bytes");
    check_immu(&e, 0x80061924, Q2_AI_HOLD_FRAME,
               "AI_HOLD_FRAME zeroes the frame distance");
    check_immu(&e, 0x800617F8, 0x2,
               "an endfunc that frees the entity stops the frame");
    /* dist * scale * 12 / 10: the *12 is a shift-and-add pair and the /10 is a
     * magic multiply. Pin the magic, because it is what fixes the divisor. */
    check_split(&e, 0x80061940, 0x80061944, 0x66666667, false,
                "the per-frame distance divides by 10");

    /* --------------------------------------------------------------------- */
    printf("\nrange bands (0x8005D56C, and again at 0x8005E19C)\n");
    check_imm(&e, 0x8005D568, Q2_CLASS_LONG_MELEE,
              "one class id gets the long melee reach");
    check_split(&e, 0x8005D570, 0x8005D578,
                (u32)(Q2_MELEE_DISTANCE_BIG * Q2_MELEE_DISTANCE_BIG - 1), false,
                "long melee is 2040 units");
    check_split(&e, 0x8005D57C, 0x8005D580,
                (u32)(Q2_MELEE_DISTANCE * Q2_MELEE_DISTANCE - 1), false,
                "melee is 1020 units");
    check_split(&e, 0x8005D58C, 0x8005D598,
                (u32)((s64)Q2_RANGE_NEAR_DIST * Q2_RANGE_NEAR_DIST - 1), false,
                "near is 6000 units (id's 500)");
    check_split(&e, 0x8005D5A4, 0x8005D5B0,
                (u32)((s64)Q2_RANGE_MID_DIST * Q2_RANGE_MID_DIST - 1), false,
                "mid is 12000 units (id's 1000)");
    check_imm(&e, 0x8005D76C, Q2_INFRONT_DOT,
              "the forward cone threshold is 1230/4096");

    /* --------------------------------------------------------------------- */
    printf("\nangles (0x8005C7B8, 0x80060964)\n");
    check_word(&e, 0x8005C7BC, 0x30820FFF,
               "anglemod is one instruction: and 0xFFF");
    check_imm(&e, 0x800609A4, Q2_ANGLE_180,
              "M_ChangeYaw takes the short way at 180 degrees");
    check_imm(&e, 0x800609B8, -Q2_ANGLE_180 + 1,
              "and on the other side too");
    check_imm(&e, 0x8005E498, -Q2_ANGLE_90,
              "ai_run_slide sidesteps a quarter turn");
    check_imm(&e, 0x8005E49C, Q2_ANGLE_90,
              "on whichever hand it is leading with");

    /* --------------------------------------------------------------------- */
    printf("\nmovement (0x8005FC78, 0x80060334, 0x80060544)\n");
    check_imm(&e, 0x8005FF18, Q2_STEPSIZE,
              "step height is 216 (id's 18)");
    check_imm(&e, 0x8005FF14, Q2_STEPSIZE_NOSTEP,
              "AI_NOSTEP drops it to 12");
    check_immu(&e, 0x8005FF0C, Q2_AI_NOSTEP,
               "and AI_NOSTEP is 0x400");
    check_split(&e, 0x8005FE78, 0x8005FE7C, Q2_MASK_MONSTERSOLID, false,
                "creatures move against MASK_MONSTERSOLID");
    check_split(&e, 0x8005E804, 0x8005E808, Q2_MASK_PLAYERSOLID, false,
                "the corner peek traces MASK_PLAYERSOLID");
    check_imm(&e, 0x800604CC, -513,
              "a step turning more than 45 degrees is refused ...");
    check_imm(&e, 0x800604D0, 3071,
              "... over a 270 degree unsigned window");
    check_imm(&e, 0x800605C0, Q2_CHASE_DEADBAND + 1,
              "the chase deadband is 120 units (id's 10)");
    check_imm(&e, 0x800605D0, Q2_ANGLE_90,
              "+X is yaw 1024");
    check_imm(&e, 0x800605D8, Q2_ANGLE_90 * 3,
              "-X is yaw 3072");
    check_imm(&e, 0x800605EC, Q2_ANGLE_180,
              "-Z is yaw 2048");
    check_imm(&e, 0x8006074C, Q2_CHASE_STEP,
              "the direction sweep steps 45 degrees");

    /* --------------------------------------------------------------------- */
    printf("\nflying height bands (0x8005FDC0)\n");
    check_imm(&e, 0x8005FDC0, -480,
              "a flyer holds 480 units over a client (id's 40)");
    check_imm(&e, 0x8005FDEC, -359,
              "and closes back inside 360 (id's 30)");
    check_imm(&e, 0x8005FE24, -96,
              "chasing anything else it uses 96 (id's 8)");
    check_immu(&e, 0x8005FD24, Q2_FL_FLY | Q2_FL_SWIM,
               "and only flyers and swimmers take that path");

    /* --------------------------------------------------------------------- */
    printf("\nthe 10 Hz clock (0x8005D048, 0x8005DFB0, 0x8005D144)\n");
    check_imm(&e, 0x8005D094, 150,
              "idle chatter waits 15 seconds plus a random 15");
    check_split(&e, 0x8005D058, 0x8005D05C, 0x80010003, false,
                "and scales the random by 1/32767");
    check_imm(&e, 0x8005DFB8, Q2_AI_SECONDS(5),
              "a sighting buys 5 more seconds of search");
    check_imm(&e, 0x8005E680, Q2_AI_SECONDS(5),
              "and so does reaching a waypoint");
    check_imm(&e, 0x8005E5F8, Q2_AI_SECONDS(20),
              "a search gives up after 20 seconds");
    check_imm(&e, 0x8005D148, Q2_AI_SECONDS(1),
              "show_hostile lasts one second");
    check_imm(&e, 0x8005DF28, Q2_AI_SECONDS(1),
              "and the first attack waits one");
    check_imm(&e, 0x8005DD54, Q2_AI_SECONDS(5) + 1,
              "a sound target goes stale after 5");
    check_split(&e, 0x8005DF5C, 0x8005DF60, 1000000000, false,
                "a creature with nothing to do pauses for 1e9 ticks");

    /* --------------------------------------------------------------------- */
    printf("\naiflags, tested one branch at a time\n");
    check_immu(&e, 0x8005CDD8, Q2_AI_STAND_GROUND,      "AI_STAND_GROUND");
    check_immu(&e, 0x8005CE5C, Q2_AI_TEMP_STAND_GROUND, "AI_TEMP_STAND_GROUND");
    check_immu(&e, 0x8005E398, Q2_AI_SOUND_TARGET,      "AI_SOUND_TARGET");
    check_immu(&e, 0x8005E638, Q2_AI_LOST_SIGHT,        "AI_LOST_SIGHT");
    check_immu(&e, 0x8005E6C0, Q2_AI_PURSUIT_LAST_SEEN, "AI_PURSUIT_LAST_SEEN");
    check_immu(&e, 0x8005E660, Q2_AI_PURSUE_NEXT,       "AI_PURSUE_NEXT");
    check_immu(&e, 0x8005E688, Q2_AI_PURSUE_TEMP,       "AI_PURSUE_TEMP");
    check_immu(&e, 0x8005D354, Q2_AI_GOOD_GUY,          "AI_GOOD_GUY");
    check_immu(&e, 0x8005DDE8, Q2_AI_BRUTAL,            "AI_BRUTAL");
    check_immu(&e, 0x8005E390, Q2_AI_COMBAT_POINT,      "AI_COMBAT_POINT");
    check_immu(&e, 0x8005DDE0, Q2_AI_MEDIC,             "AI_MEDIC");
    check_imm(&e, 0x8005DE2C, -79,
              "AI_BRUTAL fights on until -80 health");

    /* --------------------------------------------------------------------- */
    printf("\nthe player trail (0x80060BBC)\n");
    check_imm(&e, 0x80060BE8, Q2_TRAIL_LENGTH, "the trail is 8 spots long");
    check_immu(&e, 0x80060C28, Q2_TRAIL_LENGTH - 1, "walked with a mask");

    /* --------------------------------------------------------------------- */
    printf("\nthe module import table (built at 0x8007D990)\n");
    check_imm(&e, 0x8007DA10, 304, "the interface record is 304 bytes");
    check_imm(&e, 0x8007DA18, 1, "at version 1");
    check_split(&e, 0x8007DAD0, 0x8007DAD4, 0x8005E350, true,
                "import +0x54 is ai_run");
    check_split(&e, 0x8007DADC, 0x8007DAE0, 0x8005ED6C, true,
                "import +0x58 is ai_walk");
    check_split(&e, 0x8007DAE8, 0x8007DAEC, 0x8005CDA8, true,
                "import +0x5C is ai_stand");
    check_split(&e, 0x8007DB00, 0x8007DB04, 0x8005ED44, true,
                "import +0x60 is ai_move");
    check_split(&e, 0x8007DAF4, 0x8007DAF8, 0x8005EE84, true,
                "import +0x64 is ai_charge");
    check_split(&e, 0x8007DD20, 0x8007DD24, 0x80061DE4, true,
                "import +0x118 registers a class method");

    /* --------------------------------------------------------------------- */
    /*
     * The eight projectile spawners, +0x80..+0x9C, contiguous and in the
     * loader's own order. This is what turns a decoded think function's
     * `call(+98)` into a rocket instead of an unclassified number, so it is
     * worth pinning: if any one of them moves, a creature fires the wrong gun.
     */
    printf("\nthe projectile spawners (import +0x80..+0x9C)\n");
    check_split(&e, 0x8007DB54, 0x8007DB58, 0x80062000, true,
                "import +0x80 is monster_fire_blaster");
    check_split(&e, 0x8007DB60, 0x8007DB64, 0x80061DFC, true,
                "import +0x84 is monster_fire_bullet");
    check_split(&e, 0x8007DB6C, 0x8007DB70, 0x80061ED0, true,
                "import +0x88 is monster_fire_shotgun");
    check_split(&e, 0x8007DB78, 0x8007DB7C, 0x8006217C, true,
                "import +0x8C is monster_fire_railgun");
    check_split(&e, 0x8007DB84, 0x8007DB88, 0x800621BC, true,
                "import +0x90 is monster_fire_bfg");
    check_split(&e, 0x8007DB94, 0x8007DB98, 0x800614D4, true,
                "import +0x94 is monster_fire_grenade");
    check_split(&e, 0x8007DBA0, 0x8007DBA4, 0x8006210C, true,
                "import +0x98 is monster_fire_rocket");
    check_split(&e, 0x8007DBAC, 0x8007DBB0, 0x800621F8, true,
                "import +0x9C is monster_fire_laser");
    {
        /* 0x8006 sign-extends to a negative s16, which is what the lui
         * being checked does. Through a variable, so the wrap is not read
         * as a constant that does not fit (MSVC C4310). */
        u32 lui_imm = 0x8006u;

        check_imm(&e, 0x8007DB54, (s32)(s16)lui_imm,
                  "the family is one lui apart");
    }

    /* --------------------------------------------------------------------- */
    /*
     * +0xFC IS NOT A SHOT, and the port used to think it was.
     *
     * The three below each write a different `think` and then call ONE shared
     * function, 0x800619E0; two of them set a flags bit first, and the bits are
     * FL_FLY and FL_SWIM. That is id's walkmonster_start / flymonster_start /
     * swimmonster_start, and 0x800619E0 is monster_start.
     */
    printf("\nthe three monster_start wrappers (import +0xFC..+0x104)\n");
    check_split(&e, 0x8007DCCC, 0x8007DCD0, 0x80062240, true,
                "import +0xFC is walkmonster_start");
    check_split(&e, 0x8007DCD8, 0x8007DCDC, 0x8006229C, true,
                "import +0x100 is flymonster_start");
    check_split(&e, 0x8007DCE4, 0x8007DCE8, 0x80062268, true,
                "import +0x104 is swimmonster_start");
    check_word(&e, 0x80062250, 0x0C018678, "walk calls monster_start");
    check_word(&e, 0x800622B8, 0x0C018678, "fly calls the same one");
    check_word(&e, 0x80062284, 0x0C018678, "and so does swim");
    check_immu(&e, 0x800622B4, Q2_FL_FLY,  "fly sets FL_FLY first");
    check_immu(&e, 0x80062280, Q2_FL_SWIM, "swim sets FL_SWIM first");
    check_imm(&e, 0x80061A64, 0x24, "monster_start counts level.total_monsters");
    check_immu(&e, 0x80061A50, Q2_AI_GOOD_GUY, "...unless it is a good guy");

    /* --------------------------------------------------------------------- */
    printf("\nthe rest of the import table a creature reaches\n");
    check_split(&e, 0x8007DBDC, 0x8007DBE0, 0x8005D104, true,
                "import +0xAC is FoundTarget");
    check_split(&e, 0x8007DBF4, 0x8007DBF8, 0x8005EF84, true,
                "import +0xB4 is range");
    check_split(&e, 0x8007DC84, 0x8007DC88, 0x8005B950, true,
                "import +0xE4 is visible");
    check_split(&e, 0x8007DC90, 0x8007DC94, 0x8005D8C8, true,
                "import +0xE8 is M_CheckAttack");
    check_split(&e, 0x8007DC9C, 0x8007DCA0, 0x80061118, true,
                "import +0xEC is fire_hit");
    check_split(&e, 0x8007DCA8, 0x8007DCAC, 0x80057D54, true,
                "import +0xF0 is T_Damage");

    /* --------------------------------------------------------------------- */
    printf("\nT_Damage and Killed (0x800627F8)\n");
    check_immu(&e, 0x800628B8, 1, "the surprise bonus needs !DAMAGE_RADIUS");
    check_immu(&e, 0x800628CC, Q2_SVF_MONSTER, "...and SVF_MONSTER");
    check_immu(&e, 0x8006291C, Q2_FL_NO_KNOCKBACK,
               "FL_NO_KNOCKBACK zeroes the knockback");
    check_immu(&e, 0x80062924, Q2_FL_GODMODE, "FL_GODMODE zeroes the damage");
    check_immu(&e, 0x80062930, 0x20, "...unless DAMAGE_NO_PROTECTION is set");
    check_word(&e, 0x80062958, 0x00501023, "health -= damage");
    check_imm(&e, 0x80062950, 0x108, "health lives at object+0x108");
    check_immu(&e, 0x8006299C, Q2_FL_NO_KNOCKBACK,
               "a body is given FL_NO_KNOCKBACK");
    check_imm(&e, 0x800629B4, -9999, "and its health floors at -9999");
    check_immu(&e, 0x800629F8, Q2_AI_GOOD_GUY,
               "a good guy is left out of the kill count");
    check_split(&e, 0x80062A00, 0x80062A04, 0x800E46D8, true,
                "the counter is in level_locals");
    check_imm(&e, 0x80062A08, 0x28, "level.killed_monsters is +0x28");
    check_imm(&e, 0x80062A1C, Q2_CLASS_MEDIC,
               "a medic that kills something owns the body");
    check_immu(&e, 0x80062AD0, Q2_AI_DUCKED, "a ducked creature does not flinch");
    check_imm(&e, 0x80062B20, Q2_AI_SECONDS(5),
              "skill 3 pushes the pain debounce to five seconds");

    /* --------------------------------------------------------------------- */
    printf("\nM_ReactToDamage (0x80062654)\n");
    check_immu(&e, 0x80062680, Q2_SVF_MONSTER,
               "only a client or a creature is worth reacting to");
    check_immu(&e, 0x800626AC, Q2_AI_GOOD_GUY,
               "a good guy does not get angry at a player");
    check_word(&e, 0x800626F4, 0x0C016E54,
               "it asks whether it can still see the one it is fighting");
    check_immu(&e, 0x80062730, Q2_FL_FLY | Q2_FL_SWIM,
               "the same base type means fly and swim together");
    check_imm(&e, 0x80062750, Q2_CLASS_TANKCOMM, "it forgives the Tankcomm");
    check_imm(&e, 0x80062758, Q2_CLASS_BOSS1,    "and Boss1");
    check_imm(&e, 0x80062760, Q2_CLASS_RIDER,    "and Rider");
    check_imm(&e, 0x80062768, Q2_CLASS_JORG,     "and Jorg");
    check_immu(&e, 0x800627CC, Q2_AI_DUCKED,
               "a ducked creature does not break out to charge");
    check_word(&e, 0x800627DC, 0x0C017441, "and otherwise it hunts");

    /* --------------------------------------------------------------------- */
    printf("\nmonster_death_use (0x800622E8)\n");
    check_immu(&e, 0x800622F8,
               (u32)(u16)(~(unsigned)(Q2_FL_FLY | Q2_FL_SWIM) & 0xFFFFu),
               "a dead flyer stops flying");
    check_immu(&e, 0x80062308, Q2_AI_GOOD_GUY,
               "and keeps no ai flag but AI_GOOD_GUY");

    /* --------------------------------------------------------------------- */
    /*
     * The class byte, which is a third numbering and had no home until the
     * loader was read: the descriptor record carries it at +0x20 and this is
     * where it becomes entity+0x23.
     */
    printf("\nthe class byte (0x8007E660)\n");
    check_imm(&e, 0x8007E660, 0x20, "read from the class record's +0x20");
    check_imm(&e, 0x8007E668, 0x23, "written to the entity's +0x23");

    printf("\n%d checks, %d mismatches\n", g_checks, g_bad);
    printf("%s\n", g_bad == 0 ? "the port's AI matches the disc"
                              : "the port's AI does NOT match the disc");

    q2_exe_free(&e);
    return g_bad ? 1 : 0;
}
