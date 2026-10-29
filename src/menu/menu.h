/*
 * menu.h — the original's menu system, reconstructed from the executable.
 *
 * The PSX build's menus are not scripted and not on the disc: they are arrays
 * of 24-byte item records in the executable's data segment, walked by a small
 * engine at 0x80019B88..0x8001BA14. Everything here — the pages, the item
 * coordinates, the labels, the navigation rules, the widget behaviour — was
 * read out of that code and those tables, and `q2psx-inspect menu <disc>`
 * reads the tables back off a real disc and compares them, so a drift in the
 * transcription is a command that fails rather than an assertion in a document.
 *
 * The item record, from the loader at 0x8001A474:
 *
 *     +0x00  const char *label
 *     +0x04  s16 x            centre x, in the console's 512x248 framebuffer
 *     +0x06  s16 y
 *     +0x08  void (*action)(void)          -> object +0x4C
 *     +0x0C  s16 *toggle                   -> object +0x58
 *     +0x10  s16 *slider                   -> object +0x50
 *     +0x14  u32  act_on_release (bit 0)   -> object flags bit 13
 *
 * A page is built by calling the loader once per group, and the *last* group is
 * the navigable one — which is how a page of pure text is expressed (the second
 * call passes the empty table at 0x8009B30C). That boundary is `first` here.
 *
 * Coordinates are the original's, in its own 512x248 framebuffer. The renderer
 * scales them to whatever surface it is handed rather than re-authoring the
 * layout, so a page keeps its proportions at any output size.
 */
#ifndef Q2PSX_MENU_H
#define Q2PSX_MENU_H

#include "gamevars.h"
#include "q2psx.h"

/* The console's framebuffer, and so the coordinate space every item x/y in the
 * tables is expressed in. CONFIRMED at 0x800B2DA0/0x800B2DA2; see FORMATS.md
 * §"the framebuffer is 512 x 248". The title's y is computed from the height
 * at run time, so it is not a constant even in the original. */
#define Q2_MENU_SCREEN_W 512
#define Q2_MENU_SCREEN_H 248

/* ------------------------------------------------------------------------- */
/* Pad
 *
 * The engine reads one 16-bit button mask per player and derives "just
 * pressed" as `cur & ~prev` (0x80019BFC). These are the console's own bit
 * assignments; the host maps its keyboard or gamepad onto them.
 */
#define Q2_PAD_SELECT   0x0001u
#define Q2_PAD_L3       0x0002u
#define Q2_PAD_R3       0x0004u
#define Q2_PAD_START    0x0008u
#define Q2_PAD_UP       0x0010u
#define Q2_PAD_RIGHT    0x0020u
#define Q2_PAD_DOWN     0x0040u
#define Q2_PAD_LEFT     0x0080u
#define Q2_PAD_L2       0x0100u
#define Q2_PAD_R2       0x0200u
#define Q2_PAD_L1       0x0400u
#define Q2_PAD_R1       0x0800u
#define Q2_PAD_TRIANGLE 0x1000u
#define Q2_PAD_CIRCLE   0x2000u
#define Q2_PAD_CROSS    0x4000u
#define Q2_PAD_SQUARE   0x8000u

/* ------------------------------------------------------------------------- */
/* Settings
 *
 * Each is a signed 16-bit global in the menu state block based at 0x800B2FE4.
 * The address is recorded so the mapping stays checkable, and the defaults are
 * the ones the original's reset routines write.
 */
typedef enum q2_menu_setting {
    Q2_SET_NONE = 0,

    Q2_SET_HORIZONTAL_SPLIT,  /* 0x800B333C  video     toggle */
    Q2_SET_SCREEN_X,          /* 0x800B3368  video     offset */
    Q2_SET_SCREEN_Y,          /* 0x800B336A  video     offset */

    Q2_SET_STEREO,            /* 0x800B333E  sound     toggle */
    Q2_SET_MUSIC,             /* 0x800B3348  sound     slider */
    Q2_SET_SFX,               /* 0x800B334C  sound     slider */

    Q2_SET_CROSSHAIR,         /* 0x800B3340  player    toggle */
    Q2_SET_AUTOCENTRE,        /* 0x800B3342  player    toggle */

    Q2_SET_PAD_STYLE,         /* 0x800B32D2 + p*34     choice */
    Q2_SET_PAD_CLASS,         /* which of the three style groups is offered */
    Q2_SET_VIBRATION,         /* controller page                            */
    Q2_SET_SWAP_Y,            /* controller page                            */
    Q2_SET_USE_MOUSE,         /* controller page                            */
    Q2_SET_MOUSE_SPEED,       /* 0x800B32C2 + p*34     slider               */

    Q2_SET_GRAVITY,           /* 0x800B335A  variables slider */
    Q2_SET_GAME_SPEED,        /* 0x800B335E  variables slider */
    Q2_SET_BLAST_FORCE,       /* 0x800B3358  variables slider */
    Q2_SET_WEAPON_STAY,       /* 0x800B3360  variables toggle */
    Q2_SET_INFINITE_AMMO,     /* 0x800B3362  variables toggle */
    Q2_SET_ALL_WEAPONS,       /* 0x800B3364  variables toggle */
    Q2_SET_FALLING_DAMAGE,    /* 0x800B3366  variables toggle */
    Q2_SET_ONE_SHOT_KILL,     /* 0x800B336C  variables toggle */

    /* Host-only settings are appended to keep saved retail indices stable. */
    Q2_SET_CRT,
    Q2_SET_BLOOM,
    /* Append the other players' seven controller fields. Existing settings
     * files retain their indices and leave absent profiles at defaults. */
    Q2_SET_EXTRA_PAD_FIRST,
    Q2_SET_COUNT = Q2_SET_EXTRA_PAD_FIRST + 3 *
                   (Q2_SET_MOUSE_SPEED - Q2_SET_PAD_STYLE + 1)
} q2_menu_setting;

