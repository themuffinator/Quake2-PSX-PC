/*
 * screen.h — the screen: display environments, double buffering, viewports,
 * the ordering table's shape, and the order a frame is put together in.
 *
 * ---------------------------------------------------------------------------
 * What this module is
 * ---------------------------------------------------------------------------
 * Everything between "the game has decided what to draw" and "a field is on the
 * television". On the console that is four cooperating pieces:
 *
 *   - a DISPENV / DRAWENV pair per buffer, describing which rectangle of VRAM
 *     is being shown and which is being drawn into;
 *   - one ordering table, carved into fixed slices — one per viewport, plus a
 *     background slice and an overlay slice — so that split screen is expressed
 *     as *where in the OT* a primitive lands rather than as a second pass;
 *   - a set of viewport layouts, one per player count, each of which is a table
 *     of literal constants in the executable rather than anything computed;
 *   - a frame lock: DrawSync(0), VSync(2), PutDispEnv, DrawOTag, in that order.
 *
 * All of it was read out of `SLES_015.34` and is transcribed here with the
 * address it came from. `q2psx-inspect screen <disc>` reads the same values
 * back off a disc and compares them to this file, so a wrong constant here is
 * a test failure rather than a subtly wrong picture.
 *
 * ---------------------------------------------------------------------------
 * The one thing that is NOT modelled, and why
 * ---------------------------------------------------------------------------
 * On hardware the two framebuffers are rectangles *inside* VRAM — (0,0) and
 * (512,0), with the texture pages living from y = 256 down. This module keeps
 * two standalone `psx_framebuffer`s instead and records where each one would
 * have been (`q2_screen_buffer_origin`).
 *
 * THE FEEDBACK EFFECT THIS PARAGRAPH USED TO SAY MIGHT ONE DAY TURN UP HAS
 * TURNED UP. The water effect (`q2_screen_water`) copies strips of the drawn
 * frame around with `DR_MOVE` packets, so it reads the framebuffer back. It is
 * still not enough to force the VRAM model: every copy it makes has both
 * rectangles inside one viewport, so it never crosses between the two buffers
 * or touches a texture page. What it does need is that a copy's coordinates be
 * whatever the effect computed and not what a draw env would impose — see
 * PSX_PRIM_MOVE in gpu.h.
 *
 * Three consequences of that layout ARE modelled, because they are observable:
 * which rectangle the television is showing (`q2_screen_display_rect`, and the
 * single-buffered boot layout that follows from both envs naming the same one),
 * and the habit BOTH the overlay camera and the water effect have of assuming a
 * buffer origin instead of reading one — see env_from_overlay and
 * water_origin_x in screen.c.
 *
 * ---------------------------------------------------------------------------
 * What a frame is made of
 * ---------------------------------------------------------------------------
 * `q2_screen_build` is the whole of 0x800780C0: the clear decision, then each
 * live viewport (0x80076A74 — publish the viewport's state, install the GTE,
 * emit the damage flash, draw the world under its own gate, link the env), then
 * the overlay camera (0x80077230). The renderers are reached through
 * `q2_screen_hooks`, which is this port's form of the callback the layouts store
 * in the view record at +308.
 *
 * One thing in that sequence is NOT from 0x800780C0: the water effect
 * (0x80062DF0), which the console runs from the per-view game update instead.
 * It is here because it writes the shake that the same frame's draw envs read,
 * and because there is nothing else in the port for it to be part of. See
 * q2_screen_water.
 */
#ifndef Q2PSX_SCREEN_H
#define Q2PSX_SCREEN_H

#include "gpu.h"
#include "gte.h"
#include "ident.h"
#include "q2psx.h"
#include "raster.h"

/* ------------------------------------------------------------------------- */
/* The display state — 0x800764DC, the display-init function called from main  */
/* ------------------------------------------------------------------------- */
/*
 * Framebuffer dimensions are runtime state, not constants: exactly one writer
 * (this function) and 42 readers. `SetVideoMode(1)` four instructions earlier
 * is the only call site in the image, which is what makes the video standard a
 * property the executable states rather than one a port infers from a serial.
 *
 * NTSC values are NOT known and must not be guessed — PAL turned out to be 248
 * lines rather than the widely repeated 256, so the folklore 240 is less
 * trustworthy now, not more. Asking for an NTSC screen yields the PAL geometry
 * with `height_is_inferred` set, so a caller can tell.
 */
#define Q2_SCREEN_PAL_WIDTH   512   /* 0x800B2DA0, stored at 0x800764F0 */
#define Q2_SCREEN_PAL_HEIGHT  248   /* 0x800B2DA2, stored at 0x800764FC */

/*
 * The projection distance the bring-up installs before any layout exists:
 * `SetGeomScreen(gp+1608)` at 0x8007656C, and the halfword at 0x800AEC48 is
 * 0x00A0. It is the one-player layout's value, not the 320 the boot layout
 * overwrites it with a moment later — so a caller with no viewport of its own
 * (an offline render, a model viewer) has a console value to reach for instead
 * of inventing one from a buffer size.
 */
#define Q2_SCREEN_BOOT_PROJECTION 160

/*
 * VRAM origins of the two buffers, the {s16 x, s16 y} table at 0x800B2EF4.
 * Buffer 0 draws at x = 0 while buffer 1 is displayed, and vice versa — the
 * draw env takes its origin from this table indexed by the *draw* buffer, and
 * the display env of the *other* buffer is what reaches PutDispEnv.
 */
typedef struct q2_screen_buf_origin {
    s16 x, y;
} q2_screen_buf_origin;

typedef struct q2_screen_display {
    u16 width, height;          /* 0x800B2DA0 / 0x800B2DA2                    */
    u32 video_mode;             /* 0x800A9E70 — 1 == MODE_PAL                 */
    u16 field_hz;               /* 50 on PAL, from SetVideoMode(1)            */
    u16 vsync_divisor;          /* the literal 2 at 0x80018974                */

    q2_screen_buf_origin buf[2];/* 0x800B2EF4, stride 4                       */

    /*
     * The two DISPENVs, i.e. which VRAM rectangle each buffer's env shows.
     * `PutDispEnv` is handed the env of the buffer being *drawn* (`0x800185AC`
     * takes 0x800B2754), so for a frame not to tear, buffer i's env must show
     * the *other* buffer's rectangle. The boot setup at 0x80077454 does exactly
     * that — `SetDefDispEnv(db0+10940, 512, 0, ...)` against
     * `SetDefDispEnv(db1+10940, 0, 0, ...)` — and no session layout touches
     * them again; only 0x80077540 rewrites both, to (0,0), which is what makes
     * the boot layout single buffered.
     */
    q2_screen_buf_origin env[2];

    u8  draw_buffer;            /* 0x800B2738                                 */
    u8  disp_buffer;            /* 0x800B2739 — always 1 - draw_buffer        */
    u8  rotate[3];              /* 0x800B273C..0x800B273E, see q2_screen_swap */

    u8  bg_rgb[3];              /* gp+1604 (0x800AEC44) — the clear colour    */
    u8  bg_enable;              /* gp+18732 (0x800B2F2C) — the clear's isbg   */

    bool height_is_inferred;    /* true when asked for a build we cannot read */
} q2_screen_display;

