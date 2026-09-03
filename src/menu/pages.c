/*
 * pages.c — the menu pages, transcribed from the executable's item tables.
 *
 * Every table below is a literal reading of a 24-byte-record array in the PSX
 * executable's data segment; the address in each page's `addr` field is where
 * it lives, and `q2psx-inspect menu --check <disc>` reads that address off a
 * real disc and compares label, x and y record by record. Nothing here is
 * authored: if a coordinate looks odd (the confirmations list NO before YES
 * while drawing YES above it), that is what the original does, and changing it
 * would change the navigation order.
 *
 * Two pages exist in more than one form, chosen at run time by the code that
 * installs them rather than by the tables:
 *
 *   VIDEO           0x800202B0 — HORIZONTAL SPLIT only exists in multiplayer
 *   GAME VARIABLES  0x8001D510 — 3/5/7/9 items by unlocked cheat level
 */
#include "menu.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* GAME VARIABLES — 0x8009A6C4 / 0x8009A724 / 0x8009A7B4 / 0x8009A874.
 *
 * The leading spaces on the slider labels are in the strings themselves: the
 * bar is drawn to the right of the text, so the label is padded to right-align
 * it against the bar rather than being positioned separately. */
static const q2_menu_item k_vars_none[] = {
    { "    GRAVITY",       168,  97, Q2_ACT_NONE,             Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { "FALLING DAMAGE",    256, 124, Q2_ACT_NONE,             Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 151, Q2_ACT_RESET_VARIABLES,  Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

static const q2_menu_item k_vars_bronze[] = {
    { "    GRAVITY",       168,  76, Q2_ACT_NONE,             Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { "WEAPON STAY",       256, 106, Q2_ACT_NONE,             Q2_SET_WEAPON_STAY,    Q2_WIDGET_TOGGLE, 0 },
    { "FALLING DAMAGE",    256, 124, Q2_ACT_NONE,             Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "ONE SHOT KILL",     256, 148, Q2_ACT_NONE,             Q2_SET_ONE_SHOT_KILL,  Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 172, Q2_ACT_RESET_VARIABLES,  Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

static const q2_menu_item k_vars_silver[] = {
    { "    GRAVITY",       168,  52, Q2_ACT_NONE,             Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { " GAME SPEED",       168,  76, Q2_ACT_NONE,             Q2_SET_GAME_SPEED,     Q2_WIDGET_SLIDER, 0 },
    { "BLAST FORCE",       168, 107, Q2_ACT_NONE,             Q2_SET_BLAST_FORCE,    Q2_WIDGET_SLIDER, 0 },
    { "WEAPON STAY",       256, 124, Q2_ACT_NONE,             Q2_SET_WEAPON_STAY,    Q2_WIDGET_TOGGLE, 0 },
    { "FALLING DAMAGE",    256, 148, Q2_ACT_NONE,             Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "ONE SHOT KILL",     256, 172, Q2_ACT_NONE,             Q2_SET_ONE_SHOT_KILL,  Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 196, Q2_ACT_RESET_VARIABLES,  Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

static const q2_menu_item k_vars_gold[] = {
    { "    GRAVITY",       168,  40, Q2_ACT_NONE,             Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { " GAME SPEED",       168,  61, Q2_ACT_NONE,             Q2_SET_GAME_SPEED,     Q2_WIDGET_SLIDER, 0 },
    { "BLAST FORCE",       168,  82, Q2_ACT_NONE,             Q2_SET_BLAST_FORCE,    Q2_WIDGET_SLIDER, 0 },
    { "WEAPON STAY",       256, 103, Q2_ACT_NONE,             Q2_SET_WEAPON_STAY,    Q2_WIDGET_TOGGLE, 0 },
    { "INFINITE AMMO",     256, 124, Q2_ACT_NONE,             Q2_SET_INFINITE_AMMO,  Q2_WIDGET_TOGGLE, 0 },
    { "ALL WEAPONS",       256, 145, Q2_ACT_NONE,             Q2_SET_ALL_WEAPONS,    Q2_WIDGET_TOGGLE, 0 },
    { "FALLING DAMAGE",    256, 166, Q2_ACT_NONE,             Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "ONE SHOT KILL",     256, 187, Q2_ACT_NONE,             Q2_SET_ONE_SHOT_KILL,  Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 208, Q2_ACT_RESET_VARIABLES,  Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

/* ------------------------------------------------------------------------- */
/* Pause — 0x8009A964 (multiplayer) and 0x8009AA0C (single player).
 *
 * The single-player table's sixth record points at the empty string and is
 * excluded from navigation by the `count -= 1` at 0x8001D6F4, which is also
 * where the KILLS/SECRETS status line is placed at (256, 204). */
static const q2_menu_item k_pause_mp[] = {
    { "RETURN TO GAME", 256,  64, Q2_ACT_RESUME,          Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "VIDEO OPTIONS",  256,  88, Q2_ACT_PAGE_VIDEO,      Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "SOUND OPTIONS",  256, 112, Q2_ACT_PAGE_SOUND,      Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "GAME VARIABLES", 256, 136, Q2_ACT_PAGE_VARIABLES,  Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "PLAYER CONTROL", 256, 160, Q2_ACT_PAGE_CONTROLLER, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "QUIT GAME",      256, 184, Q2_ACT_ASK_QUIT,        Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
};

static const q2_menu_item k_pause_sp[] = {
    { "RETURN TO GAME", 256,  72, Q2_ACT_RESUME,       Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "MISSION",        256,  96, Q2_ACT_MISSION,      Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "OPTIONS",        256, 120, Q2_ACT_PAGE_OPTIONS, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "RESTART LEVEL",  256, 144, Q2_ACT_ASK_RESTART,  Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "QUIT GAME",      256, 168, Q2_ACT_ASK_QUIT,     Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    /* Record six: the empty string, present in the table and then excluded. */
};

/* OPTIONS — 0x8009AB14 */
static const q2_menu_item k_options[] = {
    { "PLAYER OPTIONS", 256,  98, Q2_ACT_PAGE_PLAYER, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "SOUND OPTIONS",  256, 124, Q2_ACT_PAGE_SOUND,  Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "VIDEO OPTIONS",  256, 150, Q2_ACT_PAGE_VIDEO,  Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/* PLAYER — 0x8009ADFC */
static const q2_menu_item k_player[] = {
    { "CROSSHAIR",         256,  85, Q2_ACT_NONE,             Q2_SET_CROSSHAIR,  Q2_WIDGET_TOGGLE, 0 },
    { "AUTOCENTRE",        256, 111, Q2_ACT_NONE,             Q2_SET_AUTOCENTRE, Q2_WIDGET_TOGGLE, 0 },
    { "CONTROLLER",        256, 137, Q2_ACT_PAGE_CONTROLLER,  Q2_SET_NONE,       Q2_WIDGET_TEXT,   0 },
    { "RESET TO DEFAULTS", 256, 163, Q2_ACT_RESET_PLAYER,     Q2_SET_NONE,       Q2_WIDGET_TEXT,   0 },
};

/* SOUND — 0x8009AE74 */
static const q2_menu_item k_sound[] = {
    { "      MUSIC",       168,  85, Q2_ACT_NONE,          Q2_SET_MUSIC,  Q2_WIDGET_SLIDER, 0 },
    { "   SOUND FX",       168, 111, Q2_ACT_NONE,          Q2_SET_SFX,    Q2_WIDGET_SLIDER, 0 },
    { "STEREO",            256, 137, Q2_ACT_NONE,          Q2_SET_STEREO, Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 163, Q2_ACT_RESET_SOUND,   Q2_SET_NONE,   Q2_WIDGET_TEXT,   0 },
};

/* VIDEO — 0x8009AEEC (multiplayer) and 0x8009AF94 (single player) */
static const q2_menu_item k_video_mp[] = {
    { "HORIZONTAL SPLIT",  256,  98, Q2_ACT_NONE,           Q2_SET_HORIZONTAL_SPLIT, Q2_WIDGET_TOGGLE, 0 },
    { "SCREEN POSITION",   256, 124, Q2_ACT_PAGE_POSITION,  Q2_SET_NONE,             Q2_WIDGET_TEXT,   0 },
    { "RESET TO DEFAULTS", 256, 150, Q2_ACT_RESET_VIDEO,    Q2_SET_NONE,             Q2_WIDGET_TEXT,   0 },
};

static const q2_menu_item k_video_sp[] = {
    { "SCREEN POSITION",   256, 111, Q2_ACT_PAGE_POSITION, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "RESET TO DEFAULTS", 256, 137, Q2_ACT_RESET_VIDEO,   Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/* SCREEN POSITION — 0x8009AF4C, followed by the empty table at 0x8009B30C,
 * so nothing on it is navigable: the d-pad drives the offset directly and
 * the page's own hook (0x8001CD64) applies it. */
static const q2_menu_item k_position[] = {
    { "USE DIRECTIONAL BUTTONS", 256, 111, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "TO ADJUST DISPLAY",       256, 137, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/*
 * CONTROLLER — 0x8009AFDC.
 *
 * Alone among the pages, its item records carry no bindings: the page's own
 * hook at 0x8001C81C reads the pad and writes the per-player configuration
 * block at 0x800B32B6 + player*34 directly. The bindings below are that hook's
 * effect, expressed the way every other page expresses it, so the page works
 * rather than being five inert labels.
 *
 * The first item cycles a named value with wrap (0x8001C944 decrements on
 * LEFT, 0x8001C968 increments on RIGHT, 0x8001C980 wraps at both ends). Which
 * nine names are offered is decided by the connected controller's class, not
 * by the menu: 0x8001C8A8 selects [0,3) for a mouse, [3,6) for an analogue
 * pad and [6,9) otherwise — and 6 is STANDARD A, which is what the player
 * reset writes. The two items that need sticks grey themselves out when the
 * pad has none (0x8001CA28 prefixes 'g' and sets the object's disable field).
 */
static const q2_menu_item k_controller[] = {
    { "STYLE A",     256,  78, Q2_ACT_NONE, Q2_SET_PAD_STYLE,   Q2_WIDGET_CHOICE, 0 },
    { "VIBRATION",   256, 101, Q2_ACT_NONE, Q2_SET_VIBRATION,   Q2_WIDGET_TOGGLE, 0 },
    { "SWAP Y AXIS", 256, 124, Q2_ACT_NONE, Q2_SET_SWAP_Y,      Q2_WIDGET_TOGGLE, 0 },
    { "USE MOUSE",   256, 147, Q2_ACT_NONE, Q2_SET_USE_MOUSE,   Q2_WIDGET_TOGGLE, 0 },
    { "MOUSE SPEED", 168, 170, Q2_ACT_NONE, Q2_SET_MOUSE_SPEED, Q2_WIDGET_SLIDER, 0 },
};

/* Confirmations. The question is one table, the answers another, so only the
 * answers are navigable — and the answer table lists NO first while drawing it
 * *below* YES, which is why "down" from NO reaches YES. */
static const q2_menu_item k_restart_confirm[] = {
    { "RESTART LEVEL?", 256,  98, Q2_ACT_NONE,       Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "NO",             256, 150, Q2_ACT_BACK,       Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "YES",            256, 124, Q2_ACT_DO_RESTART, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
};

static const q2_menu_item k_quit_confirm[] = {
    { "QUIT GAME?", 256,  98, Q2_ACT_NONE,    Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "NO",         256, 150, Q2_ACT_BACK,    Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "YES",        256, 124, Q2_ACT_DO_QUIT, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
};

static const q2_menu_item k_resupply_confirm[] = {
    { "USE A RESUPPLY TO",   256,  59, Q2_ACT_NONE,        Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "BUY HEALTH AND AMMO", 256,  85, Q2_ACT_NONE,        Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "",                    256, 111, Q2_ACT_NONE,        Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "NO",                  256, 163, Q2_ACT_BACK,        Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "YES",                 256, 189, Q2_ACT_DO_RESUPPLY, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
};

/* Terminal pages: text only, and the request fires on entry. 0x8009AC4C is
 * shared by RESTARTING and RESUPPLYING — the original installs the same table
 * from 0x8001FE10 and 0x8001FEB4. */
static const q2_menu_item k_restarting[] = {
    { "RESTARTING", 256, 111, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "LEVEL",      256, 137, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

static const q2_menu_item k_quitting[] = {
    { "QUITTING", 256, 111, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "GAME",     256, 137, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/*
 * The loading screen — 0x800A3314, with 0x800A3344 as its empty second table.
 *
 * Installed by 0x80079178 at size 16 and with the highlight flag set
 * (0x80079398 writes 1 into drawable 0's +0x48), so the one line is drawn in
 * palette 70 — the bright one. It is a terminal page in the same sense
 * RESTARTING is: the second call installs a NULL record, nothing is navigable,
 * and no selection bar is drawn. See loading.h.
 */
static const q2_menu_item k_loading[] = {
    { "LOADING", 256, 124, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

static const q2_menu_item k_no_controller[] = {
    { "PLEASE INSERT",      256,  98, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "CONTROLLER INTO",    256, 124, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "CONTROLLER PORT XX", 256, 150, Q2_ACT_NONE, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/*
 * DEATH — 0x8009AB74. All three records carry a null action on disc; the
 * page's hook patches them in once a 600-tick delay has run down
 * (0x8002052C), which is why the screen is inert for a moment after you die.
 * The middle record's label is the empty string on disc and is rewritten at
 * entry with the resupply count, prefixed 'g' when there are none left
 * (0x8001D774) — which greys it *and* takes it out of the navigation.
 */
static const q2_menu_item k_death[] = {
    { "RESTART LEVEL", 256,  98, Q2_ACT_DO_RESTART,   Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "",              256, 124, Q2_ACT_ASK_RESUPPLY, Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
    { "QUIT GAME",     256, 150, Q2_ACT_ASK_QUIT,     Q2_SET_NONE, Q2_WIDGET_TEXT, 1 },
};

/* ------------------------------------------------------------------------- */
#define N(a) (u8)(sizeof(a) / sizeof((a)[0]))

/*
 * `first` is the index the last set-items call started at — the boundary
 * between a page's static text and its navigable part.
 */
#define EMPTY 0x8009B30Cu   /* the terminator table every text-only page uses */

/*
 * The front end, transcribed from QFRONT's `LevelBin` rather than from the
 * executable — openquestions #44. The addresses are module offsets at
 * Q2_MOD_BASE, which is what `q2psx-inspect modstrings` and `modxrefs` print,
 * and every record was found by asking what points at its string: each has
 * exactly one word reference and it is the record's own first field.
 *
 * The record layout is the executable's own 24 bytes, so the engine's item
 * installer takes a module's record and the executable's without knowing the
 * difference. Rows are centred at x = 256 with a 26-pixel pitch.
 */

/* module+0x0EC3C — over the QFRONT scene, below the Q2LOGO model */
static const q2_menu_item k_front_title[] = {
    { "START",   256, 151, Q2_ACT_PAGE_FRONT_START,   Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "OPTIONS", 256, 177, Q2_ACT_PAGE_FRONT_OPTIONS, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/* module+0x0EC84 */
static const q2_menu_item k_front_start[] = {
    { "SINGLE PLAYER", 256, 111, Q2_ACT_NEW_GAME,     Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "MULTI PLAYER",  256, 137, Q2_ACT_MULTIPLAYER,  Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/*
 * module+0x0EF9C, module+0x0EFE4 and module+0x0F104 — the pages below START.
 *
 * The MULTI PLAYER page is the one that breaks the pattern: its five rows are
 * 22 apart (80, 102, 124, 146, 168) where every other front-end page is 26. A
 * five-row page is tightened to fit; two-, three- and four-row pages are not.
 *
 * And its first three rows share ONE action, `module+0x4AD8`, so the mode is
 * decided by which row is on rather than by three handlers — which is what
 * `QMULTI.C` wants, since it implements six modes of which three are
 * selectable (#0).
 */
static const q2_menu_item k_front_newload[] = {
    { "NEW GAME",  256, 111, Q2_ACT_PAGE_FRONT_SKILL, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "LOAD GAME", 256, 137, Q2_ACT_LOAD_GAME,        Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

static const q2_menu_item k_front_skill[] = {
    { "EASY",   256,  98, Q2_ACT_SKILL_EASY,   Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "MEDIUM", 256, 124, Q2_ACT_SKILL_MEDIUM, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "HARD",   256, 150, Q2_ACT_SKILL_HARD,   Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

static const q2_menu_item k_front_multi[] = {
    { "DEATHMATCH",      256,  80, Q2_ACT_DM_MODE,     Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "TEAM DEATHMATCH", 256, 102, Q2_ACT_DM_MODE,     Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "VERSUS",          256, 124, Q2_ACT_DM_MODE,     Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "LOAD SETTINGS",   256, 146, Q2_ACT_MP_SETTINGS, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "SAVE SETTINGS",   256, 168, Q2_ACT_MP_SETTINGS, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/*
 * module+0x0F914 — the deathmatch setup, and the reason its rows look like
 * widgets and are not.
 *
 * Every record here is the same bare 24 bytes as every other page's, with bytes
 * +8 onward all zero: there is no widget field, no setting index and no bound
 * variable. The values are in the TEXT — the pool holds `"TIME LIMIT   10"` and
 * `"FRAG LIMIT   10"` with the number padded into the string, and the module
 * rewrites it in place when the value changes. That is the same trick as the
 * leading spaces on `"      MUSIC"` and `"    GRAVITY"`, which right-align
 * against their sliders: this front end lays out with padding rather than with
 * fields.
 *
 * The row at y = 81 carries a short string the module fills at run time — the
 * chosen map's name — and the 74-pixel gap below it is where the capture's
 * `multipics.lbm` preview thumbnail goes.
 */
static const q2_menu_item k_front_dmsetup[] = {
    { "2 3 4 PLAYERS",   256,  64, Q2_ACT_NONE,           Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "",                256,  81, Q2_ACT_NONE,           Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "TIME LIMIT   10", 256, 155, Q2_ACT_NONE,           Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "FRAG LIMIT   10", 256, 172, Q2_ACT_NONE,           Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "GAME VARIABLES",  256, 189, Q2_ACT_GAME_VARIABLES, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "PROCEED",         256, 206, Q2_ACT_PROCEED,        Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/* module+0x0F9BC — VERSUS replaces both timed limits with one round limit. */
static const q2_menu_item k_front_versus_setup[] = {
    { "2 3 4 PLAYERS", 256,  63, Q2_ACT_NONE,           Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "",              256,  82, Q2_ACT_NONE,           Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "ROUND LIMIT   3", 256, 153, Q2_ACT_NONE,         Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "GAME VARIABLES", 256, 172, Q2_ACT_GAME_VARIABLES, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "PROCEED",        256, 191, Q2_ACT_PROCEED,        Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

/*
 * QFRONT+0x49F8, module page 12. These are NOT the in-game PAUSED layouts:
 * the front end carries four separate arrays at +F194/+F1F4/+F284/+F344 and
 * uses the GAME VARIABLES banner. The small coordinate differences are the
 * bytes in those records, not layout adjustments made by the port.
 */
static const q2_menu_item k_front_vars_none[] = {
    { "    GRAVITY",       168,  97, Q2_ACT_NONE,            Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { "FALLING DAMAGE",    256, 124, Q2_ACT_NONE,            Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 151, Q2_ACT_RESET_VARIABLES, Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

static const q2_menu_item k_front_vars_bronze[] = {
    { "    GRAVITY",       168,  76, Q2_ACT_NONE,            Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { "WEAPON STAY",       256, 100, Q2_ACT_NONE,            Q2_SET_WEAPON_STAY,    Q2_WIDGET_TOGGLE, 0 },
    { "FALLING DAMAGE",    256, 124, Q2_ACT_NONE,            Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "ONE SHOT KILL",     256, 148, Q2_ACT_NONE,            Q2_SET_ONE_SHOT_KILL,  Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 172, Q2_ACT_RESET_VARIABLES, Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

static const q2_menu_item k_front_vars_silver[] = {
    { "    GRAVITY",       168,  58, Q2_ACT_NONE,            Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { " GAME SPEED",       168,  80, Q2_ACT_NONE,            Q2_SET_GAME_SPEED,     Q2_WIDGET_SLIDER, 0 },
    { "BLAST FORCE",       168, 102, Q2_ACT_NONE,            Q2_SET_BLAST_FORCE,    Q2_WIDGET_SLIDER, 0 },
    { "WEAPON STAY",       256, 124, Q2_ACT_NONE,            Q2_SET_WEAPON_STAY,    Q2_WIDGET_TOGGLE, 0 },
    { "FALLING DAMAGE",    256, 146, Q2_ACT_NONE,            Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "ONE SHOT KILL",     256, 168, Q2_ACT_NONE,            Q2_SET_ONE_SHOT_KILL,  Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 190, Q2_ACT_RESET_VARIABLES, Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

static const q2_menu_item k_front_vars_gold[] = {
    { "    GRAVITY",       168,  56, Q2_ACT_NONE,            Q2_SET_GRAVITY,        Q2_WIDGET_SLIDER, 0 },
    { " GAME SPEED",       168,  73, Q2_ACT_NONE,            Q2_SET_GAME_SPEED,     Q2_WIDGET_SLIDER, 0 },
    { "BLAST FORCE",       168,  90, Q2_ACT_NONE,            Q2_SET_BLAST_FORCE,    Q2_WIDGET_SLIDER, 0 },
    { "WEAPON STAY",       256, 107, Q2_ACT_NONE,            Q2_SET_WEAPON_STAY,    Q2_WIDGET_TOGGLE, 0 },
    { "INFINITE AMMO",     256, 124, Q2_ACT_NONE,            Q2_SET_INFINITE_AMMO,  Q2_WIDGET_TOGGLE, 0 },
    { "ALL WEAPONS",       256, 141, Q2_ACT_NONE,            Q2_SET_ALL_WEAPONS,    Q2_WIDGET_TOGGLE, 0 },
    { "FALLING DAMAGE",    256, 158, Q2_ACT_NONE,            Q2_SET_FALLING_DAMAGE, Q2_WIDGET_TOGGLE, 0 },
    { "ONE SHOT KILL",     256, 175, Q2_ACT_NONE,            Q2_SET_ONE_SHOT_KILL,  Q2_WIDGET_TOGGLE, 0 },
    { "RESET TO DEFAULTS", 256, 192, Q2_ACT_RESET_VARIABLES, Q2_SET_NONE,           Q2_WIDGET_TEXT,   0 },
};

/* module+0x0ED44 */
static const q2_menu_item k_front_options[] = {
    { "PLAYER OPTIONS", 256,  85, Q2_ACT_PAGE_PLAYER, Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "SOUND OPTIONS",  256, 111, Q2_ACT_PAGE_SOUND,  Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "VIDEO OPTIONS",  256, 137, Q2_ACT_PAGE_VIDEO,  Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
    { "VIEW CREDITS",   256, 163, Q2_ACT_CREDITS,     Q2_SET_NONE, Q2_WIDGET_TEXT, 0 },
};

static const q2_menu_page k_pages[] = {
    { Q2_PAGE_SCREEN_POSITION,  "POSITION",   k_position,         N(k_position),         N(k_position),      Q2_ACT_PAGE_VIDEO,   0x8009AF4Cu, EMPTY },
    { Q2_PAGE_PAUSE_SP,         "PAUSED",     k_pause_sp,         N(k_pause_sp),         0,                  Q2_ACT_NONE,         0x8009AA0Cu, 0 },
    { Q2_PAGE_OPTIONS,          "OPTIONS",    k_options,          N(k_options),          0,                  Q2_ACT_BACK,         0x8009AB14u, 0 },
    { Q2_PAGE_PLAYER,           "PLAYER",     k_player,           N(k_player),           0,                  Q2_ACT_PAGE_OPTIONS, 0x8009ADFCu, 0 },
    { Q2_PAGE_SOUND,            "SOUND",      k_sound,            N(k_sound),            0,                  Q2_ACT_BACK,         0x8009AE74u, 0 },
    { Q2_PAGE_VIDEO,            "VIDEO",      k_video_sp,         N(k_video_sp),         0,                  Q2_ACT_BACK,         0x8009AF94u, 0 },
    { Q2_PAGE_CONTROLLER,       "CONTROLLER", k_controller,       N(k_controller),       0,                  Q2_ACT_BACK,         0x8009AFDCu, 0 },
    { Q2_PAGE_RESTART_CONFIRM,  NULL,         k_restart_confirm,  N(k_restart_confirm),  1,                  Q2_ACT_BACK,         0x8009ABD4u, 0x8009AC04u },
    { Q2_PAGE_RESTARTING,       NULL,         k_restarting,       N(k_restarting),       N(k_restarting),    Q2_ACT_NONE,         0x8009AC4Cu, EMPTY },
    { Q2_PAGE_RESUPPLY_CONFIRM, NULL,         k_resupply_confirm, N(k_resupply_confirm), 3,                  Q2_ACT_BACK,         0x8009AD54u, 0x8009ADB4u },
    { Q2_PAGE_RESUPPLYING,      NULL,         k_restarting,       N(k_restarting),       N(k_restarting),    Q2_ACT_NONE,         0x8009AC4Cu, EMPTY },
    { Q2_PAGE_QUIT_CONFIRM,     NULL,         k_quit_confirm,     N(k_quit_confirm),     1,                  Q2_ACT_BACK,         0x8009AC94u, 0x8009ACC4u },
    { Q2_PAGE_QUITTING,         NULL,         k_quitting,         N(k_quitting),         N(k_quitting),      Q2_ACT_NONE,         0x8009AD0Cu, EMPTY },
    { Q2_PAGE_NO_CONTROLLER,    NULL,         k_no_controller,    N(k_no_controller),    N(k_no_controller), Q2_ACT_NONE,         0x8009B42Cu, 0 },
    { Q2_PAGE_DEATH,            NULL,         k_death,            N(k_death),            0,                  Q2_ACT_NONE,         0x8009AB74u, 0 },
    { Q2_PAGE_VARIABLES,        "PAUSED",     k_vars_none,        N(k_vars_none),        0,                  Q2_ACT_BACK,         0x8009A6C4u, 0 },
    { Q2_PAGE_PAUSE_MP,         "PAUSED",     k_pause_mp,         N(k_pause_mp),         0,                  Q2_ACT_NONE,         0x8009A964u, 0 },
    { Q2_PAGE_LOADING,          NULL,         k_loading,          N(k_loading),          N(k_loading),       Q2_ACT_NONE,         0x800A3314u, 0x800A3344u },

    /*
     * The front end.
     *
     * The BANNERS are the module's, and they were missing because they are not
     * in the page arrays: a front-end page is built by `module+0x3414(title,
     * page_id)` and the banner is that call's first argument, installed through
     * the same `0x8001F820` the in-game pages use. Every builder is one such
     * call followed by `engine+0x200(records, 32)`, so the pairing is exact —
     * `module+0xCDC4` is the START page and it passes `module+0x30`, "START".
     *
     * Only the title screen passes NULL, and that is the Q2LOGO model standing
     * where a banner would go, which is why its two rows sit lower than any
     * sub-page's first row.
     */
    { Q2_PAGE_FRONT_TITLE,      NULL,            k_front_title,      N(k_front_title),      0,                  Q2_ACT_NONE,         0x8010EC3Cu, 0 },
    { Q2_PAGE_FRONT_START,      "START",         k_front_start,      N(k_front_start),      0,                  Q2_ACT_BACK,         0x8010EC84u, 0 },
    { Q2_PAGE_FRONT_OPTIONS,    "OPTIONS",       k_front_options,    N(k_front_options),    0,                  Q2_ACT_BACK,         0x8010ED44u, 0 },
    { Q2_PAGE_FRONT_NEWLOAD,    "SINGLE PLAYER", k_front_newload,    N(k_front_newload),    0,                  Q2_ACT_BACK,         0x8010EF9Cu, 0 },
    { Q2_PAGE_FRONT_SKILL,      "DIFFICULTY",    k_front_skill,      N(k_front_skill),      0,                  Q2_ACT_BACK,         0x8010EFE4u, 0 },
    { Q2_PAGE_FRONT_MULTI,      "MULTIPLAYER",   k_front_multi,      N(k_front_multi),      0,                  Q2_ACT_BACK,         0x8010F104u, 0 },
    { Q2_PAGE_FRONT_DMSETUP,    "DEATHMATCH",   k_front_dmsetup,    N(k_front_dmsetup),    0,                  Q2_ACT_BACK,         0x8010F914u, 0 },
};

/* Variants, kept out of the main list so `q2_menu_pages` stays one page per
 * id and the disc check does not see the same id twice. */
static const q2_menu_page k_video_mp_page = {
    Q2_PAGE_VIDEO, "VIDEO", k_video_mp, N(k_video_mp), 0, Q2_ACT_BACK, 0x8009AEECu, 0
};

static const q2_menu_page k_vars_pages[4] = {
    { Q2_PAGE_VARIABLES, "PAUSED", k_vars_none,   N(k_vars_none),   0, Q2_ACT_BACK, 0x8009A6C4u, 0 },
    { Q2_PAGE_VARIABLES, "PAUSED", k_vars_bronze, N(k_vars_bronze), 0, Q2_ACT_BACK, 0x8009A724u, 0 },
    { Q2_PAGE_VARIABLES, "PAUSED", k_vars_silver, N(k_vars_silver), 0, Q2_ACT_BACK, 0x8009A7B4u, 0 },
    { Q2_PAGE_VARIABLES, "PAUSED", k_vars_gold,   N(k_vars_gold),   0, Q2_ACT_BACK, 0x8009A874u, 0 },
};

static const q2_menu_page k_front_setup_pages[3] = {
    { Q2_PAGE_FRONT_DMSETUP, "DEATHMATCH",      k_front_dmsetup,       N(k_front_dmsetup),       0, Q2_ACT_BACK, 0x8010F914u, 0 },
    { Q2_PAGE_FRONT_DMSETUP, "TEAM DEATHMATCH", k_front_dmsetup,       N(k_front_dmsetup),       0, Q2_ACT_BACK, 0x8010F914u, 0 },
    { Q2_PAGE_FRONT_DMSETUP, "VERSUS",          k_front_versus_setup, N(k_front_versus_setup), 0, Q2_ACT_BACK, 0x8010F9BCu, 0 },
};

static const q2_menu_page k_front_vars_pages[4] = {
    { Q2_PAGE_FRONT_VARIABLES, "GAME VARIABLES", k_front_vars_none,   N(k_front_vars_none),   0, Q2_ACT_BACK, 0x8010F194u, 0 },
    { Q2_PAGE_FRONT_VARIABLES, "GAME VARIABLES", k_front_vars_bronze, N(k_front_vars_bronze), 0, Q2_ACT_BACK, 0x8010F1F4u, 0 },
    { Q2_PAGE_FRONT_VARIABLES, "GAME VARIABLES", k_front_vars_silver, N(k_front_vars_silver), 0, Q2_ACT_BACK, 0x8010F284u, 0 },
    { Q2_PAGE_FRONT_VARIABLES, "GAME VARIABLES", k_front_vars_gold,   N(k_front_vars_gold),   0, Q2_ACT_BACK, 0x8010F344u, 0 },
};

typedef struct q2_front_arena {
    const char *name;
    const char *directory;
} q2_front_arena;

/* The contiguous arena run in the executable's level table, records 13..24. */
static const q2_front_arena k_front_arenas[Q2_MENU_MP_ARENA_COUNT] = {
    { "COLD STORAGE", "MATRIX6" },
    { "WAREHOUSE",    "MATRIX7" },
    { "AQUAPLEX",     "MATRIX2" },
    { "COLOSSEUM",    "THEVAT"  },
    { "MAINFRAME",    "MATRIX9" },
    { "QUICKFIRE",    "TIMS"    },
    { "CAPTIVITY",    "MATRIX1" },
    { "THE SHAFT",    "PODCITY" },
    { "HYDRAPHOBIA",  "MATRIX3" },
    { "TOXIC VATS",   "MATRIX8" },
    { "THE FORGE",    "MATRIX4" },
    { "BADLANDS",     "MATRIX5" },
};

const q2_menu_page *q2_menu_pages(u32 *count)
{
    if (count)
        *count = (u32)(sizeof(k_pages) / sizeof(k_pages[0]));
    return k_pages;
}

const q2_menu_page *q2_menu_page_find(int id)
{
    u32 i;

    for (i = 0; i < sizeof(k_pages) / sizeof(k_pages[0]); i++)
        if (k_pages[i].id == (u8)id)
            return &k_pages[i];
    return NULL;
}

const q2_menu_page *q2_menu_variables_page(int cheat_level)
{
    if (cheat_level < 0) cheat_level = 0;
    if (cheat_level > 3) cheat_level = 3;
    return &k_vars_pages[cheat_level];
}

const q2_menu_page *q2_menu_video_page(bool multiplayer)
{
    return multiplayer ? &k_video_mp_page : q2_menu_page_find(Q2_PAGE_VIDEO);
}

const q2_menu_page *q2_menu_front_setup_page(int mode)
{
    if (mode == Q2_MENU_MP_TEAM_DEATHMATCH)
        return &k_front_setup_pages[1];
    if (mode == Q2_MENU_MP_VERSUS)
        return &k_front_setup_pages[2];
    return &k_front_setup_pages[0];
}

const q2_menu_page *q2_menu_front_variables_page(int cheat_level)
{
    if (cheat_level < 0) cheat_level = 0;
    if (cheat_level > 3) cheat_level = 3;
    return &k_front_vars_pages[cheat_level];
}

const char *q2_menu_mp_arena_name(int arena)
{
    if (arena < 0 || arena >= Q2_MENU_MP_ARENA_COUNT)
        return "";
    return k_front_arenas[arena].name;
}

const char *q2_menu_mp_arena_directory(int arena)
{
    if (arena < 0 || arena >= Q2_MENU_MP_ARENA_COUNT)
        return "";
    return k_front_arenas[arena].directory;
}

/* 0x800AB4D8..0x800AB4F0 */
const char *q2_menu_cheat_level_name(int level)
{
    static const char *const names[] = { "NONE", "BRONZE", "SILVER", "GOLD" };
    if (level < 0 || level > 3)
        return "NONE";
    return names[level];
}

/* 0x800AB4F8..0x800AB508 */
const char *q2_menu_difficulty_name(int level)
{
    static const char *const names[] = { "EASY", "MEDIUM", "HARD" };
    if (level < 0 || level > 2)
        return "EASY";
    return names[level];
}

/*
 * The controller page's first item cycles these, in the order the strings sit
 * in the pool at 0x800AB1D8..0x800AB244 — which is reverse of the order the
 * compiler emitted them, so they read forwards here.
 */
const char *q2_menu_pad_style_name(int style)
{
    static const char *const names[] = {
        "RIGHT MOUSE", "RIGHT MOUSE 2", "HUNTER MOUSE", "RIGHT STICK",
        "LEFT STICK",  "BOTH STICKS",   "STANDARD A",   "STANDARD B",
        "STANDARD C"
    };
    if (style < 0 || style >= (int)(sizeof(names) / sizeof(names[0])))
        return "STANDARD A";
    return names[style];
}

int q2_menu_pad_style_count(void)
{
    return 9;
}
