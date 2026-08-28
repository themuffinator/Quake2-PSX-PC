/*
 * levelbin.h — reading a map's own `LevelBin` module without running it.
 *
 * ---------------------------------------------------------------------------
 * The problem this solves
 * ---------------------------------------------------------------------------
 * `population.h` states it plainly: a Population group is not spawned because
 * it exists, it is spawned because a script SELECTED it. `0x80056C60` takes a
 * twelve-byte name and sets bit 0 of that group's flags; the spawn pass runs
 * only the selected ones. **The flags word is zero on disc for all 222 groups
 * of all 49 maps**, so at level load nothing is selected — and which groups a
 * level starts with therefore lives in its `LevelBin`, which is MIPS code.
 *
 * This port has no MIPS interpreter and does not want one. But it does not need
 * to RUN the module to know what it asks for, and there is precedent: the glint
 * is turned on by `q2_fx_glint_scan` reading the same kind of module for the
 * instruction that raises its flag.
 *
 * ---------------------------------------------------------------------------
 * What the module does, from BASE1's
 * ---------------------------------------------------------------------------
 * `q2psx-inspect levdisasm BASE1` relocates it to 0x80100000. Its init (export
 * 0) opens with a VERSION CHECK on the engine's import block — `lw v1, 0(v0)`
 * against 1268, the block's own first word (FORMATS §15.5) — and the mismatch
 * arm builds a name and prints *"Please get a programmer to recompile level
 * %s"*. The real work is past that branch:
 *
 *     80100344  addiu a0, a0, 124      ; module+0x7C = "Base1Batches"
 *     80100348  lw    v0, 40(v0)       ; block +40
 *     80100350  jalr  v0               ; select that group
 *
 *     8010039C  addiu t0, v0, 136      ; module+0x88 = "MapTitle"
 *     801003A4..80100430               ; twelve bytes loaded into a0/a1/a2
 *     80100424  lw    v0, 292(v0)      ; block +292
 *     8010042C  jalr  v0               ; a Strings lookup
 *
 * A twelve-byte name is passed BY VALUE in three registers, which is the
 * convention `0x80056C60` takes. And `+40` settles the block's numbering, which
 * §15.5 left ambiguous: it lists the selector as slot 9, and 4 + 9*4 = 40 — so
 * slot N sits at `+4 + 4*N`, past the size word, not at `4*N`.
 *
 * ---------------------------------------------------------------------------
 * Why this reads STRINGS rather than instructions
 * ---------------------------------------------------------------------------
 * The scan below looks for a map's own Population group NAMES inside its
 * `LevelBin`, rather than decoding the call. That is weaker in principle and
 * stronger in practice, for a reason worth stating: the operand is a twelve-byte
 * name loaded from the module's own data, and the ONLY thing a module does with
 * a group name is select the group. Decoding the call site instead would mean
 * tracking a `lui`/`addiu` pair through whatever order the scheduler left them
 * in — which `q2_fx_glint_scan` already had to do for one immediate and found
 * fragile enough to warn about.
 *
 * It is also checkable, which the instruction decode would not be: every name
 * it finds must be a group the map actually ships, and the count of maps with
 * at least one is a number that can be compared against the 222 groups the disc
 * carries. `q2psx-inspect zonescript` prints both.
 */
#ifndef Q2PSX_LEVELBIN_H
#define Q2PSX_LEVELBIN_H

#include "../build/itemtable.h"     /* Q2_ITEM_COUNT — the scene's id list */
#include "../formats/collision.h"
#include "effect.h"
#include "events.h"
#include "population.h"
#include "userfuncs.h"
#include "q2psx.h"

/* The group selector, `0x80056C60`. See the sweep above. */
#define Q2_LEVELBIN_SLOT_SELECT 36

/*
 * Does `module` name Population group `g`?
 *
 * Matched as a whole twelve-byte field, NUL-padded as the disc stores it, so a
 * group called `Zone1` cannot match inside a `Zone1Key`. Names are compared
 * case-sensitively: the module and the Population chunk are produced by the
 * same tool from the same source and agree exactly on the 89 CREBATCH calls
 * already checked (#79).
 */