/* ------------------------------------------------------------------------- */
/* The double-buffer block — 0x800B3730, stride 32408                          */
/* ------------------------------------------------------------------------- */
/*
 * Offsets into one buffer's block. A port does not need to lay memory out this
 * way, but the numbers are how every "db + N" in the disassembly reads, and the
 * OT offsets in particular are load-bearing: the slice arithmetic below is what
 * makes split screen work.
 */
#define Q2_SCREEN_DB_BASE          0x800B3730u
#define Q2_SCREEN_DB_STRIDE            32408u
#define Q2_SCREEN_DRAWENV_SIZE            92u
#define Q2_SCREEN_DISPENV_SIZE            20u
#define Q2_SCREEN_DB_DRAWENV_BG          436u  /* the full-screen background   */
#define Q2_SCREEN_DB_DRAWENV_VIEW        528u  /* + 92*p, four of them         */
#define Q2_SCREEN_DB_DRAWENV_OVERLAY     896u  /* 528 + 4*92                   */
#define Q2_SCREEN_DB_DISPENV           10940u
#define Q2_SCREEN_DB_OT                10984u
/*
 * Two more blocks of `TILE` packets live in the same 32408 bytes, and both are
 * part of drawing a viewport rather than of drawing the world:
 *
 *   10876   4 x 16   one damage-flash tile per viewport (0x8007677C: the index
 *                    is shifted left by 4 and added to 10876, so the four end
 *                    exactly where the display env begins)
 *   21236   9 x 16   the performance meter's bars (0x80076EB0 onward)
 */
#define Q2_SCREEN_DB_FLASH_TILES       10876u
#define Q2_SCREEN_DB_METER_TILES       21236u

/* ------------------------------------------------------------------------- */
/* The ordering table                                                          */
/* ------------------------------------------------------------------------- */
/*
 * `ClearOTag(db + 10984, 217)` at 0x80018398 — forward order, so bucket 0 is
 * drawn first and higher buckets land on top of it. The 217 is not arbitrary;
 * it is exactly what the slices below add up to, which is the strongest
 * evidence that the slicing is real:
 *
 *      1   root                                   OT[0]
 *      1   the full-screen background env         OT[1]
 *    204   four viewport slices of 51             OT[2 .. 205]
 *     11   the overlay slice                      OT[206 .. 216]
 *      = 217
 *
 * The 51 is confirmed independently: the boot display path clears exactly 51
 * entries at db + 10992 (0x8006E188 / 0x8006E194).
 *
 * Within a slice the draw env sits at slice bucket 1, not 0 — the env packet is
 * AddPrim'd last (0x80076D44) so it draws first inside its bucket, and slice
 * bucket 0 is left for whatever wants to precede the clip change.
 *
 * The slice's LAST bucket, 50, is the front of that viewport: the damage flash
 * links there (`AddPrim(slice + 200)` at 0x80076968) and the performance meter
 * links at the absolute equivalent for viewport 0 (`AddPrim(db + 11192)` at
 * 0x80076F14, and 11192 == 10984 + 4*52). Both are drawn over the world because
 * the table runs forward — see the direction argument in gpu.h.
 */
#define Q2_SCREEN_OT_ENTRIES        217
#define Q2_SCREEN_OT_BACKGROUND       1
#define Q2_SCREEN_OT_VIEW_BASE        2
#define Q2_SCREEN_OT_VIEW_STRIDE     51
#define Q2_SCREEN_OT_VIEW_ENV         1   /* env bucket within a slice        */
#define Q2_SCREEN_OT_VIEW_FLASH      50   /* the slice's frontmost bucket     */
#define Q2_SCREEN_OT_OVERLAY        206
#define Q2_SCREEN_OT_OVERLAY_LEN     11
#define Q2_SCREEN_MAX_VIEWS           4

/*
 * One bucket below the flash, and the only other structural bucket in a slice:
 * the water effect links its strip copies and its tint here (0x80062E64 builds
 * `db + 11180 + 204*p`, and 11180 == 10984 + 4*49). Global OT entry 49 is
 * slice entry 47 because the first viewport slice begins at global entry 2.
 * So a viewport's slice ends with the world, then the water, then the flash.
 */
#define Q2_SCREEN_OT_VIEW_WATER      47

/* The absolute bucket the performance meter links into: viewport 0's front. */
#define Q2_SCREEN_OT_METER \
    (Q2_SCREEN_OT_VIEW_BASE + Q2_SCREEN_OT_VIEW_FLASH)

/* ------------------------------------------------------------------------- */
/* A viewport                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * The fields the screen owns inside the 784-byte per-player view record at
 * 0x800D5C30. Offsets are kept in the comments because they are how the layout
 * functions and the per-frame draw at 0x80076A74 talk to each other.
 */
/*
 * The damage flash — view+672…+680, drawn by 0x80076764 as part of the viewport
 * it belongs to, not by the overlay. It is one full-viewport `TILE` with the
 * semi-transparency bit set (`SetSemiTrans` at 0x800767A0), linked at the
 * slice's frontmost bucket, and `strength` counts down one per DRAWN frame
 * (0x80076988) rather than per logic tick.
 *
 * There are four modes, and only one of them is a plain fade. All four are
 * transcribed because the game selects mode 2 for damage but the field is a
 * halfword the rest of the engine can set:
 *
 *   0  the colour is left at whatever the tile held last frame
 *   1  rgb = b - (rgb * strength/initial)     (0x80076830 — and yes, all three
 *      channels subtract from the BLUE component; it is what the code does)
 *   2  rgb = rgb * strength/initial           (0x800768A4 — the damage fade)
 *   3  rgb = rgb                              (0x80076904 — no fade at all)
 */
typedef enum q2_screen_flash_mode {
    Q2_SCREEN_FLASH_KEEP  = 0,
    Q2_SCREEN_FLASH_SHIFT = 1,
    Q2_SCREEN_FLASH_FADE  = 2,
    Q2_SCREEN_FLASH_SOLID = 3
} q2_screen_flash_mode;

typedef struct q2_screen_flash {
    u8  rgb[3];      /* +672..674                                          */
    s16 strength;    /* +676, and the whole thing is inert when it is zero */
    s16 initial;     /* +678, what strength is scaled against              */
    s16 mode;        /* +680                                               */
    u8  out[3];      /* the tile's current colour, so mode 0 can keep it   */
} q2_screen_flash;