/* Sliders are clamped by the adjust loop at 0x8001C1B8: below zero snaps to
 * zero, 128 or more snaps to 127. The step is per *frame held*, not per press. */
#define Q2_MENU_SLIDER_MAX  127
#define Q2_MENU_SLIDER_STEP 2

typedef struct q2_menu_settings {
    s16 v[Q2_SET_COUNT];
} q2_menu_settings;

/* Only the controller fields belong to the selected player's 34-byte retail
 * config record. Video, sound, crosshair and autocentre remain shared. */
q2_menu_setting q2_menu_player_setting(q2_menu_setting id, int player);

/* The reset routines the menu exposes separately: 0x8001FA18 (video),
 * 0x8001FA50 (sound), 0x8001BDA8 (player), 0x8002048C (game variables).
 * `q2_menu_settings_defaults` runs all four. */
void q2_menu_settings_defaults(q2_menu_settings *s);
void q2_menu_reset_video(q2_menu_settings *s);

/*
 * SCREEN POSITION's default Y, which is the display env's `screen.y` itself
 * (Config_InstallVideo, 0x8001C29C): 24 lines down PAL's taller raster, and 0
 * on NTSC, whose 240 lines fill its own — FrontEndResetVideoDefaults stores
 * zero in SLUS-00757 where SLES-01534 stores 24. It is the picture's neutral
 * position, so it is also what an offset is measured from. `screen_h` is the
 * framebuffer's height, which is what tells the two builds apart here.
 */
int  q2_menu_screen_y_default(int screen_h);
void q2_menu_reset_video_for(q2_menu_settings *s, int screen_h);
void q2_menu_reset_sound(q2_menu_settings *s);
void q2_menu_reset_player(q2_menu_settings *s);
void q2_menu_reset_variables(q2_menu_settings *s);

/*
 * What the game variables actually do, from 0x8001C698. The engine turns the
 * sliders into physics scalars and the toggles into a cheat mask; a port that
 * stores the settings without applying them has not implemented the page.
 *
 * The mask itself is in gamevars.h because the game layer reads it — the item
 * dispatch changes how much ammo a weapon grants when INFINITE AMMO is on.
 */

typedef struct q2_menu_rules {
    s32 gravity;    /* (GRAVITY + 64) >> 2 — 32 when the variables are off   */
    s32 tick_rate;  /* base * (GAME_SPEED + 64) >> 7                          */
    u16 cheats;     /* the mask above                                         */
    /*
     * BLAST FORCE, untransformed. The other three rows are recomputed by
     * 0x8001C698 and reset by 0x8001C7E4; this one is not — the slider's own
     * store at 0x8001BE38 is the only writer besides the reset routine, and the
     * damage function reads the halfword raw. So it carries on BOTH arms.
     */
    s32 blast_force;
} q2_menu_rules;

void q2_menu_apply_variables(const q2_menu_settings *s, bool enabled,
                             s32 base_tick_rate, q2_menu_rules *out);

/* ------------------------------------------------------------------------- */
/* Pages and items */

typedef enum q2_menu_widget {
    Q2_WIDGET_TEXT = 0,   /* a label, which may run an action              */
    Q2_WIDGET_TOGGLE,     /* "LABEL  ON  OFF"; left picks ON, right OFF    */
    Q2_WIDGET_SLIDER,     /* a 133x8 bar, adjusted while left/right is held */
    Q2_WIDGET_CHOICE      /* a named value cycled by left/right, wrapping   */
} q2_menu_widget;

/*
 * Actions, each named after the original's handler address so the page graph
 * can be checked against the executable rather than taken on trust.
 */