bool q2_levelbin_names_group(const u8 *module, u32 size, const q2_pop_group *g);

/*
 * ---------------------------------------------------------------------------
 * The string scan establishes the VOCABULARY, not the timing
 * ---------------------------------------------------------------------------
 * `q2_levelbin_names_group` finds every group a module mentions, and that turns
 * out to be a superset of the ones it selects at load: JAIL3's `Bridge` is
 * named by its LevelBin AND by a `CREBATCH`, so the mention is the name being
 * held for a later call rather than a selection. Useful — it confirms #79's
 * zone rule on 71 of the 75 groups the disc's modules name — and not enough.
 *
 * Separating the two needs the CALL SITE, which is what the scan below reads.
 */

/*
 * Groups the module SELECTS, by decoding the calls rather than the strings.
 *
 * The shape, from BASE1's init:
 *
 *     801000A0  addiu a3, v0, 12     ; a3 = module + 0xC = "Zone0"
 *     801000A4  lbu   v1, 1(a3)      ; ...and twenty more, into a0/a1/a2
 *     ...       lw    vX, 36(rY)     ; the engine block's slot 9
 *     ...       jalr  vX
 *
 * `+36` is the selector, and it was SWEPT rather than assumed. For every offset
 * a module calls with a name-shaped argument, count how many of those names are
 * groups the map actually ships:
 *
 *     +36    71 / 83      <- the selector
 *     +136    4 / 5
 *     +28     1 / 20
 *     +32     1 / 20
 *     everything else  0 / n
 *
 * One offset accounts for essentially every hit and the rest for none, which is
 * what makes this a measurement rather than a guess. 36 = 4 * 9, so §15.5's
 * slot numbering indexes the block directly — slot 0 IS the size word — and the
 * "4 + 4*N" reading an earlier pass here used was wrong.
 *
 * `out` receives up to `max` module offsets, each the start of a twelve-byte
 * name. Returns how many were written. A caller resolves each against the map's
 * Population, which is also the check: an offset that does not name a real
 * group means the decode is wrong, and there is no reason for it to be right by
 * accident.
 */
u32 q2_levelbin_selected(const u8 *module, u32 size, u32 *out, u32 max);

/* The same, for an arbitrary slot offset — which is how the selector's slot was
 * FOUND rather than assumed: sweep every offset a module calls with a name and
 * see which one's names are all real groups. */
u32 q2_levelbin_selected_slot(const u8 *module, u32 size, s32 slot_off,
                              u32 *out, u32 max);



/* ------------------------------------------------------------------------- */
/* The map's own MISEVENT table                                               */
/* ------------------------------------------------------------------------- */
/*
 * `MISEVENT` looks its key up in two namespaces (userfuncs.h): the executable's
 * three-record table at 0x8009B680, and the level's own at `[0x800B30E4]` —
 * which no engine code ever writes a non-zero into, so a module writes it.
 *
 * The module carries it as data, in the same `name[12] + handler` records the
 * engine's table uses and the same zero terminator 0x8006DB10 stops on. LAB's
 * is at module+0x16D4 and reads exactly:
 *
 *     801016D4  "Laser0\0\0\0\0\0\0"  80101628
 *     801016E4  "Laser1\0\0\0\0\0\0"  80101648
 *     801016F4  00000000                          <- the terminator
 *
 * so it can be RECOVERED rather than executed, the way the group selector is
 * (q2_levelbin_selected): find a run of records whose first twelve bytes are a
 * NUL-padded printable name and whose thirteenth through sixteenth are an
 * address inside this module. Two independent things have to hold at once for a
 * false positive, and the names are checkable against the script's own keys.
 *
 * `load_base` is where the module was relocated to — the handlers are absolute
 * addresses in it, and that is the whole test.
 */
typedef struct q2_levelbin_misevent {
    char name[13];
    u32  handler;       /* absolute, inside the module */
    u32  offset;        /* where the record sits in the module */
} q2_levelbin_misevent;