/* ------------------------------------------------------------------------- */
/* The water effect — 0x80062DF0, and it is what the screen shake is FOR      */
/* ------------------------------------------------------------------------- */
/*
 * Being underwater does three things to a viewport, and they are one function:
 * the picture wobbles, the viewport is inset by a pixel or three, and a dark
 * blue tile is laid over it. The port had none of them, and the shake in
 * particular had been sitting in the view record with no writer — this is its
 * writer.
 *
 * It is not guesswork about what an underwater screen ought to look like. The
 * pool the wobble draws out of is described in the executable's own allocator
 * table as **"Water Moves"** (`0x800ACF7C`), and the bit that turns the whole
 * thing on is `0x100` of the owning entity's flag word — the same
 * `Q2_ENT_UNDERWATER` the player's movement reads.
 *
 * ---------------------------------------------------------------------------
 * The wobble is a framebuffer copy, not geometry
 * ---------------------------------------------------------------------------
 * This is the effect screen.h's header comment predicted: "if a screen wipe or
 * a feedback effect ever turns up, this is the seam it will need". The wobble
 * is a run of `DR_MOVE` packets — VRAM-to-VRAM rectangle copies, PSX_PRIM_MOVE
 * in gpu.h — that displace strips of the frame that has just been drawn. Two
 * passes over the viewport:
 *
 *   COLUMNS  walk x from 0 to w inclusive, stepping the phase by 20 per column;
 *            each strip is copied UP by its own offset.
 *   ROWS     walk y from 0 to h inclusive, stepping the phase by 13 per row;
 *            each strip is copied LEFT by its own offset.
 *
 * Both are run-length encoded: a copy is emitted only where the offset CHANGES,
 * which is what keeps a 512-pixel-wide wobble inside a 24-packet pool.
 *
 * ---------------------------------------------------------------------------
 * Why the offsets are never negative, and why the viewport shrinks
 * ---------------------------------------------------------------------------
 * An offset is `(cos(phase) * amplitude) >> 24` plus the shake, and the shake is
 * that same amplitude divided by 4096 — i.e. exactly the largest value the
 * cosine term can reach. So the sum runs from 0 to twice the shake and never
 * goes negative, and every copy therefore reads from INSIDE the viewport.
 *
 * That is what the shake is for. `env_from_view` insets the draw rectangle by
 * the shake and shrinks it by the same amount, so the frame is drawn into a
 * slightly smaller box with a margin around it, and the wobble has somewhere to
 * pull from. Read on its own the inset looks like a screen shake that forgot to
 * shake; read with this, it is the margin.
 */
typedef struct q2_screen_water {
    /*
     * +776. Ramps to 4096 at 24 per dt unit while submerged and back to 0 at
     * the same rate when not, so surfacing fades the effect out rather than
     * cutting it. Everything else here is derived from it.
     */
    s16  amp;

    /*
     * +772 and +774, both masked to the 4096-step circle when read. They
     * advance at DIFFERENT rates — 5 and 7 per dt unit — which is what stops
     * the two passes from beating together into a visible diagonal.
     *
     * Both are reset to zero the moment the amplitude reaches zero, so leaving
     * the water and going back in starts the wave from the same phase.
     */
    u16  phase_row;   /* +772, the ROW pass    */
    u16  phase_col;   /* +774, the COLUMN pass */

    /*
     * view+288 and the one bit of it this reads. A viewport with no owner is
     * left entirely alone — not faded out, not reset — because the original
     * returns before the ramp when the pointer is null.
     */
    bool owned;
    bool submerged;
} q2_screen_water;

/* The ceiling the ramp stops at, and the step per dt unit (0x80062EA4). */
#define Q2_SCREEN_WATER_AMP_MAX    4096
#define Q2_SCREEN_WATER_AMP_RATE     24

/* Phase steps per dt unit (0x80062FCC, 0x80062FE0) and per strip
 * (0x800630FC, 0x80063220). */
#define Q2_SCREEN_WATER_ROW_RATE      5
#define Q2_SCREEN_WATER_COL_RATE      7
#define Q2_SCREEN_WATER_COL_STEP     20
#define Q2_SCREEN_WATER_ROW_STEP     13

/*
 * The tint at full amplitude: `amp*15/4096`, `0`, `amp*55/4096` — the three
 * shift-and-subtract chains at 0x80063290 and 0x800632B8. Blue with a little
 * red in it and no green at all, drawn semi-transparent over the whole
 * viewport.
 */
#define Q2_SCREEN_WATER_TINT_R       15
#define Q2_SCREEN_WATER_TINT_G        0
#define Q2_SCREEN_WATER_TINT_B       55

/*
 * How many copies a frame may spend — 0x80062CF8, per buffer and refilled every
 * frame. It is a hard cap and it is visible: when it runs out, BOTH passes stop
 * where they are and the tint is still drawn, so a wobble that overruns loses
 * its tail rather than degrading evenly. Twenty-four is about what a full-screen
 * viewport needs, which is the strongest sign the number was chosen for this.
 */
#define Q2_SCREEN_WATER_MOVES        24
#define Q2_SCREEN_WATER_MOVES_MP     48

/* view+144 bit 0 gates the world draw (`andi 1` at 0x80076CF0). A view becoming
 * live sets bits 0-2 together (`ori 7` at 0x800380AC and 0x8003848C); only bit 0
 * is read by anything the screen owns. */
#define Q2_SCREEN_VIEW_DRAW_WORLD 0x0001u
#define Q2_SCREEN_VIEW_LIVE       0x0007u