typedef enum q2_menu_action {
    Q2_ACT_NONE = 0,
    Q2_ACT_RESUME,             /* 0x80020140  leave the menu, resume play     */
    Q2_ACT_PAGE_VIDEO,         /* 0x80020284                                  */
    Q2_ACT_PAGE_SOUND,         /* 0x80020218                                  */
    Q2_ACT_PAGE_VARIABLES,     /* 0x8001D460                                  */
    Q2_ACT_PAGE_CONTROLLER,    /* 0x8001D130                                  */
    Q2_ACT_PAGE_PLAYER,        /* 0x800201B8                                  */
    Q2_ACT_PAGE_OPTIONS,       /* 0x8001FDB0                                  */
    Q2_ACT_PAGE_POSITION,      /* 0x8001D088                                  */
    Q2_ACT_MISSION,            /* 0x8002033C  leave for the mission screen    */
    Q2_ACT_ASK_RESTART,        /* 0x8001FE74  page 32                         */
    Q2_ACT_DO_RESTART,         /* 0x8001FE10  page 33, then restart           */
    Q2_ACT_ASK_QUIT,           /* 0x800200B0  page 36                         */
    Q2_ACT_DO_QUIT,            /* 0x8001FFF8  page 37, then quit              */
    Q2_ACT_ASK_RESUPPLY,       /* 0x8001FF24  page 34                         */
    Q2_ACT_DO_RESUPPLY,        /* 0x8001FEB4  page 35, then resupply          */
    Q2_ACT_BACK,               /* 0x8001D5A8 / 0x8001D738 — the page's parent */
    Q2_ACT_RESET_VIDEO,        /* 0x8001FA18                                  */
    Q2_ACT_RESET_SOUND,        /* 0x8001FA50                                  */
    Q2_ACT_RESET_PLAYER,       /* 0x8001BDA8                                  */
    Q2_ACT_RESET_VARIABLES,    /* 0x8002048C                                  */

    /* The front end's own, from QFRONT's module rather than the executable —
     * the address after each is a module offset at Q2_MOD_BASE. */
    Q2_ACT_PAGE_FRONT_START,   /* module+0xCDC4  START   -> SINGLE / MULTI    */
    Q2_ACT_PAGE_FRONT_OPTIONS, /* module+0xCCA4  OPTIONS -> the four rows     */
    Q2_ACT_NEW_GAME,           /* module+0xCD40  SINGLE PLAYER                */
    Q2_ACT_MULTIPLAYER,        /* module+0xCF68  MULTI PLAYER                 */
    Q2_ACT_CREDITS,            /* module+0x35C8  VIEW CREDITS                 */
    Q2_ACT_PAGE_FRONT_NEWLOAD, /* module+0xCD40  SINGLE PLAYER                */
    Q2_ACT_PAGE_FRONT_SKILL,   /* module+0xD0AC  NEW GAME                     */
    Q2_ACT_LOAD_GAME,          /* module+0xD400  LOAD GAME                    */
    Q2_ACT_SKILL_EASY,         /* module+0xD380                               */
    Q2_ACT_SKILL_MEDIUM,       /* module+0xD3A8                               */
    Q2_ACT_SKILL_HARD,         /* module+0xD3D4                               */
    Q2_ACT_DM_MODE,            /* module+0x4AD8 — shared by all three modes   */
    Q2_ACT_MP_SETTINGS,        /* module+0xD148 / +0xD1FC                     */
    Q2_ACT_GAME_VARIABLES,     /* module+0x49F8                               */
    Q2_ACT_PROCEED,            /* module+0x59CC                               */
    Q2_ACT_UNLOCK_SAVE,        /* module+0xD2C4  save the campaign reward      */
    Q2_ACT_PAGE_FRONT_TITLE,   /* module+0xCEE0                               */
    Q2_ACT_COUNT
} q2_menu_action;

typedef struct q2_menu_item {
    const char *label;      /* may carry colour codes — see below           */
    s16         x, y;
    u8          action;     /* q2_menu_action                                */
    u8          setting;    /* q2_menu_setting                               */
    u8          widget;     /* q2_menu_widget                                */
    u8          on_release; /* fire on release rather than press (bit 13)    */
} q2_menu_item;

/*
 * Page ids are the original's own, as passed to the page-enter routine at
 * 0x8001A384. They are not contiguous: the same id space carries screens that
 * are not menus.
 */