u32 q2_levelbin_misevents(const u8 *module, u32 size, u32 load_base,
                          q2_levelbin_misevent *out, u32 max);

/* ------------------------------------------------------------------------- */
/* The front end's own menu pages                                             */
/* ------------------------------------------------------------------------- */
/*
 * The front end is not in the executable. It is `QFRONT`'s `LevelBin` — a
 * 118,216-byte module against 13,008 bytes of `LevelRel` — which is why every
 * sweep of the EXE for `START` and `OPTIONS` failed, and why page 46's two item
 * records are `LOADING` and a NULL rather than the title screen (#44).
 *
 * Its pages are STATIC ARRAYS in the module, not built at run time, and the
 * layout is the executable's own so `0x8001A474` takes a module's record and
 * the engine's without knowing the difference:
 *
 *     +0x00  char *text      absolute, into the module's own text pool
 *     +0x04  s16   x         256 on every row the front end draws: centred
 *     +0x06  s16   y
 *     +0x08  void (*action)(void)
 *     ...    24 bytes in total
 *
 * A page is a run of those. The deathmatch SETUP page is the shape that stops a
 * looser rule working: its six records have bytes +8 onward all zero — no
 * action, no widget, no bound variable — because its values live IN THE TEXT
 * (`"TIME LIMIT   10"`, rewritten in place, since the module image is RAM). So
 * a null action is a legal record and cannot be the terminator.
 *
 * What anchors a run is the TEXT POINTER: it must land on a printable
 * NUL-terminated string inside this module. Two independent things then have to
 * hold for a false positive — a plausible pointer AND a plausible x/y — and the
 * pages that come out can be checked against the capture #44 transcribed.
 */
typedef struct q2_lb_menu_row {
    char name[32];      /* the row's text, from the module's pool */
    s16  x, y;
    u32  action;        /* absolute; 0 when the row does nothing */
    u32  offset;        /* where the record sits in the module   */
} q2_lb_menu_row;

typedef struct q2_lb_menu_page {
    u32         offset;         /* the first record */
    u32         count;
    q2_lb_menu_row row[8];
} q2_lb_menu_page;

/*
 * Recover the module's menu pages. Returns how many were found; `max` bounds
 * what is written. `load_base` is where the module was relocated to.
 */
u32 q2_levelbin_menu_pages(const u8 *module, u32 size, u32 load_base,
                           q2_lb_menu_page *out, u32 max);

/* ------------------------------------------------------------------------- */
/* VIEW CREDITS — the one front-end page whose text is not in a page array     */
/* ------------------------------------------------------------------------- */
/*
 * The title screen offers VIEW CREDITS and this port bounced straight back off
 * it. The words are all there: QFRONT's text pool carries the whole credit roll
 * as a contiguous run of strings, from `HAMMERHEAD LTD` to just before
 * `PLEASE WAIT WHILE` — which is the disc-swap prompt and the first string past
 * the end of it.
 *
 * They are NOT a page array. `q2_levelbin_menu_pages` finds 45 pages and 186
 * rows in this module and exactly one of them mentions the credits: the row
 * labelled `VIEW CREDITS` on the OPTIONS page. So there is no `{text, x, y}`
 * record for a credit line anywhere, and the arrangement — how the roll is
 * paged or scrolled, which lines are headings and which are names — is in the
 * module's CODE, which this port does not run.
 *
 * So the words are the module's and the LAYOUT IS THIS PORT'S, exactly as it is
 * for the deathmatch scoreboard (#106), and it is marked here rather than left
 * to be discovered. What the port does not do is guess at the pairing: the run
 * is presented in module order, which is the order the roll was written in.
 *
 * Anchored on the two strings rather than on offsets, so a build that moves the
 * pool still finds it.
 */
#define Q2_LB_CREDITS_MAX 256

u32 q2_levelbin_credits(const u8 *module, u32 size,
                        const char **out, u32 max);