typedef struct q2_screen_view {
    s16 x, y;            /* +270 / +272  origin inside the framebuffer       */
    s16 w, h;            /* +274 / +276  size                                */
    s16 vw, vh;          /* +278 / +280  the 2D extent; differs on 2P-H      */
    s16 ofs_x, ofs_y;    /* +266 / +268  GTE SetGeomOffset, always (w/2,h/2) */
    s16 proj;            /* +262         GTE SetGeomScreen — the FOV control */
    s16 far_z;           /* +264         parked in 0x800B2CCC, /4 in ..2CC8  */
    s16 aspect;          /* +282         (w << 9) / fb_w or fb_h, see below  */
    /*
     * +156 — 8192 full screen, 6144 in every split. NOT a depth scale, which is
     * what an earlier pass assumed from the values alone: its only reader is the
     * per-view update at 0x80038374, which multiplies it by an elapsed-tick
     * count, shifts down 12, and adds the result into a running three-halfword
     * total alongside two other amplitude groups (view+154 and view+160…164,
     * each with its own timer and its own decay divisor). The total is written
     * back through the object at view+0 and handed to the caller as three
     * halfwords. So this is one of three timed contributions to the view's own
     * wobble, in 1.3.12.
     *
     * AND IT IS NOT THE SCREEN'S. The wobble is ANGULAR and it moves the VIEW
     * WEAPON, not the camera: 0x80038260 has exactly one caller, 0x8004F404,
     * which sums its three halfwords with the weapon's own angle triple, negates
     * x and hands the result to `RotMatrix` (src/game/viewweapon.c). So the
     * screen neither applies it nor should — a viewport carries the amplitude
     * because the amplitude is per-viewport, and the weapon reads it from there.
     *
     * That also settles what this field is for, which the values alone could not:
     * 8192 full screen and 6144 in every split is the weapon swaying less when
     * the viewport is smaller.
     */
    s16 kick_scale;
    /*
     * +304 / +306 — the STATUS BAR'S ANCHOR, not padding.
     *
     * Recorded here as `pad_a`/`pad_b` while the project still believed there
     * was no status bar. There is (FORMATS.md §11.1), and these two halfwords
     * are its origin: every one of its thirteen fields is a literal offset from
     * this point (statusbar.h), so a layout places the whole bar by writing two
     * numbers into the view record and the fields follow.
     *
     * That is also why the bar is per viewport and shrinks in split screen — it
     * is drawn by the viewport's own draw hook at view+308, from the same call
     * that draws that viewport's world.
     */
    s16 sbar_x, sbar_y;  /* +304 / +306                                      */

    u8  bg_rgb[3];       /* +256..258    the viewport's own clear colour     */
    u8  bg_enable;       /* +260         isbg; cleared when the full-screen
                          *              background env is doing the clear   */

    u16 flags;           /* +144         bit 0 draws the world              */
    /*
     * +780 / +782. NOT an input: the water effect is the only writer in the
     * image (0x80062F90), and it derives both from its own amplitude — see
     * q2_screen_water for why the inset is a margin rather than a shake. A
     * viewport with no owner is never visited by that update, so setting these
     * by hand does work on one; on an owned viewport it lasts until the next
     * frame.
     */
    s16 shake_x, shake_y;

    q2_screen_flash flash;
    q2_screen_water water;
} q2_screen_view;

/* ------------------------------------------------------------------------- */
/* What a viewport publishes while it is being drawn                          */
/* ------------------------------------------------------------------------- */
/*
 * The per-viewport draw does not pass anything to the world renderer: it parks
 * the viewport's state in globals and calls it. Those globals are what the
 * renderers actually consume — the clip extent at 0x800B2C20/0x800B2C22 bounds
 * every polygon (read at 0x800657B8, 0x800659FC, 0x80065B64, 0x8006BFD4 and by
 * the assembly emitter at 0x800AF820), the slice pointer at 0x800B2D60 is where
 * primitives link, and 0x800B2CC8 is far/4.
 *
 * **0x800B2CC8 is NOT "the distance past which an entity is not drawn", which
 * this note used to call it, and the title screen is the counter-example.**
 * QFRONT's front end installs far = 4000 (`engine+0x174(0, 160, 4000)`, its
 * `init`'s second act), so far/4 is 1000 — and the Q2LOGO it draws stands at
 * z = 1700 and is the only thing on the screen. A cut-off read that way would
 * blank the retail title screen. Whatever the field gates, it is not a plain
 * compare against an entity's z, so nothing here applies it and the port does
 * not cull on it. See openquestions #44.
 *
 * Keeping them in one struct rather than as loose globals is the only liberty
 * taken; the contents and the moment each is written are the original's.
 */
typedef struct q2_screen_ctx {
    const q2_screen_view *view;   /* 0x800B2C24 / 0x800B28B4                 */
    int  index;                   /* 0x800B2C1C — the viewport number        */
    s16  clip_w, clip_h;          /* 0x800B2C20 / 0x800B2C22, after the shake */
    s16  shake_x, shake_y;        /* 0x800B2C34 / 0x800B2C36                 */
    s32  far_z;                   /* 0x800B2CCC                              */
    s32  far_z_entities;          /* 0x800B2CC8 — far/4, the entity cut-off  */
    u32  slice_base;              /* 0x800B2D60, as a bucket index           */
    u32  slice_len;
    bool overlay;                 /* true while the overlay camera is up     */
} q2_screen_ctx;

/* ------------------------------------------------------------------------- */
/* Layouts                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * One per setup function. Which one runs is chosen by the session mode at
 * 0x800B3356 through the jump table at 0x800AC90C (0x8003F8D8):
 *
 *   mode 0, 1 -> ONE          (0x80077D0C)
 *   mode 2    -> TWO_H or TWO_V, on HORIZONTAL SPLIT at 0x800B333C
 *   mode 3    -> QUAD with the view count forced to 3 (0x8003FAE4)
 *   mode 4    -> QUAD         (0x8007771C)
 *
 * FULL_SINGLE (0x80077540) is not a session layout: it is the boot-time
 * full-screen setup, and it is the only one that leaves both display envs
 * pointing at VRAM (0,0), i.e. single-buffered.
 */
typedef enum q2_screen_layout {
    Q2_SCREEN_LAYOUT_ONE = 0,     /* 0x80077D0C — 512x248, double buffered   */
    Q2_SCREEN_LAYOUT_TWO_H,       /* 0x80077900 — stacked, HORIZONTAL SPLIT  */
    Q2_SCREEN_LAYOUT_TWO_V,       /* 0x80077AEC — side by side               */
    Q2_SCREEN_LAYOUT_QUAD,        /* 0x8007771C — 2x2, used for 3 and 4      */
    Q2_SCREEN_LAYOUT_FULL_SINGLE, /* 0x80077540 — boot, single buffered      */
    Q2_SCREEN_LAYOUT_COUNT
} q2_screen_layout;

const char *q2_screen_layout_name(q2_screen_layout l);