typedef enum q2_menu_page_id {
    Q2_PAGE_NONE             = 0,
    Q2_PAGE_SCREEN_POSITION  = 7,
    Q2_PAGE_PAUSE_SP         = 26,
    Q2_PAGE_OPTIONS          = 27,
    Q2_PAGE_PLAYER           = 28,
    Q2_PAGE_SOUND            = 29,
    Q2_PAGE_VIDEO            = 30,
    Q2_PAGE_CONTROLLER       = 31,
    Q2_PAGE_RESTART_CONFIRM  = 32,
    Q2_PAGE_RESTARTING       = 33,
    Q2_PAGE_RESUPPLY_CONFIRM = 34,
    Q2_PAGE_RESUPPLYING      = 35,
    Q2_PAGE_QUIT_CONFIRM     = 36,
    Q2_PAGE_QUITTING         = 37,
    Q2_PAGE_NO_CONTROLLER    = 38,
    Q2_PAGE_DEATH            = 41,
    Q2_PAGE_VARIABLES        = 42,
    Q2_PAGE_PAUSE_MP         = 43,

    /*
     * The front end. Page 46 is the console's own id for it, special-cased
     * inside q2_menu_open (0x8001A40C); the two below it have no id in the
     * executable because they are not the executable's — the whole front end
     * is QFRONT's LevelBin, and its item records are a static array in that
     * module (openquestions #44). These two are the port's own numbering,
     * chosen above every id the executable uses so they cannot collide.
     *
     * The MODULE's own ids are small and they are recorded here rather than
     * adopted, because they are not free. Each builder opens with
     * `module+0x3414(banner, id)` and that id goes straight to the page-enter
     * routine at `0x8001A384`, so the front end and the executable share one id
     * space and the front end sits at the bottom of it:
     *
     *     0  title        1  START         2  OPTIONS      3  PLAYER
     *     4  SOUND        5  VIDEO         6  CONTROLS     8  SINGLE PLAYER
     *     9  DIFFICULTY  10  MULTIPLAYER  15  CREDITS     16  LOAD
     *
     * They collide with nothing in the original because a page's TABLE comes
     * from whichever image installed it, and only one front end is ever up.
     * This port keeps one page array for both, so adopting them would make id 3
     * mean two different pages; the numbering below is what keeps that honest.
     * The ids still matter — the scene's own code reads the live one and
     * changes what the logo does (levelbin.h) — so they are written down.
     */
    Q2_PAGE_FRONT_TITLE      = 46,
    Q2_PAGE_FRONT_START      = 200,
    Q2_PAGE_FRONT_OPTIONS    = 201,
    Q2_PAGE_FRONT_NEWLOAD    = 202,   /* NEW GAME / LOAD GAME             */
    Q2_PAGE_FRONT_SKILL      = 203,   /* EASY / MEDIUM / HARD             */
    Q2_PAGE_FRONT_MULTI      = 204,   /* the five multiplayer rows        */
    Q2_PAGE_FRONT_DMSETUP    = 205,   /* mode-specific match setup        */
    Q2_PAGE_FRONT_VARIABLES  = 206,   /* QFRONT's GAME VARIABLES layouts */

    /*
     * THE LOADING SCREEN, and the console's own id for it is 46.
     *
     * `0x80079178` — the level transition's first act — enters page 46 and
     * installs `0x800A3314`, one record reading `{ "LOADING", 256, 124 }`, then
     * `0x800A3344`, which is all zeros and installs nothing. That is the page:
     * one line of text with nothing navigable under it. See loading.h for the
     * whole function and for where the logo behind it comes from.
     *
     * It cannot BE 46 here, because 46 is what this port gave the title screen
     * — the engine's page 46 is "the front end" as a whole and the title screen
     * needed a number. So this is the port's own numbering, above every id the
     * executable uses, exactly as the six front-end pages are.
     */
    Q2_PAGE_LOADING          = 207,

    /*
     * The front end's own version of it, and the module has no id for this one
     * either. `module+0x0EBF4` installs `STARTING` at (256, 111) and `GAME` at
     * (256, 137) — two rows on the same 26-pixel pitch every other front-end
     * page uses, and the same shape as RESTARTING / LEVEL. It is what the half
     * second between a confirmed difficulty and the opening reel looks like.
     */
    Q2_PAGE_STARTING         = 208,

    /*
     * THE MULTIPLAYER RULES PAGE, which the module calls page 14.
     *
     * SQUARE on one of the MULTIPLAYER page's three mode rows runs
     * `module+0x4868`: it reads the mode it wrote into `engine+0x370` from the
     * cursor row (`module+0x45F8`, `0x4604`, `0x460C` store 0, 1 and 5), indexes
     * the six-entry jump table at `module+0x11C4`, and installs that mode's
     * banner and rules table through `module+0x3414(banner, 14)`. Every arm
     * then installs the empty table at `module+0x0FADC` as the second one, so
     * nothing on the page is navigable, and sets `engine+0x28C` to the
     * MULTIPLAYER page's own builder, so TRIANGLE comes straight back.
     *
     * 14 collides with nothing here only because this port numbers the front
     * end from 200, as the note above explains.
     */
    Q2_PAGE_FRONT_RULES      = 209,
    Q2_PAGE_FRONT_PLAYERS    = 210, /* QFRONT's independently controlled panels */
    Q2_PAGE_FRONT_UNLOCK     = 211  /* QFRONT page 17, +4454 completion reward */
} q2_menu_page_id;

/*
 * A page is one or two item tables. The loader is called once per table and
 * the last call decides what is navigable, so `first` is where the second
 * table's items begin — and a page of pure text is a real table followed by
 * the empty one at 0x8009B30C, which is why `first` can equal `count`.
 *
 *   items[0 .. first)      come from `addr`
 *   items[first .. count)  come from `addr2` when there is one, `addr`
 *                          otherwise (a page whose single table is navigable)
 */
typedef struct q2_menu_page {
    u8                  id;
    const char         *title;  /* NULL when the page installs no title      */
    const q2_menu_item *items;
    u8                  count;
    u8                  first;  /* first navigable index; == count means the
                                 * page is pure text                         */
    u8                  back;   /* q2_menu_action for TRIANGLE, or none      */
    u32                 addr;   /* the first table's address in the EXE      */
    u32                 addr2;  /* the second table's, or 0                  */
} q2_menu_page;

/* Every page, one entry per id, in the order the tables appear. This is what
 * the inspect tool iterates when it checks the transcription. */
const q2_menu_page *q2_menu_pages(u32 *count);
const q2_menu_page *q2_menu_page_find(int id);

/* Pages whose installed table depends on live state. */
const q2_menu_page *q2_menu_variables_page(int cheat_level); /* 0x8001D510 */
const q2_menu_page *q2_menu_video_page(bool multiplayer);    /* 0x800202B0 */
/* Host extension; the original tables above remain available to disc audits. */
const q2_menu_page *q2_menu_video_host_page(bool multiplayer);
/*
 * CONTROLLER is the same page in both images with four of its five labels
 * abbreviated: the executable's table (0x8009AFDC) reads VIBRATION / SWAP Y
 * AXIS / USE MOUSE, QFRONT's own (module+0x0FA4C, installed by module+0x39D8)
 * reads VIBRATE / SWAP Y / MOUSE. Same coordinates, same five records.
 */
const q2_menu_page *q2_menu_controller_page(bool front_end); /* QFRONT+39D8 */
const q2_menu_page *q2_menu_front_setup_page(int mode);       /* QFRONT+4AD8 */
/*
 * The rules page for one game mode, keyed on the Q2_MENU_MP_* value the way
 * `module+0x4868` keys its jump table. Six arms: 0 DEATHMATCH, 1 TEAM
 * DEATHMATCH, 2 CAPTURE THE FLAG, 3 TAG, 4 TEAM TAG, 5 VERSUS; anything else
 * gets the empty table at module+0x0FADC, which is a page with a banner and
 * nothing on it.
 */