/* ------------------------------------------------------------------------- */
/* The title screen's SCENE — what the menu is drawn over                     */
/* ------------------------------------------------------------------------- */
/*
 * #44 left this open and said why: QFRONT's world is two nodes and eight
 * vertices, its models are all authored centred on their own origin, and its
 * single `StartPos` puts the eye at the world origin — so drawing them where
 * they sit would put the camera inside the logo, and there was no honest
 * position to draw them at. The port showed the menu over an empty scene.
 *
 * The position is in the module, and it is not a scene graph. **The title
 * screen's models are ITEMS.** `init` (module+0x30F4) calls
 * `module+0xC5BC(module+0x10D7C, module+0x12B20)`, and that function is a
 * five-line spawner:
 *
 *     8010C5E8  lw   a2, 0(t1)         ; a 16-byte q2_pop_place template,
 *     ...                              ; module+0xE48, all zero but +12 = 0x1000
 *     8010C614  lh   v0, 0(s0)         ; the next id; < 0 ends the list
 *     8010C628  sh   v1, 30(sp)        ; ...into the template's `id` field
 *     8010C634  jalr v0                ; engine+0x2C = 0x800599DC, item spawn
 *     8010C650  sw   v0, 0(v1)         ; keep the entity in module+0x12B20[i]
 *     8010C658  sh   2048, 232(v0)     ; +0xE8  angles[1] — yaw, half a circle
 *     8010C660  sw   zero, 268(v0)     ; +0x10C render_flags
 *     8010C664  sw   zero, 164(v0)     ; +0xA4  origin[0]
 *     8010C668  sw   zero, 168(v0)     ; +0xA8  origin[1]
 *     8010C66C  sw   1700, 172(v0)     ; +0xAC  origin[2]
 *     8010C680  jalr v1                ; engine+0x114 = 0x80089E38, RotMatrix
 *
 * So every object stands at (0, 0, 1700) facing yaw 2048, and the list at
 * `module+0x10D7C` is five ITEM TABLE ids terminated by -1:
 *
 *     57  Q2LOGO        59  Male2        60  Male2red
 *     61  Male2purple   62  Male2aqua
 *
 * ---------------------------------------------------------------------------
 * Which answers the spinning as well, and it was never the front end's
 * ---------------------------------------------------------------------------
 * All five of those table records carry the `spin` flag (`q2psx-inspect
 * items`), and `0x80059330` — the item think the spawner installs — is what
 * turns them. The logo on the title screen rotates for exactly the same reason
 * and at exactly the same rate as a shotgun on a pedestal: it IS an item, and
 * the front end adds nothing to make it move. Ids 57..64 are the eight records
 * `records no map ever places` has always listed, and this is what they are
 * for.
 *
 * The module then marks each one taken by all four players
 * (`module+0x3414`, bit 0x80 of +0x118 + p*100). That is the touch sweep being
 * kept off them, not a draw flag: the only reader of that bit is the deathmatch
 * respawn countdown at `0x80059374`, and the front end has no players for the
 * sweep to run over anyway.
 *
 * Only the id list is read from the module here. The transform is the
 * builder's code, not its data, so it is transcribed with the address it came
 * from — the same split every other table in this port uses.
 */
#define Q2_LB_SCENE_MAX 8

/* 0x8010C654 / 0x8010C664..0x8010C66C. */
#define Q2_LB_SCENE_YAW  2048
#define Q2_LB_SCENE_DIST 1700

typedef struct q2_lb_scene {
    u32 offset;                    /* where the id list sits in the module */
    u32 count;
    u16 id[Q2_LB_SCENE_MAX];       /* item table ids, in spawn order       */
} q2_lb_scene;