/* ------------------------------------------------------------------------- */
/* Why the game loop stopped — 0x800B2E28, dispatched at 0x8001860C            */
/* ------------------------------------------------------------------------- */
/*
 * The in-game loop at 0x800182C8 runs until this halfword goes non-zero, then
 * returns it. Everything that leaves the world — dying, restarting, the mission
 * screen, quitting — is expressed as one of these, which is why the screen owns
 * it: it is the thing that decides what is on screen next.
 *
 * The names come from what each dispatch arm calls. Values with no arm of their
 * own (3, 4, 9) are returned to the outer state machine at 0x80018A10 and are
 * NOT identified here rather than guessed at.
 *
 * ---------------------------------------------------------------------------
 * 2 IS THE LEVEL CHANGE, and it is the one single player runs on
 * ---------------------------------------------------------------------------
 * This note used to list 2 among the unidentified. It is `LOADMAP`: the
 * primitive's exec at `0x8002DCE0` compares the item's 12-byte map name at
 * `+4` against the current map at `0x800E46B4`, and when they differ copies it
 * to `0x800E46C0`, copies the start-position name at `+16` to `0x800C8CD0`,
 * and writes **2** at `0x8002DD80`. A name equal to the map already loaded
 * falls straight out, which is why several maps carry a LOADMAP naming
 * themselves.
 *
 * There is no dispatch arm because it needs none. The in-game loop returns,
 * and the outer state machine's tail at `0x80018BB8`..`0x80018C70` — reached
 * whenever none of the four request flags is standing — reads the twelve bytes
 * at `0x800E46C0` back out and calls the level selector `0x8007C54C` with
 * them. **The transition IS the loop going round again.** Three globals are
 * the whole of the interface:
 *
 *     0x800E46B4   the map now loaded, 12 bytes
 *     0x800E46C0   the map to load next, 12 bytes — a level table DISPLAY name
 *     0x800C8CD0   the start position to arrive at, 12 bytes
 *
 * 7 is the other exit a single-player session takes, and it is `MISCOMPLETE`
 * rather than anything to do with a level: `0x8002DC68` writes "Default" into
 * the start-position buffer and 7 into the state word. Its arm `0x80018ED8`
 * holds the level-tally board (mission.h) until a press, and then writes the
 * destination itself — `"EndMission "` with the unit digit added into the
 * twelfth byte at `0x80019028`, or `"Extro FMV"` when the unit is 5 — and
 * drops to 2, which is how a unit end becomes an ordinary level change.
 *
 * `0x800B271C` (gp+16668) carries the exit code ACROSS the transition: the
 * outer loop stores it at `0x80018C84` just before re-entering the level, and
 * `0x8003E730` is the only reader.
 */
typedef enum q2_screen_exit {
    Q2_SCREEN_EXIT_NONE       = 0,   /* keep running                          */
    Q2_SCREEN_EXIT_LEVEL_DONE = 1,   /* 0x80041548 when the session is mode 2 */
    Q2_SCREEN_EXIT_CHANGE_MAP = 2,   /* LOADMAP, 0x8002DD80 - see above       */
    Q2_SCREEN_EXIT_5          = 5,   /* 0x800411C8                            */
    Q2_SCREEN_EXIT_FRONTEND   = 6,   /* 0x8007CCE0(0)                         */
    Q2_SCREEN_EXIT_MISCOMPLETE = 7,  /* MISCOMPLETE, 0x8002DCB4 -> 0x80018ED8 */
    Q2_SCREEN_EXIT_8          = 8,   /* 0x8004149C                            */
    Q2_SCREEN_EXIT_10         = 10,  /* 0x80041300; also forced at boot       */
    Q2_SCREEN_EXIT_11         = 11,  /* 0x800415F4                            */
    Q2_SCREEN_EXIT_FLAG_12    = 12,  /* sets 0x800B2A54                       */
    Q2_SCREEN_EXIT_FLAG_13    = 13,  /* sets 0x800B2A50; a unit-5 MISCOMPLETE
                                      * falls into this arm at 0x80018F78     */
    Q2_SCREEN_EXIT_FLAG_14    = 14,  /* sets 0x800B2A5C                       */
    Q2_SCREEN_EXIT_FLAG_15    = 15,  /* sets 0x800B2A88                       */
    Q2_SCREEN_EXIT_16         = 16,  /* 0x80041974                            */
    Q2_SCREEN_EXIT_FRONTEND_1 = 17,  /* 0x8007CCE0(1), then becomes 6         */
    Q2_SCREEN_EXIT_FRONTEND_2 = 18,  /* 0x8007CCE0(2), then becomes 6         */
    Q2_SCREEN_EXIT_19         = 19,  /* 0x80041958                            */
    Q2_SCREEN_EXIT_20         = 20,  /* 0x80040608(1)                         */
    Q2_SCREEN_EXIT_21         = 21   /* 0x80040608(0)                         */
} q2_screen_exit;

/* ------------------------------------------------------------------------- */
/* Timing — 0x800182C8                                                         */
/* ------------------------------------------------------------------------- */
/*
 * `dt` is in 1/300 s units and is clamped, not averaged: `slti 31` at
 * 0x800184B8 with 30 in the delay slot. VSync(2) at 50 Hz is 12 units, so the
 * clamp bites at two and a half dropped fields.
 */
#define Q2_SCREEN_DT_NOMINAL   12
#define Q2_SCREEN_DT_MAX       30

/* ------------------------------------------------------------------------- */
/* The performance meter — 0x80076E88, gated on 0x800B2A64                     */
/* ------------------------------------------------------------------------- */
/*
 * Nine `TILE` bars two pixels wide, linked into viewport 0's frontmost bucket.
 * Each is a duration in `GetRCnt(RCntCNT1)` units halved to give its height,
 * and six of the nine accumulators are zeroed once the bars are built, so a bar
 * shows one frame's worth and not a running total.
 *
 * The port cannot invent the durations, so the bars read an array a caller
 * fills with whatever it measured; what is transcribed here is the meter's
 * shape — which bar is which colour, where it sits, and which ones reset.
 */
#define Q2_SCREEN_METER_BARS   9
#define Q2_SCREEN_METER_BASE_Y 20   /* gp+1612 (0x800AEC4C)                  */
#define Q2_SCREEN_METER_WIDTH   2   /* the literal at 0x80076ED0             */

typedef struct q2_screen_meter {
    bool enable;                        /* 0x800B2A64                        */
    s16  base_y;                        /* gp+1612                           */
    s32  value[Q2_SCREEN_METER_BARS];   /* the accumulators, in ticks        */
} q2_screen_meter;

/* Bar i's column, colour and whether building the meter clears it. */
extern const s16 q2_screen_meter_x[Q2_SCREEN_METER_BARS];
extern const u8  q2_screen_meter_rgb[Q2_SCREEN_METER_BARS][3];