const q2_menu_page *q2_menu_front_rules_page(int mode);       /* QFRONT+4868 */
const q2_menu_page *q2_menu_front_variables_page(int cheat_level); /* +49F8 */

/* QFRONT's twelve selectable arenas, level-table records 13..24. */
#define Q2_MENU_MP_ARENA_COUNT 12
const char *q2_menu_mp_arena_name(int arena);
const char *q2_menu_mp_arena_directory(int arena);

const char *q2_menu_cheat_level_name(int level);  /* NONE/BRONZE/SILVER/GOLD */
const char *q2_menu_difficulty_name(int level);   /* EASY/MEDIUM/HARD        */
const char *q2_menu_pad_style_name(int style);
int         q2_menu_pad_style_count(void);

/* ------------------------------------------------------------------------- */
/* Colour codes
 *
 * A label may contain one-character codes that the glyph loop at 0x8001AEF8
 * consumes without printing. The set is exactly {'b','d','g','u'}: the length
 * routine at 0x8001FD18 skips those four and no others.
 */
#define Q2_MENU_CODE_BRIGHT 'b'   /* back to the normal colour               */
#define Q2_MENU_CODE_DIM    'd'   /* the second font palette                 */
#define Q2_MENU_CODE_GREY   'g'   /* dim and grey — and NOT navigable        */
#define Q2_MENU_CODE_UNDER  'u'   /* an extra emphasis pass                  */

bool q2_menu_is_code(char c);
int  q2_menu_text_length(const char *s);   /* printable length — 0x8001FD18 */

/* ------------------------------------------------------------------------- */
/* Sounds the engine plays, by the original's sound-bank keys. */
typedef enum q2_menu_sound {
    Q2_MSND_NONE = 0,
    Q2_MSND_MOVE,      /* "msc_menu2"    the cursor moved   */
    Q2_MSND_SELECT,    /* "msc_menu1"    an item fired      */
    Q2_MSND_BACK,      /* "msc_menu3"    triangle           */
    Q2_MSND_TOGGLE,    /* "itm_pkup"     a toggle changed   */
    Q2_MSND_SLIDE      /* "msc_comp_up"  a slider is moving */
} q2_menu_sound;

const char *q2_menu_sound_name(q2_menu_sound s);

/* ------------------------------------------------------------------------- */
/*
 * What the menu asks the host to do. The original called straight into the
 * game; a port needs the request surfaced so the client owns the side effects.
 */
typedef enum q2_menu_request {
    Q2_MREQ_NONE = 0,
    Q2_MREQ_RESUME,      /* close the menu and carry on           */
    Q2_MREQ_RESTART,     /* restart the level                     */
    Q2_MREQ_QUIT,        /* leave the game                        */
    Q2_MREQ_MISSION,     /* show the mission/intermission screen  */
    Q2_MREQ_RESUPPLY,    /* spend a resupply and restart          */

    /* The front end's, which a caller answers by leaving the title screen. */
    Q2_MREQ_NEW_GAME,    /* SINGLE PLAYER: begin the game         */
    Q2_MREQ_MULTIPLAYER, /* MULTI PLAYER                          */
    Q2_MREQ_CREDITS,     /* VIEW CREDITS                          */
    Q2_MREQ_LOAD_GAME,   /* LOAD GAME: enter the card front end   */
    Q2_MREQ_MP_LOAD_SETTINGS,
    Q2_MREQ_MP_SAVE_SETTINGS,
    Q2_MREQ_MP_PROCEED, /* all players READY: begin the configured match */
    Q2_MREQ_UNLOCK_SAVE /* completion reward's YES */
} q2_menu_request;

#define Q2_MENU_MAX_ITEMS 12
#define Q2_MENU_TEXT_MAX  40   /* the original's per-object text buffer is 32 */

/*
 * The front end stores option INDICES and converts them when all players are
 * READY (QFRONT+0x1F60, after +0x59CC's setup). Modes are QMULTI's 0, 1 and 5; keeping
 * them numeric here avoids making the menu library depend back on game code.
 */
#define Q2_MENU_MP_DEATHMATCH       0
#define Q2_MENU_MP_TEAM_DEATHMATCH  1
#define Q2_MENU_MP_VERSUS           5
#define Q2_MENU_MP_MAX_PLAYERS      4
#define Q2_MENU_MP_TIME_OPTIONS     12
#define Q2_MENU_MP_FRAG_OPTIONS      9
#define Q2_MENU_MP_ROUND_OPTIONS     5
#define Q2_MENU_MP_TIME_DEFAULT      5
#define Q2_MENU_MP_FRAG_DEFAULT      5
#define Q2_MENU_MP_ROUND_DEFAULT     2

/* ------------------------------------------------------------------------- */
/*
 * The selection bar's colour — bits 6..8 of a drawable's flag word.
 *
 * FrontEndDrawCursor recovers the field with `srl 6` / `andi 7` at 0x8001A9C4
 * and switches on 0, 1, 2 and 3; the value 4 is the sentinel that suppresses
 * the bar entirely (`(flags & 0x1C0) == 0x100` at 0x8001A828). The palettes are
 * in menudraw.h — this is the field, which the menu fills in and the drawing
 * code only reads.
 */
#define Q2_MENU_BAR_BLUE    0
#define Q2_MENU_BAR_RED     1
#define Q2_MENU_BAR_PURPLE  2
#define Q2_MENU_BAR_GREEN   3
#define Q2_MENU_BAR_NONE    4