/*
 * ---------------------------------------------------------------------------
 * Only the first object is ever shown, and it does not use the item think
 * ---------------------------------------------------------------------------
 * `module+0x3414` — the two lines every front-end page builder opens with —
 * hides all five, and then its tail shows exactly one back:
 *
 *     80103554  lh   v1, 710(engine)   ; the live page id
 *     8010355C  beq  v1, 11            ; page 11 keeps even this one hidden
 *     8010356C  lw   v0, 280(objs[0])  ; +0x118
 *     80103574  and  v0, ~0x80         ; ...shown
 *     80103584  lh   v0, 710(engine)
 *     8010358C  bne  v0, zero          ; page 0 is the title screen
 *     801035B0  sw   v1, 60(objs[0])   ; +0x3C — the THINK, per page
 *
 * So `objs[0]`, the Q2LOGO, is the only thing on screen; the four coloured
 * player models are spawned and stay hidden, and belong to a screen this port
 * has not reached. And the logo's motion is not the item think at all — the
 * module overwrites `+0x3C` with one of its own on every page change:
 *
 *     module+0x9D24   the title screen        module+0x9E0C   every other page
 *     scale += 256, capped at 4096            scale -= 256, floored at 1024
 *     yaw   -= 4 * dt                         yaw   -= 4 * dt
 *
 * Two things fall out of that pair. The logo GROWS into the title screen from
 * nothing and SHRINKS to a quarter when you step into a sub-page, which is the
 * animation a still capture cannot show. And it turns at `4 * dt` against the
 * item spin's `3 * dt` (`Q2_ITEM_SPIN_RATE`, 0x8005947C) — so it is close to a
 * pickup's rotation but is not the same number, and a port that reused the item
 * think would be a third slow.
 */
#define Q2_LB_SCENE_SPIN       4     /* module+0x9D24: yaw -= 4 * dt        */
#define Q2_LB_SCENE_SCALE_STEP 256   /* per FRAME, not per tick             */
#define Q2_LB_SCENE_SCALE_FULL 4096  /* the title screen's size             */
#define Q2_LB_SCENE_SCALE_SUB  1024  /* sub-page light-intensity floor      */

/*
 * ---------------------------------------------------------------------------
 * The LIGHTS, which are the reason a faithful title screen is not a dark one
 * ---------------------------------------------------------------------------
 * `q2psx-inspect lit QFRONT` accepts ZERO lights at the spawn point, and the
 * map's `Lights` chunk is 28 bytes. Shading the logo through the light path
 * therefore takes it to black, and that is not a gap in the map: the front end
 * lights its own scene, every frame, from code.
 *
 * The menu's own frame calls two module hooks — `0x8001A1E8` and `0x8001A200`
 * read `engine+0x298` and `engine+0x294`, which QFRONT's `init` fills with
 * `module+0x32BC` and `module+0x2BD8`. The first is only the
 * controller-unplugged check. **The second is a five-light rig**, and it reaches
 * `engine+0x3C` — `0x80075C34`, `q2_light_add_dynamic` — five times a frame.
 *
 * The signature had to be read off the callee, because the call sites write
 * their arguments to `sp+48..56` and that is not where an o32 stack argument
 * goes. `0x80075C34` spills a1/a2/a3 into the caller's own home slots and reads
 * them back as halfwords, so there are only ever four register arguments:
 *
 *     a0  s32 *pos            80075CA4  lw t1,0(t0) / t2,4 / t3,8
 *     a1  r | g<<8 | b<<16    80075CBC  lwl/lwr sp+4 -> light+12
 *     a2  inner | outer<<16   80075CD0  lh sp+8 squared, lh sp+10 squared
 *     a3  style | size<<16    80075C70  lhu sp+12 & 7, lhu sp+14 & 3
 *
 * and `sp+48..56` is the local `pos[3]` whose address `a0` carries — `s3` is
 * `addiu s3, sp, 48` in the hook's prologue. With that, the rig reads out
 * whole:
 *
 *     three at (0, y, 500) for y = -200, 0, +200   rgb (16,255,64)  100/1500
 *     two   at (x, 600, 900), x mirrored           rgb (64, g, 127) 500/1500
 *
 * The two big ones are alive and the three small ones are not. Each big light
 * draws `rand()` twice a frame: its GREEN channel is `(rand() & 63) + 64`, so
 * it flickers between 64 and 127 while red and blue hold, and its X eases a
 * quarter of the way toward `±((rand() & 511) + 200)` every frame — one to the
 * right, one to the left, from persistent stores at `module+0x11664` and
 * `module+0x11668` which the module image starts at zero. So the pair drifts
 * apart and back across the logo, and that drift is what makes the retail title
 * screen's lighting move while the geometry only turns.
 *
 * `rand()` is BIOS `A(2Fh)` — `engine+0x10C` is `0x80089E28`, the two-
 * instruction thunk `addiu t2,0xA0; jr t2; addiu t1,47` — which is the
 * generator `q2_rng_next` already reproduces bit for bit (weapon.h). The four
 * draws happen in a fixed order, and it is kept: right x, right green, left x,
 * left green.
 *
 * One consequence worth stating because it is load-bearing: the dynamic list
 * holds sixteen and `0x80075C34` silently drops the seventeenth, so the caller
 * must clear it per frame (`q2_light_world_begin_frame`) or the rig fills it in
 * four frames and then stops updating.
 */