/* ------------------------------------------------------------------------- */
/* The screen                                                                  */
/* ------------------------------------------------------------------------- */
typedef struct q2_screen {
    q2_screen_display disp;

    q2_screen_view    view[Q2_SCREEN_MAX_VIEWS];
    int               view_count;      /* 0x800B2C2C                         */
    q2_screen_layout  layout;

    q2_screen_view    overlay;         /* the view at 0x800D6870             */
    bool              overlay_armed;   /* 0x80077230 ran this frame          */
    bool              background_armed;/* 0x800769A0 ran this frame          */

    /* Which viewports linked their draw env this frame — the port's stand-in
     * for the packet itself being in the table (0x80076D44). A viewport that
     * was never begun leaves no env, and its rectangle is never clipped to. */
    bool              env_linked[Q2_SCREEN_MAX_VIEWS];

    /*
     * What 0x800780C0 decides before anything is drawn. `suppress_clear` is
     * 0x800B2D94, which is set by the host-filesystem boot path at 0x8006E144
     * and cleared at 0x80018C9C and 0x8007C92C; `background_enable` is
     * gp+18712 (0x800B2F18), written around level load at 0x800704B4 /
     * 0x800704C4 and read nowhere else. Between them they pick one of three
     * behaviours — see q2_screen_build.
     */
    bool              suppress_clear;
    bool              background_enable;

    q2_screen_ctx     ctx;             /* the viewport currently being drawn */
    q2_screen_meter   meter;

    /*
     * The "Water Moves" pool, as a per-frame allowance rather than as two
     * arrays of pre-filled packets: `q2_screen_frame_begin` refills it, every
     * strip copy spends one, and running out stops both passes of every
     * viewport still to be drawn. Set it to Q2_SCREEN_WATER_MOVES_MP for a
     * multiplayer session, which is the only thing 0x800AEBCC changes here.
     */
    u32               water_moves;
    u32               water_moves_used;

    /* The two buffers. `buf[disp.draw_buffer]` is the one being drawn. */
    psx_framebuffer   buf[2];

    q2_screen_exit    exit_code;       /* 0x800B2E28                         */
    s32               dt;              /* 0x800B2DB4, clamped                */
    u32               frame;
} q2_screen;

/* ------------------------------------------------------------------------- */
/* The two things a frame calls out to                                         */
/* ------------------------------------------------------------------------- */
/*
 * `view` stands in for 0x80066858, the world draw the per-viewport function
 * calls once it has installed the viewport's state — and it is called under the
 * same gate, `view->flags & Q2_SCREEN_VIEW_DRAW_WORLD`. `overlay` stands in for
 * 0x8006AB8C, which the overlay camera calls with its own env already in force.
 *
 * The original expresses the first of these as a function pointer the layout
 * stores in the view record at +308 (`sw` at 0x80077A94 and friends, from the
 * layout's own argument: 0x800337D0 for one player, 0x80033D30 and 0x80034288
 * for the two splits). This is that pointer.
 */
typedef struct q2_screen_hooks {
    void (*view)(void *user, q2_screen *s, int p, psx_ot *ot, gte_state *gte);
    void (*overlay)(void *user, q2_screen *s, psx_ot *ot, gte_state *gte);
    void *user;
} q2_screen_hooks;

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Reproduces 0x800764DC: video mode, framebuffer size, buffer indices, the
 * three-slot rotation, the display envs from 0x80077454, and the boot-time
 * full-screen single-buffered view from 0x80077540 — which is the state the
 * front end runs in before a session picks a layout.
 *
 * Allocates the two framebuffers, so it can fail.
 */
q2_result q2_screen_init(q2_screen *s, q2_video_std video);
void      q2_screen_free(q2_screen *s);

/* Install one of the layouts. `players` is honoured only for QUAD, where the
 * session code sets 3 after the layout has written 4 (0x8003FAE4). */
void q2_screen_set_layout(q2_screen *s, q2_screen_layout layout, int players);

/* Pick the layout the session mode would pick. `horizontal_split` is the menu
 * setting at 0x800B333C and is consulted only for two players. */
q2_screen_layout q2_screen_layout_for(int players, bool horizontal_split);

/* Where in VRAM buffer `index` lives — the model this module does not itself
 * enforce. See the header comment. */
void q2_screen_buffer_origin(const q2_screen *s, int index, int *x, int *y);

/*
 * The rectangle of VRAM the television is showing: the display env of the
 * buffer being drawn, which the cross pairing makes the *other* buffer's
 * rectangle. When the two come out equal the layout is single buffered and the
 * frame is visible as it is drawn — which is only the boot layout.
 */
void q2_screen_display_rect(const q2_screen *s, int *x, int *y, int *w, int *h);
bool q2_screen_single_buffered(const q2_screen *s);

/* ------------------------------------------------------------------------- */
/* The shape of a pixel, and therefore of the field of view                   */
/* ------------------------------------------------------------------------- */
/*
 * NONE OF THIS IS IN THE EXECUTABLE. It is what the television does with what
 * the executable emits, and it is here because the alternative — putting the
 * framebuffer on a modern display one buffer pixel to one window pixel — is a
 * 1.5x horizontal stretch, and a stretched picture makes a correctly
 * reconstructed field of view read as a wrong one.
 *
 * The hardware fact it rests on: the GPU's five horizontal modes (256, 320,
 * 368, 512, 640) all span the SAME 2560 GPU cycles of active line. A 512-wide
 * frame is therefore not a wider picture than a 320-wide one — it is the same
 * picture with pixels half as wide. Vertically the standard non-interlaced
 * picture is 256 lines on PAL and 240 on NTSC, and that is what fills the 4:3
 * raster.
 *
 * So one pixel is `4 * lines / (3 * width)` as wide as it is tall:
 *
 *      PAL   512 wide, 256-line picture ->  4*256 / (3*512) = 2/3 exactly
 *      NTSC  512 wide, 240-line picture ->  4*240 / (3*512) = 5/8
 *
 * This game draws 248 of PAL's 256 lines, so its picture is very slightly
 * shorter than the full 4:3 frame: 512 * 2/3 by 248, i.e. 1.376:1. The eight
 * missing lines are a border on the television, not a stretch.
 *
 * ---------------------------------------------------------------------------
 * What that makes the field of view — and the squeeze IS the console's
 * ---------------------------------------------------------------------------
 * The GTE projects with ONE distance for both axes (`H` reaches SX and SY
 * alike), so the frustum is symmetric in FRAMEBUFFER pixels, not in displayed
 * ones. For the one-player layout — 512 x 248, `proj` 160, offset (256,124):
 *
 *      horizontal   2*atan(256/160) = 116.0 degrees
 *      vertical     2*atan(124/160) =  75.5 degrees
 *
 * The ratio of those tangents is 2.06, and the picture is shown at 1.376, so
 * the console's own image is horizontally compressed by a factor of 1.5.
 *
 * THERE IS AN ANAMORPHIC TERM, AND IT WAS FOUND. This used to say "it must not
 * be corrected: there is no anamorphic term anywhere in the image to undo it".
 * 0x80055DE4 is one: it builds the matrix the world draw loads from view+160
 * (0x800313DC) by scaling a basis row by row — row 0 by vw/320 and row 1 by
 * vh/240, with vw the immediate 320 at 0x800779BC and vh the immediate 160 at
 * 0x80077948. So the console's camera is diag(1, 2/3, 1) and its effective
 * projection is (H, 2H/3), not (H, H).
 *
 * Measured against a native capture of the BASE0 spawn, with the 2D briefing
 * panel proving a shared framebuffer origin and width: the sky wedge above the
 * panel is 136 px on the console against the port's 90, and the weapon's
 * emitter sits at x 325..337 against 301..307, with the VERTICAL identical at
 * y 157..166. Horizontal one and a half times out, vertical exact — which is
 * (240, 160) against (160, 160). `q2_rotation_view_anamorphic` carries it now
 * and closes both to within three pixels. The numbers below are the frustum in
 * framebuffer pixels BEFORE that term; with it the one-player horizontal is
 * 2*atan(256/240) = 93.6 degrees. The only matrix scale
 * on the world's transform chain is the uniform (768,768,768) at 0x800AEB30,
 * applied per COLUMN by 0x80055AF8 — an object scale of exactly 3, not a screen
 * one — and the projection distance reaches the GTE untouched from view+262.
 *
 * The compression is also what the game's own 2D art expects: the menu faces in
 * `qk_menu.lbm` are drawn texel-for-pixel into square rectangles and are
 * authored about 1.35x wider than tall, which is a normal letterform only once
 * the television has narrowed the pixels.
 */