typedef struct q2_menu_mp_setup {
    s16 mode;          /* 0 DEATHMATCH, 1 TEAM DEATHMATCH, 5 VERSUS */
    s16 players;       /* 2..connected controller count              */
    s16 arena;         /* 0..11                                      */
    s16 time_option;   /* index into QFRONT's twelve-value table     */
    s16 frag_option;   /* index into its nine-value table            */
    s16 round_option;  /* index into its five-value table            */
} q2_menu_mp_setup;

/* QFRONT +0x128C/+0x13FC: input and drawing dispatch the same seven states. */
typedef enum q2_menu_player_page {
    Q2_MP_PLAYER_MAIN, Q2_MP_PLAYER_OPTIONS, Q2_MP_PLAYER_NAME,
    Q2_MP_PLAYER_COLOUR, Q2_MP_PLAYER_CONTROLLER, Q2_MP_PLAYER_READY,
    Q2_MP_PLAYER_DISCONNECTED
} q2_menu_player_page;

typedef struct q2_menu_player_panel {
    u16 previous;
    u8 page, cursor, letter;
    u8 resume_page, resume_cursor;
} q2_menu_player_panel;

typedef struct q2_menu_player_row {
    char text[64];
    s16 y, size;
    bool selected, disabled;
} q2_menu_player_row;

typedef struct q2_menu {
    q2_menu_settings   *set;          /* not owned                            */

    const q2_menu_page *page;
    int                 cursor;
    int                 page_id;
    int                 prev_page_id;

    /* Labels the page rewrites at run time: the death screen's resupply line
     * (0x8001D774) and anything else a hook formats into an object's text. An
     * empty entry means "use the table's label". */
    char                text[Q2_MENU_MAX_ITEMS][Q2_MENU_TEXT_MAX];
    /* The object's +0x4A field: a hook can take an item out of the navigation
     * without touching its label (0x8001CA28 does this to SWAP Y AXIS). */
    u8                  disabled[Q2_MENU_MAX_ITEMS];

    /*
     * The single-player pause menu's status line, placed at (256, 204) by
     * 0x8001D6B4 and formatted with "KILLS %d/%d    %d/%d SECRETS".
     *
     * It belongs to page 26's own install, so it is composed in `page_entered`
     * and wiped by every other page entry, exactly as 0x8001D614's arm is the
     * only one that writes the object at 0x800C3818. The four numbers below are
     * what a caller pushes in; they outlive a page change so that re-entering
     * the pause page re-composes the row without the caller being asked again.
     */
    char                status[48];
    int                 stat_kills;          /* 0x800B29E8 */
    int                 stat_total_kills;    /* 0x800B29E4 */
    int                 stat_secrets;        /* 0x800B29FC */
    int                 stat_total_secrets;  /* 0x800B29F8 */

    /*
     * CursorMemory (0x800C68A0), the 47 two-byte records the engine keeps one
     * per page.
     *
     * InGameInitScreen SAVES the outgoing page's cursor into byte 0 on every
     * switch (`lbu a2, 712(v1)` at 0x8001A3B4, `sb a2, 0(v0)` at 0x8001A3C0),
     * but RESTORING is opt-in: see `cursor_restores` in menu.c for the seven
     * executable pages that ask for it. Byte 1
     * is a never-visited flag, seeded to 1 for all 47 records by
     * FrontEndClearCursorMemory (0x8001FC0C) and cleared on the first restore
     * (0x8001FBF0), so a page's first visit takes the builder's default rather
     * than a saved zero.
     */
    u8                  cursor_save[256];
    u8                  cursor_seen[256];

    /* Where the page graph came from, so BACK can retrace it. The original
     * stored one handler per page; a stack is the same thing said honestly. */
    u8                  stack[16];
    int                 depth;

    u16                 pad_prev;
    u16                 pad_new;   /* cur & ~prev — 0x80019BFC */
    u16                 pad_rel;   /* prev & ~cur              */
    u16                 pad_cur;
    /* One frame is latched but not acted on after the menu opens, so the
     * button that opened it cannot also select something. */
    u8                  settle;

    bool                open;
    bool                multiplayer;  /* 0x800AEBCC — picks page 43 over 26  */

    /*
     * Who pressed START (0x800AE8B8, `Pauser`), and the two halfwords of that
     * player's 34-byte configuration record the pause banner is coloured from.
     *
     * FrontEndAddPausedTitle reads the TEAM halfword at 0x800B32B8 + p*34 when
     * the session mode is 1 and the player-colour halfword at 0x800B32BA + p*34
     * otherwise (0x8001CFE0..0x8001D038), and IsPlayerPadShockable is asked
     * about the same `Pauser` index (0x8001C9B4, 0x8001C9D0). The seeds below
     * are FrontEndResetMultiplayerDefaults': colour = p (0x8001BEDC), team =
     * p & 1 (0x8001BF04).
     */
    int                 pauser;
    s16                 mp_team[Q2_MENU_MP_MAX_PLAYERS];    /* config +2  */
    s16                 mp_colour[Q2_MENU_MP_MAX_PLAYERS];  /* config +4  */
    char                mp_name[Q2_MENU_MP_MAX_PLAYERS][9]; /* config +24 */
    q2_menu_player_panel mp_panel[Q2_MENU_MP_MAX_PLAYERS];
    bool                mp_panel_settle;
    bool                mp_host_mouse; /* one host mouse, owned by player one */
    s16                 mp_mode;      /* 0x800B3354 — 1 is TEAM DEATHMATCH */

    /*
     * Whether each player's pad has rumble motors — IsPlayerPadShockable
     * (0x80033250), which reads bit 8 of the pad record's +0x0E, a bit
     * PadDualShock sets at 0x80031844 only when PadInfoAct reports two
     * actuators and clears at 0x80031878 otherwise.
     *
     * This is a PAD CAPABILITY, not the VIBRATION setting: the setting is a
     * separate halfword (0x800B32BC + p*34) that the capability gates. Zero
     * here is the console's answer for a pad with no motors, so the client has
     * to say otherwise — q2_menu_set_pad_shockable.
     */
    u8                  pad_shockable[Q2_MENU_MP_MAX_PLAYERS];

    /*
     * The bar colour of the page BANNER, which is a per-drawable field and not
     * a per-page one: FrontEndAddPausedTitle stamps bits 6..8 of drawable 0's
     * flag word (0x8001D044) and FrontEndDrawCursor reads them straight back
     * (`srl 6` / `andi 7` at 0x8001A9C4). Rows never carry it — FrontEndCreateMenu
     * clears those bits on every record it appends (0x8001A524) — so the
     * selected row's band stays blue while the banner identifies the pauser.
     */
    u8                  title_bar_colour;   /* Q2_MENU_BAR_* */

    /*
     * Whether the pages come from QFRONT rather than from the executable.
     *
     * On the console this is not a flag at all: the front end is a different
     * image, so `module+0xCC0C` installs QFRONT's own VIDEO table and
     * `module+0x39D8` its own CONTROLLER table, and neither can be reached
     * while the executable's menu is up. This port keeps one page array for
     * both, so the two pages that actually differ are resolved through this.
     */
    bool                front_end;
    bool                us_english;   /* the North American build's spellings */
    int                 cheat_level;  /* 0x800B335C                          */
    int                 resupplies;   /* 0x800B335D                          */
    int                 screen_h;     /* 0x800B2DA2 — the title's y follows  */

    /*
     * The framebuffer this menu's 512 x 248 block is centred in: 248 on PAL,
     * 240 on NTSC, where the block lands four lines up. That IS the NTSC
     * build's layout: 105 of the 111 rows in its executable's page tables and
     * all 53 of QFRONT's that are transcribed here are PAL's less four — see
     * q2_menu_item_y for the table it did not move.
     */
    int                 fb_h;

    /* The death screen is inert until its countdown expires (0x8002052C). */
    /*
     * The death page's own countdown, in the LEVEL CLOCK's units — 300 to the
     * second, the same units the mover scripting and the weapon refire use.
     *
     * 0x80020544 subtracts the frame's dt from it, so the console's 600 is two
     * seconds. This port decremented it by ONE PER FRAME, which made it 600
     * frames: twenty seconds at the headless 1/30 s step, and a different wait
     * on every host frame rate.
     */
    int                 arm_ticks;

    /* The frame delta `q2_menu_advance` spends on `arm_ticks`, in those same
     * units. Zero falls back to a nominal 30 Hz frame so a caller that does not
     * set it still counts down in real time rather than stalling. */
    s32                 frame_dt;

    /* Held-direction repeat gate for the slider sound (gp+184). */
    s32                 slide_repeat;

    q2_menu_sound       sound;        /* one-shot, cleared by the reader     */
    q2_menu_request     request;      /* one-shot, cleared by the reader     */

    /* What EASY / MEDIUM / HARD chose, 0..2, read alongside Q2_MREQ_NEW_GAME.
     * The AI's own `q2_cre_set_skill` is what consumes it. */
    int                 skill;

    /* QFRONT's multiplayer globals at engine+866 and +874..+882. */
    q2_menu_mp_setup    mp_setup;
    int                 controller_count;
} q2_menu;

