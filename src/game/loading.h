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
 * THE LOGO IS NOT A MODEL. It is a sprite strip in `frontend.lbm`
 * ---------------------------------------------------------------------------
 * The turning logo in the corner is a PRE-RENDERED SPRITE STRIP, and it is in
 * the atlas this screen already writes its word with. Rows 144..203 of
 * `frontend.lbm` — under the 32-pixel face at rows 0..109 and the 16-pixel face
 * at 111..142, above the panel and prompt art at 213..255 — hold a **23-cell
 * rotation of the Quake II logo** on a 32 x 20 grid, eight cells across and
 * three rows down, with `RETRY` occupying the twenty-fourth slot.
 *
 * The cell WIDTHS are what prove both the grid and what the strip is. Read on
 * that grid they run
 *
 *     17 16 14 13 11 9 7 5 4 5 7 9 11 13 14 16 18 19 19 21 22 22 22
 *
 * — a smooth narrowing to a single minimum and out again, which is |cos| and
 * is a rotation about the vertical axis. Every cell is the full 20 rows tall,
 * because a thing turning that way does not change height.
 *
 * The grid was found by getting it wrong first. Read as two rows of 30 the same
 * sixty rows give widths 17 16 14 13 12 13 14 16 / 18 19 19 21 22 22 22, which
 * is lumpy, has its minimum in the wrong place, and makes a broadside cell 22
 * wide by 30 tall — an aspect of 0.47 once the frame buffer's 2:3 pixel is
 * accounted for, against the logo's own 1452:1997 = 0.73. On the 20-row grid a
 * broadside cell is 22 by 20, which is 0.71. The row-density profile agrees:
 * it dips to 18 lit texels at row 164 and to 8 at row 184, the two boundaries.
 *
 * That settles three things a model could not. It explains the COLOUR — the
 * strip is in the menu font's own palette, so the logo is the pale blue the
 * text is rather than the gold of the title screen's model or the mint green of
 * `q2logowire`'s line sheet. It explains how an in-level load can show a logo
 * AT ALL, since `frontend.lbm` is in every playable map's `SNDVRAM.DAT` and no
 * model of the logo is. And it explains the size: 32 x 20 drawn 1:1, like every
 * other thing cut from this sheet.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS RETRACTS, TWICE
 * ---------------------------------------------------------------------------
 * This file first argued that the screen draws `Q2LOGO` out of
 * `LEVELS/QDUMMY/`, on the strength of what that directory holds — level table
 * record 3, `Dummy`, with ONE model (`Q2LOGO`), ONE named image
 * (`FrontEnd.lbm`) and an 840-byte zone with no world in it. A retail capture
 * showed the logo HOLLOW, so the solid model was wrong; the next reading
 * reached for `q2logowire`, QFRONT's outlined twin of the same mesh, which is
 * the right shape, the wrong colour, and in the one directory an in-level load
 * cannot borrow from.
 *
 * Both were models because the title screen's logo is a model. The sheet was
 * open in front of this port the whole time — `frontend.lbm` is what the menu
 * font is cut from — and neither pass looked at it.
 *
 * QDUMMY is still where the asset comes from here, and now for a reason that
 * needs no inference: at 21,332 bytes its `SNDVRAM.DAT` is the smallest thing
 * on the disc carrying `FrontEnd.lbm`, which is the whole of what this screen
 * needs. Its lone `Q2LOGO` is not drawn. FORMATS §11.13 keeps the shape of that
 * directory as an observation rather than as an argument.
 *
 * ---------------------------------------------------------------------------
 * The turn, and the corner
 * ---------------------------------------------------------------------------
 * The strip's RATE is not on the disc — a cell index is a number some code
 * counts, and that code has not been found — so it is set here to the one rate
 * the disc does give for this logo, the front end's own. See
 * Q2_LOADING_CELL_UNITS; it is in the level clock's 1/300 s units so that it is
 * the same speed at every frame rate.
 *
 * WHERE it stands is measured off the capture, in the console's own 512 x 248
 * pixels, against the two text rows as a ruler: those are `STARTING` at
 * (256, 111) and `GAME` at (256, 137) from QFRONT's own records, so a distance
 * in the picture is a known number of frame-buffer pixels.
 */
#ifndef Q2PSX_LOADING_H
#define Q2PSX_LOADING_H

#include "disc.h"
#include "gpu.h"
#include "menu.h"
#include "menufont.h"
#include "q2psx.h"
#include "vram.h"

/*
 * Where the screen's one asset comes from.
 *
 * Any map carrying `frontend.lbm` would do — which is every playable one — but
 * this screen holds its bank open for the whole run, so the smallest carrier is
 * the right one to hold: QDUMMY's `SNDVRAM.DAT` is 21 KB against BASE0's 684.
 */
#define Q2_LOADING_MAP    "QDUMMY"

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
 * The logo strip in `frontend.lbm`, measured off the decoded sheet.
 *
 * Eight cells across on a 32-texel pitch and three rows down on a 20-texel one,
 * at v = 144; the twenty-fourth slot is the word `RETRY`, so the rotation is 23
 * cells long. Cell content is centred horizontally and fills the full height.
 */
#define Q2_LOADING_CELL_W     32
#define Q2_LOADING_CELL_H     20
#define Q2_LOADING_CELL_V    144
#define Q2_LOADING_CELL_COLS   8
#define Q2_LOADING_CELLS      23