/* The line count that fills the 4:3 raster, per video standard. */
#define Q2_SCREEN_LINES_4_3_PAL   256
#define Q2_SCREEN_LINES_4_3_NTSC  240

/*
 * The width and height of one framebuffer pixel on the television, as an exact
 * ratio in lowest terms — 2:3 for this game's PAL 512-wide buffer. Both outputs
 * are optional.
 */
void q2_screen_pixel_aspect(const q2_screen *s, int *num, int *den);

/* How the finished picture is fitted into a host window of `win_w` x `win_h`. */
typedef enum q2_screen_fit {
    /*
     * The drawn buffer as exactly 4:3, letterboxed or pillarboxed into whatever
     * the window is. THE DEFAULT, and it is the default because it is what
     * capture of the running game shows: a 4:3 picture pillarboxed inside a 16:9
     * frame, with the 512 x 248 buffer filling it.
     *
     * It differs from FIT_TELEVISION below by 3%, because this treats the 248
     * lines the game draws as the whole 4:3 picture where that one treats them
     * as 248 of PAL's 256. Three percent is not worth arguing about; matching
     * what the game is actually played and captured at is.
     */
    Q2_SCREEN_FIT_FULL_4_3 = 0,

    /*
     * The strict reading of the hardware: a pixel is 4*256 : 3*512 = 2:3, so the
     * 248 drawn lines are 248/256 of the 4:3 raster and the picture comes out at
     * 1.376:1 with the eight missing lines as extra border. Very slightly taller
     * than FULL_4_3 and arguably what a real television did.
     */
    Q2_SCREEN_FIT_TELEVISION,

    /*
     * One buffer pixel to one window pixel, aspect preserved. Not what the
     * console looked like — it is the 1.5x stretch — but it is what every raw
     * framebuffer dump of this game is, so it is worth being able to ask for.
     */
    Q2_SCREEN_FIT_SQUARE,

    /* Fill the window. Whatever shape it is. */
    Q2_SCREEN_FIT_STRETCH,

    Q2_SCREEN_FIT_COUNT
} q2_screen_fit;

const char *q2_screen_fit_name(q2_screen_fit fit);

/*
 * Where the picture goes inside a window of any size or shape: the largest
 * rectangle of the right aspect that fits, centred, with the remainder left as
 * border. Every output is optional.
 *
 * `win_w` / `win_h` may be anything, including shapes no television ever was —
 * 16:9, 16:10, 21:9, a portrait window, one pixel tall. The result is clamped
 * to at least 1x1 so a caller can always blit it.
 */
void q2_screen_fit_rect(const q2_screen *s, q2_screen_fit fit,
                        int win_w, int win_h,
                        int *x, int *y, int *w, int *h);

/*
 * The size a host window should open at to hold `scale` buffer pixels across
 * with no vertical squash: `scale * fb_w` wide, and tall enough to make the
 * pixels the right shape. For PAL at scale 3 that is 1536 x 1116.
 */
void q2_screen_window_size(const q2_screen *s, q2_screen_fit fit, int scale,
                           int *w, int *h);

/* ------------------------------------------------------------------------- */
/* A viewport's field of view                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Derived entirely from `proj`, `w` and `h`, so it is a reading of the
 * transcribed constants rather than a new fact — which is the point: it turns
 * "the halfword at 0x80077E50 is 160" into something a test can assert about
 * and a report can print.
 */
typedef struct q2_screen_fov {
    double horizontal;      /* degrees, 2*atan(w/2 / proj)                    */
    double vertical;        /* degrees, 2*atan(h/2 / proj)                    */

    /*
     * The ratio of the two tangents against the aspect the viewport is actually
     * shown at. 1.0 would be an undistorted picture; this game is 1.5 in every
     * layout, because a single `H` cannot be anamorphic and nothing else in the
     * image compensates. See the header note.
     */
    double display_aspect;  /* the viewport's own w:h once the TV has it      */
    double squeeze;         /* tan_h/tan_v divided by display_aspect          */
} q2_screen_fov;

void q2_screen_view_fov(const q2_screen *s, const q2_screen_view *v,
                        q2_screen_fov *out);

/* ------------------------------------------------------------------------- */
/* Texture-page blend bits — 0x800B36D8, filled by 0x80078378 at bring-up      */
/* ------------------------------------------------------------------------- */
/*
 * Five halfwords, `{32, 0, 32, 64, 96}`, built by a loop that writes
 * `(i-1) << 5` into slots 1…4 and then puts 32 back into slot 0. They are the
 * semi-transparency field of a `tpage` word, and both polygon emitters index
 * them with the top three bits of a face's texture byte before OR-ing the
 * result into the primitive's tpage (`0x8006DE44`…`0x8006DE5C`, and the same
 * pair of materialisations at 0x800680DC and 0x8006A384). So a face's blend
 * mode is chosen by that byte, and slot 0 — an unset selector — is ADD, not
 * the 50/50 blend an unset field would suggest.
 */
#define Q2_SCREEN_BLEND_SLOTS 5
extern const u16 q2_screen_blend_word[Q2_SCREEN_BLEND_SLOTS];

/*
 * The tpage bits and the decoded mode for selector `sel` — a face's texture
 * byte shifted right by five, so three bits against a five-entry table. The
 * original would read the halfwords *after* the table for selectors 5…7; what
 * is there is not part of the table and the port will not pretend to know it,
 * so those clamp to slot 0 and `q2_screen_blend_defined` says they were out of
 * range.
 */
u16       q2_screen_blend_bits(unsigned sel);
psx_blend q2_screen_blend_mode(unsigned sel);
bool      q2_screen_blend_defined(unsigned sel);

