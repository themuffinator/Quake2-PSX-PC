/*
 * loading.h — the screen between two levels: black, LOADING, and the logo
 * turning in the top right.
 *
 * ---------------------------------------------------------------------------
 * There IS one, and it is a menu page — 0x80079178
 * ---------------------------------------------------------------------------
 * A level change on this disc does not go straight to the loader. The event
 * script's transition opcodes call `0x80079178` with the twelve-byte name of
 * the level they want, and that function is the loading screen:
 *
 *     8007917C  lw   v0, 0x800AEBCC     ; suppressed while this is set
 *     8007919C  bne  v0, zero, +0x54    ;   ...return 0, no screen
 *     800791E0  jal  0x8006DBC0         ; the name against 0x800E465C
 *     800791EC  bne  v0, zero, ...      ;   already there: return 0
 *     ...       the name -> 0x800E465C and 0x800E4674
 *     80079360  sw   0x8007901C, 0x800B2D90   ; the deferred LOAD
 *     80079364  jal  0x8001A384; li a0, 46    ; enter page 46
 *     80079374  jal  0x8001A474; li a1, 16    ; 0x800A3314, at size 16
 *     80079384  jal  0x8001A474; li a1, 16    ; 0x800A3344 — all zeros
 *     80079398  sh   1, 0x800C3638             ; drawable 0's +0x48: highlight
 *
 * `0x800A3314` is `{ "LOADING", 256, 124 }` and `0x800A3344` is a NULL record,
 * so the page is one line of text with nothing navigable under it — the same
 * shape as RESTARTING and QUITTING (pages.c). `0x800C35F0` is drawable 0 and
 * `0x800C3638` is its `+0x48`, which menufont.h records as the highlight flag:
 * at size 16 that selects palette 70, the bright one.
 *
 * The LOAD ITSELF IS DEFERRED. `0x8007901C` is installed as a one-shot hook and
 * clears its own slot at `0x8007916C` when it has run, so the frame that goes
 * out carries the screen and the load happens after it. That is the whole
 * mechanism: one frame with LOADING on it, then a synchronous read during which
 * nothing is drawn at all, so the console's loading screen is a still.
 *
 * `0x800AEBCC` is written by the level selector at `0x8007C7C8` / `0x8007C7E8`
 * from the level record's `+0x20`, which leveltable.h records as "always 1 on a
 * real level" — so the screen is armed for every level and disarmed only for a
 * record that is not one.
 *
 * ---------------------------------------------------------------------------
 * Where the logo comes from, and why QDUMMY is on the disc
 * ---------------------------------------------------------------------------
 * The text is the executable's. The picture is not: page 46 draws over whatever
 * scene is standing, and a screen that is meant to be up while a level is being
 * replaced cannot borrow that level's models.
 *
 * `LEVELS/QDUMMY/` is what it borrows instead, and its contents are the
 * argument. It is level table record 3, `Dummy`, and it holds:
 *
 *     COMMON.DAT   39,804 bytes — ONE model, `Q2LOGO`, and nothing else
 *     SNDVRAM.DAT  21,332 bytes — ONE named image, `FrontEnd.lbm`
 *     ZONE0.DAT       840 bytes — a header; there is no world in it
 *
 * `FrontEnd.lbm` is the menu font atlas (menufont.h) and `Q2LOGO` is the title
 * screen's logo. A directory carrying exactly the letterforms this screen
 * writes with and exactly the model it turns, over an empty zone, is a loading
 * screen and is not anything else — every other screen map on the disc carries
 * `chars.lbm`, a world, or both. Its own `LevelBin` agrees: `module+0x96C`
 * compares the level name against `"Dummy"` and installs `module+0x2E1C`, which
 * is four instructions long and asks for the next state once four frames have
 * gone by.
 *
 * So this module loads QDUMMY once, into its own VRAM image, and keeps it for
 * the life of the run. That is not how the console does it — the console reads
 * the directory every time, because reading a directory is the only way its
 * engine gets to a screen — but the pixels are the same and a port whose loads
 * are instant has no reason to spend one on the loading screen.
 *
 * ---------------------------------------------------------------------------
 * The turn
 * ---------------------------------------------------------------------------
 * `Q2_LB_SCENE_SPIN` — `yaw -= 4 * dt` with dt in the level clock's 1/300 s
 * units, which is the front end's own rate for the same model (levelbin.h,
 * module+0x9D24) and a third faster than a pickup's. A full turn takes 3.4 s.
 *
 * WHERE it stands is this port's. The console's own placement is in QDUMMY's
 * module, in code this port does not run, and nothing static in the module says
 * it — so the corner, the distance and the size are chosen here and marked as
 * chosen. Everything else on this screen is read.
 */
#ifndef Q2PSX_LOADING_H
#define Q2PSX_LOADING_H

#include "disc.h"
#include "gpu.h"
#include "gte.h"
#include "level.h"
#include "levelbin.h"
#include "menu.h"
#include "menufont.h"
#include "model.h"
#include "q2psx.h"
#include "vram.h"
#include "world.h"

/* The directory the screen's two assets come out of, and what they are. */
#define Q2_LOADING_MAP    "QDUMMY"
#define Q2_LOADING_MODEL  "Q2LOGO"

/*
 * How long the screen is held, in the level clock's 1/300 s units — the same
 * units `Q2_START_BEAT_UNITS` is in, and for the same reason: a frame count
 * would be a different duration at every frame rate.
 *
 * THE CONSOLE HAS NO SUCH NUMBER. Its screen lasts exactly as long as the disc
 * takes, which is one deferred frame plus the read. This port's read is
 * effectively free, so without a floor the screen would flash for a single
 * frame and be gone. 150 is half a second.
 */