void q2_menu_init(q2_menu *m, q2_menu_settings *settings, int screen_h);

/* Open on the pause page this session would have used (43 in multiplayer,
 * 26 otherwise — 0x8001D5C8). */
void q2_menu_open(q2_menu *m);
void q2_menu_close(q2_menu *m);
void q2_menu_goto(q2_menu *m, int page_id);

/* One frame. `buttons` is the current pad mask; "just pressed" is derived. */
void q2_menu_advance(q2_menu *m, u16 buttons);

/* ------------------------------------------------------------------------- */
/* Pointing at it
 *
 * Two things a pad cannot express and a pointer can: landing on a row without
 * passing through the ones above it, and naming a slider's value outright
 * rather than walking to it two units a frame. Both are the port's — the
 * console had no pointer — and both are here rather than in the client so the
 * cursor rules and the slider's clamp stay in one place. Everything else a
 * mouse does is a pad button (menumouse.h), and goes through q2_menu_advance
 * like any other press.
 */

/* Put the cursor on `index`. False, and no move, when it is not selectable or
 * is already where the cursor is. Plays the cursor sound when it moves, so a
 * page sounds the same swept with a pointer as walked with the d-pad. */
bool q2_menu_point_at(q2_menu *m, int index);

/* Set a slider outright. False unless `index` really is a bound slider; the
 * value is clamped by the adjust loop's own rule (0x8001C1B8). */
bool q2_menu_set_slider(q2_menu *m, int index, int value);