/* ------------------------------------------------------------------------- */
/* One frame                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * The order below is the order in 0x800182C8's loop body, and it matters: the
 * swap happens *before* the table is cleared and the world is built, so a frame
 * is always built into the buffer that is not being shown.
 *
 *      q2_screen_frame_begin(s, ot);            swap + ClearOTag(.., 217)
 *      q2_screen_build(s, ot, gte, hooks);      everything 0x800780C0 does
 *      q2_screen_compose(s, ot, vram, opts);    DrawOTag: walk, apply envs
 *      q2_screen_present(s);                    flip
 *
 * `q2_screen_build` is the whole per-frame build in one call because the order
 * inside it is not a caller's business — the clear decision has to happen
 * before the viewports, each viewport's state has to be published before its
 * geometry, and the env packet has to be linked after it. A caller that wants
 * to drive the pieces itself can still use the three functions below, which is
 * what the offline tools do.
 */

/* Rotate the buffers (0x80018214) and clear the ordering table. */
void q2_screen_frame_begin(q2_screen *s, psx_ot *ot);

/*
 * 0x800780C0 — build one frame's ordering table.
 *
 * The clear is a three-way decision, not the two-way one it looks like:
 *
 *   suppress_clear            nothing clears at all. Every viewport's isbg is
 *                             forced off and no background env is armed, so the
 *                             frame draws over whatever the buffer held.
 *   else background_enable    one full-screen env at OT[1] clears everything
 *                             (0x800769A0), and then every viewport's isbg is
 *                             forced off — so the screen is cleared exactly
 *                             once and, in a split, that one env is what paints
 *                             the gutters between the viewports.
 *   else                      neither is armed and the per-viewport isbg values
 *                             stand, so each viewport clears its own rectangle
 *                             and the gutters keep the previous frame.
 *
 * Then each live viewport is drawn (0x80076A74) and the overlay camera last
 * (0x80076E68 -> 0x80077230). `hooks` may be NULL, which builds every env, the
 * flash and the meter but no world or overlay content.
 */
void q2_screen_build(q2_screen *s, psx_ot *ot, gte_state *gte,
                     const q2_screen_hooks *hooks);

/*
 * Arm the single full-screen background clear (0x800769A0) and, exactly as
 * 0x800780C0 does immediately afterwards, turn every viewport's own clear off
 * so the screen is cleared once rather than once per viewport.
 */
void q2_screen_background(q2_screen *s);

/*
 * Prepare viewport `p` — the first half of 0x80076A74. Publishes the viewport's
 * state in `s->ctx`, loads the GTE's projection distance and screen offset from
 * the view, emits the damage flash, and restricts the ordering table to that
 * viewport's slice so an emitter can add primitives without knowing the
 * slicing — which is exactly the arrangement on the console, where the world
 * renderer is handed the slice base in 0x800B2D60. Returns false when `p` is
 * not a live viewport.
 */
bool q2_screen_view_begin(q2_screen *s, int p, psx_ot *ot, gte_state *gte);

/* The second half: link the viewport's draw-env packet at slice bucket 1, which
 * happens after its geometry so that the env is at the head of the bucket and
 * therefore executes first (0x80076D44). */
void q2_screen_view_end(q2_screen *s, psx_ot *ot);

/* Same for the overlay camera at 0x800D6870 (0x80077230 / 0x80077EAC). */
void q2_screen_overlay_begin(q2_screen *s, psx_ot *ot, gte_state *gte);
void q2_screen_overlay_end(q2_screen *s, psx_ot *ot);

/*
 * Raise the damage flash on viewport `p` — the state at view+672…+680 that
 * 0x8003AE10 writes and the viewport draw consumes. `strength` is both the
 * initial value and the countdown, so the flash lasts that many drawn frames.
 */
void q2_screen_flash_set(q2_screen *s, int p, const u8 rgb[3],
                         s16 strength, s16 mode);

/*
 * Publish what the water effect reads off view+288: whether viewport `p` has an
 * owner at all, and whether that owner's flag word has Q2_ENT_UNDERWATER set.
 * Call it once per frame per live viewport, before q2_screen_build.
 *
 * `owned == false` is not the same as `submerged == false`. A viewport with no
 * owner is skipped entirely — its amplitude, phase and shake all keep whatever
 * they held — which is what the original's early return does, and is the state
 * a viewport is in before its player's camera has been installed.
 */
void q2_screen_water_set(q2_screen *s, int p, bool owned, bool submerged);

/*
 * The two numbers the amplitude turns into. Exposed because they are the whole
 * of the effect's shape and are worth checking directly: the shake is one of
 * them divided by 4096, and the strip offsets are `(cos * amplitude) >> 24`
 * plus that shake.
 *
 *   horizontal   amp * 4095 / 1024   — the ROW pass, and shake_x
 *   vertical     amp * 4095 / 2048   — the COLUMN pass, and shake_y
 *
 * Both divides truncate toward zero, and the halving between them is why the
 * picture slides twice as far sideways as it does up and down.
 */
void q2_screen_water_amplitudes(s16 amp, s32 *horizontal, s32 *vertical);

/*
 * Map a depth into a viewport's slice, returning an ABSOLUTE bucket for
 * `psx_ot_add_bucket`. Depth counts down from the far end, so 0 is the front of
 * the viewport and the largest value is its back — the same convention
 * `psx_ot_add` uses once `q2_screen_view_begin` has installed the window.
 */
u16 q2_screen_view_otz(const q2_screen *s, int p, u32 z);
u16 q2_screen_overlay_otz(const q2_screen *s, u32 z);

/*
 * Walk the ordering table exactly as DrawOTag would: bucket 0 upwards, and
 * within a bucket most-recently-added first. The draw-env packets sitting in
 * the table change the clip rectangle and, when their isbg is set, clear it —
 * so this one walk produces the background, every viewport and the overlay,
 * each correctly clipped, with no second pass.
 */
void q2_screen_compose(q2_screen *s, psx_ot *ot,
                       const psx_vram *vram, const psx_raster_opts *opts);

/* Flip: the buffer just drawn becomes the buffer being shown. */
void q2_screen_present(q2_screen *s);

/*
 * The buffer a host window should be showing. Under a single-buffered layout
 * both display envs name the same VRAM rectangle, so the buffer being drawn is
 * also the one on the television and this returns the back buffer — which is
 * the only way a two-framebuffer port can express "you can see it being drawn".
 */
const psx_framebuffer *q2_screen_front(const q2_screen *s);
psx_framebuffer       *q2_screen_back(q2_screen *s);

/* Fold a real elapsed time into the console's clamped dt (0x800184B8). */
s32 q2_screen_tick_dt(q2_screen *s, double seconds);

/*
 * The frame lock, as a number rather than as prose: `VSync(2)` waits two fields
 * of a 50 Hz PAL signal, so the console draws every 0.04 s and its logic runs at
 * 25 Hz. A host is free to run faster — that is one of the reasons to have a
 * native port at all — but the game's own `dt` must still come from
 * q2_screen_tick_dt, because everything the simulation does is expressed in the
 * 1/300 s units this produces.
 */
double q2_screen_frame_period(const q2_screen *s);

#endif /* Q2PSX_SCREEN_H */