#define Q2_LOADING_HOLD_UNITS  150

/*
 * The logo's place, and all four numbers are the port's — see the header.
 *
 * The distance and the projection are the front end's own (`engine+0x174(0,
 * 160, 4000)`, and Q2_LB_SCENE_DIST), so the logo is lit and framed by the same
 * arithmetic that puts it on the title screen; the scale takes it down to a
 * quarter and the offsets put that quarter in the top right of the console's
 * 512 x 248 screen. `Q2_LOADING_SCALE` is 1.0.12, so 1024 is a quarter.
 */
#define Q2_LOADING_PROJ    160
#define Q2_LOADING_DIST    1700
#define Q2_LOADING_SCALE   1024
#define Q2_LOADING_OFS_X   200   /* right of centre, in 512 x 248 pixels */
#define Q2_LOADING_OFS_Y   -68   /* above it; screen y runs downward     */

/*
 * Turning those two pixel offsets back into world coordinates, and the factor
 * of 3/2 is the reason this is written out rather than done by eye.
 *
 * At distance z with projection h, one world unit is h/z pixels — which is
 * exactly the arithmetic the front end's own note uses, where the logo sitting
 * 54 units below the origin is five pixels of error at z = 1700 and h = 160.
 * That holds on the VERTICAL only. The view basis is `q2_rotation_view_
 * anamorphic`, whose row 0 is scaled by 3/2 (0x80055DE4) because a PAL
 * framebuffer pixel is 2:3 and the horizontal has to be stretched to come out
 * square on a television. So a world x reaches the screen 1.5x wider than a
 * world y does, and undoing that is the 2/3 here.
 *
 * Getting it wrong is not subtle and is how this was found: 200 pixels' worth
 * of unscaled x put the logo at 556 on a 512-wide screen, entirely off it.
 */
#define Q2_LOADING_WORLD_X  (Q2_LOADING_OFS_X * Q2_LOADING_DIST * 2 / \
                             (Q2_LOADING_PROJ * 3))
#define Q2_LOADING_WORLD_Y  (Q2_LOADING_OFS_Y * Q2_LOADING_DIST / \
                             Q2_LOADING_PROJ)

/*
 * The ordering-table bucket the logo is linked into.
 *
 * Inside the overlay slice at OT[206..216] (screen.h) and BELOW the menu's own
 * OT[213], because compose walks the table in ascending order and the text has
 * to survive the model being drawn. A model is one thing in the table, not one
 * per face (modeldraw.h), so naming a bucket outright costs it no internal
 * sorting.
 */
#define Q2_LOADING_OT_BUCKET  (210 * PSX_OT_SUBDIV)

typedef struct q2_loading {
    /*
     * QDUMMY, held open for the life of the run. `common` owns the buffer and
     * `bank` and `logo` point into it, so this struct must be MOVED and never
     * assigned — the same rule q2_common_file carries (level.h).
     */
    q2_common_file  common;
    q2_model_bank   bank;
    q2_model        logo;
    bool            have_logo;

    /*
     * Its own VRAM image rather than the live one.
     *
     * The screen is up while a map's pages are being replaced, and a zone gate
     * inside one map does not re-upload them at all (client_load_zone) — so
     * uploading QDUMMY's bank over the session's would take the level's
     * textures away and not give them back. A separate 1 MB image is what makes
     * the screen independent of whatever is being loaded behind it.
     */
    psx_vram       *vram;
    q2_menu_font    font;
    u8              clut4_count_a;
    bool            font_ready;
    bool            textures;

    /* The page, through the same draw path every other page uses. A second
     * q2_menu rather than the session's, for the reason the memory-card front
     * end keeps one: it must not disturb where the player was. */
    q2_menu         menu;

    bool            ready;    /* the assets are open                       */

    /* Live state. */
    bool            open;
    double          hold;     /* 1/300 s units still to run                */
    s32             yaw;      /* 4096-step circle                          */
} q2_loading;

/*
 * Open the screen's assets. Q2_ERR_NOT_FOUND when the disc has no QDUMMY, which
 * is a disc without a loading screen and not a fault: the caller carries on
 * with `ready` false and the screen is simply never raised.
 *
 * `tab` supplies the palette bank the font upload needs; `settings` is the one
 * the session already owns, since a q2_menu holds a pointer to it.
 */
q2_result q2_loading_open(q2_loading *l, const disc *d,
                          const q2_hud_tables *tab,
                          q2_menu_settings *settings);
void      q2_loading_close(q2_loading *l);

/* Raise the screen, or restart its hold if it is already up. Does nothing when
 * the assets are not there. */
void q2_loading_raise(q2_loading *l);

/*
 * Spend `dt` seconds on the hold and turn the logo. Returns true while the
 * screen still owns the frame; the caller stops ticking the world for exactly
 * as long as that is true.
 */
bool q2_loading_step(q2_loading *l, double dt);

/* Build the black screen's contents into `ot`: the logo, then the page over it.
 * `width` and `height` are the framebuffer's, so the 512 x 248 layout can be
 * centred in it. Returns the number of primitives emitted. */
u32 q2_loading_build_ot(const q2_loading *l, psx_ot *ot, gte_state *gte,
                        int width, int height);

#endif /* Q2PSX_LOADING_H */