/*
 * One cell every 22 units of the level clock.
 *
 * The atlas gives the frames and not their rate, and no code that counts a cell
 * index has been found — so this is the port's. It is not arbitrary, though:
 * the disc does state a rate for this logo, and it is the front end's own
 * `yaw -= 4 * dt` (Q2_LB_SCENE_SPIN, module+0x9D24), which turns the model
 * through 4096 in 1024 ticks. The strip's widths trace a single minimum, so its
 * 23 cells are one HALF turn, and half of 1024 over 23 cells is 22.3 ticks a
 * cell. Rounding to 22 makes the sprite turn at the rate the title screen turns
 * the model it was rendered from.
 */
#define Q2_LOADING_CELL_UNITS  22

/*
 * The top-left corner of the quad, in the console's 512 x 248 pixels.
 *
 * From the capture: the drawn logo spans x 465..488, centred on 476, and a
 * broadside cell's own content sits at texels 5..26 of its 32 — so the cell
 * starts at 460. Vertically it is centred on 50, and the cell is 20 tall.
 *
 * The capture measures 23 x 19 for that logo, against the 22 x 20 a broadside
 * cell drawn 1:1 gives. Both halves agreeing to a pixel is what says the quad
 * is 1:1, which is what every other thing cut from this sheet is.
 */
#define Q2_LOADING_X   460
#define Q2_LOADING_Y    40

/*
 * The ordering-table bucket the logo is linked into.
 *
 * Inside the overlay slice at OT[206..216] (screen.h) and BELOW the menu's own
 * OT[213], because compose walks the table in ascending order. The two do not
 * overlap on screen, so this says which is furniture and which is the page
 * rather than deciding who wins.
 */
#define Q2_LOADING_OT_BUCKET  (210 * PSX_OT_SUBDIV)

/*
 * WHICH SCREEN, because there are two and they are the same furniture.
 *
 * LOADING is the executable's, `0x800A3314`, put up by every transition. The
 * other is the front end's own and is the one the retail capture shows:
 * QFRONT's module+0x0EBF4 installs `STARTING` at (256, 111) and `GAME` at
 * (256, 137), the same two-row shape as RESTARTING / LEVEL, and it is what the
 * half second between a difficulty being confirmed and the opening reel
 * actually looks like. This port drew a blank front end for those fifteen
 * frames.
 *
 * The module carries a third, one row reading `DEMO OF GAME` at (256, 111)
 * (module+0x0EBC4), which belongs to the attract loop this port does not run.
 * It is not offered here because nothing would raise it.
 */
typedef enum q2_loading_page {
    Q2_LOADING_PAGE_LOADING = 0,   /* 0x800A3314, the executable's   */
    Q2_LOADING_PAGE_STARTING       /* QFRONT module+0x0EBF4          */
} q2_loading_page;

typedef struct q2_loading {
    /*
     * Its own VRAM image rather than the live one.
     *
     * The screen is up while a map's pages are being replaced, and a zone gate
     * inside one map does not re-upload them at all (client_load_zone) — so a
     * screen drawing out of the session's image would be drawing out of a bank
     * that is being taken away. A separate 1 MB image is what makes it
     * independent of whatever is loading behind it.
     */
    psx_vram       *vram;
    q2_menu_font    font;
    bool            font_ready;

    /* The page, through the same draw path every other page uses. A second
     * q2_menu rather than the session's, for the reason the memory-card front
     * end keeps one: it must not disturb where the player was. */
    q2_menu         menu;

    bool            ready;    /* the atlas is open                           */

    /* Live state. */
    bool            open;
    bool            timed;    /* it runs its own hold, rather than a caller's */
    double          hold;     /* 1/300 s units still to run, when `timed`    */
    double          spin;     /* 1/300 s units spent on the strip            */
} q2_loading;

/*
 * Open the screen's atlas. Q2_ERR_NOT_FOUND when the disc has no QDUMMY or no
 * `frontend.lbm` in it, which is a disc without a loading screen and not a
 * fault: the caller carries on with `ready` false and the screen is never
 * raised.
 *
 * `tab` supplies the palette bank the font upload needs; `settings` is the one
 * the session already owns, since a q2_menu holds a pointer to it.
 */
q2_result q2_loading_open(q2_loading *l, const disc *d,
                          const q2_hud_tables *tab,
                          q2_menu_settings *settings);
void      q2_loading_close(q2_loading *l);

/*
 * Raise LOADING and run the half-second hold, or restart that hold if it is
 * already up. Does nothing when the assets are not there.
 */
void q2_loading_raise(q2_loading *l);

/*
 * Put a page up with no clock of its own, for a caller that already has one —
 * the STARTING screen rides the opening reel's beat, and two countdowns for one
 * half second is exactly the pair that drifts apart.
 */
void q2_loading_show(q2_loading *l, q2_loading_page page);
void q2_loading_hide(q2_loading *l);

/*
 * Turn the logo, and spend the hold if this screen is running one.
 *
 * Returns true only while the screen OWNS the frame, which a raised one does
 * and a shown one does not: the caller that showed it is the one deciding what
 * else runs. Call it once per frame whatever is on screen.
 */
bool q2_loading_step(q2_loading *l, double dt);

/* Which cell of the strip is up, so the turn can be checked without a
 * framebuffer. */
u32 q2_loading_cell(const q2_loading *l);

/* Build the black screen's contents into `ot`: the logo, then the page over it.
 * `width` and `height` are the framebuffer's, so the 512 x 248 layout can be
 * centred in it. Returns the number of primitives emitted. */
u32 q2_loading_build_ot(const q2_loading *l, psx_ot *ot,
                        int width, int height);

#endif /* Q2PSX_LOADING_H */