#define Q2_LB_LIGHT_MAX 5

typedef struct q2_lb_light {
    s32 pos[3];
    u8  rgb[3];
    s32 inner, outer;
} q2_lb_light;

/*
 * Build the front end's five lights for this frame, advancing `wander` and
 * `rng` exactly as `module+0x2BD8` advances its own two stores and the BIOS
 * generator. Writes `Q2_LB_LIGHT_MAX` entries and returns that count.
 *
 * `wander` is the module's own pair of persistent stores — `[0]` is `+0x11668`,
 * the light that drifts RIGHT, and `[1]` is `+0x11664`, the one that drifts
 * left. Zero-initialise it once, as the module image is.
 */
u32 q2_levelbin_scene_lights(q2_lb_light out[Q2_LB_LIGHT_MAX],
                             s32 wander[2], q2_rng *rng);

/*
 * Find the scene's id list. True when one was found.
 *
 * Anchored the way `q2_levelbin_menu_pages` is, on two things that have to hold
 * at once rather than on an offset: the run must be u32 item ids below the
 * table's own record count, terminated by -1, AND the module must materialise
 * its address with a `lui`/`addiu` pair — a list nothing points at is not a
 * list.
 *
 * **This one needs the RELOCATED image**, unlike the page and credit scans,
 * and the reason is the second test. An unrelocated `lui`/`addiu` pair holds
 * only the low half of the offset — QFRONT's is `lui a0, 0` / `addiu a0, a0,
 * 0x0D7C` for a list at +0x10D7C — and the high half arrives from the fixup.
 * Matching on the low half alone would be a 16-bit coincidence rather than a
 * reference, so the caller relocates (`q2_level_module_load`) and passes the
 * base it relocated to.
 */
bool q2_levelbin_scene(const u8 *module, u32 size, u32 load_base,
                       q2_lb_scene *out);

/* ------------------------------------------------------------------------- */
/* QENDMIS — the movie player, and what the end of the campaign actually is   */
/* ------------------------------------------------------------------------- */
/*
 * `QENDMIS1`..`QENDMIS5` are what the level table names `EndMission 1`..
 * `EndMission 5`, and a campaign run ends on one (#88). This port loads
 * QENDMIS5 as two quads, eight vertices and a black screen, which was recorded
 * as content the port fails to draw.
 *
 * **There is no content to draw.** A QENDMIS map is a container for the MOVIE
 * PLAYER, and its module says so outright:
 *
 *     module+0x0310  "\Q2DATA\"
 *     module+0x031C  "MOVIES\"
 *     module+0x03E0  "MDEC_in_sync"      module+0x0348  "vlc buffer 0"
 *     module+0x03F0  "MDEC_out_sync"     module+0x0358  "vlc buffer 1"
 *     module+0x033C  "ring buffer"       module+0x0368  "image buffer"
 *
 * with a table of 36-byte records from module+0xB8 — three twelve-byte fields,
 * a screen name, a trace label and a FILENAME:
 *
 *     +0x00B8  "Intro FMV"   "Do Intro\n"   "TAKE1BP.STX"
 *     +0x00DC  "Extro FMV"   "Do Extro\n"   "OUTRO1P.STX"
 *
 * and the disc carries exactly those, 15.1 MB and 19.5 MB, in /Q2DATA/MOVIES.
 *
 * **THIS ANSWERS #16.** That question read "the executable contains no `.STX`
 * / `MOVIES` / `STX` string at all, so both the player and its filename
 * assembly live elsewhere" — and elsewhere is here. All five QENDMIS maps carry
 * the same module and no gameplay map carries any of it. #15 (the MDEC output
 * depth) was blocked on #16 and is now only blocked on reading this module's
 * code.
 *
 * So the end of the campaign is a 19.5 MB MDEC video, and a port that has no
 * MDEC decoder cannot show it. What it can do is stop pretending: name the
 * movie, say what would play, and end the campaign visibly rather than on a
 * black screen that reads as a crash.
 */