/*
 * What CHECK_FOR_SLIDER's tail (0x8001C218) would write into the CD volume
 * target for the page that is up: `full` when the MUSIC row is the cursor row,
 * 0 when the page carries a MUSIC row and the cursor is elsewhere, and -1 —
 * "leave the target alone" — when it carries none at all, which is the s7 = -1
 * seed at 0x8001BF2C surviving the walk. `full` is the slider doubled; see the
 * definition for why the caller and not this does the doubling.
 */
int q2_menu_music_audition(const q2_menu *m, int full);

/* Context the pages read. Set these before opening. */
void q2_menu_set_multiplayer(q2_menu *m, bool on);

/*
 * Who paused, and what that player's session record says — 0x800AE8B8,
 * 0x800B32B8 + p*34, 0x800B32BA + p*34 and 0x800B3354. Together they decide
 * the banner's bar colour in a split-screen match; in single player
 * FrontEndAddPausedTitle branches straight past all of it (0x8001CFD0) and the
 * band is blue, so a single-player client need set none of them.
 */
void q2_menu_set_pauser(q2_menu *m, int player);
void q2_menu_set_player_colour(q2_menu *m, int player, int team, int colour);
void q2_menu_reset_identities(q2_menu *m);
void q2_menu_players_begin(q2_menu *m);
void q2_menu_advance_players(q2_menu *m, const u16 pad[4], u32 connected);
/* The page wrapper consumes -1 (back) and +1 (all ready). */
int q2_menu_players_step(q2_menu *m, const u16 pad[4], u32 connected);
bool q2_menu_players_two_teams(const q2_menu *m);
int q2_menu_player_rows(const q2_menu *m, int player, q2_menu_player_row out[7]);
void q2_menu_player_origin(const q2_menu *m, int player, int *x, int *y);
bool q2_menu_player_point_at(q2_menu *m, int index);
void q2_menu_set_mp_mode(q2_menu *m, int mode);

/*
 * Whether player `player`'s pad has rumble motors — the answer
 * IsPlayerPadShockable (0x80033250) gives for that pad. The CONTROLLER page
 * greys VIBRATION and skips it when this is false (0x8001C9D8). It is a
 * capability, not the setting: the setting is Q2_SET_VIBRATION.
 */
void q2_menu_set_pad_shockable(q2_menu *m, int player, bool on);

/* The banner's bar colour for the page now installed — Q2_MENU_BAR_*. */
int q2_menu_title_bar_colour(const q2_menu *m);

/* Serve QFRONT's tables where they differ from the executable's. See
 * q2_menu.front_end for why this is a flag here and nothing at all there. */
void q2_menu_set_front_end(q2_menu *m, bool on);

/*
 * Spell the menu's words the North American build's way. Two of them differ,
 * and both are strings in the executable rather than in the level data:
 * AUTOCENTRE is AUTOCENTER (0x800AB014 in SLES-01534 terms, and QFRONT's own
 * copy at module+0xD4 likewise), and the level table's COLOSSEUM, which the
 * arena list shows, is COLISEUM in SLUS-00757's record.
 */
void q2_menu_set_us_english(q2_menu *m, bool on);

/* A table's word as the build in hand spells it. */
const char *q2_menu_word(const char *word, bool us_english);
void q2_menu_set_controller_count(q2_menu *m, int count);
void q2_menu_set_cheat_level(q2_menu *m, int level);
void q2_menu_set_resupplies(q2_menu *m, int n);
void q2_menu_set_stats(q2_menu *m, int kills, int total_kills,
                       int secrets, int total_secrets);

/* Take the pending sound / request, clearing it. */
q2_menu_sound   q2_menu_take_sound(q2_menu *m);
q2_menu_request q2_menu_take_request(q2_menu *m);

/* The label to draw for an item: the runtime override when a hook has written
 * one, otherwise the table's. */
const char *q2_menu_item_text(const q2_menu *m, int index);

/*
 * The line the screen actually shows, colour codes included — what the
 * original composes into the object's text buffer every frame (0x8001B670).
 * A toggle becomes "b<label> bON dOFF", a choice becomes the current value's
 * name, everything else is the label. Returns `out`.
 */
const char *q2_menu_item_display(const q2_menu *m, int index,
                                 char *out, u32 out_size);

/* Navigable? Grey labels, disabled items and the static group are skipped —
 * the rule at 0x80019CC4. */
bool q2_menu_item_selectable(const q2_menu *m, int index);

/*
 * Is the cursor on one of the MULTIPLAYER page's three mode rows? That is the
 * one predicate module+0x4630 computes and then uses twice — to raise the
 * RULES prompt and to decide whether SQUARE opens the rules page — so the
 * prompt bar and the engine ask the same question here too.
 */
bool q2_menu_front_rules_row(const q2_menu *m);

/* The title's y, which the original derives from the framebuffer height:
 * (h - 188) / 2 + 10  (0x8001CF74). */
int  q2_menu_title_y(int screen_h);

/* The framebuffer the block is centred in (q2_menu.fb_h). */
void q2_menu_set_fb_height(q2_menu *m, int fb_h);

/*
 * Where row `index` of the current page is drawn, in block coordinates: the
 * table's y, except on the one table SLUS-00757 left where PAL had it while
 * it moved every other row up four — FET_LOADSAVE_CHOOSEFILE, the memory
 * card's SAVE FILE screen (0x8009B114). Centring the block would lift those
 * five rows too, so on a shorter framebuffer they are put back down.
 */
int  q2_menu_item_y(const q2_menu *m, int index);

#endif /* Q2PSX_MENU_H */