typedef struct q2_levelbin_movie {
    char screen[13];        /* "Intro FMV", "Extro FMV" */
    char label[13];         /* the module's own trace label */
    char file[13];          /* "TAKE1BP.STX" */
    u32  offset;
} q2_levelbin_movie;

/*
 * Recover the movie table. Returns how many records were found; `max` bounds
 * what is written. A map with no player module returns 0, which is every map
 * that is not a QENDMIS.
 */
u32 q2_levelbin_movies(const u8 *module, u32 size,
                       q2_levelbin_movie *out, u32 max);

/* ------------------------------------------------------------------------- */
/* LASERBEAM — the beams a level ships                                        */
/* ------------------------------------------------------------------------- */
/*
 * 72 `LASERBEAM` items on the disc and not one trigger volume reaches any of
 * them. That looked like GLASS's problem (#66) — a primitive with no caller —
 * and it is not. Nothing needs to reach a LASERBEAM. Which beams burn is a
 * property of WHICH ZONE THE PLAYER IS STANDING IN, and it is carried in the
 * bottom bit of a coordinate.
 *
 * THE CONSTRUCTOR, 0x8002E718, runs at every zone load:
 *
 *     8002E744  lw   v0, 372(gp)    ; the chunk being walked (COMMON's)
 *     8002E748  lw   v1, 376(gp)    ; the chunk the engine reads from (the ZONE's)
 *     8002E74C  subu v0, s0, v0     ; this item's offset...
 *     8002E750  addu v0, v0, v1     ; ...rebased into the zone's copy
 *     8002E754  lw   v1, 4(v0)      ; origin_a's first word, TAKEN FROM THERE
 *     8002E760  lw   v0, 20(v0)     ; origin_b's, likewise
 *     8002E758  addiu a2, zero, -1  ; hint  = -1
 *     8002E764  addiu a3, zero, 1   ; brute = true
 *     8002E768  jal  0x80044F54     ; q2_coll_find_node(PrimaryColl, &origin_a)
 *     8002E780  sh   v0, 18(s0)     ; the NODE INDEX into item +18
 *     8002E778  slti v1, v1, 6      ; and if +34 is NOT below six...
 *     8002E784  sh   zero, 34(s0)   ; ...zero it
 *
 * THE EXEC, 0x8002E694, registers the beam if that word says to:
 *
 *     8002E6A8  lh   v0, 16924(gp)  ; the registration-pass gate, below
 *     8002E6B8  lw   v0, 4(a2)      ; origin_a word 0...
 *     8002E6C0  andi v0, v0, 1      ; ...bit 0 is the ENABLE FLAG
 *     8002E6D4  slti v0, v1, 32     ; the list holds 32
 *     8002E6F8  lhu  a1, 16936(gp)  ; the record now executing
 *     8002E700  sh   a1, 0(v1)      ; -> 0x800C7014[n].raiser
 *     8002E704  sh   v0, 2(v1)      ; -> 0x800C7014[n].item
 *
 * and the two halves together are the whole mechanism. The X the exec tests is
 * not COMMON's X: it is the ZONE's, which the constructor has just copied over
 * it. JAIL2's corridor grid is X=7352 in COMMON and in zone 0, and X=7353 in
 * zones 1 and 2 — the same coordinate with the bottom bit set, one unit wide of
 * nothing and invisible in a world this size. So the level author lights a grid
 * in the rooms it guards by nudging one number, and the beam is dark everywhere
 * else without a trigger, a timer or a script.
 *
 * Across the disc that reads: 71 of the 72 beams are lit in at least one zone,
 * NONE is lit in every zone, and the single beam lit nowhere is JAIL2's
 * (0,0,0)->(0,0,0), which is a dead entry. Reading COMMON's copy instead calls
 * 41 of the 72 dark and is simply the wrong buffer — which is the same mistake,
 * in the same field, that #56 was about.
 *
 * THE WALK, 0x8002EE38, runs every frame over the registered list. Nothing
 * clears it — `gp+0x420C` is only ever incremented — so a beam registered at
 * zone load burns until the zone changes:
 *
 *     8002EE88  lbu  v0, 3(base + entry.raiser)
 *     8002EE90  andi v0, v0, 128    ; the raiser record's dead bit...
 *     8002EE94  bne  -> skip        ; ...is the one off switch there is
 *     8002EEB0  s0 = item + 4       ; from
 *     8002EEC0  s1 = item + 20      ; to
 *     8002EEB4  s2 = lh 18(item)    ; area
 *     8002EEB8  s3 = lh 34(item)    ; kind
 *     8002EEBC  jal  0x80089E18     ; the fifth argument, zeroed on the stack
 *     8002EED0  jal  0x80048DC8     ; q2_fx_laser(from, to, area, kind, 0)
 *
 * and the zeroed fifth argument is why a level's beams do not spit particles:
 * `ends = 0`, so neither end burst fires. Only the tube is drawn.
 *
 * `gp+0x421C` decides register-or-act, and LASERWALL is what proves it: the
 * same flag, read at 0x8002E228, sends it to the same kind of list when set and
 * straight to T_Damage (0x80057D54, mod 11) when clear. A port has no reason to
 * model the flag. It can raise the beams at zone load, which is when the
 * console's registration pass raises them.
 */
typedef struct q2_laserbeam {
    s32 from[3];        /* item +4;  bit 0 of [0] is the enable flag  */
    s32 to[3];          /* item +20                                   */
    s16 area;           /* item +18, written by the constructor       */
    s16 kind;           /* item +34, clamped below six there          */
    u32 raiser;         /* the record's offset; its dead bit gates us */
} q2_laserbeam;

/* `slti v0, v1, 32` at 0x8002E6D4. */
#define Q2_LASERBEAM_MAX 32

typedef struct q2_laserbeam_set {
    q2_laserbeam beam[Q2_LASERBEAM_MAX];
    u32          count;
    u32          declined;   /* declared, but dark in THIS zone */
} q2_laserbeam_set;

/*
 * Raise every LASERBEAM this zone lights, as the registration pass does.
 *
 * `ops` is not a refinement here but the mechanism: `base_b` must be the
 * RESIDENT ZONE's Events chunk, because that is the buffer holding the enable
 * bit, and passing COMMON's for both leaves most of a level's lasers dark.
 *
 * `coll` should be PrimaryColl and may be NULL, in which case the area is left
 * at the disc's value rather than resolved — the beam still draws, it is just
 * not sorted into a room.
 *
 * Returns the number raised.
 */
u32 q2_laserbeams_build(q2_laserbeam_set *out, const q2_events *events,
                        const q2_userfuncs *uf, const q2_uf_operands *ops,
                        const q2_collision *coll);

/*
 * Queue every raised beam into this frame's pool. The transient pool empties
 * every frame (effect.h), which is why the console's walk re-submits its whole
 * list every frame too. Returns how many were queued.
 */
u32 q2_laserbeams_draw(const q2_laserbeam_set *set, q2_fx_world *w,
                       q2_rng *rng);

#endif /* Q2PSX_LEVELBIN_H */
