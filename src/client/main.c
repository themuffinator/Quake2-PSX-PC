/*
 * main.c — the playable client.
 *
 * Opens a disc, loads a zone, and lets you fly through it in real time. The
 * internal framebuffer is the console's own — 512x248, read out of the display
 * init at 0x800764DC rather than assumed — and is upscaled to the window. That
 * is not a concession to performance; rendering at the original resolution is
 * part of looking right, because the dither pattern and the vertex snapping are
 * both defined in terms of real pixels.
 *
 * Upscaled, NOT stretched to fill. A framebuffer pixel is two thirds as wide as
 * it is tall on a PAL television — every one of the GPU's horizontal modes spans
 * the same active line, so 512 columns are narrow columns, not a wider picture —
 * and putting the buffer on a window one-for-one is a 1.5x horizontal stretch
 * that makes a correctly reconstructed field of view read as a wrong one. The
 * shape is q2_screen_fit_rect's, the window may be any size or aspect, and V
 * cycles the choice. See the pixel-aspect section of src/screen/screen.h.
 *
 * The frame is put together the way the console puts one together: swap, one
 * background clear, then each viewport into its own slice of a single 217-entry
 * ordering table, then one walk of that table with the draw-env packets in it
 * doing the clipping. See src/screen.
 *
 * Controls
 *   W/A/S/D      move
 *   Q/E          down / up
 *   arrows       look
 *   shift        move faster
 *   1..9, 0      switch zone
 *   F1           toggle dithering
 *   F2           toggle affine UVs (perspective-correct comparison)
 *   F3           toggle the ordering-table sort
 *   F4           toggle simulated movement vs free-fly camera
 *   F5           cycle the console's viewport layouts (one, the two splits,
 *                the 2x2, and the boot screen's single-buffered full screen)
 *   F6           show the GlintMod glint (BIGGUN only; off by default because
 *                nothing the engine does turns one on — see effect.h)
 *   F7 / F8      the memory-card front end, saving and loading
 *   F9 / F10     quick save and quick load, slot 1
 *   F11          screenshot, of the 512x248 framebuffer rather than the window
 *   P            toggle the view's own world position and angles along the
 *                top of the frame, in the units --at, --yaw and --pitch
 *                take. Not the console's: nothing on this disc puts a
 *                position on screen. `--coords` starts a run with it up
 *   V            cycle how the picture is shaped: the console's own pixel, the
 *                raw buffer, a forced 4:3, or filling the window
 *   space        jump — and, held, swim up. One key because it is one BUTTON:
 *                the pad's tail writes bit 22 from its press edge and bit 21
 *                from it being held (pad.h)
 *   up/down      look — and holding BOTH is the console's own view recentre,
 *                which is a chord rather than a setting (0x8003A780)
 *   ctrl / c     hold a crouch. The one key here that is NOT the console's:
 *                crouching is authored per map as a trigger volume, and this
 *                asserts the same flag such a volume would (worldscale.h). The
 *                map's own crouch volumes work without it
 *   Esc          the pause menu — and QUIT GAME inside it leaves
 *
 * The mouse
 *   move         look. The three MOUSE control styles are the console's own —
 *                0x80019224 and the two after it — so this selects one rather
 *                than adding a tenth: USE MOUSE on the CONTROLLER page decides
 *                which class of styles that page offers, exactly as the
 *                connected controller decides it at 0x8001C8A8, and on a PC
 *                the mouse is what is connected. MOUSE SPEED is its
 *                sensitivity and SWAP Y AXIS inverts it
 *   mouse 1      attack        mouse 2      jump — and, held, swim up
 *   wheel        next / previous weapon
 *
 * Both buttons are the scheme's rather than this port's choice: RIGHT MOUSE
 * fires on the mouse's left button and jumps on its right, because the console
 * merges the pair into L3 and R3 and that is which mask each one feeds.
 *
 * Turning USE MOUSE off puts the styles back to the STANDARD group and every
 * key below keeps working unchanged — the keyboard is bound to MEANINGS through
 * q2_pad_style_bindings, not to pad buttons, so a style change moves what a key
 * means instead of breaking it. Under a mouse style the arrows have no look
 * buttons to press and drive the look AXIS instead, so they still turn.
 *
 * In the menu the keyboard stands in for the pad, because the menu engine is
 * written against the console's 16-bit button mask and nothing is gained by
 * giving it a second input model:
 *
 *   arrows       d-pad          Enter / Space   cross   (select)
 *   Esc          triangle       Backspace       triangle (back)
 *
 * and the mouse is a pad too, with the two things a pad cannot say — land on
 * THAT row, set that slider to THAT value — going through menu.c's pointer
 * entry points instead (menumouse.h):
 *
 *   move         highlight the row under the pointer
 *   mouse 1      select it; on a toggle, aim at the ON or OFF word; on a
 *                slider, click or drag the bar itself
 *   mouse 2      back, which is triangle — and on the pause page, where
 *                triangle has no parent to go to, it closes the menu as Esc
 *                does
 *   wheel        move the cursor
 *
 * The two save keys are the port's, and the reason they exist is that the
 * console's own route to SAVE? is not reachable here: on the disc that prompt
 * is reached from the front end and at a level boundary, neither of which this
 * client has. Everything BEHIND the prompt is the console's — the screens, the
 * four rows, the release rule and the state machine (memcard.h, saveui.h).
 */
#include "random.h"
#include <SDL3/SDL.h>

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "aiworld.h"
#include "crebind.h"
#include "creworld.h"
#include "levelbin.h"
#include "lighting.h"
#include "loading.h"
#include "rotator.h"
#include "spacelights.h"
/* Creatures on the biggest map plus three other players, with room to spare. */
#define Q2_CLIENT_MAX_TARGETS 96

/* The item table's sound-name array — eleven, at 0x800AC240. */
#define Q2_CLIENT_ITEM_SOUNDS 11

#include "multiplayer.h"
#include "playerdeath.h"
#include "userfuncs.h"
#include "disc.h"
#include "entity.h"
#include "entitydraw.h"
#include "fxtables.h"
#include "hudtables.h"
#include "ident.h"
#include "launcher.h"
#include "item.h"
#include "loadout.h"
#include "itemtable.h"
#include "menu.h"
#include "menudraw.h"
#include "memcard.h"
#include "menufont.h"
#include "menumouse.h"
#include "leveltable.h"
#include "musictable.h"
#include "briefing.h"
#include "leveltext.h"
#include "mover.h"
#include "explosive.h"
#include <stdlib.h>
#include "mission.h"
#include "modelent.h"
#include "movie.h"
#include "panel.h"
#include "prompt.h"
#include "q2psx.h"
#include "raster.h"
#include "save.h"
#include "saveui.h"
#include "screen.h"
#include "sortdata.h"
#include "pad.h"
#include "gamepad.h"
#include "shock.h"
#include "sim.h"
#include "statusbar.h"
#include "trace.h"
#include "trig.h"
#include "vag.h"
#include "version.h"
#include "viewweapon.h"
#include "vmtables.h"
#include "vram.h"
#include "world.h"
#include "xa.h"

/* ------------------------------------------------------------------------- */
/* One playing effect                                                         */
/*                                                                            */
/* The console's SPU has twenty-four voices and an effect does not stop the    */
/* track, so there are twenty-four here and an effect is summed into it. A     */
/* voice borrows the bank's ADPCM rather than copying it — the sample is       */
/* decoded 28 samples at a time as it plays (vag.h) — which is why every voice */
/* has to be stopped before the bank it points into is freed.                  */
/* ------------------------------------------------------------------------- */
#define CLIENT_VOICES 24

/* Two blocks: enough that a compaction always leaves room for the next one. */
#define CLIENT_VOICE_BUF (SPU_SAMPLES_PER_BLOCK * 2)

/*
 * The constant-power pan table at 0x800A1F20 — 31 entries, one side of the
 * curve. The near channel takes `pan[idx]` and the far one `pan[30 - idx]`,
 * with idx 15 dead centre, which is why the middle nine entries are not flat:
 * a source in front of the listener is not half in each ear.
 */
#define CLIENT_PAN_STEPS 31
static const u8 k_pan[CLIENT_PAN_STEPS] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xF3,0xF3,
    0xE6,0xE6,0xDA,0xDA,0xCD,0xB3,0xA7,0x9B,0x8F,0x83,
    0x77,0x6B,0x5F,0x53,0x47,0x3C,0x30,0x23,0x17,0x0C,
    0x00
};

/*
 * The distance at which a positional sound reaches zero.
 * `level = req_vol * (reach - dist) / 4096`, so a source at the listener is
 * three times the request's own volume and one at the reach is silent.
 */
#define CLIENT_SFX_REACH  12288
/*
 * A REQUEST'S OWN VOLUME, which is where the 63 above comes from.
 *
 * It is not a constant on the console: it is byte +2 of the 12-byte sound
 * request record at 0x800DFDE4, written as 63 when the request is made and
 * altered afterwards only by SoundSetRelativeVolume. `0x80072AF0` reads that
 * byte, scales it by the caller's own factor and writes the result into the
 * voice's three volume fields — so with the default byte the voice volume IS
 * 63, and this port's attenuation base has been that number under another name
 * all along. The arithmetic itself lives beside the request's PITCH byte in
 * vag.h, which is the other byte of the same record.
 */
#define CLIENT_SFX_REQ_VOL ((s32)Q2_SFX_VOLUME_DEFAULT)
/* What a listener-local sound gets instead — the menu, the player's own weapon
 * and footsteps, the HUD. Never attenuated, never panned. */
#define CLIENT_SFX_LOCAL  254

typedef struct client_voice {
    bool         active;
    /*
     * WHICH SOUND THIS SLOT IS, so a caller can hold on to one.
     *
     * The console's play-and-keep entry point 0x80073734 writes a four-byte
     * handle: the voice index at +0 and, at +2, the halfword the SPU driver
     * keeps at voice_record+52 — a tag it bumps every time the slot is taken.
     * 0x800739E4 ("is it still playing") and 0x80073924 (the stop behind
     * 0x8007398C) both bounds-check the index against 24 and then compare that
     * tag, returning 0 when it has moved on. That is what makes an owner's
     * handle safe after its sound has ended and the slot has been reused.
     *
     * This port needs the same guard for the same reason and cannot get it from
     * a bare `client_voice *`: client_play_sound memsets the slot before
     * reusing it, so `active` alone would report an unrelated later sound as
     * the one the caller started. `serial` is this port's tag; it is assigned
     * after the memset, never zero, and never repeats.
     */
    u32          serial;
    q2_spu_voice dec;
    s16          buf[CLIENT_VOICE_BUF];  /* decoded, not yet stepped over */
    u32          have;                   /* samples in `buf`              */
    u32          pos;                    /* 16.16 read cursor into `buf`  */
    u32          step;                   /* 16.16 advance per output frame */
    s32          vol;                    /* 0..127, latched at the start  */

    /*
     * WHERE IT IS, which nothing used to record — so a creature idling across
     * the map, a door eight thousand units away and the shotgun in your hands
     * were all mixed at identical level and dead centre. That is the single
     * most audible departure from the console in the whole audio path.
     */
    bool         positional;
    s32          pos_world[3];

    /* Recomputed every frame from the listener, because a source moves and so
     * does the listener; 0..255 each. */
    s32          level;
    s32          pan_l, pan_r;

    /*
     * The REQUEST's volume byte this voice was started from — 0x800DFDE4's +2,
     * 63 unless something called SoundSetRelativeVolume on the request. The
     * per-frame recompute above overwrites `level` from the distance every
     * frame, so a request's own volume cannot live there; it is folded in as a
     * ratio against the default instead.
     */
    u8           req_vol;
} client_voice;

/* One render-clock cursor per articulated world model. `frame_stamp` prevents
 * split-screen from advancing it once per viewport instead of once per frame. */
typedef struct client_model_anim {
    q2_model_cursor cursor;
    const q2_mmove *move;
    long             frame_stamp;
    u8               source;
    bool             stamped;
} client_model_anim;

typedef struct client_player_anim {
    q2_model_cursor cursor;
    q2_player_move  move;
    long            frame_stamp;
    u32             shot_serial;
    bool            stamped;
    bool            attack_latched;
    /*
     * The sim's flinch serial as this record last saw it, and the latch it
     * raises. Same shape as `shot_serial`/`attack_latched` above and for the
     * same reason: the pose runs on the display clock and several viewports
     * may ask for it in one frame, so the request cannot be a flag the first
     * asker clears. Dropped when the pain clip wraps.
     */
    u32             pain_serial;
    bool            pain_latched;
    /*
     * Whether the move now playing has run past its end at least once --
     * bit 0 of entity+0x102, raised by 0x8003DF90 and read by the chooser at
     * 0x8003D008 and 0x8003D1D0. Without it `q2_player_anim_pick`'s rule that
     * a pain move holds until it wraps degenerates and the next tick's
     * RUN/STAND cuts the flinch off after a single frame.
     */
    bool            wrapped;
} client_player_anim;

/*
 * A creature's mesh AS IT WAS LAST DRAWN, for the damage-effect drawers.
 *
 * 0x8006CC44 poses a vertex on the entity's current frame whenever an effect
 * asks. The port poses a creature in the draw (client_draw_view), from a
 * render-clock cursor that must advance exactly once per display frame, so
 * posing it a second time from the combat tick would either move that cursor
 * or need a second copy of the pose selection. Instead the draw leaves its
 * result here and the hook (client_fx_mesh) hands it over: the mesh the player
 * saw on the last frame, at most one frame behind the tick that asks — which
 * is the presentation pass's own timing divergence (effect.h), not a new one.
 *
 * `inst.pose` points at `pose` below, or is NULL for an unposed draw. The
 * light and page-table pointers are cleared: they point into the draw's frame
 * and nothing that asks for a vertex needs them.
 */
#define CLIENT_FX_POSE_MAX 64   /* the creature draw's own pose[] bound */

typedef struct client_fx_pose {
    q2_model_instance inst;
    q2_model_pose     pose[CLIENT_FX_POSE_MAX];
    bool              valid;    /* a draw has posed it since the load */
} client_fx_pose;

typedef struct client {
    disc            *disc;
    q2_build_id      build;
    q2_world_zone    zone;
    q2_common_file   common;
    char             map[64];
    /*
     * The same level under the name the level table shows it by, which is what
     * `0x800E46B4` holds — the loader copies record `+0` there at
     * `0x8007C6C8`, not the directory at `+0x0C`. A `LOADMAP` names a DISPLAY
     * name, and the guard that makes one naming the loaded level a no-op
     * compares against this rather than against the folder.
     */
    char             map_display[64];
    int              zone_index;

    /* Streamed music. The XA decoder carries per-channel history across
     * sectors, so it must persist for the life of a track. */
    q2_xa_track      music;
    q2_xa_decoder    music_dec;
    u32              music_cursor;
    bool             music_open;

    /*
     * WHICH music, which used to be "track A, because the mapping is not
     * decoded yet". It is decoded now (musictable.h): a level record carries a
     * seven-entry playlist of track ids, and an id names a file and a channel
     * through the table at 0x800A1DD8. `music_cursor_at` is the engine's own
     * walk position — the cursor at gp+1536 — so a track that ends advances the
     * list instead of looping itself, which is what the console does.
     */
    q2_music_table   music_table;
    bool             music_table_ready;
    q2_level_table   level_table;
    bool             level_table_ready;
    const q2_level_entry *level;
    int              music_cursor_at;
    int              music_id;
    SDL_AudioStream *audio;

    /*
     * ---------------------------------------------------------------------
     * The mixer, and why there has to be one
     * ---------------------------------------------------------------------
     * There is ONE device stream, and `SDL_PutAudioStreamData` APPENDS to it.
     * It is a FIFO, not a mixer. Music was pushed into it a sector at a time
     * and every effect was pushed into the same FIFO, so an effect did not
     * play with the music — it played AFTER everything already queued, and it
     * pushed the music further back by its own length.
     *
     * That is what made the sound "randomly ongoing". The music pump tops up
     * only while the queue is under its target, so it stops contributing as
     * soon as effects fill it; the effects do not stop, because footsteps,
     * creature idles and weapon fire keep arriving. The queue therefore grew
     * without bound and the device played out an ever-later backlog of
     * unrelated effects, minutes behind the thing that raised them.
     *
     * Two further faults rode along with it. The stream is declared STEREO at
     * XA's 37800 Hz because that is what the music is, and a bank effect is
     * MONO at its own 11025 or 22050 (vag.h) — so each effect was read as
     * interleaved stereo, splitting alternate samples across the two channels,
     * and played back between 1.7 and 3.4 times too fast.
     *
     * So the effects go through voices instead: twenty-four of them, which is
     * the SPU's own count and the number the comment on the menu path always
     * claimed this had. Each one decodes its sample a block at a time, steps
     * through it at the ratio between its rate and the stream's, and is summed
     * into the music bed. One push per chunk, mixed, at the stream's format.
     */
    client_voice     voice[CLIENT_VOICES];
    u32              voice_started;
    u32              voice_dropped;   /* all 24 busy, as the SPU would be */

    /*
     * Weapon-fire sounds ASKED FOR, which is a different number from voices
     * started: a headless run has no audio device and starts none, and the
     * question "does the chaingun bang once per bullet" has to be answerable
     * there. `chaingun_bangs` is the per-bullet clone alone.
     */
    u32              weapon_sounds;
    u32              chaingun_bangs;

    /*
     * The quad's firing sound, on both sides of its gate: how many shots asked
     * for one and how many of those 0x8004FBB0 would have thrown away because
     * the previous copy was still sounding.
     *
     * `quad_gated` can only be non-zero when a voice actually started, so a
     * headless run — which has no device and therefore no voice — reads it as
     * zero however hard the trigger is held. That is a property of the counter,
     * not of the gate; `quad_raises` is the reproducible half.
     */
    u32              cocks_played;
    u32              quad_raises;
    u32              quad_gated;

    /*
     * The shot this client has already been told about, against the sim's own
     * counter — see the reads in `client_input_simulated`.
     *
     * TWO cursors against ONE counter, because each read CONSUMES: the first
     * one to notice a new serial moves the cursor past it, and a shared cursor
     * would therefore let whichever ran first swallow the shot and leave the
     * other with nothing. The view model runs first, so sharing would give a
     * firing weapon a fire clip and no sound.
     */
    u32              shot_serial_heard[Q2_MP_MAX_PLAYERS];   /* the sound has been played  */
    u32              shot_serial_shown[Q2_MP_MAX_PLAYERS];   /* the view model has been told */

    /*
     * The bed the voices are mixed into: one decoded sector of music or film,
     * handed out in chunks smaller than a sector so the queue can sit close to
     * its target instead of overshooting by 53 ms at a time.
     */
    s16              bed[XA_FRAMES_PER_SECTOR * XA_CHANNELS];
    u32              bed_frames;      /* frames decoded into it   */
    u32              bed_pos;         /* frames handed out so far */

    q2_camera        cam;
    /*
     * One sim per player. Index 0 owns the WORLD — the items, the script, the
     * entity list, the effects — and 1..3 are movement instances: each has its
     * own position, view, inventory and pad, and their world-side halves are
     * ticked but never read or drawn. Sharing one world properly means pulling
     * the player out of q2_sim, which is a change to sim.c and not to its
     * caller; see openquestions #53.
     */
    q2_sim           sim[Q2_MP_MAX_PLAYERS];
    bool             sim_ready[Q2_MP_MAX_PLAYERS];
    q2_pad_state     mp_pad[Q2_MP_MAX_PLAYERS];
    u16             mp_pad_pend[Q2_MP_MAX_PLAYERS];
    bool            mp_pad_resume[Q2_MP_MAX_PLAYERS];
    q2_input        mp_input[Q2_MP_MAX_PLAYERS];
    s32             mp_camera_angles[Q2_MP_MAX_PLAYERS][3];
    q2_gamepads     gamepads;
    bool            gamepad_player_one;
    q2_mp_results   mp_results;
    double          mp_results_dt_frac;
    u32             mp_results_prev[Q2_MP_MAX_PLAYERS];
    bool            mp_results_entered;
    u32             mp_rounds_restarted;

    /*
     * PLAYER 0'S PAD, and it lives here rather than as a static inside the
     * input function because two things outside that function's roll need it:
     * the death page's respawn button, and the resume detection below.
     *
     * `pad_pend` is the raw pad accumulated between ticks (see the roll), and
     * `pad_frame` is the last frame on which the input function ran. When that
     * is not the frame before this one, something else owned the frame in
     * between and the pair is RESEEDED rather than rolled — the level start,
     * the film ending, a board closing. See q2_pad_roll_resume.
     */
    q2_pad_state     pad;
    u16              pad_pend;
    long             pad_frame;
    bool             pad_resume;    /* a gap is outstanding, awaiting a tick   */
    u32              pad_resumes;   /* gaps the roll had to be reseeded across */
    bool             sim_enabled;

    /* World render state. It lives here rather than in the draw because the
     * texture-page table's ABR promotions must persist across frames — see
     * q2_world_render. */
    q2_world_render  render;

    /* The pause menu, and the settings it edits. The settings outlive a zone
     * load; the menu does not own them.
     *
     * The FONT does not outlive one: its two atlases are VRAM images inside the
     * map's own SNDVRAM.DAT (menufont.h), so it is uploaded per zone load
     * alongside the texture pages, exactly as the console's image registration
     * runs per level. */
    q2_menu_settings settings;
    q2_menu          menu;
    q2_menu_font     menu_font;
    bool             menu_font_ready;
    q2_hud_tables    hud_tables;
    bool             hud_tables_ready;

    /* The HUD's own font — the same atlas, reached through the overlay's
     * markup layer rather than the menu's glyph path — and the level-completion
     * screen that draws through it. */
    q2_hud_font      hud_font;
    bool             hud_font_ready;
    q2_mission       mission;
    bool             mission_open;

    /* The briefing screen, and the two pieces of UI chrome it shares with the
     * mission screen and the memory-card questions. */
    q2_briefing      briefing;
    bool             briefing_open;

    /*
     * THE OBJECTIVES POP-UP — the state machine around the same screen.
     *
     * THIS IS THE ONLY ONE THE CONSOLE HAS. `briefing_open` above is a debug
     * key's screen; it used to be raised by the zone load as well, on the
     * reading that a level change shows a briefing. Nothing in the transition
     * path raises either field, and both draw through the same composer, so
     * that second state machine was a duplicate of this one. HELPCOMPUTER
     * raises this from a trigger volume and the pause menu's MISSION row
     * raises it on demand, it holds the world while it is up, and it closes on
     * its own deadline or on CROSS. See briefing.h — the composer was already
     * here and everything around it was not.
     *
     * The two strings live on the pop-up rather than on `briefing` because
     * they are GLOBAL on the console: two writers, one reader, never reset per
     * level. A zone load rebuilds `briefing`; it must not rebuild these.
     */
    q2_briefing_popup popup;
    bool             popup_cross_prev;
    client_voice    *voice_last;   /* the voice client_play_sound just took */
    u32              voice_last_serial;  /* and its tag — the pair is the handle */
    u32              voice_serial_next;  /* the tag source; 0 is "no handle"  */

    /*
     * THE QUAD'S ONE VOICE SLOT.
     *
     * All four of the console's quad-fire sites share a single handle at
     * 0x800B2B80 — a gp-relative global, not a per-player field — so one pair
     * here is the parity-correct shape even in split screen. See the drain in
     * client_advance_view_weapon.
     */
    client_voice    *quad_voice;
    u32              quad_voice_serial;

    /*
     * THE SHOTGUN'S COCK, which is deferred rather than played with the bang.
     *
     * 0x8004C460 `sb s4, 695(s5)` raises a byte on the player entity on every
     * shotgun shot that actually fires, and the view weapon's per-substep
     * driver spends it: 0x80050484 admits only weapon 2, 0x80050494 tests the
     * byte, and 0x800504AC asks 0x800739B8 whether the handle at entity+276/278
     * — the voice `wep_shotgf1b` was started on — is STILL SOUNDING. While it
     * is, 0x800504B4 drops the request and asks again next substep; the first
     * one after the bang has finished plays `[0x800B2B78]`, which is
     * `wep_shotgr1b`, and 0x800504CC clears the byte.
     *
     * So the cock lands about half a second after the shot (the sample is
     * 12,936 frames at 22,050 Hz, 0.536 s at the console's 35/32 pitch) and,
     * because a second shot restarts the voice and re-raises the byte, after
     * the LAST shot of a burst rather than between shots.
     *
     * It is the plain shotgun's alone: 0x8004C488..0x8004C740, the super
     * shotgun's fire function, contains no write to +695, and `wep_sshotr1b`
     * is only the fallback NAME for the same slot at 0x8004DC38.
     *
     * Per player, because the byte is on the player entity; the handle is the
     * one the shot itself took, so it is latched where the shot is played.
     */
    bool             cock_pending[Q2_MP_MAX_PLAYERS];
    client_voice    *shot_voice[Q2_MP_MAX_PLAYERS];
    u32              shot_voice_serial[Q2_MP_MAX_PLAYERS];
    u32              popup_raises;
    u32              popup_opens;
    s32              popup_at_frame;   /* --objectives N; 0 is off */
    q2_prompt_bar    prompts;

    /*
     * The overlay itself: the notification ring, the centre line, the crosshair
     * and the damage flash. It was reconstructed before anything called it —
     * this is the call, and it is now fed the player's real condition every
     * tick, so the flash reacts to damage the way the console's does.
     */
    q2_hud           hud[Q2_MP_MAX_PLAYERS];
    u32              hud_flashes[Q2_MP_MAX_PLAYERS];
    /* FlashPowerUp's two outputs (0x80039A80 and 0x80039A00). The tint count is
     * per DRAWN frame, because that is what the raise is: a run holding a
     * powerup should show one per frame minus the three blink windows. */
    u32              powerup_tints[Q2_MP_MAX_PLAYERS];
    u32              powerup_warns[Q2_MP_MAX_PLAYERS];
    /* The absorb stages' washes, 0x80057B80 and 0x80057D24. One per hit the
     * power shield or the armour took, not one per frame. */
    u32              power_flashes[Q2_MP_MAX_PLAYERS];
    u32              armour_flashes[Q2_MP_MAX_PLAYERS];
    bool             hud_ready;

    /* The map's own sound bank, for the menu's five effects. Per zone load, as
     * the bank is per map. */
    q2_sound_bank    sfx;
    bool             sfx_ready;

    /*
     * The status bar — the thing this project once proved did not exist. It is
     * drawn per viewport by the same hook that draws that viewport's world
     * (statusbar.h), so it is fed and emitted inside client_draw_view.
     */
    q2_icon_tables   icons;
    bool             icons_ready;
    q2_statusbar     sbar[Q2_MP_MAX_PLAYERS];

    /*
     * The carousel's writes (client_sbar_write_slots): one count per event
     * this side calls the writer at, and how often the draw's latch found a
     * mark of an event it could not see (q2_statusbar_weapon_slots_track).
     * Recomputing every drawn frame would put one write per bar per frame
     * here; the console writes at the events alone.
     */
    u32              slots_written[3];
    u32              slots_inferred;

    /*
     * The memory-card front end. Its screens and its release-gated state
     * machine are the console's (memcard.h); the card operations behind them
     * are `libmcrd` talking to hardware this port does not have, so what sits
     * behind the three function pointers here is the port's own file-backed
     * save system (saveui.h) rather than a stub.
     *
     * `card_menu` is a second menu instance that exists only to NAVIGATE and
     * DRAW one of those screens. The screens are ordinary 24-byte item tables,
     * so the menu engine already knows how to walk them — running them through
     * it is what gives the front end the same cursor rules, the same selection
     * bar and the same font as every other page, instead of a second
     * implementation that would drift.
     */
    q2_mcard         mcard;
    q2_mcard_host    mcard_host;
    bool             mcard_open;
    q2_save_ui       save_ui;
    q2_save_ui_mode  card_mode;
    q2_menu          card_menu;
    q2_mcard_screen  card_screen;
    int              card_return_page; /* front-end page restored on close */

    /*
     * "DO YOU WANT TO SAVE?" — the stop the console puts on the first frame of
     * every new level (GameCheckPaused 0x8003E6FC..0x8003E75C). Its screen is
     * Q2_MCARD_SAVE_PROMPT, drawn and navigated through a shadow menu of its
     * own, for the same reason the card front end has one.
     */
    bool             save_prompt_open;
    q2_menu          save_prompt_menu;
    /* LevelChange (0x800AEBD0) and GameCheckPaused's private latch (gp+700).
     * The counter is bumped by every level load, the latch by the question
     * being asked — or by a load that arrived on an exit code that does not
     * ask. See q2_mcard_save_prompt_due. */
    u32              level_change_serial;
    u32              save_prompt_latch;
    u16              save_prompt_pad;    /* the release edge, prev & ~cur    */
    u32              save_prompt_frames; /* the headless stand-in's count    */
    u32              save_prompts;       /* report.level.save_prompts        */

    /* What a save writes. Held here rather than built inside the front end
     * because q2_save_ui borrows it and the capture needs the whole client —
     * the sim, the mission tallies and the menu settings. */
    q2_save          snapshot;

    /*
     * The item table — 64 records, the 55-slot touch dispatch and the eleven
     * sound names — out of the same executable as everything else here. Per
     * disc rather than per map, and handed to the spawner so the items standing
     * in the level come from the disc's own table rather than from the
     * transcription of it.
     */
    q2_item_table    item_table;
    bool             item_table_ready;

    q2_fx_tables     fx_tables;
    bool             fx_tables_ready;
    /*
     * The weapon in the player's hands. The bank is per disc — the animation
     * clips live in the executable, not on a map — while the model itself comes
     * out of whichever map is loaded, so the two are bound at different times.
     */
    q2_vm_tables     vm_tables;
    bool             vm_ready;
    q2_viewweapon    vw[Q2_MP_MAX_PLAYERS];
    q2_model_bank    model_bank;
    bool             model_bank_ready;
    q2_model         vw_model[Q2_MP_MAX_PLAYERS];
    bool             vw_model_ready[Q2_MP_MAX_PLAYERS];
    int              vw_last_weapon[Q2_MP_MAX_PLAYERS];
    u32              vw_drawn[Q2_MP_MAX_PLAYERS];

    /* Multiplayer bodies. Male2 owns the ten animation clips; the three
     * colour variants carry matching geometry/palettes and consume its
     * eighteen-part pose, as retail's four player identities do. */
    q2_model          player_model[Q2_MP_MAX_PLAYERS];
    bool              player_model_ok[Q2_MP_MAX_PLAYERS];
    bool              player_anim_base_ok;
    client_player_anim player_anim[Q2_MP_MAX_PLAYERS];

    /*
     * The things in the level that are trying to kill you.
     *
     * Every piece of this was written and nothing called it: the modules
     * relocate, decode and bind, the Population records spawn, and the AI runs
     * — but only the inspector ever asked, and it draws a creature standing
     * still at its spawn point. A level in the client was its geometry, its
     * items, and nothing that moves. This is the join (creworld.h).
     *
     * `cre_model` is resolved once per zone load rather than per frame: a
     * creature's model is named by its class and lives in either the map's
     * CastList or the zone's, and searching two banks for every monster every
     * frame is work with a fixed answer.
     */
    q2_creature_world creatures;
    bool              creatures_ready;
    q2_model_bank     zone_bank;
    bool              zone_bank_ready;
    q2_model         *cre_model;      /* one per monster, NULL when unresolved */
    bool             *cre_model_ok;
    client_model_anim *cre_anim;
    q2_actor         *cre_actor;      /* what combat shoots at                 */
    /*
     * ...and what it BUMPS INTO, which is a different list with a different
     * radius. See client_bodies_publish and q2_sim_set_bodies.
     */
    q2_move_body     *cre_body;
    q2_actor        **cre_target;
    client_fx_pose   *cre_fx;         /* the last drawn pose, for 0x8006CC44   */
    /*
     * The world the AI asks its three questions of — line of sight, a box
     * move, and whether there is ground under a creature's feet. Without this
     * the AI runs on the open stand-in, where every creature can see through
     * every wall and stand on thin air. It borrows the sim's SecondaryCol, so
     * it must not outlive a zone.
     */
    q2_ai_world_bind  ai_world;

    double            ai_accum;       /* seconds owed to the 10 Hz AI clock    */
    u32               ai_thoughts;
    u32               cre_swings, cre_shots;   /* hook invocations */
    /* Fire reports asked for; misses land in cre_sound_missing beside the
     * creature voices. See client_cre_fire. */
    u32               cre_fire_sounds;
    u32               cre_sounds;
    u32               cre_sound_missing;
    u32               cre_sound_unnamed;  /* the play site resolved to no name */

    /*
     * A shot that reached the hook naming an IMPORT SLOT rather than one of the
     * Soldier's flash tables — a decoded creature's, whose damage and speed are
     * arguments its module supplies and this port has not read. Counted rather
     * than fired, and counted rather than silently returned. See client_cre_fire.
     */
    u32               cre_fire_no_figures;

    /*
     * Creature rounds through the one bullet path (q2_sim_creature_hitscan):
     * how many were traced and what each met. `cre_dying_shots` is every shot
     * a creature fired after its `die` ran — the Soldier's two Death1 frames
     * and the Infantry's twelve Death2 ones — and `cre_flashes` the muzzle
     * flashes the three flashing modules raised (q2_fx_flash). `flash_spin` is
     * gp+16996, the flash's running turn.
     */
    u32               cre_rounds;
    u32               cre_rounds_player;
    u32               cre_rounds_creature;
    u32               cre_rounds_world;
    u32               cre_launched;      /* bolts and rockets that flew */
    u32               cre_dying_shots;
    u32               cre_flashes;
    s32               flash_spin;

    /*
     * The death drops (client_cre_drop): requests the queue flushed to us,
     * items actually spawned, and requests 0x8002085C itself turned down —
     * no record, no model, no entity. Counted apart so "no creature dropped
     * anything" can be told from "the spawner refused".
     */
    u32               cre_drop_requests;
    u32               cre_drops;
    u32               cre_drops_declined;

    /* The mesh hook (client_fx_mesh): how often combat asked for an actor's
     * mesh, and how often a creature's posed mesh was there to give. */
    u32               fx_mesh_asked;
    u32               fx_mesh_given;
    u32               fx_mesh_armed;   /* ...while an effect byte was running */

    /*
     * The gib dispatcher's (client_gib_describe): the posed mesh handed to
     * 0x8005B320's spray for a creature coming apart — held here because the
     * parent points at it for the length of the dispatch, after the describe
     * callback has returned — and how many creature bodies were handed one.
     * That counts the describe, not the spray: the boss-ring arms and ARM_NONE
     * are handed a mesh and spray nothing (modelent.c). `gib_pushed` counts the
     * bodies whose +0x2F8 knockback reached the throw non-zero.
     */
    q2_fx_mesh_src    gib_mesh;
    u32               gib_mesh_posed;
    u32               gib_pushed;

    /* The music countdown, in 50 Hz ticks — 0x800B2710 and 0x800B2708. */
    s32               music_total;
    s32               music_left;
    double            music_clock;
    /*
     * THE CD VOLUME IS A PAIR, and the port used to model only half of it.
     *
     * `targetcdvol` (0x800B2E50) is where the volume is being asked to go and
     * `actcdvol` (0x800B2E74) is where it actually is; CdCheckMusic walks the
     * live one toward the target by exactly four per call and clamps it on
     * overshoot (0x800718F8..0x8007194C), and it is the LIVE value that the
     * two 64-tick track fades scale and the CD mix is built from (0x80071958).
     * Nothing in this tree used to correspond to either address: the bed was
     * scaled straight off the MUSIC slider, so the volume stepped where the
     * console slides and never moved at all when the pause menu came up.
     *
     * `actcdvol` lives in BSS and so starts at zero, which is why the first
     * track of a session swells in from silence rather than starting at level.
     */
    s16               music_target;
    s16               music_level;
    /* The ramp is per MainGameLoop iteration — one video field — not per
     * rendered frame, so it accumulates on the build's own tick rate the way
     * the countdown above does. */
    double            music_ramp_clock;
    /* Playlist starts, for `--report`: a session that works through a level's
     * seven tracks and one that plays the first for ever look identical in the
     * log otherwise. */
    u32               music_tracks;
    u32               ent_light_added;
    u32               script_lights;
    u32               pose_by_name;
    u32               pose_name_no_pos;
    u32               pose_no_name;
    u32               pose_name_absent;   /* the name is not in block D */
    u32               pose_held;   /* ...by holding the timeline's last frame */
    u32               ent_light_dropped;
    u32               ent_bursts;
    u32               burst_no_fx, burst_no_table, burst_no_model;
    u32               burst_no_bank, burst_bad_model, burst_no_verts;
    /* Faces the projectile bodies put in the table. Counted because "bolts N"
     * says how many are alive, not whether any of them reached the screen. */
    u32               proj_prims;
    /* Primitives the effect pool put in the table, for the same reason: the
     * pool's "N live" figure says nothing about whether the area routing let
     * any of them through. q2_fx_build_ot's return was discarded, so a whole
     * class of bursts could be culled in every frame with no counter moving. */
    u32               fx_prims;
    /* Debris models this map's bank yielded (0..3) and faces the live pieces
     * put in the table. Both are zero on the 30 banks with no Debris model,
     * which is the console's own answer there. */
    u32               debris_models;
    u32               debris_faces;
    u32               player_attacks;
    u32               rot_moved;
    u32               rot_steps;   /* step requests the script has made */
    /* The other thing a CALL can be: a pane of glass. Counted separately
     * because "the script ran a CALL" and "something broke" are different
     * questions, and only the second says the debris path is alive. */
    u32               glass_calls;
    u32               glass_pieces;
    q2_uf_operands    ev_operands;  /* COMMON's chunk, and the zone's */

    /*
     * LOADMAP, queued rather than acted on where it fires.
     *
     * A CALL runs inside `q2_sim_advance`, and loading a map there would free
     * the triggers and the script the runtime is standing in the middle of.
     * The zone gate is deferred for the same reason and to the same place —
     * the top of the frame — so the two transitions behave alike.
     */
    /*
     * Fire every trigger volume once, on the first simulated frame.
     *
     * The same argument `--watch` and `--dm-stage` are made of: a scripted
     * demo wanders, and the things worth testing are the ones a player reaches
     * deliberately. 28 of the disc's LOADMAP calls are behind a trigger volume
     * and a wandering pad walks into none of them in three thousand frames, so
     * without this the level transition can only be argued for, not shown.
     */
    bool              fire_triggers;
    long              fire_at_frame;
    /*
     * Re-armed after every level change, so one invocation walks the game
     * rather than one level. The interval is measured from each arrival, which
     * is what gives a map time to load, spawn and settle before its volumes
     * are fired.
     */
    long              fire_interval;

    /*
     * ONE named script entry point, rather than every trigger volume on the
     * map.
     *
     * `--fire-triggers` fires the lot, which on most maps includes a TELEPORT
     * and a LOADMAP, so the thing under test gets about four seconds before the
     * level ends underneath it. That is fine for proving a transition works and
     * useless for watching a mover run. Every Events chunk carries a named
     * directory (events.h) and the level authors name the interesting ones —
     * BIGGUN's platform record is literally called `PLATFORM` — so naming one
     * is both possible and the natural way to ask for it.
     */
    const char       *fire_event;
    bool              fire_event_done;

    /*
     * WHAT THE PLAYER TAKES THROUGH A DOOR.
     *
     * `client_load_zone` re-inits the sim, and `q2_sim_init` memsets it, so
     * before this every transition handed the player back a fresh blaster and
     * 100 health — walking through a ZONE GATE, which is a door inside one
     * level, reset the game as thoroughly as starting a new one did.
     *
     * What carries is what the save system already treats as the player's
     * rather than the level's (`save.c`): the inventory, the weapon in hand,
     * and the chaingun's spin-up count. Everything else in the sim is the
     * map's — the triggers, the script, the entity set — and is meant to be
     * rebuilt.
     *
     * The level CLOCK is the awkward one, because the powerup deadlines are
     * absolute against it. A zone gate stays inside one level, so the clock
     * carries and the deadlines need nothing. A LOADMAP starts a new level at
     * zero, so the deadlines are rebased to preserve REMAINING time rather
     * than absolute time — which is PC Quake II's behaviour on a level change
     * and is a stated choice here, since nothing in the executable has been
     * read that settles it.
     */
    bool              carry_player;    /* set by the two transition paths */
    bool              carry_same_map;

    /* The zone gate's own target name ('Zone1'), kept across the load for the
     * log. Empty for anything that is not a gate. */
    char              gate_name[24];
    q2_inventory      carry_inv;
    int               carry_weapon_id;
    int               carry_chaingun;
    s32               carry_level_time;

    /*
     * WHERE THE PLAYER WAS STANDING, taken off the sim before the load frees
     * it, because a zone gate does not move them and the new sim has to be
     * spawned exactly where the old one left off.
     *
     * `cam.pos` cannot serve: during play it holds the EYE, which is the feet
     * plus the view height, so spawning at it would lift the player by 576
     * units at every gate.
     */
    s32               carry_pos[3];
    s32               carry_vel[3];
    s32               carry_yaw;
    s32               carry_pitch;
    s32               carry_ground_y;
    bool              carry_on_ground;
    bool              carry_pos_valid;

    /*
     * A zone seam does not spawn a new player. Keep the complete client-side
     * movement/view state so the first frame in the destination continues the
     * same bob, lean, crouch, kick, look rate and pending impulse. Only the
     * collision cache (`q2_player.ent`) belongs to the zone and is retained
     * from the freshly attached destination hull when this snapshot is put
     * back. The scalar fields above remain useful to the trace log and to the
     * new-map carry path, where a full movement-state restore would be wrong.
     */
    q2_player         carry_motion;
    s32               carry_next_fire;
    s16               carry_fire_kick[3];

    /*
     * COMMON's script latches for the load now running (events_rt.h,
     * q2_event_carry): every record's flags byte and every item's latched
     * bit 7. Filled from the ENTERING map's block below and put back on the
     * fresh runtime within one client_load_zone; `size` is 0 whenever the map
     * being entered has left nothing behind.
     */
    q2_event_carry    carry_events;

    /*
     * AND THE BLOCKS THEMSELVES, one per map visited (events_rt.h,
     * q2_event_store). The console keeps these in save memory, which a level
     * boundary does not empty — the only clear in the image is the new-game
     * memset 0x8003DDB8 (mission.h, on the sibling "PlayerSave" block) — so
     * the store outlives the level, which is what makes a map walked back into
     * arrive with the script state it was left in.
     */
    q2_event_store    event_store;

    /* Non-NULL only during a saved-game load. Its latches must reach the
     * constructor/replay pass before the old session can contribute any. */
    const q2_save   *restoring_save;

    /*
     * THE MISSION SCREEN'S TWO COUNTERS, which mission.h names as the reason
     * it stayed unimplemented long after the machinery to draw it existed:
     * "Kills and Secrets are simulation state, and the sim did not tally
     * either".
     *
     * SECRETS are `INSECRET` — a UserFuncs primitive a trigger volume calls,
     * 34 of them on the disc and 33 reachable by a volume. The total is how
     * many the map's script carries; the found count is how many DISTINCT ones
     * have run. Distinct is the port's choice and is stated: the event runtime
     * fires a volume on entry rather than continuously, so walking in and out
     * of one would otherwise count the same secret twice, and a secrets figure
     * that climbs as you pace about is the one shape that is definitely wrong.
     *
     * KILLS are the creature world's: how many of the map's placed creatures
     * are dead. Both totals are per MAP, so both reset with it.
     *
     * "Per MAP" is what `zone_dead`/`zone_placed` are for. The console keeps
     * one live kills word (`0x800B29E8`, stamped into the row by `0x80022420`
     * on every death) and one live total (`0x800B29E4`, counted once out of
     * the whole of Common.Dat at `0x8007AB34`), and clears neither at a zone
     * boundary — the only writer of zero is `0x80070874`'s level reset. The
     * port cannot hold the count the same way: `client_load_creatures` frees
     * and rebuilds the creature set at every zone load, so every `dead` flag
     * and the `cre_in_zone` denominator go with it, and the tally reported
     * only the zone the player happened to be standing in.
     *
     * So each zone's pair is kept in its own slot, written as the zone is
     * LEFT, and the level's figure is the sum with the resident zone's half
     * recomputed live. Indexed by zone rather than accumulated, so walking
     * back through a gate into a zone already visited replaces that zone's
     * slot instead of counting it twice. Cleared with `secrets_found`, on a
     * real level change and not on a gate.
     *
     * `secrets_total` needs no such treatment and does not get one: it is
     * counted out of COMMON.DAT's script, which is the MAP's file — every
     * zone of one level recounts the same INSECRET items to the same number.
     */
    u32               secrets_total;
    u32               secrets_found;
    u32               secret_seen[64];   /* item offsets already counted */
    u32               secret_seen_count;
    u32               zone_dead[Q2_SAVE_LEVEL_ZONES];
    u32               zone_placed[Q2_SAVE_LEVEL_ZONES];
    /*
     * How many of those secrets had `msc_secret` in the level's bank to chime
     * with (0x80028F98). Bank presence, not voices started, so it means the
     * same thing headless as it does with a device — see the call site.
     */
    u32               secret_sounds;
    /*
     * Which of the six mission-table rows this level holds, or -1 when it has
     * none — a level outside a unit, or a seventh distinct one. Claimed on
     * ARRIVAL by name, not on departure in visit order: see
     * `client_mission_enter`.
     */
    int               mission_row;
    char              map_title[64];     /* the level's own name, `MapTitle` */
    char              secret_message[64];/* `FoundASecret`, the map's words  */
    int               map_unit;          /* from `Unit<N>Miss1`              */
    /*
     * The map's own text, kept open for the level rather than read once for
     * the briefing and dropped. STRING names a key in it, and 33 of the disc's
     * 68 STRING calls are reachable by a trigger volume — so a script that
     * wants to say something needs this to still be here when it asks. It
     * borrows `common`, which outlives the zone.
     */
    q2_leveltext      leveltext;
    bool              leveltext_ready;
    u32               script_strings;   /* STRING calls that said something */
    u32               script_sounds;    /* SIMPLESOUND calls that played    */
    u32               script_gated;     /* ONKEYDO predicates that said no  */
    /*
     * Nodes OBJDRAWOFF has hidden, one byte per Scene node. The zone borrows
     * it (world.h), so it is owned here and lives as long as the zone does.
     */
    u8               *node_hidden;
    u32               node_hidden_count;
    u32               script_hidden;    /* nodes hidden this level */
    u32               script_summoned;  /* creatures a CREBATCH woke */
    u32               script_timers;
    u32               script_disabled;  /* DISABLEME, records retired*/
    u32               script_units;     /* MISCOMPLETE, units ended  */
    u32               script_teleports; /* TELEPORT calls that moved us */
    /*
     * A queued TELEPORT. Deferred to the top of the frame like the zone gate
     * and the LOADMAP, and for the same reason: the CALL that raised it runs
     * inside the script a zone load would free.
     */
    q2_start_pos      pending_teleport[Q2_MP_MAX_PLAYERS];
    bool              pending_teleport_have[Q2_MP_MAX_PLAYERS];

    /*
     * `--zone-trace`: the instrumentation asked for after "it still happens".
     *
     * The first fix named the zone gate as the culprit on circumstantial
     * evidence and did not cure the report, which means either the gate is not
     * the only thing that moves a player without being asked to, or it is being
     * fired when it should not be. So rather than guess a second time, EVERY
     * path that can relocate the player announces itself, and a watchdog catches
     * any displacement that no path claimed.
     *
     * `move_reason` is set by whichever path is about to move the player and is
     * consumed by the watchdog on the next tick. A jump the watchdog finds with
     * no reason pending is the interesting one: it means the player was moved by
     * something that does not know it is a teleport — a collision push-out, a
     * mover carrying them, a spawn that ran twice.
     */
    /*
     * The zone's authored draw order, parsed once per load and indexed per
     * frame by the camera's PrimaryColl cell. `sort_cell` is the previous
     * frame's answer, kept as the search hint.
     */
    q2_sortdata       sortdata;
    bool              sort_ready;
    bool              use_sort;     /* authored SortData; --depth-sort opts out */
    /* --no-autoswitch: keep the disc's rule, which only leaves the blaster. */
    bool              no_autoswitch;
    bool              god;          /* --god: capture aid, no damage  */
    s32               ot_range;     /* --ot-range: sweep the sort range    */
    s32               sort_cell;

    bool              zone_trace;
    /*
     * `--report`: one machine-readable block at exit, so a scripted run has
     * something to assert on besides its exit code.
     *
     * The counters below already existed and were each printed, if at all, by
     * whichever subsystem owned them, at whichever moment it happened to be
     * finished — which is why a harness that wanted "did anything shoot" had to
     * grep prose. This gathers them in one place, in one shape, at one time.
     */
    bool              report;
    u32               loads_done;      /* client_load_zone calls that landed  */
    u32               loads_failed;    /* ...and ones that found no such zone */
    u32               loading_raises;  /* screens the transitions put up      */
    const char       *move_reason;    /* set by a deliberate relocation      */
    s32               last_pos[3];    /* the player's position last tick     */
    bool              last_pos_valid;
    u32               trace_frame;
    u32               jumps_seen;

    /*
     * The doors and lifts.
     *
     * `mover.[ch]` has been complete for a long time — the seven-state machine,
     * the three payload shapes, the displacement the zone draw already adds
     * through `q2_movers_node_offset` — and had NO CALLER. `q2_movers_build`,
     * `q2_movers_tick` and `q2_mover_trigger` were all dead, so 1,006 MOVER_A
     * items, 20 MOVER_B and 292 MOVER_C stood still. It is the same shape as
     * the rotators before #56: a finished mechanism with nothing driving it.
     */
    /*
     * The beams a level ships. Built at load because their constructor runs at
     * load — see levelbin.h — and re-queued every frame because the transient
     * pool is refilled from empty each frame (effect.h).
     */
    q2_laserbeam_set  lasers;
    u32               laser_drawn;

    /*
     * And the teleport pads, which are the same two halves: PSXTELEPORT_T
     * registers a volume at zone load and DrawTelePorts walks the list every
     * frame, right beside the two laser walks this client already makes.
     */
    q2_teleport_set   teleports;
    u32               teleport_drawn;
    u32               teleport_lights;

    /*
     * The movie table a QENDMIS map's module carries, the film it names, and
     * the end-of-mission screen shown when there is no film to play.
     *
     * The placard used to be shown INSTEAD of a movie because there was no
     * decoder. There is one now (stx.h), so a named `.STX` plays and the
     * placard is what a unit whose ending this port has not read still gets.
     */
    q2_levelbin_movie movies[4];
    u32               movie_count;
    bool              endmission;      /* this map IS an end-of-mission */
    q2_endmission     endmis;
    bool              endmis_open;
    int               endmis_unit;

    q2_movie          film;
    bool              film_open;       /* a film is loaded and running   */
    bool              film_done;       /* ...and has reached its end     */
    u8               *film_rgb;        /* the frame most recently decoded */
    bool              film_have_frame;
    u32               film_frames;
    const char       *film_arg;        /* --movie NAME: play it and stop */

    /*
     * ---------------------------------------------------------------------
     * QFMV: the level that IS a cinematic
     * ---------------------------------------------------------------------
     * The level table's records 10 and 11 are `Intro FMV` and `Extro FMV` and
     * BOTH resolve to the directory `QFMV` — 45 KB with no geometry, no font
     * and no icon sheet. Loading it is not loading a level; it is asking the
     * shared module to play a film, and WHICH film is chosen by the display
     * name the level was entered under, which the module compares against its
     * own table (levelbin.h) before calling the player:
     *
     *     if (!strcmp(screen, "Intro FMV")) play("TAKE1BP.STX", 1281, ...)
     *     if (!strcmp(screen, "Extro FMV")) play("OUTRO1P.STX", 1500, ...)
     *
     * So the screen name has to survive the map change, which is what this is:
     * `film_screen` is the name QFMV was entered under, and `film_next_map` is
     * where to go when the film is over — because a cinematic is a step on the
     * way somewhere and not a destination.
     */
    char              film_screen[16];
    char              film_next_map[64];
    bool              film_is_start;    /* the front end's own opening reel */
    bool              film_to_front;    /* ...and the intro, which opens on
                                         * the title screen rather than a map */

    /*
     * ---------------------------------------------------------------------
     * THE BOOT CHAIN — what runs before the menu
     * ---------------------------------------------------------------------
     * The port booted straight into QFRONT. The console does not: it boots
     * into two SCREENS and a film, and the menu is the fourth thing you see.
     *
     *   0x80018DA4  sw v0, 10888(at)    ; -> 0x800B2A88 = 1, at boot
     *
     * and the dispatcher's flag chain answers that with `0x80041748`, which
     * writes `"QLogos2"` into the next-map buffer. From there each screen names
     * the next by writing the game-state word through `engine+0x3AC`:
     *
     *   QLOGOS2  0x80101FA0  sh 14      -> 0x800B2A5C -> `QLogos`
     *   QLOGOS   0x801021D0  sh 12      -> 0x800B2A54 -> `Intro FMV`
     *
     * `Intro FMV` is QFMV, which plays `TAKE1BP.STX`; QFMV's own handler then
     * asks for state 6, no request flag is left standing, and the dispatcher's
     * fall-through at `0x80018B54` loads `QFront`. So the retail order is
     *
     *     Legal -> Hammerhead -> id -> Activision -> TAKE1BP.STX -> the menu
     *
     * and the intro cinematic is a PRE-MENU cinematic. (The port had it after
     * the difficulty, which is the reel's slot, not the intro's.)
     *
     * Each screen is two full-screen 8bpp images out of its map's SNDVRAM,
     * CROSS-FADED: the second begins its fade in on the frame the first begins
     * its fade out. The numbers are the modules' own, and both handlers use the
     * same shape — eight frames up in steps of 16 to 128, a hold, eight frames
     * down. See `k_boot_screens`.
     *
     * NOT SKIPPABLE ON THE CONSOLE. Neither logo module reads the pad anywhere
     * — `engine+0x2AC`, the word QFRONT's title hook tests, is never loaded in
     * all 30 KB of it — so the twelve seconds are twelve seconds. A press ends
     * the current screen here anyway, because that is what was asked for and a
     * legal screen you cannot dismiss is a worse thing to reproduce than an
     * exact one.
     */
    q2_vram_section   boot_vram;       /* the screen's SNDVRAM, while it runs */
    bool              boot_vram_open;
    u8               *boot_rgb[2];     /* its two images, decoded             */
    u16               boot_w[2], boot_h[2];
    u32               boot_index;      /* which screen of the chain           */
    u32               boot_frame;      /* frames it has been up               */
    double            boot_carry;      /* ...and the fraction of the next     */
    bool              boot_open;
    bool              boot_chain;      /* this run walks it                   */
    bool              no_boot;         /* --no-boot: and this one will not    */
    bool              boot_skip;       /* a press, taken on the next frame    */

    /*
     * NO MUSIC UNTIL THE MENU IS UP, and this is the port's problem rather
     * than the console's.
     *
     * On the console the boot chain is three LEVELS — QLOGOS2, QLOGOS and then
     * QFMV — and none of the three has a playlist: the level table's records 1,
     * 2 and 10 carry no track ids at all, so nothing is playing over the logos
     * or under the intro film, and QFront's record 0 starts the front end's one
     * looping track when the front end is loaded.
     *
     * This port loads QFRONT FIRST, before the chain, because the logo screens
     * here are two decoded images rather than levels and the front end needs
     * something to stand on when the film ends. That load took QFRONT's
     * playlist with it, so the menu track played over the legal screen, the
     * two logo pairs and the whole of TAKE1BP.STX.
     *
     * So the load that arms the chain holds the music, and
     * `client_enter_front_end` — which is where the chain ends, whether through
     * the film or past it — is what lets go.
     */
    bool              music_held;

    /*
     * THE LOADING SCREEN, and it is in front of everything while it is up.
     *
     * Raised by `client_load_zone` — every load this client makes goes through
     * that one function, which is what keeps a level change, a zone gate, a
     * restart and the front end's own arrival on the same screen. See
     * loading.h for what the screen is and where its two assets come from.
     *
     * The console defers the LOAD by a frame and shows the screen while the
     * disc turns; this port holds it for half a second AFTER a load that took
     * no time at all. The order is the one thing about it that is not the
     * console's, and it is invisible: nothing is drawn during a synchronous
     * read on either machine.
     */
    q2_loading        loading;

    /*
     * ---------------------------------------------------------------------
     * The opening reel — and the attract loop it is NOT
     * ---------------------------------------------------------------------
     * `ROGUEINP.STX` is the third film on the disc — 28.8 MB, 2,459 frames,
     * the fleet approaching the Strogg homeworld — and it is named by NO
     * movie-table record, because the table's filename field is twelve bytes
     * and "ROGUEINP.STX" needs thirteen with its terminator. It is a literal
     * at QFRONT's module+0xDC4 and the front end plays it itself.
     *
     * THIS PORT PLAYED IT AS AN IDLE ATTRACT REEL. IT IS NOT ONE. The title
     * screen does have an idle countdown, and it does not lead here: they are
     * two different stores in the same module, and what separates them is
     * following each one to its CALLER.
     *
     *   the title's idle   module+0x12DC0, parked with 9000 by the title page
     *                      builder 0x8010CEE0 and counted down by the page
     *                      hook 0x8010C6AC, which resets it to 9000 whenever
     *                      engine+0x2AC says the pad moved. At zero it calls
     *                      0x80101B08 — and that function plays NO FILM. It
     *                      hides the five title objects, installs the one-row
     *                      page `"DEMO OF GAME"` (module+0xC) and hands off.
     *                      9000 of the console's 1/300 s units is the thirty
     *                      seconds this port had, and what waits at the end of
     *                      it is a DEMO OF THE GAME, which this port has no
     *                      player for. So the countdown is gone rather than
     *                      pointed at the wrong film.
     *
     *   the reel's beat    module+0x12D90, parked with 150 by 0x80101E4C and
     *                      counted down by the page hook 0x80101CD0:
     *
     *     80101E54  addiu v1, zero, 150
     *     80101E6C  jal   0x80103414
     *     80101E70  sh    v1, 11664(v0)     ; -> module+0x12D90, delay slot
     *     ...
     *     80101CF4  lhu   v0, 0x2D90(a0)    ; the beat
     *     80101D00  subu  v0, v0, v1        ; ...minus the frame delta
     *     80101D0C  bgez  v0, +0x128        ; still counting: do nothing
     *     80101D34  addiu a0, a0, 0xDC4     ; "ROGUEINP.STX"
     *     80101D38  addiu a1, zero, 2457
     *     80101D4C  jal   0x8010B2EC        ; play it
     *
     * The store hid its writer in a DELAY SLOT eight instructions below its own
     * `lui`, which is why a scan that pairs a `lui` with a nearby load or store
     * found only the reader and read the whole thing as an idle timeout with an
     * unrecoverable threshold. It has a writer, and the writer has three
     * callers: 0x8010D380, 0x8010D3A8 and 0x8010D3D4 — the EASY, MEDIUM and
     * HARD records at module+0xEFE4 / +0xEFFC / +0xF014. Each stores its skill
     * into engine+0x366 and calls 0x80101E4C, which arms 150, hides the same
     * five title objects and installs the countdown as the page hook.
     *
     * So the reel is what plays WHEN A DIFFICULTY IS CONFIRMED, and 150 of the
     * same 1/300 s units is HALF A SECOND — the beat between the title going
     * away and the film starting. Nothing about it is this port's invention any
     * more, threshold included.
     *
     * One more thing this cost, and it is worth recording. `0x8010D5F8` — a
     * standalone `play("ROGUEINP.STX", 2457, ...)` and nothing else, which is
     * what `docs/FORMATS.md` recorded as the attract reel's call site — has
     * ZERO references in all 118 KB of the module. It is an earlier build's
     * entry point left in, and it looks exactly like the live one.
     */
    double            start_beat;      /* 1/300 s units left of the beat  */

    /* The map's own mission-event namespace, out of its LevelBin. */
    q2_levelbin_misevent misevent[32];
    u32               misevent_count;
    u32               misevents;      /* MISEVENT calls run           */
    u32               misevent_exe;   /* ...naming an EXE event       */
    u32               misevent_map;   /* ...naming one of this map's  */
    u32               misevent_unknown;
    char              misevent_last[Q2_UF_NAME_LEN + 1];

    /* --at / --yaw: stand somewhere specific, for capture. */
    s32               at[3];
    s16               at_yaw;
    bool              at_given;
    bool              yaw_given;
    s16               at_pitch;
    bool              pitch_given;
    bool              no_lasers;   /* --no-lasers: the before picture */
    bool              shoot;       /* --shoot: hold fire              */
    /* --shoot-to-kill: hold fire until the first creature dies, then let
     * go, so its death plays out instead of being shot past gib_health. */
    bool              shoot_to_kill;
    long              save_load_at; /* --save-load N                  */
    bool              show_credits; /* --credits                      */
    bool              start_new_game; /* --new-game                   */
    bool              all_keys;    /* --keys: every key in the pocket */

    /*
     * `--armour <class>`: put a class of armour on the player, the way `--keys`
     * puts keys in the pocket. The status bar's armour field is a five-way
     * select on the inventory flag word and there is no other way to reach four
     * of the five arms from a headless run — a scripted player cannot go and
     * find a Body P, and three of the four armour items are placed on maps the
     * demo pad never reaches. Zero means "leave the inventory alone".
     */
    u32               give_armour_flag;
    s16               give_armour_points;
    s16               give_armour_cells;

    /* `--powerup <kind>` holds one of the four thirty-second effects for a
     * deterministic headless HUD capture. -1 leaves retail gameplay alone. */
    int               give_powerup;

    /*
     * `--weapon N`: own every weapon, hold slot N, and keep it fed. The eleven
     * fire functions are all transcribed and only ONE of them can be reached
     * from a headless run — a player who spawns with the blaster and cannot go
     * and find a shotgun. So the hitscan path, the rail, the grenades and the
     * BFG had no way to be looked at at all. Zero leaves the inventory alone.
     */
    int               give_weapon;

    /*
     * The launch loadout (loadout.h): what a player reaching `--map`'s level
     * through the campaign would be carrying. Derived once at launch and
     * handed out on every fresh load of THAT level -- the first and each
     * restart -- because a fresh load re-runs q2_sim_init, which would put
     * the blaster alone back in their hands. A level reached from it by
     * LOADMAP carries what the player really has instead. `--no-loadout`
     * starts with the blaster, as the bare level start does.
     */
    bool              no_loadout;
    bool              launch_loadout_valid;
    char              launch_loadout_map[Q2_UF_NAME_LEN + 1];
    q2_inventory      launch_loadout;

    q2_mover_set      movers;
    bool              movers_ready;

    /*
     * The train's two RUMBLES — not voices, which this used to call them.
     *
     * PLATFORM asks SetShockData for ShockData_Explosion (13) while it moves
     * and ShockData_Stomp (14) when it arrives (mover.h, shock.h). They are
     * TAKEN every tick so the latch cannot stick, counted, and handed to every
     * player's pad through the positional arm of `client_shock`.
     */
    u32               train_move_calls;
    u32               train_stop_calls;

    /*
     * THE DUALSHOCK — GameWindows[p]+732 and +764, one per player, and the
     * field clock that drives ProcessShockData.
     *
     * The console runs ProcessShockData from the vertical-blank ISR, once a
     * field, adding PalNtscRate to each slot's time. Here the sim's ticks are
     * banked in `shock_clock` and spent a field (`dt_per_field`) at a time, so
     * the patterns play at the console's rate whatever the host's frame rate.
     * `shock_out` is the last field's pair of motor bytes — little, then big,
     * as PadSetAct takes them — and `shock_sent` what the pad was last told,
     * so an unchanged pair is not re-sent every frame.
     */
    q2_shock          shock[Q2_SIM_MAX_PLAYERS];
    s32               shock_clock;
    u8                shock_out[Q2_SIM_MAX_PLAYERS][2];
    u8                shock_sent[Q2_SIM_MAX_PLAYERS][2];
    bool              shock_live;       /* the world ran this frame        */
    u32               shock_requests;   /* SetShockData calls, all players */
    u32               shock_taken;      /* ...that a slot accepted         */
    u32               shock_fields;     /* ProcessShockData samples, p0    */
    u32               shock_big_fields; /* ...with the big motor running   */
    u32               shock_little_fields; /* ...and the little one on     */
    u32               shock_by_id[Q2_SHOCK_PATTERNS + 1]; /* requests, by id */
    u32               mover_triggers;   /* items the script reached */
    u32               mover_moved;      /* ticks on which one moved */
    u32               conveyor_steps;   /* BASE0 DOCRATES object writes */
    u32               mover_sounds;     /* transitions that made a noise */
    u32               rot_sounds;       /* ...and the hatches' own       */
    /*
     * The motor loop, on both ends. A START is counted when the bank carries
     * `pt1__mid` — not when a voice starts, which is never true headless — and
     * a STOP every time one is drained, so the two must balance once every
     * hatch has arrived. 0x8002B3DC and 0x8002B568.
     */
    u32               rot_loop_starts;
    u32               rot_loop_stops;
    /* ROTBUTTON's amb_butn2, a press or a release, counted on the bank
     * carrying it as the motor is: a headless run starts no voice. */
    u32               rot_clicks;
    /* Locked doors that named their key on the centre line (0x800254EC).
     * Counted where the request is drained rather than where it is raised, so
     * it reports sentences PUT ON SCREEN and not refusals reached. */
    u32               key_prompts;
    /* Lines put on the centre line by a script — STRING (0x8002B930) and the
     * secret handler (0x80028F90), both of which reach `centreprintf`. Counted
     * where the buffer is written, so it reports lines the player is shown and
     * not calls decoded. A RUN counter, not a per-map one, for the same reason
     * `key_prompts` is: a map change is exactly when it would otherwise be
     * thrown away, and every capture that fires a script crosses one. */
    u32               centre_lines;
    /* Transitions whose voice did NOT start: no audio device, or the name is
     * not in this map's bank. Counted apart so a headless run cannot be read
     * as proof that anything was heard. */
    u32               mover_sounds_missed;
    /* The module's own pain/die callbacks, which had no caller until they had
     * somewhere to be installed. `move_via_set` cannot see these: it only
     * counts the GENERIC fallback dispatcher, so a creature with a real
     * implementation reads zero there however often it is hurt. */
    u32               cre_pain_calls;
    u32               cre_die_calls;
    u32               breakable_opened; /* doors opened by being shot   */

    /*
     * The map's `func_explosive` groups — opcode 0x08, explosive.h.
     *
     * The SET lives here rather than in the sim for the same reason the mover
     * set does: destroying a group changes which Scene nodes DRAW, and the hide
     * array is this side's. The sim borrows the pointer so a shot can reach it,
     * and hands back visibility changes through `q2_sim_next_node_vis`.
     */
    q2_explosive_set  explosives;
    bool              explosives_ready;
    u32               explosive_boxes;     /* shootable parts registered  */
    u32               explosive_scripted;  /* groups a script blew up     */
    u32               explosive_vis;       /* node show/hide changes made */
    /* Detonations whose report this map's bank CARRIES, and ones it does not.
     * Deliberately not "played": a headless run has no audio device and would
     * report every one of them as missing — the mistake the creature sound
     * counter already made and now documents. */
    u32               ent_drawn;      /* entity draws the OT accepted   */
    u32               ent_no_model;   /* ...and ones the bank could not resolve */
    u32               ent_outside;    /* ...and ones no cell of the zone holds  */
    u32               ent_faces;
    u32               ent_shadows;    /* retail posed-footprint FT4s emitted */
    u32               explosive_sounds;
    u32               explosive_sounds_missed;
    /* The same pair for `amb_brkglas`, drained from the sim's smash queue. */
    u32               glass_sounds;
    u32               glass_sounds_missed;
    /* And for the Explosion model's own bang, SoundByCreature at 0x8005AA54,
     * drained from the sim's `explosion_sounds`: a second voice at a brush,
     * and the only one a rocket, a grenade or a boss's gib arm makes. */
    u32               explosion_sounds;
    u32               explosion_sounds_missed;
    /* Every append ProcessTimedLights made, and every world vertex
     * ProcessMobileLights repainted, over the whole run. */
    u32               timed_lights_emitted;
    u32               world_lit_points;
    /* Corners a mobile light reached whose point's four slot bytes do not name
     * them — the console cannot paint those either. */
    u32               world_lit_unslotted;
    bool              mission_after_map; /* the screen is holding a LOADMAP */
    /*
     * Has this level put a frame on the screen yet? Cleared by every zone load
     * and set once the first frame is composed, so a script call can tell "the
     * player walked into this volume" from "this volume contains the spawn
     * point". The objective board uses it — see the HELPCOMPUTER arm.
     */
    bool              level_frames_drawn;

    /*
     * How many creatures this ZONE placed, fixed when the map loads. The kill
     * tally's denominator — see client_level_tally for why it cannot be
     * recomputed from the live set.
     */
    u32               cre_in_zone;

    /*
     * The eleven item sounds as THIS map's bank can actually play them, with
     * the console's substitutions applied. See client_item_sounds_resolve.
     */
    char              item_sound[Q2_CLIENT_ITEM_SOUNDS][Q2_ITEM_MODEL_LEN + 1];

    u32               mission_frames;
    u32               briefing_frames;

    /*
     * gp+362 (0x800AE76A) — the tally board's dismissal lock, in the console's
     * own 1/300 s units. Seeded 1200 in .data and re-armed only by the
     * dismissal, which is why it is initialised once here and never at the
     * raise. See `q2_mission_pad_end`.
     */
    s32               mission_gate;
    /* The pad record's previous FIRE, latched every tally frame whether or not
     * `PadEndInterMission` got as far as reading it — the console's edge is
     * against 0x800D5EDC+16, which the pad reader fills regardless. */
    bool              mission_fire_prev;

    bool              map_change_pending;
    char              pending_map[Q2_UF_NAME_LEN + 1];
    char              pending_start[Q2_UF_NAME_LEN + 1];

    /*
     * The arrival point: the StartPos a level is entered at, by name. The
     * console keeps it at 0x800C8CD0 -- a LOADMAP writes its operand there and
     * a fresh start (MISCOMPLETE's 0x8002DC68 among them) writes "Default".
     * A load that does not carry the player stands them on it when it lies
     * in the zone being loaded, so a restart returns to the doorway the level
     * was entered by rather than to whichever StartPos the file lists first.
     */
    char              arrival[Q2_UF_NAME_LEN + 1];

    /*
     * Which primitive queued it, because they do not leave by the same door.
     *
     * A LOADMAP is state 2 and nothing else happens: the outer state machine
     * loads what the primitive named (screen.h). A MISCOMPLETE is state 7,
     * which holds the tally board first and only then writes a destination of
     * its own. `unit_over` is that difference.
     */
    bool              unit_over;

    /*
     * Where the unit's last level was ALSO pointing, kept across the
     * end-of-mission screen.
     *
     * A unit's last level carries both primitives, and on the console the
     * MISCOMPLETE simply overwrites the LOADMAP's destination with
     * `EndMission N` — the next unit's first level is then `QENDMIS<N>`'s own
     * module's business, and this port does not run that module. Rather than
     * end the campaign at every unit boundary, the port carries the LOADMAP's
     * destination here and continues to it when the placard is dismissed.
     * STATED as the port's choice: nothing read from the executable says the
     * console gets there this way, only that it gets there.
     */
    char              unit_next_map[Q2_UF_NAME_LEN + 1];
    char              unit_next_start[Q2_UF_NAME_LEN + 1];
    bool              endmis_await;   /* a placard with somewhere to go */
    u32               endmis_frames;  /* headless release, as the board has */

    u32               map_changes;    /* how many the session has made */
    u32               vw_events;
    s16               vw_last_event;

    /*
     * Player 0's shots, counted off the sim's serial rather than off its latch,
     * so one pull is one count no matter how many frames pass before the next.
     * `mp_shots[]` is the same figure for players 1..3 and its loop starts at 1;
     * player 0 does not go through `q2_sim_advance_player` and so was never
     * counted at all.
     *
     * The point of having them is the comparison in the shot report:
     * `vw.fires_started` should equal `shots_fired`, and any other answer means
     * the view model is being told about shots that did not happen.
     */
    u32               shots_fired;
    u32               shots_dry;

    /* "Selected <weapon>" lines posted to the overlay, which the console
     * raises from the cycle routine 0x8004ECB4 and nowhere else. A harness
     * number: it should equal the number of weapon-cycle presses and nothing
     * else, so a pickup or a spawn adding to it is this defect coming back. */
    u32               weapon_lines;

    int               cre_last_sound;
    /* ------------------------------------------------------------------- */
    /* The multiplayer session. QMULTI.C is a per-map LevelBin module and the
     * engine only carries the hook, so this is what stands in for the module
     * being installed: the rules ran nowhere before it. */
    bool              mp_enabled;
    q2_mp_session     mp;
    u32               mp_spawn_count;
    u32               mp_rng_state;
    s32               mp_level_time;   /* 0x800AEBAC, in dt units             */
    q2_mp_request     mp_last_request;
    bool              mp_reported;

    u32               mp_deaths;      /* kills fed to the session              */
    bool              mp_scoreboard;  /* QMRESULT is up                        */
    /* Frames the end-of-match banner was drawn on. It is the one number that
     * says the words reached the screen rather than the log. */
    u32               mp_banner_frames;

    /* Creatures plus the other players, rebuilt per player. */
    q2_actor         *mp_target[Q2_CLIENT_MAX_TARGETS];
    q2_actor         *mp_world_target[Q2_CLIENT_MAX_TARGETS];
    bool              mp_targets_logged;
    bool              mp_stage;
    u32               mp_shots[Q2_MP_MAX_PLAYERS];
    u32               mp_dry[Q2_MP_MAX_PLAYERS];
    bool              mp_dead[Q2_MP_MAX_PLAYERS];

    /*
     * THE BODY, which `mp_dead` above is not: that is the scoring latch and
     * nothing else. This is the chain 0x800396AC starts — the death move, the
     * corpse's five seconds, the fade, and the two different ends single player
     * and deathmatch give it. See playerdeath.h.
     */
    q2_player_death   death[Q2_MP_MAX_PLAYERS];
    /* 0x800B2A10: armed by a single-player death, and when it runs out the
     * console loads QFRONT by itself. */
    s32               death_abandon;
    bool              death_abandoned; /* it ran out; the main loop acts    */
    u32               death_bodies;   /* bodies that reached the fade          */
    u32               death_gibs;
    u32               death_respawns;
    /* The zone's MultiSpawn points, kept past the load so a respawn has
     * somewhere to put the player back. */
    q2_mp_spawn       mp_spawns[Q2_MP_MAX_SPAWNS];
    /* The level start's loadout, kept so a respawn can hand it back: the
     * console builds a whole new player entity (0x8003B250), which clears the
     * client record (0x8003B2BC, 224 bytes) and runs the spawn loadout
     * (0x8003D4FC), and this is the port's side of that. 0x8003B040, which
     * the respawn also calls (0x8003DE38), is the ALL WEAPONS grant: 0x8003B070
     * branches to its epilogue unless bit 0x20 of 0x800B29EC is up. */
    q2_inventory      mp_start_inv;
    int               mp_start_weapon;
    bool              mp_start_valid;
    /* 0x800B335D, the byte the death page's middle row greys itself on and
     * the only thing 0x8001FF0C ever writes. It is BSS on the console, so it
     * starts at zero and the row starts greyed; `--continues N` seeds it. */
    int               continues;
    int               trace_cre;      /* creature index to trace, -1 for none  */
    u32               trace_ticks;
    s32               trace_prev[3];  /* the traced creature's last position */

    /* Where each player's viewport looks from. Player 0's is the sim's. */
    s32               mp_view_pos[Q2_MP_MAX_PLAYERS][3];
    s16               mp_view_yaw[Q2_MP_MAX_PLAYERS];
    bool              mp_view_valid[Q2_MP_MAX_PLAYERS];
    u32               cre_bodies;     /* deaths that found a death move        */
    u32               cre_drawn;      /* creatures with faces in the last view */
    u32               cre_faces;
    u32               player_drawn;
    u32               player_faces;
    s32              *cre_home;      /* where each creature spawned          */
    s32              *cre_cell;      /* link +0xA2: its last SecondaryCol cell */
    s16              *cre_area;      /* link +0x9E: the area it is drawn in   */

    /* The map's CLUT split. A model face's palette index is offset by it —
     * model palettes live in the second section of the array (model.h §233). */
    u32              clut4_count_a;

    psx_ot           ot;
    gte_state        gte;
    q2_screen        screen;
    psx_vram        *vram;
    psx_raster_opts  opts;

    SDL_Window      *window;
    SDL_Renderer    *renderer;
    SDL_Texture     *texture;

    int              width, height;

    /*
     * How the 512x248 buffer is fitted into the window. The default is 4:3, the
     * shape the game is captured and played at; it is NOT the
     * one-buffer-pixel-to-one-window-pixel a framebuffer dump suggests, which is
     * a 1.5x horizontal stretch. V cycles it.
     */
    q2_screen_fit    fit;

    /*
     * ---------------------------------------------------------------------
     * The mouse
     * ---------------------------------------------------------------------
     * The console supports one — three of the nine control styles are its, and
     * the CONTROLLER page has a USE MOUSE row — so mouselook is not an invented
     * feature here. What is the port's is the plumbing: turning a host's
     * DISPLACEMENT into the rate the look path wants, and pointing at a menu
     * the console could only walk with a d-pad.
     *
     * `look_acc_*` are motion that has arrived and not yet been handed to a
     * tick, in window pixels, sign-corrected to the pad's axes. They are
     * doubles because the conversion divides by the tick's own step and the
     * remainder has to survive: truncating it every frame would quietly lose a
     * fraction of every movement, which reads as a mouse that drifts short.
     */
    bool             mouse_look;      /* wanted at all — never headless      */
    bool             mouse_grabbed;   /* relative mode is on right now       */
    double           look_acc_x;
    double           look_acc_y;

    bool             mouse_left;      /* held, this frame                    */
    bool             mouse_right;
    bool             mouse_left_prev; /* so a click has an edge              */
    bool             mouse_right_prev;
    bool             menu_esc_back;   /* Esc on a sub-page: one TRIANGLE     */

    /*
     * The wheel, queued rather than applied. Every pad bit it feeds — weapon
     * next and previous in play, the cursor in a menu — is tested as a PRESS
     * EDGE, so a notch has to be one frame on and one frame off however fast
     * the wheel is spun. Positive is up.
     */
    int              wheel_queue;
    bool             wheel_gap;

    /* Where the pointer is, in window pixels, while it is not grabbed. */
    float            pointer_x, pointer_y;
    bool             pointer_valid;

    /*
     * What the left button went down on, held until it comes up: a press
     * belongs to the row it started on however far the pointer then travels,
     * which is what makes a slider draggable and stops a click sliding off one
     * control onto another.
     */
    int              menu_click_index;
    u8               menu_click_part;  /* q2_menu_hit_part                   */

    /*
     * The title screen. `QFRONT` is a real level — the level table's record 0 —
     * so the front end is that level loaded with the menu's page 46 over it,
     * not a page of art. While it is up the simulation does not run: there is
     * no player in it.
     */
    /*
     * Rotating brush geometry — ROTHATCH, SIMROT, SIMROT2, ROTBUTTON. The
     * builder and the integrator have both existed since rotator.[ch] was
     * written and the only caller was an inspector command, so nothing in the
     * game ever turned. The set borrows the zone, which draws through it.
     */
    q2_rotator_set   rotators;
    bool             rotators_ready;

    /*
     * The zone's lights, for everything that is not the world. The world's own
     * lighting is baked into MapMod's per-corner RGB and nothing at runtime
     * touches it (FORMATS §17), so this exists to shade MODELS — the items and
     * the creatures — which the client had been drawing at a flat glow tint
     * because it passed NULL for the light world.
     */
    q2_light_list    lights;
    q2_spacelights   spacelights;
    q2_light_world   light_world;
    bool             lights_ready;

    /* The title screen's two wandering lights keep their x across frames, in
     * the module's own stores at +0x11664 and +0x11668. See
     * q2_levelbin_scene_lights. */
    s32              scene_wander[2];

    bool             in_front_end;

    /*
     * VIEW CREDITS. The words are QFRONT's own roll (levelbin.h); the layout —
     * a centred scroll — is this port's, and is marked so at the reader.
     */
    bool             credits_open;
    const char      *credits[Q2_LB_CREDITS_MAX];
    u32              credits_count;
    s32              credits_scroll;
    char             first_map[64];

    bool             show_glint;
    bool             force_underwater;   /* F3 — stands in for a water volume */

    /*
     * P, or `--coords`: the view's own world position and angles, along the
     * top of the frame.
     *
     * A reader's tool and nothing the console has. It answers the question
     * every capture in this project starts with — where am I standing, and
     * which way am I looking — in the units `--at`, `--yaw` and `--pitch`
     * take, so a spot found by walking can be handed straight to a headless
     * run. Off unless asked for, and drawn nowhere near the notification
     * column so it cannot cover a pickup line.
     */
    bool             show_coords;
    bool             running;

    /*
     * ---------------------------------------------------------------------
     * Running without a window
     * ---------------------------------------------------------------------
     * The whole of the game's per-frame work — the sim tick, the screen's
     * viewport build, the world draw, the ordering table, the rasteriser, the
     * HUD and the menu — happens before a single SDL call. Only the last
     * twenty lines of `client_frame` need a renderer, so the frame loop can run
     * with none, at a fixed step, and write its framebuffer out.
     *
     * That is not a convenience. It is the only way the CLIENT's own wiring can
     * be checked the way `q2psx-inspect` checks the libraries: the inspector
     * composes its own frames and so cannot catch anything that goes wrong
     * between the client's systems — a table loaded after the thing that reads
     * it, a model never bound, a screen never fed.
     */
    bool             headless;
    bool             demo;               /* drive the pad from a script     */
    bool             watch;              /* frame the nearest live creature  */
    u32              watch_hold;         /* ...and stay on a kill this long   */
    u32              watch_slot;         /* framed creature + 1; 0 is none    */
    u32              watch_hold_left;    /* frames of that hold still to run  */
    q2_world_stats   shot_stats;         /* what the last viewport drew     */
    long             frame_index;
    long             frames_total;       /* 0 = run until the window closes */
    long             shot_every;         /* 0 = only the last frame         */
    const char      *shot_path;          /* NULL = do not capture           */

    /*
     * `--shot-display`: the same frames as `--shot`, but placed — what the
     * television is shown rather than what the console drew. The only way to
     * see the presentation path, since `--shot` writes the buffer before it.
     */
    const char      *shot_display_path;

    /* `--screen-pos X Y`, an override for the SCREEN POSITION setting, the way
     * `--crosshair` overrides its own. Lets a capture show the page's effect
     * without driving the menu. */
    bool             screen_pos_set;
    int              screen_pos_x, screen_pos_y;

    /* `--scale`: the window's width in buffer pixels. Kept here so a headless
     * `--shot-display`, which has no window to measure, writes the same size a
     * windowed run would show. */
    int              win_scale;
    long             shots_written;
} client;

static void client_bind_view_model(client *c, int pi);
static void client_reset_view_model(client *c, int pi);
static void client_bind_player_models(client *c);

/* Defined with the movie player, and called from the zone load that finds a
 * QENDMIS map naming a film. */
static bool client_film_start(client *c, const char *name);
static void client_film_stop(client *c);
static void client_film_tick(client *c, float dt);

/* Defined with the rest of the sound path, and called from the tick. */
/*
 * The pickup particle burst — 0x8005B6C0, which is a four-line wrapper around
 * the shared spawner at 0x8005AB70:
 *
 *     q2_burst(pos, ramp 10, ramp 0, size 6144, area 0)
 *
 * The two pointers it passes are `0x8009BF88` and `0x8009BA60`, and the second
 * is the ramp table itself (fxtables.h, nineteen 132-byte records), so their
 * difference of 1320 makes the first ramp index 10.
 *
 * Its fifth argument is zero, which selects the spawner's second branch:
 * **life 32**, and a velocity per component of
 *
 *     v = ((rand() - 16384) * 3) / 16384        ; truncating toward zero
 *
 * — the `sll 1 / addu / bgez +16383 / sra 14` at 0x8005AC4C, giving a drift of
 * plus or minus three.
 *
 * And the COUNT is not a constant, which is why this could not be one of the
 * port's seven presets: 0x8006D6AC totals the model's vertices across its
 * eight-byte part records (`num_verts` at +3) and the caller divides by fifteen.
 * A bigger item bursts bigger, in proportion to its own mesh.
 */
static void client_pickup_burst(client *c, const s32 pos[3], s32 model_index)
{
    q2_model mdl;
    s16 vel[Q2_FX_GROUP_QUADS][3];
    u32 total = 0, count, k;

    /* Counted, not assumed: an early return here is silent otherwise, and
     * "0 drawn" would read as "no bursts happened". */
    if (!c->sim[0].fx_ready)      { c->burst_no_fx++;    return; }
    if (!c->fx_tables_ready)      { c->burst_no_table++; return; }
    if (model_index < 0)          { c->burst_no_model++; return; }
    if (!c->model_bank_ready)     { c->burst_no_bank++;  return; }

    if (q2_model_get(&c->model_bank, (u32)model_index, &mdl) != Q2_OK) {
        c->burst_bad_model++;
        return;
    }

    for (k = 0; k < mdl.hdr.num_parts; k++) {
        q2_model_part part;

        if (q2_model_get_part(&mdl, k, &part))
            total += part.num_verts;
    }

    count = total / 15;
    if (count == 0) {
        c->burst_no_verts++;
        return;
    }
    if (count > Q2_FX_GROUP_QUADS)
        count = Q2_FX_GROUP_QUADS;

    for (k = 0; k < count; k++) {
        int a;

        for (a = 0; a < 3; a++) {
            s32 v = ((s32)q2_rng_next(&c->sim[0].combat.rng) & 0x7FFF) - 16384;

            v *= 3;
            if (v < 0)
                v += 16383;
            vel[k][a] = (s16)(v >> 14);
        }
    }

    /*
     * THE AREA, which used to be a literal 0 here and is what kept every
     * pickup burst off the screen in any zone with a SortData stream.
     *
     * 0x8005AB70 passes entity+0x9E — `lbu v0, 158(s6)` at 0x8005AD28 — into
     * the THIRD spawner, and that spawner resolves it: 0x8002FED0 calls
     * 0x800686C4, which returns the argument when it is non-zero and otherwise
     * looks the point up in PrimaryColl and takes the cell's byte +32
     * (`lbu v0, 32(v0)` at 0x80068710), storing the answer into group+0xC7 at
     * 0x8002FEDE. This function has no entity to read +0x9E off, so it does
     * what the else-arm does, against the same hull. Without it the group draw
     * culls the burst outright: no collision cell on the disc carries area 0.
     */
    {
        q2_coll_node cell;
        s32 node = -1;
        u8  area = 0;
        q2_collision *hull = c->sim[0].coll_primary_ready
                                 ? &c->sim[0].coll_primary
                                 : (c->sim[0].coll_ready ? &c->sim[0].coll
                                                         : NULL);

        if (hull)
            node = q2_coll_find_node(hull, pos, -1, true);
        if (node >= 0 && q2_collision_get_node(hull, (u32)node, &cell))
            area = (u8)(cell.contents & 0x7F);

        q2_fx_group_spawn(&c->sim[0].fx, pos, vel, count,
                          q2_fx_ramp_at(&c->fx_tables, 10),
                          q2_fx_ramp_at(&c->fx_tables, 0), 32, 6144, area);
    }
    c->ent_bursts++;
}

static void client_entity_events(client *c);

/* ------------------------------------------------------------------------- */
/* Movers meeting the player                                                  */
/* ------------------------------------------------------------------------- */
/*
 * The two things a solid door has to be able to do beyond standing still.
 *
 * BLOCKED: 0x80025CBC asks its integrator whether the step it is about to take
 * sweeps through an entity, and stops, reverses or crushes on the answer. The
 * port's whole state machine for that was decoded and unreachable because
 * nothing ever answered the question. This is the answer, for the one entity
 * this port has: the player.
 *
 * TOUCH: 65 of the disc's 1,006 MOVER_A items carry a non-zero +20, which
 * mover.h records as "also opens on touch". Nothing read it.
 *
 * Both work in the ENTITY ORIGIN frame — the mover boxes are world-space and
 * the player's `pos` is the feet — and both use the same 286/285/286
 * half-extents the engine's own overlap test does (trace.h).
 */
typedef struct client_mover_ctx { client *c; } client_mover_ctx;

/* The player's box, in the frame the mover boxes are in. */
static void client_player_box(const client *c, s32 lo[3], s32 hi[3])
{
    s32 o[3];

    o[0] = c->sim[0].player[0].pos[0];
    o[1] = q2_sim_origin_y(c->sim[0].player[0].pos[1]);
    o[2] = c->sim[0].player[0].pos[2];

    lo[0] = o[0] - Q2_CONTENTS_HALF_XZ;
    lo[1] = o[1] - Q2_CONTENTS_HALF_Y;
    lo[2] = o[2] - Q2_CONTENTS_HALF_XZ;
    hi[0] = o[0] + Q2_CONTENTS_HALF_XZ;
    hi[1] = o[1] + Q2_CONTENTS_HALF_Y;
    hi[2] = o[2] + Q2_CONTENTS_HALF_XZ;
}

/*
 * Does any part of mover `index`, swept by `step` along its own axis, overlap
 * the player? The SWEPT box, not the current one — a door moving faster than
 * the player is wide would otherwise step straight over them.
 */
/*
 * The world-space centre of a Scene node, which is where a sound belonging to
 * that node comes from. A node's bounding box is already in world space
 * (scene.h), so this is its middle.
 */
static bool client_node_centre(const client *c, s32 node, s32 out[3])
{
    q2_scene_node n;
    int k;

    if (!c || node < 0 || (u32)node >= c->zone.scene.node_count)
        return false;

    /*
     * `scene.nodes` IS NOT AN ARRAY OF `q2_scene_node`. It is the borrowed
     * chunk — 52 raw bytes a record, decoded by `q2_scene_get_node` (scene.h)
     * — and this used to index it as a struct array, which compiles with a
     * warning and reads whatever the stride mismatch lands on. Node 31 of
     * BIGGUN's zone 2 came back as (-5240449, 715263, -77568).
     *
     * The only consumer until now was the rotator sound, so every turning
     * hatch on the disc has been playing from a position several thousand
     * screens away — silently correct-looking, because `client_play_sound_at`
     * attenuates it to nothing and a missing sound is what a rotator with no
     * bound node is supposed to do anyway.
     */
    if (!q2_scene_get_node(&c->zone.scene, (u32)node, &n))
        return false;

    for (k = 0; k < 3; k++)
        out[k] = (n.bbox_min[k] + n.bbox_max[k]) / 2;
    return true;
}

static bool client_mover_blocked(u32 index, const s32 step[3], void *user)
{
    client_mover_ctx *ctx = (client_mover_ctx *)user;
    client *c = ctx->c;
    s32 plo[3], phi[3];
    u32 t;

    if (!c->movers_ready || index >= c->movers.count)
        return false;
    if (!c->movers.movers[index].blocks_player)
        return false;
    if (!c->sim[0].mover_count)
        return false;

    client_player_box(c, plo, phi);

    for (t = 0; t < c->sim[0].mover_count; t++) {
        const q2_move_target *mt = &c->sim[0].volumes[t];
        s32 lo[3], hi[3];
        int k;

        if (!mt->active || mt->id != (s32)index)
            continue;

        /*
         * The box the mover is about to sweep through, grown along EVERY axis
         * it is moving on. A door or a lift fills one component of `step` and
         * this is the axis-aligned grow it always was; a train (mover.h) fills
         * three, and taking only one of them tested a corridor the platform
         * never travels down.
         */
        for (k = 0; k < 3; k++) {
            lo[k] = mt->min[k];
            hi[k] = mt->max[k];
            if (step[k] > 0) hi[k] += step[k];
            else             lo[k] += step[k];
        }

        if (!q2_move_box_overlap(plo, phi, lo, hi))
            continue;

        /*
         * A RIDER IS NOT AN OBSTRUCTION, and this is what broke the lifts.
         *
         * The test above is "does the player's box overlap the box this mover
         * is about to sweep into", and a player STANDING ON a lift overlaps it
         * by definition — the swept box grows along the axis it is travelling
         * and the rider is sitting on that face. So every vertical mover with
         * somebody on it reported blocked on its first step, went to
         * Q2_MV_BLOCKED, and never moved: the start sound played and the
         * platform stayed where it was.
         *
         * That surfaced when the invented `Q2_MV_BLK_IGNORE_OPENING` came off
         * the CALL primitives. The flag was wrong — only MOVER_A has it — but
         * it had been hiding this, because ignoring obstruction entirely also
         * ignores the rider.
         *
         * The distinction the console gets from carrying the entity, this gets
         * geometrically: +Y is down, so the player's FEET are `phi[1]` and the
         * mover's TOP face is `mt->min[1]`. Feet at or above that face, within a
         * step, means the player is standing ON the mover and is carried by it.
         * Anything else — beside it, under it, embedded in it — still blocks.
         */
        if (phi[1] <= mt->min[1] + Q2_STEP_HEIGHT)
            continue;

        /*
         * 0x800519B0 does not simply reject the step. It first sends every
         * overlap through 0x80046234 by the mover's complete three-vector.
         * The push either frees the volume (so it can commit this tick) or is
         * rolled back; only the latter reaches 0x80051E74's 30-point
         * MOD_CRUSH T_Damage.  Returning "blocked" without this attempt made
         * every pusher a static stop and discarded the generic crush arm.
         */
        if (q2_sim_mover_push(&c->sim[0], step)) {
            /* A multi-part door can meet the player again on its next part;
             * test the carried origin, not the box from before the push. */
            client_player_box(c, plo, phi);
            continue;
        }

        q2_sim_mover_crush(&c->sim[0]);
        return true;
    }

    return false;
}

static bool client_play_sound(client *c, const char *want);
static void client_sound_set_relative_volume(client *c, s32 delta);
static bool client_play_sound_at(client *c, const char *want, const s32 at[3]);
static bool client_find_sound(client *c, const char *want, q2_vag *out);
/* The console's 0x800739B8 / 0x8007398C, on a (slot, tag) handle. */
static bool client_voice_playing(const client_voice *v, u32 serial);
static void client_voice_stop(client_voice *v, u32 serial);

/* Defined with the mixer. The zone load calls this before it frees the bank a
 * playing voice is reading out of. */
static void client_voices_stop(client *c);

/* The eleven item sounds, resolved against THIS map's bank with the console's
 * own substitutions applied. Called once the bank is open. */
static void client_item_sounds_resolve(client *c);

/* The pause page's KILLS/SECRETS row, filled from the same counters the level
 * tally uses. Called just before the page opens. */
static void client_menu_fill_stats(client *c);

/* The creature hooks, defined below with the AI clock they run on. */
static void client_cre_melee(q2_monster *m, const s32 aim[3], s32 damage,
                             s32 kick, void *user);
static void client_cre_sound(q2_monster *m, int which, void *user);
static void client_cre_fire(q2_monster *m, int flash, void *user);
static void client_cre_shot(q2_monster *m, const q2_cre_shot *shot, void *user);
static bool client_cre_muzzle(const q2_monster *m, s16 vert_a, s16 vert_b,
                              s32 out[3], void *user);
static bool client_cre_attach_blend(const q2_monster *m, s16 vert_a,
                                    s16 vert_b, q2_cre_attach *out,
                                    void *user);
static bool client_cre_attach_place(const q2_monster *m,
                                    const q2_cre_attach *a, s32 out[3],
                                    void *user);
static void client_cre_drop(const q2_monster_drop_request *req, void *user);
static bool client_cre_bank_has(const char *name, void *user);
static bool client_fx_mesh(void *user, const q2_actor *a, q2_fx_mesh_src *out);

/*
 * WHICH AREA A CREATURE IS DRAWN IN, and why it is remembered rather than
 * found afresh — the BASE1 Soldier that painted over the start room's wall.
 *
 * The console keeps it on the creature's link object, which the model draw
 * reads at 0x8006BE9C: +0x9E, byte +32 of the SecondaryCol cell at +0xA2.
 * PrimaryQuakeCreature (0x8007EBC4) moves that object with NewTrueMove3d, and
 * its cell is never forgotten there: the lookup walks out from +0xA2
 * (0x80045D98, Find3dSpaceIndxLink with the cell as the hint), a miss puts
 * the old cell back (0x80045E04), and UpdateBoxStuff (0x80054DD4, mode 1)
 * then rewrites +0x9E from it. linkentity (0x8005C8C8) copies the origin and
 * touches neither. So a body is drawn through the area of the last cell it
 * stood in.
 *
 * The draw used to look the cell up from scratch every frame and, on a miss,
 * leave the sort area at -1 — which modeldraw takes as "no area" and files by
 * global depth, the one path with no portal behind it. BASE1's Soldier at
 * (-15523, 114, -13554) is that miss: the SecondaryCol floor under it is at
 * 113, so its origin is a unit inside the eroded hull and no cell holds it.
 * Seen from the raised floor at (-13748, -16758) it was drawn over the wall
 * between that room and its own, area 11, which is not visible from there.
 *
 * So the lookup walks out from the remembered cell (the hint is the portal
 * walk 0x80044F54 makes) and a miss keeps the last area. A body that has never
 * been inside SecondaryCol takes the area of the PrimaryColl cell holding its
 * origin — the cell that admitted it to the zone, and the hull UpdateBoxStuff's
 * own mode 2 reads +0x9E from (+0xA0 against [0x800C8E94]). The two hulls
 * number areas alike: of the SecondaryCol cells on the disc whose centre lies
 * inside them, 10,564 read the same byte as the PrimaryColl cell holding that
 * centre and 7 do not, each a centre on a portal plane.
 *
 * Returns the area, or -1 with neither hull loaded, and hands back the
 * remembered cell: the light gather keys on it as the console's does on +0xA2.
 */
static s32 client_cre_area(client *c, u32 i, s32 *cell_out)
{
    const q2_monster *m;
    q2_coll_node node;
    s32 cell = -1, area = -1, found;

    if (cell_out)
        *cell_out = -1;
    if (!c || i >= c->creatures.set.count)
        return -1;
    m = &c->creatures.set.monsters[i];

    if (c->cre_cell && c->cre_area) {
        cell = c->cre_cell[i];
        area = c->cre_area[i];
    }

    if (c->sim[0].coll_ready) {
        found = q2_coll_find_node(&c->sim[0].coll, m->pos, cell, true);
        if (found >= 0 &&
            q2_collision_get_node(&c->sim[0].coll, (u32)found, &node)) {
            cell = found;
            area = node.contents & 0x7F;
        } else if (cell < 0 && c->sim[0].coll_primary_ready) {
            found = q2_coll_find_node(&c->sim[0].coll_primary, m->pos, -1,
                                      true);
            if (found >= 0 &&
                q2_collision_get_node(&c->sim[0].coll_primary, (u32)found,
                                      &node))
                area = node.contents & 0x7F;
        }
    }

    if (c->cre_cell && c->cre_area) {
        c->cre_cell[i] = cell;
        c->cre_area[i] = (s16)area;
    }
    if (cell_out)
        *cell_out = cell;
    return area;
}

/* Every body has been put somewhere new — a save restored over the level —
 * so what each remembered of its cell is from before. */
static void client_cre_area_forget(client *c)
{
    u32 i;

    if (!c || !c->cre_cell || !c->cre_area)
        return;
    for (i = 0; i < c->creatures.set.count; i++) {
        c->cre_cell[i] = -1;
        c->cre_area[i] = -1;
    }
}

/* ------------------------------------------------------------------------- */
/*
 * The creatures a map places, made live.
 *
 * Called from the zone load, after the sim exists and the player has been put
 * where the level starts them, because the AI's sight client is the player and
 * a creature that wakes before there is one has nothing to acquire.
 */
static void client_free_creatures(client *c)
{
    free(c->cre_model);
    free(c->cre_model_ok);
    free(c->cre_anim);
    free(c->cre_actor);
    free(c->cre_target);
    free(c->cre_home);
    free(c->cre_cell);
    free(c->cre_area);
    free(c->cre_fx);
    free(c->cre_body);
    c->cre_body     = NULL;
    c->cre_home     = NULL;
    c->cre_cell     = NULL;
    c->cre_area     = NULL;
    c->cre_model    = NULL;
    c->cre_model_ok = NULL;
    c->cre_anim     = NULL;
    c->cre_actor    = NULL;
    c->cre_target   = NULL;
    c->cre_fx       = NULL;

    if (c->creatures_ready) {
        q2_sim_set_targets(&c->sim[0], NULL, 0);
        q2_sim_set_bodies(&c->sim[0], NULL, 0);
        q2_ai_world_bind_bodies(&c->ai_world, NULL, NULL, 0);
        q2_creature_world_free(&c->creatures);
        c->creatures_ready = false;
    }
    c->zone_bank_ready = false;
}

static void client_load_creatures(client *c, const s32 eye[3])
{
    u32 i, resolved = 0;

    client_free_creatures(c);

    if (q2_creature_world_load(&c->creatures, c->disc, &c->build, &c->common,
                               c->sim[0].coll_ready ? &c->sim[0].coll
                                                    : NULL) != Q2_OK) {
        Q2_WARN("%s: creatures will not load", c->map);
        return;
    }
    c->creatures_ready = true;

    /* The zone's own CastList as well as the map's: a creature's model is as
     * likely to be in one as the other, which is why the inspector's census
     * searches both. */
    c->zone_bank_ready =
        (q2_model_bank_from_zone(&c->zone_bank, &c->zone.zone) == Q2_OK);

    if (c->creatures.set.count) {
        c->cre_model    = (q2_model *)calloc(c->creatures.set.count,
                                             sizeof(*c->cre_model));
        c->cre_model_ok = (bool *)calloc(c->creatures.set.count,
                                         sizeof(*c->cre_model_ok));
        c->cre_anim     = (client_model_anim *)calloc(c->creatures.set.count,
                                                       sizeof(*c->cre_anim));
        c->cre_actor    = (q2_actor *)calloc(c->creatures.set.count,
                                             sizeof(*c->cre_actor));
        c->cre_target   = (q2_actor **)calloc(c->creatures.set.count,
                                              sizeof(*c->cre_target));
        c->cre_fx       = (client_fx_pose *)calloc(c->creatures.set.count,
                                                   sizeof(*c->cre_fx));
        /* The solid half of the same set — see client_bodies_publish. */
        c->cre_body     = (q2_move_body *)calloc(c->creatures.set.count,
                                                 sizeof(*c->cre_body));
    }

    if (!c->cre_model || !c->cre_model_ok || !c->cre_anim ||
        !c->cre_actor || !c->cre_target || !c->cre_fx || !c->cre_body) {
        if (c->creatures.set.count)
            Q2_WARN("no memory for %u creatures", c->creatures.set.count);
        client_free_creatures(c);
        return;
    }

    /*
     * Confine them to THIS zone.
     *
     * Population is per MAP and a session is in one ZONE — the same thing that
     * makes q2_item_spawn_zone necessary — but a spawn record carries no zone
     * field, so the test has to be geometric: a creature that is inside no cell
     * of this zone's hull belongs to another one. On BASE1 that is eleven of
     * twenty, standing in ZONE1's rooms while ZONE0 is loaded, thinking and
     * being drawn and shootable through the void.
     */
    /*
     * AND NOT EVERY CREATURE THE MAP PLACED IS ONE THE PLAYER IS ASKED TO KILL.
     * CreatureCount (CREATURE.C 0x8005771C) is the only writer of the
     * denominator at 0x800B29E4, and per record it does
     *
     *     80057860  addiu v0, v0, -34
     *     80057864  sltiu v0, v0, 3
     *     80057868  bne   v0, zero, <skip the increment>
     *
     * so classes 34, 35 and 36 — Insane, Rider2 and Rider3 — are never in it.
     * `q2_mission_class_counts` is that window test; the wounded marines lying
     * about BASE1 were being counted as monsters to kill and the tally could
     * not be finished. It reads `pop_class_id`, the Population ROW, and not
     * `class_id`, which q2_creature_spawn has already overwritten with the
     * module's own descriptor key.
     */
    /* Everything the map placed that the tally is about, until the zone test
     * says otherwise. */
    c->cre_in_zone = 0;
    for (i = 0; i < c->creatures.set.count; i++)
        if (q2_mission_class_counts(c->creatures.set.monsters[i].pop_class_id))
            c->cre_in_zone++;

    if (c->sim[0].coll_primary_ready) {
        u32 elsewhere = 0;
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            if (q2_coll_find_node(&c->sim[0].coll_primary, m->pos, -1, true) < 0) {
                m->in_use = false;
                elsewhere++;
            }
        }
        if (elsewhere)
            Q2_INFO("creatures: %u of %u belong to another zone",
                    elsewhere, c->creatures.set.count);
        /*
         * THE KILL TOTAL IS FIXED AT LOAD, and this is the only place that can
         * fix it. Both this test and the CREBATCH hold below clear `in_use`, so
         * by the time the tally is drawn the two are indistinguishable — and
         * counting live creatures there made the denominator "creatures woken
         * so far", which climbs as the player springs each ambush. A level with
         * 22 records and 13 in another zone has 9 kills to get, whether or not
         * the player ever triggers the batch that holds five of them.
         *
         * Recounted rather than subtracted, because the class filter and the
         * zone filter are not independent: a subtraction would take a
         * discounted Insane out of a denominator it was never in.
         */
        c->cre_in_zone = 0;
        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            if (m->in_use && q2_mission_class_counts(m->pop_class_id))
                c->cre_in_zone++;
        }
    }

    /*
     * And how many of the ones that stay are standing CLEAR of the geometry.
     *
     * SecondaryCol is PrimaryColl eroded by the body's own half-extent, so a
     * creature whose origin resolves to a cell of it is one whose whole box
     * fits — and one whose origin does not is embedded in a wall or a floor by
     * up to 286 units. That is the "monsters stuck in geometry" report, made
     * countable.
     *
     * It reads zero for every map while a creature's position is the spawn
     * record's FEET, which is how the wrong hull came to be wired in for the
     * AI in the first place (see the bind below).
     */
    if (c->sim[0].coll_ready) {
        u32 clear = 0, live = 0;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];

            if (!m->in_use)
                continue;
            live++;
            if (q2_coll_find_node(&c->sim[0].coll, m->pos, -1, true) >= 0)
                clear++;
        }
        if (live)
            Q2_INFO("creatures: %u of %u stand clear of the geometry "
                    "(inside SecondaryCol)", clear, live);
    }

    c->cre_home = (s32 *)calloc(c->creatures.set.count ?
                                c->creatures.set.count * 3 : 1, sizeof(s32));
    c->cre_cell = (s32 *)calloc(c->creatures.set.count ?
                                c->creatures.set.count : 1, sizeof(s32));
    c->cre_area = (s16 *)calloc(c->creatures.set.count ?
                                c->creatures.set.count : 1, sizeof(s16));
    client_cre_area_forget(c);
    {
        u32 outside = 0;

        for (i = 0; i < c->creatures.set.count; i++) {
            s32 cell;

            if (c->creatures.set.monsters[i].in_use &&
                client_cre_area(c, i, &cell) >= 0 && cell < 0)
                outside++;
        }
        if (outside)
            Q2_INFO("creatures: %u outside SecondaryCol take their draw "
                    "area from PrimaryColl", outside);
    }

    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        const char *name = q2_creature_world_model_name(&c->creatures, m);
        s32 idx;

        if (c->cre_home) {
            c->cre_home[i * 3 + 0] = m->pos[0];
            c->cre_home[i * 3 + 1] = m->pos[1];
            c->cre_home[i * 3 + 2] = m->pos[2];
        }

        c->cre_target[i] = &c->cre_actor[i];
        q2_actor_init(&c->cre_actor[i]);
        q2_actor_from_monster(&c->cre_actor[i], m);

        if (!name)
            continue;

        /*
         * AND THE MODEL'S BIAS INTO THE CREATURE, which is where its eye
         * height comes from. The loader stores `lh model[+0x1C]` into the link
         * object's +0xF8 (0x80056700 `jal 0x8006D100`, 0x80056710 `sh v0,
         * 248(s0)`) and walkmonster_go complements it into the view height
         * (0x80062448/0x80062450 `nor`). Nothing filled `model_ext2`, so every
         * walker kept the -290 stand-in and only its turn rate changed.
         *
         * Here because this is the one place the creature and its model are
         * both in hand, and before q2_creature_world_wake below, which runs
         * the go-routines that read it. `m` is const in this loop, so the set
         * is indexed instead.
         */
        if (c->model_bank_ready) {
            idx = q2_model_bank_find(&c->model_bank, name);
            if (idx >= 0 &&
                q2_model_get(&c->model_bank, (u32)idx,
                             &c->cre_model[i]) == Q2_OK) {
                c->cre_model_ok[i] = true;
                c->creatures.set.monsters[i].model_ext2 =
                    c->cre_model[i].hdr.ext2;
                /*
                 * AND THE HIT CYLINDER, which is the model's too. 0x8007E50C
                 * calls GetCreHitSize with the creature and hands the result
                 * to SetupBoxDrop as its fifth argument; with a model bound at
                 * cre+0x10 that is the header's `lhu +0x18`/`lhu +0x1A` pair.
                 * Only a creature whose model resolved may be given it — the
                 * no-model fallback of 286/600 that q2_monster_init leaves is
                 * exactly what the console gives a creature with cre+0x10
                 * null, so a failed bind must not call this.
                 */
                q2_monster_bind_hit_size(&c->creatures.set.monsters[i], true,
                                         c->cre_model[i].hdr.ext0,
                                         c->cre_model[i].hdr.ext1);
                resolved++;
                continue;
            }
        }
        if (c->zone_bank_ready) {
            idx = q2_model_bank_find(&c->zone_bank, name);
            if (idx >= 0 &&
                q2_model_get(&c->zone_bank, (u32)idx,
                             &c->cre_model[i]) == Q2_OK) {
                c->cre_model_ok[i] = true;
                c->creatures.set.monsters[i].model_ext2 =
                    c->cre_model[i].hdr.ext2;
                /* The zone bank's arm, same call for the same reason. */
                q2_monster_bind_hit_size(&c->creatures.set.monsters[i], true,
                                         c->cre_model[i].hdr.ext0,
                                         c->cre_model[i].hdr.ext1);
                resolved++;
            }
        }
    }

    q2_sim_set_targets(&c->sim[0], c->cre_target, c->creatures.set.count);

    /*
     * NO WORLD LIST IN SINGLE PLAYER, because it would be this one. Every
     * reader of `world_targets` — the projectile hit list, the three detonation
     * sites and the per-actor presentation pass (0x8005B880,
     * q2_fx_actor_present), all in simcombat.c — takes `world_targets ?
     * world_targets : combat.targets`, and the zone load's q2_sim_init leaves
     * it NULL. Publishing cre_target here as well is what used to get a
     * single-player creature presented at all; that pass has the fallback now.
     * Deathmatch builds its own list per frame (client_targets_for).
     */

    /*
     * The world the AI asks its three questions of — and it is BOTH hulls,
     * because 0x8005BD3C picks one per call on whether the caller handed it a
     * real box. Sight and the ground probe get `PrimaryColl`; a walking
     * creature's step trace gets `SecondaryCol`, which is PrimaryColl already
     * eroded by the body's 286-unit half-extent.
     *
     * THE MEASUREMENT THAT USED TO BE HERE was real and answered the wrong
     * question. It said: with the eroded hull, 214 of 214 traces cannot place
     * their start. True — because the creature's position was the Population
     * record's FEET, and every hull query is in the entity ORIGIN frame, 286
     * above them. The conclusion drawn was "creatures are not inside
     * SecondaryCol, so use PrimaryColl", and the consequence was that a
     * creature's CENTRE was swept as a point through the un-eroded hull and
     * could travel until the centre reached the wall — half a body inside it.
     * With the spawn lifted (spawn.c) the same records are inside the eroded
     * hull, and the creature stops flush against the wall instead.
     */
    q2_ai_world_bind_init(&c->ai_world,
                          c->sim[0].coll_primary_ready ? &c->sim[0].coll_primary
                                                    : NULL,
                          c->sim[0].coll_ready ? &c->sim[0].coll : NULL);

    /*
     * AND THE DOORS, which are in NEITHER hull.
     *
     * A mover is a runtime entity whose box lives in the sim's move world, and
     * until this line that world had exactly one reader: the player's own step
     * sweep. So a door stopped the player and nothing else — a guard behind a
     * shut one could see you through it, shoot you through it (every fire hook
     * gates each shot on `q2_visible`), and walk into it.
     *
     * After `q2_ai_world_bind_init`, which memsets the binding, and before the
     * install. The address handed over is the sim's `move_world` itself rather
     * than its target array, because `q2_sim_attach_movers` reallocates that
     * array — this load already has, further up `client_load_zone`, and the
     * next zone will again.
     */
    q2_ai_world_bind_entities(&c->ai_world, &c->sim[0].move_world);

    /*
     * AND THE OTHER BODIES, which are in neither hull either.
     *
     * The sim rebuilds its actor list every world tick (sim.h) from its own
     * players plus whatever `q2_sim_set_bodies` registered — which is
     * `client_bodies_publish` below, once per frame, from the creature set. The
     * address handed over is the sim's `body_world` itself, whose own array
     * moves as creatures come and go.
     *
     * The creature set's `monsters` array is what turns the AI's `ignore`
     * pointer back into a handle: `q2_creature_world_load` allocates it once
     * per zone and it does not move afterwards, so it is taken here, beside the
     * bind that needs it, rather than at each trace.
     */
    q2_ai_world_bind_bodies(&c->ai_world, &c->sim[0].body_world,
                            c->creatures_ready ? c->creatures.set.monsters
                                               : NULL,
                            c->creatures_ready ? c->creatures.set.count : 0);

    q2_ai_world_bind_install(&c->ai_world);

    /*
     * The hooks, before anything wakes. A creature that swings on its first
     * think would otherwise swing into a null pointer.
     *
     * The fire hook carries the Soldier's own figures, read out of its module
     * and matching id's exactly. Other creatures reach it with a table it does
     * not know and are dropped rather than given a Soldier's gun.
     */
    /*
     * The breadcrumb trail the AI hunts along — `0x800D517C`, the eight-slot
     * ring at gp+17892 (`addiu v1, zero, 8` at 0x80060BE8 and the `andi ..., 7`
     * masks). `q2_trail_add` had no caller anywhere in the tree, so
     * the trail was always empty and the three-stage pursuit a creature runs
     * when it loses you could never reach its second stage: it had nowhere to
     * follow you to.
     */
    q2_trail_init();

    q2_cre_set_melee_hook(client_cre_melee, c);
    q2_cre_set_sound_hook(client_cre_sound, c);
    q2_cre_set_fire_hook(client_cre_fire, c);
    q2_cre_set_shot_hook(client_cre_shot, c);
    q2_cre_set_muzzle_hook(client_cre_muzzle, c);
    q2_cre_set_attach_hooks(client_cre_attach_blend, client_cre_attach_place,
                            c);

    /*
     * WHERE A FLAGGED CREATURE'S DROP GOES. monster_death_use records the
     * death (0x8006233C -> 0x80020D60) and the once-a-frame flush below turns
     * it into a request; this is the spawner at the end of that chain,
     * 0x8002085C. The queue has no clear on the disc other than the flush, so
     * emptying it here — and in client_load_zone, beside the set it would have
     * spawned into — is the port's choice: a death in the frame a zone changed
     * must not drop an item into the next zone.
     */
    q2_monster_drop_reset();
    q2_monster_set_drop_hook(client_cre_drop, c);

    /*
     * Hold back the batches before waking anything.
     *
     * The console spawns nothing at load — every group's flags word is zero on
     * disc — and a script selects what stands there. This port spawns every
     * record and holds the ones a script owns dormant instead, which reaches
     * the same place: an ambush is not in the room until it is called for.
     * See q2_creature_world_hold_batches.
     */
    {
        const dat_chunk *lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];
        u32 held = q2_creature_world_hold_batches(
            &c->creatures, c->zone_index,
            lb ? lb->data : NULL, lb ? lb->size : 0);
        if (held)
            Q2_INFO("creatures: %u held back, waiting for a CREBATCH", held);
    }

    /* The sight client is placed at the player's entity ORIGIN, not the eye —
     * q2_visible adds the view height itself. See creworld.h. */
    {
        s32 player_origin[3];

        player_origin[0] = eye[0];
        player_origin[1] = q2_sim_origin_y(c->sim[0].player[0].pos[1]);
        player_origin[2] = eye[2];
        q2_creature_world_wake(&c->creatures, player_origin,
                               c->sim[0].combat.inv.health);
    }
    c->ai_accum = 0.0;

    {
        u32 live = 0, droppers = 0;
        for (i = 0; i < c->creatures.set.count; i++) {
            if (c->creatures.set.monsters[i].in_use)
                live++;
            /* 0x80062330: record flag 0x100, the one monster_death_use tests
             * before it queues a drop. Counted over every record the map
             * placed — held batches and other zones' included — so this is
             * the map's figure, not the zone's. */
            if (c->creatures.set.monsters[i].spawnflags &
                Q2_SPAWNFLAG_DROP_ITEM)
                droppers++;
        }
        if (droppers)
            Q2_INFO("creatures: %u carry the drop flag (0x80062330)",
                    droppers);

        /* The eyes the go-routines just installed, for the ones that woke:
         * a walker's is ~ext2 (0x80062450), so a Soldier's 251 reads -252. A
         * -290 here is the q2_monster_init stand-in, i.e. no bias reached it. */
        {
            u32 own = 0, woke = 0;
            s32 first = 0;

            for (i = 0; i < c->creatures.set.count; i++) {
                const q2_monster *m = &c->creatures.set.monsters[i];

                if (!m->in_use || m->model_ext2 == 0)
                    continue;
                woke++;
                if (m->view_height != -290) {
                    if (!own)
                        first = m->view_height;
                    own++;
                }
            }
            if (woke)
                Q2_INFO("creatures: %u of %u with a model bias see from "
                        "their own eye (first %d)", own, woke, (int)first);
        }

        Q2_INFO("creatures: %u live in this zone, %u of %u spawn records "
                "placed (%u removed by skill %d), %u with a model%s",
                live, c->creatures.stats.placed, c->creatures.stats.records,
                c->creatures.stats.skill_filtered, (int)q2_cre_skill(),
                resolved,
                c->creatures.stats.no_module
                    ? ", some classes have no module" : "");

        /*
         * WHICH GUARDS, by the class-table row the Soldier's spawn dispatches
         * on (cre_soldier.c, soldier_base_skin): 19 blaster, 18 shotgun, 20
         * machinegun. Three different fights on one model, and nothing else
         * in the log says which a map holds.
         */
        {
            u32 k, n[3] = { 0, 0, 0 };
            s32 first[3] = { -1, -1, -1 };
            static const char *const kind[3] = {
                "blaster", "shotgun", "machinegun"
            };

            for (k = 0; k < c->creatures.set.count; k++) {
                const q2_monster *m = &c->creatures.set.monsters[k];
                int v = m->pop_class_id == 19 ? 0
                      : m->pop_class_id == 18 ? 1
                      : m->pop_class_id == 20 ? 2 : -1;

                if (!m->in_use || v < 0)
                    continue;
                if (n[v]++ == 0)
                    first[v] = (s32)k;
            }
            if (n[0] || n[1] || n[2])
                Q2_INFO("creatures: guards %u blaster, %u shotgun, "
                        "%u machinegun", n[0], n[1], n[2]);

            /* And the first of every other module, by name. */
            for (k = 0; k < c->creatures.set.count; k++) {
                const q2_monster *m = &c->creatures.set.monsters[k];
                const q2_cre_bind *bind = q2_cre_bind_for(m);
                const char *name = bind && bind->impl ? bind->impl->name : NULL;
                u32 j;
                bool seen = false;

                if (!m->in_use || !name || !strcmp(name, "Soldier"))
                    continue;
                for (j = 0; j < k && !seen; j++) {
                    const q2_cre_bind *b2 =
                        q2_cre_bind_for(&c->creatures.set.monsters[j]);
                    seen = c->creatures.set.monsters[j].in_use && b2 &&
                           b2->impl && b2->impl->name &&
                           !strcmp(b2->impl->name, name);
                }
                if (!seen)
                    Q2_INFO("creatures: first %s is #%u at (%d,%d,%d) yaw %d",
                            name, k, m->pos[0], m->pos[1], m->pos[2],
                            (int)m->angles[2]);
            }
            /* Where to stand to meet each: the first of every kind, by the
             * index --trace-cre takes. */
            for (k = 0; k < 3; k++) {
                const q2_monster *m;

                if (first[k] < 0)
                    continue;
                m = &c->creatures.set.monsters[first[k]];
                Q2_INFO("creatures: first %s guard is #%d at (%d,%d,%d) "
                        "yaw %d", kind[k], (int)first[k], m->pos[0],
                        m->pos[1], m->pos[2], (int)m->angles[2]);
            }
        }

        /*
         * AND THE HIT CYLINDER EACH ONE IS SHOT AT THROUGH — GetCreHitSize's
         * pair, entity+0x94 and +0x96. A line rather than a counter because
         * the whole point is that they DIFFER: while the pair came from the
         * movement hull every creature in the game was one 286-radius,
         * 600-tall cylinder, so a run where `own` is zero or where every pair
         * reads 286/600 is a run where the bind did not happen.
         */
        {
            u32 own = 0, k;
            s16 fr = 0, fh = 0;

            for (k = 0; k < c->creatures.set.count; k++) {
                const q2_monster *m = &c->creatures.set.monsters[k];

                if (m->hit_radius == 286 && m->hit_height == 600)
                    continue;
                if (!own) { fr = m->hit_radius; fh = m->hit_height; }
                own++;
            }
            Q2_INFO("creatures: %u of %u are shot at through their MODEL's "
                    "cylinder (first %d x %d; the no-model pair is 286 x 600)",
                    own, c->creatures.set.count, (int)fr, (int)fh);
        }
    }
}

/* ------------------------------------------------------------------------- */
/*
 * The three hooks a creature reaches the rest of the game through.
 *
 * `crebind.h` has always defined them and NOTHING had ever set them, so every
 * claw, every shot and every sound a creature made went to a null pointer.
 * Creatures chased the player and could not touch them.
 *
 * They are hooks rather than direct calls for the reason the header gives: the
 * module reaches the engine through its import table in the original, and
 * keeping that shape stops every creature having to know about combat.
 */
/*
 * The combat actor that stands for a creature. The monster and its actor are
 * parallel arrays, so the index is the pointer difference; a monster outside
 * the set, or a client with no actors yet, has none. One lookup for the three
 * attack hooks, so the claw and the two kinds of shot cannot disagree about
 * who hit you.
 */
static q2_actor *client_cre_actor(client *c, const q2_monster *m)
{
    size_t idx;

    if (!c || !c->cre_actor || !m || m < c->creatures.set.monsters)
        return NULL;
    idx = (size_t)(m - c->creatures.set.monsters);
    return idx < c->creatures.set.count ? &c->cre_actor[idx] : NULL;
}

static void client_cre_melee(q2_monster *m, const s32 aim[3], s32 damage,
                             s32 kick, void *user)
{
    client *c = (client *)user;

    (void)aim; (void)kick;

    if (!c || !c->creatures_ready || damage <= 0)
        return;

    /*
     * Only a creature that has actually acquired the player. A module's melee
     * runs off its own animation frame and does not check who is in front of
     * it, which is the engine's job — here that check is "is the player the
     * thing it is hunting", because the port has one player and the AI's
     * `enemy` is the sight client whenever it has one.
     */
    if (m->enemy != &c->creatures.sight)
        return;

    /*
     * OUT OF REACH never reaches this hook at all: `fire_hit`'s own range test
     * (0x80061198) is q2_cre_fire_hit's, in crebind.c, where the import lives.
     */
    c->cre_swings++;

    /*
     * WHICH creature is swinging, as an actor.
     *
     * This used to pass NULL, and a NULL attacker costs three things at once:
     * the damage point defaults to the player's own position, so the blood and
     * the flinch's roll both lose their direction (see q2_sim_hurt_player); the
     * knockback has no source to push away from; and `last_attacker` is never
     * recorded, so a player clawed to death by a Berserk died with no killer —
     * which is precisely the attribution the scoring rule was built to honour.
     *
     * The monster and its actor are parallel arrays, so the index is the
     * pointer difference. The actors are re-synced from the monsters at the top
     * of every frame, before the tick this runs inside, so the origin here is
     * where the creature is now.
     *
     * MOD 7 is `0x800612F0`, a creature's contact hit (combat.h) — armour
     * applies, which is what makes it different from the environment's.
     */
    q2_sim_hurt_player(&c->sim[0], client_cre_actor(c, m), (s16)damage,
                       Q2_MOD_MELEE, c->creatures.sight.pos);
}

/*
 * HOW MANY PELLETS A CREATURE'S SHOTGUN THROWS, and it is not the number the
 * module passes.
 *
 * `monster_fire_shotgun` (import +0x88, 0x80061ED0) ignores every argument
 * past a3. It reads a0 (the module), a1 (the start point), a2 (the aim) and a3
 * (the damage), and then loops on its OWN constant:
 *
 *     80061F6C  addu  s0, zero, zero     ; pellet = 0
 *     80061F78  jal   0x80089E28         ; rand(), the horizontal jitter
 *     80061F7C  addiu s0, s0, 1
 *     ...
 *     80061FCC  jal   0x8004874C         ; deliver one traced pellet
 *     80061FD4  slti  v0, s0, 5          ; five, and the module cannot say
 *     80061FD8  bne   v0, zero, 0x80061F78
 *
 * — five. The Soldier's module passes twelve (crebind.h) and 1000/500 of
 * spread, and the engine reads neither: its `lhu s1, 112(sp)` is the only
 * stack argument 0x80061ED0 or 0x8004874C ever touches, and that is the
 * trace's cell hint, not the module's. So a shotgun guard's blast is at most
 * 5 x 2 = 10 points on the console.
 *
 * THE JITTER IS TRANSCRIBED, and it is stranger than a spread. The aim is
 * scaled `<< 2` into three halfwords at sp+24 (0x80061F1C..0x80061F50). Each
 * pellet then ADDS `((rand() - 16384) << 10) >> 14` to the first of them and,
 * from a second rand(), to the second (0x80061F80..0x80061FC4), and stores the
 * sums back with `sh`. Nothing resets them between pellets, so the five are a
 * RANDOM WALK away from the aim rather than five draws around it — and only X
 * and the vertical move: a guard firing along Z sprays sideways and up and
 * down, one firing along X only up and down.
 *
 * `monster_fire_bullet` (+0x84, 0x80061DFC) has no jitter at all — one trace,
 * no `rand()` — so a machinegun guard's round goes exactly where it aimed; its
 * miss is the aim's own crandom (cre_soldier.c).
 */
#define Q2_CRE_SHOTGUN_PELLETS 5   /* 0x80061FD4 */

/*
 * The reports the engine's two hitscan spawners play THEMSELVES, at the
 * object, through import +0x20 — whatever the module played first:
 * `SoundMachinegunFire` (gp+17744, 0x80061EAC) and the shotgun's (gp+17768,
 * 0x80061F04), registered from `wep_machgf1b` (0x800ACC4C) and `wep_shotgf1b`
 * (0x800ACC34). So every round the Infantry fires is heard, the twelve of its
 * death burst included, where this port played nothing for think 1 at all;
 * and a Soldier's round is two voices of one sample, its module's and the
 * engine's, because 0x8007270C starts a fresh voice on every call
 * (0x8007278C).
 */
#define Q2_CRE_SND_BULLET  "wep_machgf1b"
#define Q2_CRE_SND_SHOTGUN "wep_shotgf1b"

/*
 * A DECODED creature's fire — the one-int hook, which now carries only the
 * import slot a decoded think called (cre_actions.c). The Soldier's three
 * weapon tables used to arrive here as `table * 8 + flash` and be put into
 * the player whenever the Soldier could see them; it hands over a whole
 * q2_cre_shot now, with its muzzle and its aim, so all this does is count.
 *
 * What a decoded creature's shot DOES is not the port's to guess:
 * `monster_fire_rocket` (0x8006210C) scales the aim by 3/2 and passes its
 * caller's `a1` and `a3` straight through to 0x8004AF28 without touching
 * them, so the damage and the speed are the MODULE's arguments, not the
 * engine's constants. Until a creature's table is read, its shot is counted
 * here rather than invented — the same rule the sound hook follows.
 */
static void client_cre_fire(q2_monster *m, int flash, void *user)
{
    client *c = (client *)user;

    (void)m;
    (void)flash;
    if (!c || !c->creatures_ready)
        return;
    c->cre_fire_no_figures++;
}

/*
 * ONE CREATURE'S DAMAGE, carried from its combat actor back to the monster.
 *
 * The frame's drain runs this for every creature once the sim tick and the
 * player's guns are done (below the sim tick in the frame). A creature's own
 * round can strike ANOTHER creature, and that happens inside the AI tick,
 * after the drain — where the next frame's actor rebuild
 * (q2_actor_from_monster, from the monster's untouched health) would quietly
 * undo it. So a round that hits a creature runs this for its victim at once,
 * which is also the console's timing: T_Damage runs inside the shooter's
 * think and calls the victim's pain or die before it returns.
 */
static void client_cre_drain_one(client *c, u32 i)
{
    q2_monster *m    = &c->creatures.set.monsters[i];
    bool        was  = m->dead;
    s16         hp   = m->health;

    q2_actor_to_monster(&c->cre_actor[i], m);

    /*
     * T_Damage ends by calling the entity's own `pain` (+0xA0) or `die`
     * (+0xA4) — 0x80062A9C for the latter — and this port had neither
     * installed, so `soldier_pain` and `soldier_die` were dead code and
     * damage only ever moved a number. This is the site that sees the
     * health CROSS a threshold, and it used to dispatch the two hooks
     * from here directly.
     *
     * IT NO LONGER DOES, because dispatching them was only half of what
     * the original does between the subtraction and the return.
     * `q2_monster_damage_reaction` is the whole of that tail —
     * M_ReactToDamage, the AI_DUCKED gate, the nightmare pain debounce,
     * the death-use pass, the kill counter and the no-knockback bit —
     * transcribed in monster.c. What is left here is deciding WHEN to
     * call it and with what.
     *
     * The attacker is the sight client, which is where the player is;
     * the port has no inflictor entity to hand over. For a creature struck
     * by another creature's round that is an approximation, stated: the
     * console's M_ReactToDamage sees the shooter.
     *
     * One honest approximation, stated: the original calls the tail on
     * every hit, INCLUDING one that armour or godmode reduced to zero —
     * and still reacts to it, because being shot at and unhurt is still
     * being shot at. The actor sync reports health, not the shot, so a
     * hit that took nothing is indistinguishable here from no hit at
     * all. The gate is therefore "the health moved", which is a subset
     * of the original's occasions and never a superset.
     */
    if (m->health != hp || (!was && m->health <= 0)) {
        s16 took = (s16)(hp - m->health);

        if (took < 0)
            took = 0;

        q2_monster_damage_reaction(m, &c->creatures.sight, took);

        if (!was && m->dead)
            c->cre_die_calls++;
        else if (!m->dead && took > 0)
            c->cre_pain_calls++;
    }

    /*
     * The frame it died on. What can be reconstructed from the module's
     * data rather than its code is the animation, so a body whose own
     * `die` did not choose a move is put into one and left to play it
     * out — and it now STOPS there rather than looping, because
     * M_MoveFrame raises the corpse bit when a dead creature's move
     * runs out (monster.c).
     *
     * Without this a killed creature simply vanished — the tick and the
     * draw both skipped anything with `dead` set, so a Soldier shot
     * dead was gone on the frame it died.
     */
    if (!was && m->dead && !m->currentmove) {
        s32 f = q2_creature_world_death_frame(&c->creatures, m);

        if (f >= 0 && q2_cre_set_move(m, f)) {
            m->frame     = (s16)f;
            c->cre_bodies++;
        }
    } else if (!was && m->dead) {
        c->cre_bodies++;
    }
}

/* A creature's last drawn pose (client_fx_pose), or NULL when it has none. */
static const q2_model_instance *client_cre_pose(client *c, const q2_monster *m)
{
    size_t idx;

    if (!c || !c->creatures_ready || !c->cre_fx || !c->cre_model_ok || !m ||
        m < c->creatures.set.monsters)
        return NULL;
    idx = (size_t)(m - c->creatures.set.monsters);
    if (idx >= c->creatures.set.count || !c->cre_model_ok[idx] ||
        !c->cre_fx[idx].valid)
        return NULL;
    return &c->cre_fx[idx].inst;
}

/* THE MUZZLE HOOK — q2_cre_muzzle (crebind.h): the posed attachment, placed. */
static bool client_cre_muzzle(const q2_monster *m, s16 vert_a, s16 vert_b,
                              s32 out[3], void *user)
{
    const q2_model_instance *inst = client_cre_pose((client *)user, m);
    s32 local[3];

    if (!inst || !q2_model_blend_local(inst, vert_a, vert_b, local))
        return false;
    q2_model_local_to_world(inst, local, out);
    return true;
}

/* q2_cre_attach_blend's host half: the cache, off the creature's own model. */
static bool client_cre_attach_blend(const q2_monster *m, s16 vert_a,
                                    s16 vert_b, q2_cre_attach *out,
                                    void *user)
{
    const q2_model_instance *inst = client_cre_pose((client *)user, m);
    q2_model_attach a;

    if (!inst || !q2_model_attach_blend(inst->model, vert_a, vert_b, &a))
        return false;
    out->off[0] = a.off[0];
    out->off[1] = a.off[1];
    out->off[2] = a.off[2];
    out->part   = a.part;
    return true;
}

/* ...and q2_cre_attach_place's: posed with the last drawn pose, then placed. */
static bool client_cre_attach_place(const q2_monster *m,
                                    const q2_cre_attach *a, s32 out[3],
                                    void *user)
{
    const q2_model_instance *inst = client_cre_pose((client *)user, m);
    q2_model_attach at;
    s32 local[3];

    if (!inst)
        return false;
    at.off[0] = a->off[0];
    at.off[1] = a->off[1];
    at.off[2] = a->off[2];
    at.part   = a->part;
    q2_model_attach_pose(inst, &at, local);
    q2_model_local_to_world(inst, local, out);
    return true;
}

/*
 * THE SWING TRAILS — the blur ribbons three creature modules lay from their
 * export 1, the per-object handler the engine calls once a frame.
 *
 * Each export compares the object's animation position (+0x100) against a
 * window its init built from a named move's block-D record and, inside it,
 * calls import +0x78 — AddBlurPoints, 0x8002F238 — over a vertex list in
 * grey (0x00202020) for six frames:
 *
 *     Arachner  module+0xAAC   "Melee" [start, end]
 *                              {290, 299} and {239, 244}  (+0x2D4, +0x2E0)
 *     Berserk   module+0xC64   "Attack1" [start + 60,  start + 140] {280, 281, 286}
 *                              "Attack2" [start + 130, start + 190] {275, 263}
 *                              "Attack3" [start + 100, start + 240] {274, 265}
 *                              (+0x1F8, +0x204, +0x20C; the inits at
 *                              module+0x370, +0x454, +0x538)
 *     Infantry  module+0xDC0   "Attack2" [start, end]  {200, 201}  (+0x2B8)
 *
 * THE WINDOWS ARE IN POSITION UNITS, block D's halfwords times five. The
 * init stores the record's +12/+14 with no scale of its own and the handler
 * compares them with +0x100 directly, so the record in RAM must already be in
 * position units — the conclusion viewweapon.c reaches for the hand grenade's
 * `Set` from the other side (its 380 could never be reached unscaled), and
 * the one the windows themselves force: read raw, the Berserk's "Attack2" and
 * "Attack3" windows and the Arachner's "Melee" all fall inside the creature's
 * FIRST move, while times five each sits mid-swing in its own. No load-time
 * multiply has been found (openquestions #51f), so the five is the port's, as
 * it is in viewweapon.c.
 *
 * A module whose name does not resolve stores (1, 0) and never trails; so does
 * a creature this frame has not posed yet. The samples come off the mesh the
 * last draw posed (client_fx_pose), through 0x8006CC44's walk.
 */
typedef struct client_blur_rule {
    const char *module;
    const char *move;
    s16         lo, hi;      /* added to the move's start; hi < 0: its end */
    s16         list[2][3];  /* -1 ended, up to two lists */
} client_blur_rule;

static const client_blur_rule k_cre_blurs[] = {
    { "Arachner", "Melee",   0,  -1, { { 290, 299, -1 }, { 239, 244, -1 } } },
    { "Berserk",  "Attack1", 60, 140, { { 280, 281, 286 }, { -1, -1, -1 } } },
    { "Berserk",  "Attack2", 130, 190, { { 275, 263, -1 }, { -1, -1, -1 } } },
    { "Berserk",  "Attack3", 100, 240, { { 274, 265, -1 }, { -1, -1, -1 } } },
    { "Infantry", "Attack2", 0,  -1, { { 200, 201, -1 }, { -1, -1, -1 } } }
};

static void client_cre_blurs(client *c)
{
    static const u8 grey[3] = { Q2_FX_BLUR_GREY, Q2_FX_BLUR_GREY,
                                Q2_FX_BLUR_GREY };
    u32 i, r;

    if (!c->creatures_ready || !c->cre_fx || !c->cre_model_ok ||
        !c->sim[0].fx_ready)
        return;

    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        const client_fx_pose *fp = &c->cre_fx[i];
        const q2_cre_bind *bind;
        const char *name = NULL;

        if (!m->in_use || !c->cre_model_ok[i] || !fp->valid ||
            !fp->inst.model)
            continue;
        bind = q2_cre_bind_for(m);
        if (bind && bind->cre && bind->cre->name[0])
            name = bind->cre->name;
        else if (bind && bind->impl)
            name = bind->impl->name;
        if (!name)
            continue;

        for (r = 0; r < Q2PSX_ARRAY_COUNT(k_cre_blurs); r++) {
            const client_blur_rule *rule = &k_cre_blurs[r];
            q2_model_move mv;
            s32 lo, hi, pos = fp->inst.anim_pos;
            int l;

            if (strcmp(rule->module, name) != 0 ||
                !q2_model_move_by_name(fp->inst.model, rule->move, &mv))
                continue;
            lo = (s32)mv.start * 5 + rule->lo;
            hi = rule->hi < 0 ? (s32)mv.end * 5
                              : (s32)mv.start * 5 + rule->hi;
            if (pos < lo || pos > hi)
                continue;

            for (l = 0; l < 2 && rule->list[l][0] >= 0; l++) {
                s32 pts[3][3];
                u32 n = 0;

                while (n < 3 && rule->list[l][n] >= 0 &&
                       q2_model_world_vertex(&fp->inst, rule->list[l][n],
                                             pts[n]))
                    n++;
                if (n < 2)
                    continue;
                q2_fx_blur_add(&c->sim[0].fx, (u16)(0x200u + i),
                               (u16)rule->list[l][0],
                               (const s32 (*)[3])pts, n, grey, 6,
                               (u8)(fp->inst.sort_area > 0
                                        ? fp->inst.sort_area : 0));
            }
        }
    }
}

/*
 * Which modules draw a muzzle flash, and from which cache: the three whose
 * export 1 calls import +0xA0 while object+0x10C is negative, each reading
 * the attachment its own init blended (crebind.h, q2_cre_muzzle).
 *
 *     Soldier   0x80102120  module+0x32D8  vertices 228, 234
 *     Infantry  0x80100DC0  module+0x1D80  vertices 200, 205
 *     Gunner    0x801014BC  module+0x1D28  vertices 227, 231
 *
 * The Tank Commander's module has no export 1 at all, and the Arachner's and
 * the Berserk's draw blur trails, so none of the three flashes whatever it
 * fires.
 */
static bool client_cre_flash_verts(const q2_monster *m, s16 *a, s16 *b)
{
    const q2_cre_bind *bind = q2_cre_bind_for(m);
    const char *name = NULL;

    if (bind && bind->cre && bind->cre->name[0])
        name = bind->cre->name;
    else if (bind && bind->impl)
        name = bind->impl->name;
    if (!name)
        return false;

    if (strcmp(name, "Soldier") == 0)  { *a = 228; *b = 234; return true; }
    if (strcmp(name, "Infantry") == 0) { *a = 200; *b = 205; return true; }
    if (strcmp(name, "Gunner") == 0)   { *a = 227; *b = 231; return true; }
    return false;
}

/*
 * THE MUZZLE FLASH, which a creature never had: 0x80031094 for the frame of
 * the round (q2_fx_flash, effect.h). The port used to raise the PLAYER's
 * machinegun light — 250..349 and 700..899 — at every creature's origin on
 * every shot of every weapon. The console raises the creature's own,
 * 850..1049 and 1200..1599 from two rand()s, and only for a bullet or a
 * shotgun round from one of the three modules above: a blaster guard's bolt,
 * the Tank Commander's guns and the Arachner's rail flash nothing.
 *
 * WHERE THE LIGHT GOES is a quirk, and it is kept. 0x80031178..0x800311AC adds
 * the attachment's MODEL-space offset to the object origin without rotating
 * it — the quad's corners are rotated at 0x8003132C, the light's point is
 * not. So the light sits where the muzzle would be if the creature faced
 * yaw 0. Its radii are several times the offset, so what it lights barely
 * moves; but it is not where the flash is drawn, and that is the console's.
 *
 * The two draws come from the BIOS stream the creatures decide on, because
 * that is the generator 0x800311B0 calls.
 */
static void client_cre_flash(client *c, const q2_monster *m)
{
    const q2_model_instance *inst;
    s32 local[3], world[3];
    s16 va, vb;
    int k;

    if (!client_cre_flash_verts(m, &va, &vb))
        return;
    inst = client_cre_pose(c, m);
    if (!inst || !q2_model_blend_local(inst, va, vb, local))
        return;

    if (c->lights_ready) {
        static const u8 colour[3] = {
            Q2_MUZZLE_LIGHT_R, Q2_MUZZLE_LIGHT_G, Q2_MUZZLE_LIGHT_B
        };
        s32 inner, outer, at[3];
        s32 ra = q2_random_next();
        s32 rb = q2_random_next();

        for (k = 0; k < 3; k++)
            at[k] = inst->origin[k] + local[k];
        q2_creature_muzzle_light(ra, rb, &inner, &outer);
        q2_light_add_dynamic(&c->light_world, at, colour, inner, outer, 0, 0);
    }

    if (c->sim[0].fx_ready) {
        s32 corner_local[4][3], corner[4][3];

        q2_model_local_to_world(inst, local, world);
        q2_fx_flash_corners(local, q2_fx_flash_angle(c->flash_spin,
                                                     (u32)c->frame_index),
                            corner_local);
        for (k = 0; k < 4; k++)
            q2_model_local_to_world(inst, corner_local[k], corner[k]);
        q2_fx_flash_add(&c->sim[0].fx, (const s32 (*)[3])corner, world,
                        q2_sim_fx_area(&c->sim[0], 0, world));
    }
    c->cre_flashes++;
}

/* One traced round, and what it met. */
static void client_cre_round(client *c, q2_monster *m, const s32 start[3],
                             const s32 dir[3], s16 damage, s16 mod,
                             bool mark_world)
{
    s32 at[3];
    s32 hit = q2_sim_creature_hitscan(&c->sim[0], client_cre_actor(c, m),
                                      start, dir, damage, mod, mark_world, at);

    c->cre_rounds++;
    if (hit == Q2_SIM_HIT_PLAYER) {
        c->cre_rounds_player++;
    } else if (hit >= 0) {
        c->cre_rounds_creature++;
        /* T_Damage's tail runs now, inside the shooter's think, as on the
         * console — see client_cre_drain_one. */
        if (c->cre_actor && (u32)hit < c->creatures.set.count)
            client_cre_drain_one(c, (u32)hit);
    } else {
        c->cre_rounds_world++;
    }

    /* Where the traced creature's rounds went, beside its per-tick line. */
    if (c->trace_cre >= 0 && m == &c->creatures.set.monsters[c->trace_cre])
        Q2_INFO("round frame %d from (%d,%d,%d) -> %s at (%d,%d,%d)",
                (int)m->frame, start[0], start[1], start[2],
                hit == Q2_SIM_HIT_PLAYER ? "player"
                : hit >= 0 ? "creature" : "world", at[0], at[1], at[2]);
}

/*
 * A CREATURE'S SHOT, WITH ITS OWN FIGURES — and, for the creatures whose aim
 * is transcribed, ITS OWN DIRECTION.
 *
 * An AIMED hitscan (the Soldier's bullet and shotgun, the Infantry's bullet)
 * goes through the engine's one bullet path, q2_sim_creature_hitscan: from
 * the posed muzzle along the module's own aim, `<< 2` into halfwords as
 * 0x80061E2C stores it — 16384 units — to the first creature, the player or
 * the wall. It hits what is in front of it. A guard's aimed round still
 * misses by its crandom spread; a dying guard's round goes where the body
 * faces as it falls; and the Infantry's death burst climbs the wall and
 * crosses the ceiling, and hurts the player only if the player stands in it.
 *
 * An aimed BOLT or ROCKET flies: the console's spawners launch a projectile,
 * and q2_sim_creature_launch launches the same one the player's gun does —
 * the Soldier's blaster bolt, the Tank Commander's bolts and rockets.
 *
 * An UNAIMED shot — a creature whose muzzle and aim are not transcribed (the
 * Gunner, the Arachner) — is resolved as before: at the player, when the
 * player is its enemy and in sight. Nothing more is known.
 */
static void client_cre_shot(q2_monster *m, const q2_cre_shot *shot, void *user)
{
    client *c = (client *)user;
    s32 shots;
    int mod;
    q2_actor *attacker;
    bool hitscan;

    if (!c || !c->creatures_ready || !m || !shot)
        return;

    /*
     * NO ENEMY GATE HERE, and there used to be one: `m->enemy != sight`
     * returned before anything else, so every shot fired with no enemy or at
     * a dead one vanished — flash, report and count with it. The engine does
     * not refuse those (TankMachineGun, module+0xF90: `beq a3, zero` and no
     * load of the enemy's +0x108; crebind.c no longer refuses them either),
     * so the flash and the count happen for EVERY shot the module fires.
     */

    /*
     * A shot with no damage read is declined rather than guessed — the same
     * rule the sound hook follows for a name the module does not carry.
     */
    if (shot->damage <= 0) {
        c->cre_fire_no_figures++;
        return;
    }

    c->cre_shots++;
    if (m->dead || m->deadflag == Q2_DEAD_DEAD)
        c->cre_dying_shots++;

    switch (shot->slot) {
    case Q2_IMP_FIRE_BLASTER: mod = Q2_MOD_ENERGY_BOLT; break;
    case Q2_IMP_FIRE_RAILGUN: mod = Q2_MOD_RAIL;        break;
    case Q2_IMP_FIRE_ROCKET:  mod = Q2_MOD_ROCKET;      break;
    case Q2_IMP_FIRE_GRENADE: mod = Q2_MOD_GRENADE;     break;
    default:                  mod = Q2_MOD_BULLET;      break;
    }

    /*
     * The two hitscan spawners' own business, before their trace: the report
     * (Q2_CRE_SND_*) and object+0x10C's sign bit, which is this frame's flash
     * for a module that draws one (client_cre_flash).
     */
    hitscan = shot->slot == Q2_IMP_FIRE_BULLET ||
              shot->slot == Q2_IMP_FIRE_SHOTGUN;
    if (hitscan) {
        const char *report = shot->slot == Q2_IMP_FIRE_SHOTGUN
                           ? Q2_CRE_SND_SHOTGUN : Q2_CRE_SND_BULLET;
        q2_vag vag;

        /*
         * Counted on the BANK, not on a voice starting: client_play_sound_at
         * returns false with no audio device, which is every headless run.
         */
        if (client_find_sound(c, report, &vag))
            client_play_sound_at(c, report, m->pos);
        else
            c->cre_sound_missing++;
        c->cre_fire_sounds++;

        client_cre_flash(c, m);
    }

    /*
     * An aimed BOLT or ROCKET flies. monster_fire_blaster (0x80062000) and
     * monster_fire_rocket (0x8006210C) hand the player's own spawners a
     * velocity made from the aim, so it is the same projectile the player's
     * gun throws — trail, body, burst and splash — and q2_sim_creature_launch
     * makes the player a target of it. Each plays its report: the blaster
     * wrapper itself through SoundByCreature (0x80062088, the handle at
     * 0x800B2B34 that the player's blaster plays too), the rocket spawner its
     * launch (0x8004B26C, 0x800B2B4C).
     */
    if (shot->aimed && (shot->slot == Q2_IMP_FIRE_BLASTER ||
                        shot->slot == Q2_IMP_FIRE_ROCKET)) {
        bool bolt = shot->slot == Q2_IMP_FIRE_BLASTER;
        const char *report = bolt ? "wep_blastf1a" : "wep_rocklf1a";
        q2_vag vag;

        if (client_find_sound(c, report, &vag))
            client_play_sound_at(c, report, shot->start);
        else
            c->cre_sound_missing++;
        c->cre_fire_sounds++;

        if (q2_sim_creature_launch(&c->sim[0], client_cre_actor(c, m),
                                   bolt ? Q2_FK_BOLT : Q2_FK_ROCKET,
                                   shot->start, shot->dir,
                                   (s16)shot->damage, m->angles[2]) >= 0)
            c->cre_launched++;
        return;
    }

    if (shot->aimed && hitscan) {
        s32 dir[3];
        int k;

        /* 0x80061E2C / 0x80061F1C: `sll 2` and stored with `sh`. */
        for (k = 0; k < 3; k++)
            dir[k] = (s16)(shot->dir[k] * 4);

        if (shot->slot == Q2_IMP_FIRE_SHOTGUN) {
            int p;

            for (p = 0; p < Q2_CRE_SHOTGUN_PELLETS; p++) {
                s32 jx = ((q2_random_next() - 16384) * 1024) >> 14;
                s32 jy = ((q2_random_next() - 16384) * 1024) >> 14;

                dir[0] = (s16)(dir[0] + jx);
                dir[1] = (s16)(dir[1] + jy);
                client_cre_round(c, m, shot->start, dir, (s16)shot->damage,
                                 (s16)mod, true);
            }
        } else {
            client_cre_round(c, m, shot->start, dir, (s16)shot->damage,
                             (s16)mod, hitscan);
        }
        return;
    }

    /*
     * The pellet count is the ENGINE's, not the module's: 0x80061FD4 loops
     * five times whatever the module passed (Q2_CRE_SHOTGUN_PELLETS above).
     * Every other slot fires once — the module's `count` is carried because it
     * is what the module passes, and it is read here only to keep a figure of
     * zero from firing nothing.
     */
    shots    = shot->slot == Q2_IMP_FIRE_SHOTGUN ? Q2_CRE_SHOTGUN_PELLETS
             : shot->count > 0                   ? shot->count
                                                 : 1;
    attacker = client_cre_actor(c, m);   /* 0x800582C8: the skill-0 halving */

    if (m->enemy != &c->creatures.sight)
        return;                          /* fired, but not at the player */

    while (shots-- > 0) {
        if (!q2_visible(m, &c->creatures.sight))
            break;
        q2_sim_hurt_player(&c->sim[0], attacker, (s16)shot->damage, (s16)mod,
                           c->creatures.sight.pos);
    }
}

static void client_cre_sound(q2_monster *m, int which, void *user)
{
    client *c = (client *)user;
    const char *name = NULL;

    (void)m;
    if (!c)
        return;

    /*
     * The module names a sound by an index into its OWN table, and that table
     * is not resolved yet (#6). This used to map indices 0 and 1 to
     * `cre_pain1` and `cre_die1`, which was an invention twice over: the bank
     * has no such names, so it silently played nothing, and the index-to-name
     * mapping was never read.
     *
     * The bank's real convention is `<creature>_<action><n>`, the same shape as
     * `wep_` and `itm_`: BASE0 carries `sol_atck1`, `sol_atck2`, `sol_atck3`,
     * `sol_deth1..3`, `sol_idle1`, `sol_pain1`, `sol_pain2` and `sol_srch1` —
     * exactly the five families id's soldier has. So the names are there to be
     * matched once the module's table says which index is which; until then
     * this stays silent rather than guessing, and the index is recorded so a
     * caller can see what was asked for.
     */
    c->cre_sounds++;
    c->cre_last_sound = which;

    /*
     * The Soldier's names are read out of its module and every one of them is
     * in the map's bank, so it can actually be played. Another creature's
     * table has not been read, so it stays silent rather than borrowing these.
     */
    /*
     * The Soldier's names are transcribed; every other module carries its own
     * table and it is read the same way. The transcription is preferred where it
     * exists because it was read out of code rather than inferred from slot
     * order.
     *
     * NOT "all seven creatures make their own sounds", which this comment used
     * to claim. `q2psx-inspect creatures` prints each table, and two of the
     * seven come back EMPTY:
     *
     *     Soldier  8   Insane 3   Arachner 6   Gunner 6   Infantry 4
     *     Tankcomm 0   Berserk 0
     *
     * The finder locates the module's own name string and then takes the first
     * run of three consecutive 12-byte name slots after it; those two modules do
     * not carry the anchor where it looks, so it falls back to scanning from
     * zero. Both results were checked against the modules' own code and are
     * correct: the Berserk's thirteen run from module+0x144 to +0x1D4 and the
     * Tank Commander's eight likewise. See openquestions #60 and #61.
     */
    /*
     * `which` is the module ADDRESS of the sound handle, not an index —
     * `cre_actions.c` passes `q2_cre_action.addr` straight through. This used
     * to hand it to `q2_creature_world_sound_name`, which indexes a name list,
     * so every decoded creature was asking for name 0x80101758 of eight and
     * getting nothing. Resolving the address against the module's own
     * registrations is what the module itself does (creature.h).
     */
    name = q2_creature_world_sound_for_addr(&c->creatures, m, (u32)which);

    /*
     * The Soldier's transcription still wins where it applies, because it was
     * read out of code rather than decoded — but it is indexed, so it is only
     * consulted when the address lookup found nothing.
     */
    if (!name)
        name = q2_cre_soldier_sound_name(which);
    if (!name)
        name = q2_creature_world_sound_name(&c->creatures, m, (u32)which);
    /*
     * Counted on whether the BANK HAS IT, not on whether it played.
     *
     * `client_play_sound` returns false when `c->audio` is NULL, which is every
     * headless run — so `%u not in bank` was reporting the absence of an audio
     * device and read as "this map does not carry the creature's sounds". JAIL4
     * carries `sol_idle1`, `sol_sght1`, `sol_pain1`, `sol_deth2` and the rest;
     * they were found every time and the counter said otherwise.
     */
    if (name) {
        q2_vag vag;

        /*
         * WHAT THE SLOT ACTUALLY HOLDS, which is not always what it was
         * registered with. Two modules overwrite a handle their map's bank
         * cannot fill with one it can, in their own spawn code: the Soldier's
         * pain and death trios (module+0xE50..+0xF28) and the Tank Commander's
         * idle, which becomes its footstep (module+0x808..+0x820). Every fill
         * store is guarded by a `bne` on the destination, so a handle that
         * resolved is never replaced. q2_cre_sound_resolve is that whole rule
         * (crebind.h); a name in no group comes back as it went in.
         */
        const char *plays = q2_cre_sound_resolve(m, name, client_cre_bank_has,
                                                 c);

        /*
         * A slot that resolves to nothing — no fallback the bank carries — is
         * the console's NULL handle and plays nothing, as is a name the bank
         * does not have. Both are SILENCE and counted as missing, never as
         * unnamed: the site did name a sound.
         *
         * On this disc that silence is usually correct rather than a gap:
         * `ara_idle1`, `ara_srch1`, `ber_idle1` and `ber_srch1` are registered
         * by their modules, appear in NO map's bank anywhere on the disc, and
         * have no fallback. (`tnk_idle1` used to be on this list; it is the one
         * the Tank Commander's own code replaces, with `tnk_step`.) See
         * openquestions #61.
         */
        if (!plays || !client_find_sound(c, plays, &vag))
            c->cre_sound_missing++;
        else
            client_play_sound_at(c, plays, m->pos);
    } else {
        c->cre_sound_unnamed++;
    }
}

/* "Does this map's SNDVRAM bank carry it" — q2_cre_sound_resolve's question,
 * answered the way the play site already asks it. */
static bool client_cre_bank_has(const char *name, void *user)
{
    q2_vag vag;

    return client_find_sound((client *)user, name, &vag);
}

/* BIOS rand(), 0x80089E28, on the same saved stream as creature decisions. */
static int client_bios_rand(void)
{
    return q2_random_next();
}

/*
 * 0x8002085C — ONE CREATURE'S DROP, handed over by q2_monster_drop_flush.
 *
 * The spawner itself is item.c's (q2_item_drop_spawn, with the think
 * 0x80020C48): the position is the request's EXACTLY — no Q2_ITEM_SPAWN_LIFT
 * and no floor sweep, which is why this does not go through q2_item_spawn
 * with a synthesised place record — and the flight is the toss mover's. What
 * this side supplies is what only it has: the entity set, the item table off
 * the disc, the CastLists the model is looked up in, and the cell.
 *
 * THE CELL. The console hands the spawner the creature's own SecondaryCol cell
 * (`lh 78(a3)` at 0x80020838, obj+0xA2). q2_monster keeps none, so it is
 * found for the recorded position — the same point, in the same hull.
 */
static void client_cre_drop(const q2_monster_drop_request *req, void *user)
{
    client *c = (client *)user;
    const q2_model_bank *banks[2];
    u32 nbanks = 0;
    s32 cell = -1;
    q2_entity *e;

    if (!c || !req)
        return;

    c->cre_drop_requests++;

    if (!c->sim[0].entities_ready) {
        c->cre_drops_declined++;
        return;
    }

    /* The map's CastList first — it is the one the entity draw resolves items
     * against — then the zone's, as the creature models are searched. */
    if (c->model_bank_ready)
        banks[nbanks++] = &c->model_bank;
    if (c->zone_bank_ready)
        banks[nbanks++] = &c->zone_bank;

    if (c->sim[0].coll_ready)
        cell = q2_coll_find_node(&c->sim[0].coll, req->pos, -1, true);

    e = q2_item_drop_spawn(&c->sim[0].entities,
                           c->item_table_ready ? &c->item_table : NULL,
                           banks, nbanks, req->item_id, req->heading,
                           req->pos, cell, client_bios_rand);
    if (e) {
        c->cre_drops++;
        Q2_INFO("drop: f%ld item %u '%s' at (%d,%d,%d) cell %d",
                c->frame_index, (unsigned)req->item_id, e->model,
                req->pos[0], req->pos[1], req->pos[2], (int)cell);
    } else {
        c->cre_drops_declined++;
        Q2_INFO("drop: f%ld item %u declined by the spawner", c->frame_index,
                (unsigned)req->item_id);
    }
}

/*
 * THE MESH HOOK (sim.h `fx_mesh`) — which posed mesh a combat actor's damage
 * effects spawn on, as 0x8006CC44 answers it for the effect drawers.
 *
 * Creatures only. The local player's own actor has no body in first person, so
 * it answers false and its effects keep their timers without drawing quads.
 * Other players' bodies are drawn too, but their poses are not kept past the
 * draw; they answer false as well and are a followup.
 *
 * The vertex callback reads the LAST DRAWN pose of that creature
 * (client_fx_pose, above the client struct), whose storage is the client's and
 * lives for the zone — so the context outlives the q2_sim_combat_tick that
 * asks for it, as the hook's contract requires.
 */
static void client_fx_vertex(void *ctx, s32 index, s32 out[3])
{
    const q2_model_instance *inst = (const q2_model_instance *)ctx;

    /* Every index a drawer asks for is below the total the source reported,
     * so this only refuses on a model that failed to decode; the origin is
     * then the one place that is certainly on the creature. */
    if (!q2_model_world_vertex(inst, index, out))
        (void)q2_model_world_vertex(inst, -1, out);
}

static bool client_fx_mesh(void *user, const q2_actor *a, q2_fx_mesh_src *out)
{
    client *c = (client *)user;
    size_t idx;

    if (!c || !a || !out)
        return false;

    c->fx_mesh_asked++;

    if (!c->creatures_ready || !c->cre_actor || !c->cre_fx ||
        a < c->cre_actor)
        return false;
    idx = (size_t)(a - c->cre_actor);
    if (idx >= c->creatures.set.count)
        return false;

    /* Removed from play, never drawn since the load, or no model: no mesh to
     * sample, which the drawers treat as the console treats a model-less
     * entity. */
    if (!c->creatures.set.monsters[idx].in_use || !c->cre_model_ok[idx] ||
        !c->cre_fx[idx].valid)
        return false;

    out->vertex = client_fx_vertex;
    out->ctx    = &c->cre_fx[idx].inst;
    out->total  = q2_model_total_verts(c->cre_fx[idx].inst.model);
    c->fx_mesh_given++;

    /* A mesh handed over while one of the damage effects is running — the
     * calls on which a drawer actually samples it. effect[3] has no reader
     * on the disc and is left out (combat.h). */
    if (a->effect[0] || a->effect[1] || a->effect[2] || a->effect[4] ||
        a->effect[5])
        c->fx_mesh_armed++;
    return true;
}

/*
 * THE GIB DISPATCHER'S QUESTIONS (modelent.h, q2_gib_describe_fn): what a
 * dying record does not carry and this side does.
 *
 * A CREATURE is already placed by its own `pos`. What is added here is the
 * actor's knockback (+0x2F8, which the dispatcher reads as halfwords and adds
 * to +0xE0 for the throw — 0x8007D100..0x8007D134 in the default Chest arm,
 * the same `lhu` pair in the others), the posed mesh 0x8005B320's blood spray
 * walks, and the area byte the spray's groups carry (+0x9E):
 *
 *   - The mesh comes through client_fx_mesh, the hook the damage effects use,
 *     so the spray samples the same last-drawn pose the crackle does. It is
 *     asked while the record is still in play — q2_monster_corpse_tick clears
 *     `in_use` only after the dispatch returns. This is not one of combat's
 *     asks, so the hook's three counters are put back and `gib_mesh_posed`
 *     counts it instead.
 *   - The area is the body's remembered +0x9E (client_cre_area): the same
 *     value this client's creature draw files the body under, so the spray
 *     sorts with the body it came out of.
 *
 * A PLAYER is placed only here: +0x54 is the ORIGIN frame, Q2_EYE_BASE above
 * the feet (sim.h), `vel` is +0xE0 in the same raw units (per dt before the
 * mover's Q2_VEL_DIV), and `impulse` is the +0x2F8 accumulator itself. The
 * death record's own `velocity` (playerdeath.h, "+0xE0") is never handed the
 * body's in this port — its init zeroes it and only the corpse friction
 * touches it after — so the sim's is used. +0xA4 is taken as the same
 * point — INFERRED; the player's draw origin is not reconstructed.
 */
static void client_gib_describe(void *user, q2_gib_victim kind,
                                const void *who, q2_gib_parent *p)
{
    client *c = (client *)user;
    q2_coll_node cn;
    int k;

    if (!c || !who || !p)
        return;

    if (kind == Q2_GIB_VICTIM_MONSTER) {
        const q2_monster *m = (const q2_monster *)who;
        size_t i;

        if (!c->creatures_ready || m < c->creatures.set.monsters)
            return;
        i = (size_t)(m - c->creatures.set.monsters);
        if (i >= c->creatures.set.count)
            return;

        if (c->cre_actor) {
            const u32 asked = c->fx_mesh_asked;
            const u32 given = c->fx_mesh_given;
            const u32 armed = c->fx_mesh_armed;

            for (k = 0; k < 3; k++)
                p->knockback[k] = (s16)c->cre_actor[i].knockback[k];
            if (p->knockback[0] || p->knockback[1] || p->knockback[2])
                c->gib_pushed++;
            if (client_fx_mesh(c, &c->cre_actor[i], &c->gib_mesh)) {
                p->mesh = &c->gib_mesh;
                c->gib_mesh_posed++;
            }
            c->fx_mesh_asked = asked;
            c->fx_mesh_given = given;
            c->fx_mesh_armed = armed;
        }
        {
            s32 area = client_cre_area(c, (u32)i, NULL);

            if (area >= 0)
                p->area = (u8)area;
        }
        return;
    }

    {
        const q2_player_death *d = (const q2_player_death *)who;
        const q2_player *pl;
        size_t pi;

        if (d < c->death)
            return;
        pi = (size_t)(d - c->death);
        if (pi >= Q2_MP_MAX_PLAYERS)
            return;
        pl = &c->sim[0].player[pi];

        p->placed = true;
        p->pos[0] = pl->pos[0];
        p->pos[1] = q2_sim_origin_y(pl->pos[1]);
        p->pos[2] = pl->pos[2];
        for (k = 0; k < 3; k++) {
            p->origin[k]    = p->pos[k];
            p->velocity[k]  = (s16)pl->vel[k];
            p->knockback[k] = pl->impulse[k];
        }
        if (c->sim[0].coll_ready && pl->ent.node >= 0 &&
            q2_collision_get_node(&c->sim[0].coll, (u32)pl->ent.node, &cn))
            p->area = cn.contents;
    }
}

/*
 * The AI clock, which is not the frame clock.
 *
 * `next_think` is on the engine's 10 Hz tick (monster.h), so the tick is run
 * from an accumulator rather than once per drawn frame — otherwise a creature
 * would think three times as often at 30 fps as it does on the console, and
 * every one of the AI's timers is expressed in those ticks.
 */
static void client_creatures_tick(client *c, float dt, const s32 eye[3])
{
    int guard = 0;

    if (!c->creatures_ready)
        return;

    c->ai_accum += (double)dt;
    while (c->ai_accum >= 0.1 && guard++ < 8) {
        c->ai_accum -= 0.1;
        c->ai_thoughts += q2_creature_world_tick(&c->creatures, eye,
                                                 c->sim[0].combat.inv.health);

        /*
         * One creature, one line per AI tick: the move it is playing, the frame
         * it is on, and its attack state. Counters say what happened across a
         * capture and cannot say what happened between two consecutive ticks —
         * and "the attack move is replaced within five ticks" is a question
         * only consecutive ticks can answer. See openquestions #57.
         */
        if (c->trace_cre >= 0 &&
            (u32)c->trace_cre < c->creatures.set.count &&
            c->trace_ticks < 400) {
            const q2_monster *m = &c->creatures.set.monsters[c->trace_cre];

            /*
             * Position and facing come with the frame, because "it moonwalks"
             * is a claim that the two disagree: the walk clip advances while
             * the body travels somewhere the model is not pointing. Neither
             * number was in this line, so the claim could only be argued from
             * the screen.
             *
             * `step` is the distance covered since the previous tick and
             * `drift` the angle between the direction actually travelled and
             * the direction faced, in the engine's 4096-step circle. A walking
             * creature should hold drift near zero; a moonwalking one holds it
             * near 2048.
             */
            {
                s32 dx = m->pos[0] - c->trace_prev[0];
                s32 dz = m->pos[2] - c->trace_prev[2];
                s32 step = (s32)sqrt((double)dx * dx + (double)dz * dz);
                s32 drift = -1;

                if (step > 1) {
                    /*
                     * YAW ZERO IS +Z, and the circle turns toward +X. Measured
                     * rather than assumed: a soldier walking due +z holds yaw
                     * 0, one walking due -x holds 3072, and one walking the
                     * +x+z diagonal holds 512. atan2(dz, dx) fits the diagonal
                     * and misses both axes by a quarter turn; atan2(dx, dz)
                     * fits all three.
                     */
                    double a = atan2((double)dx, (double)dz);
                    s32 moved = (s32)(a * 4096.0 / (2.0 * 3.14159265358979));
                    drift = (moved - (s32)m->angles[2]) & 4095;
                    if (drift > 2048) drift -= 4096;
                }

                Q2_INFO("t%-4u move %-4d frame %-4d as %d flags %08X"
                        "  pos %d,%d,%d yaw %-5d ideal %-5d step %-4d drift %-5d"
                        "  %s%s%s",
                        c->trace_ticks,
                        m->currentmove ? m->currentmove->first_frame : -1,
                        m->frame, m->attack_state, m->aiflags,
                        m->pos[0], m->pos[1], m->pos[2],
                        (int)m->angles[2],
                        (int)m->ideal_yaw, step, drift,
                        m->enemy ? "enemy " : "no-enemy ",
                        m->dead ? "dead " : "",
                        m->in_use ? "" : "gone");

                c->trace_prev[0] = m->pos[0];
                c->trace_prev[1] = m->pos[1];
                c->trace_prev[2] = m->pos[2];
            }
            c->trace_ticks++;
        }
    }
    if (c->ai_accum > 0.5)
        c->ai_accum = 0.0;
}

/*
 * Advance an animation once per DISPLAY frame, however many viewports ask for
 * its pose. `source` distinguishes an absolute named timeline from the one
 * unnamed-move fallback; changing either the move or the source is a rebase,
 * exactly where retail installs a new runtime animation record.
 */
static s32 client_model_anim_sample(client_model_anim *a,
                                    const q2_mmove *move, u8 source,
                                    s32 target, s32 dt, long frame)
{
    bool rebased = false;

    if (!a)
        return target;

    if (!a->cursor.ready || a->move != move || a->source != source) {
        q2_model_cursor_reset(&a->cursor, target);
        a->move   = move;
        a->source = source;
        rebased   = true;
    }

    /* A newly installed record starts at phase zero. Subsequent display frames
     * add phase while its AI base is unchanged; a new base resets it. */
    if (rebased) {
        a->frame_stamp = frame;
        a->stamped     = true;
    } else if (!a->stamped || a->frame_stamp != frame) {
        q2_model_cursor_phase(&a->cursor, target, dt);
        a->frame_stamp = frame;
        a->stamped     = true;
    }
    return a->cursor.position;
}

static q2_player_move client_player_visual_move(client *c, int pi)
{
    client_player_anim *a = &c->player_anim[pi];
    const q2_player *p = &c->sim[0].player[pi];
    const q2_player_death *d = &c->death[pi];
    u32 serial = pi == 0 ? c->sim[0].combat.shot_serial
                         : c->sim[0].pcombat[pi].shot_serial;
    u32 pain = c->sim[0].player[pi].pain_serial;
    q2_player_anim want;
    q2_player_move pick;

    if (d->stage != Q2_PDEATH_ALIVE)
        return d->move != Q2_PMOVE_NONE ? d->move : Q2_PMOVE_DEATH1;

    if (serial != a->shot_serial) {
        a->shot_serial    = serial;
        a->attack_latched = true;
    }
    if (pain != a->pain_serial) {
        a->pain_serial  = pain;
        a->pain_latched = true;
    }

    /*
     * WHICH ANIMATION IS WANTED, as one id, exactly as 0x8003A1C8 builds one
     * in s5 before handing it to `player_anim` at 0x8003AFC8. PAIN outranks
     * everything: 0x8003AE08 `addiu s5, zero, 3` is unconditional once the
     * damage byte is set, where every other request is a branch.
     *
     * The three below it keep the order this function already had.
     * DEVIATION, called out rather than quietly fixed: the console lets
     * ATTACK replace only STAND (0x8003ADAC `bne s5, zero`), so a console
     * player who fires while running keeps running; this latch puts ATTAK
     * ahead of JUMP and RUN. Changing that is a separate reading of
     * 0x8003A930/0x8003AB5C and is not made here.
     */
    if (a->pain_latched)          want = Q2_PANIM_PAIN;
    else if (a->attack_latched)   want = Q2_PANIM_ATTACK;
    else if (!p->on_ground)       want = Q2_PANIM_JUMP;
    else if (abs(p->vel[0]) + abs(p->vel[2]) > 8)
                                  want = Q2_PANIM_RUN;
    else                          want = Q2_PANIM_STAND;

    /*
     * And 0x8003CE14 itself decides what that becomes. This used to be four
     * `return`s of the port's own, so `q2_player_anim_pick` was reconstructed
     * -- the PAIN arm at 0x8003CF74, the hold rule at 0x8003D188 -- and asked
     * for nothing but DEATH, and Male2's Pain 1/2/3 were never played.
     *
     * THE ROLL IS THE PORT'S. 0x8003CF74 picks the clip with `rand() % 3`
     * inside the game think; this call is on the DISPLAY path, where drawing
     * from the sim's generator would move a stream the shot spread and the
     * creature AI share. The flinch serial is used instead, so consecutive
     * hits still walk the three clips and a headless run stays reproducible.
     */
    pick = q2_player_anim_pick(want, a->move,
                               a->wrapped ? Q2_PDEATH_ANIM_WRAPPED : 0u,
                               a->pain_serial);
    if (pick != Q2_PMOVE_NONE)
        return pick;

    /* 0x8003D1F8 returns without installing anything: the move already
     * playing keeps playing. */
    return a->move != Q2_PMOVE_NONE ? a->move : Q2_PMOVE_STAND;
}

/* Build a player pose from Male2's ten named retail moves. The coloured body
 * models have the same eighteen-part geometry but only a rest clip; they use
 * this pose exactly so colour selection does not throw animation away. */
static bool client_player_pose(client *c, int pi, q2_model_pose *pose)
{
    client_player_anim *a = &c->player_anim[pi];
    q2_player_death *d = &c->death[pi];
    q2_player_move move = client_player_visual_move(c, pi);
    q2_model_move mv;
    q2_model_anim clip;
    s32 first, limit;
    u32 within;
    bool rebased = false;

    if (!c->player_anim_base_ok || !pose || move == Q2_PMOVE_NONE ||
        !q2_model_move_by_name(&c->player_model[0],
                               q2_player_move_name(move), &mv))
        return false;

    first = (s32)mv.start * 5;
    limit = first + (s32)q2_model_move_frames(&mv) *
                          Q2_MODEL_TICKS_PER_FRAME;
    if (limit <= first)
        return false;

    if (!a->cursor.ready || a->move != move) {
        q2_model_cursor_reset(&a->cursor, first);
        a->move    = move;
        rebased    = true;
        a->wrapped = false;   /* a freshly installed move has not wrapped */
    }

    /* Installing a retail runtime move exposes its first key for one display
     * frame. Advancing in the same call skipped that key entirely at 30 Hz. */
    if (rebased) {
        a->frame_stamp = c->frame_index;
        a->stamped     = true;
    } else if (!a->stamped || a->frame_stamp != c->frame_index) {
        s32 next = a->cursor.position + c->screen.dt;
        bool terminal = q2_player_move_is_death(move);

        if (next >= limit) {
            if (terminal) {
                /* The final key starts ten position units before the end. */
                a->cursor.position = limit - Q2_MODEL_TICKS_PER_FRAME;
                a->cursor.target   = a->cursor.position;
                q2_player_death_anim_ended(d);
            } else {
                s32 span = limit - first;
                a->cursor.position = first + (next - first) % span;
                a->cursor.target   = a->cursor.position;
                if (move == Q2_PMOVE_ATTAK)
                    a->attack_latched = false;
                /* 0x8003DF90 raises bit 0 of entity+0x102 when a move runs
                 * past its end; the chooser reads it to know a pain clip
                 * has played out and may now be replaced. */
                if (q2_player_move_is_pain(move))
                    a->pain_latched = false;
                a->wrapped = true;
            }
        } else {
            a->cursor.position = next;
            a->cursor.target   = next;
        }
        a->frame_stamp = c->frame_index;
        a->stamped     = true;
    }

    if (a->cursor.position < 0 ||
        !q2_model_anim_at_position(&c->player_model[0],
                                   (u32)a->cursor.position,
                                   &clip, &within))
        return false;

    return q2_model_pose_at(&c->player_model[0], &clip, within, pose) == Q2_OK;
}

/*
 * How many rotators are standing at an angle other than the one they started
 * at. `rot moved` counts TICK movement and a SNAP never tick-moves — it takes
 * its whole rotation the moment it is asked (0x8002BFD8) — so a level whose
 * only rotator is a button reads as still while its geometry has turned.
 */
static u32 client_rot_turned(const client *c)
{
    u32 i, n = 0;

    if (!c->rotators_ready)
        return 0;

    for (i = 0; i < c->rotators.count; i++)
        if (c->rotators.rotators[i].angle != 0)
            n++;

    return n;
}

/*
 * The rotating hatches, and how many of them are not shut.
 *
 * A separate census from `client_rot_turned` because a hatch's ANGLE is not the
 * question: one whose target is 3084 and which has just finished closing sits
 * at angle 0 exactly like one that never opened, and what a run needs to know
 * is whether the seven-state machine ran to the end. `state` answers that —
 * anything but Q2_ROTST_IDLE is a hatch mid-delay, mid-sweep or standing open.
 */
static u32 client_hatches(const client *c, u32 *not_shut)
{
    u32 i, n = 0;

    if (not_shut)
        *not_shut = 0;
    if (!c->rotators_ready)
        return 0;

    for (i = 0; i < c->rotators.count; i++) {
        const q2_rotator *r = &c->rotators.rotators[i];

        if (r->kind != Q2_ROT_TARGET)
            continue;
        n++;
        if (not_shut && r->state != Q2_ROTST_IDLE)
            (*not_shut)++;
    }

    return n;
}

/*
 * The same question for PISTONs, which have no state to read: `running` is the
 * console's obj+0x52 latch and `strokes` its obj+0x4E half-stroke counter, and
 * the pair is the only way to tell a piston that has finished its count from
 * one that was never triggered.
 */
static u32 client_pistons(const client *c, u32 *running, u32 *done, s32 *offset)
{
    u32 i, n = 0;

    if (running) *running = 0;
    if (done)    *done    = 0;
    if (offset)  *offset  = 0;
    if (!c->movers_ready)
        return 0;

    for (i = 0; i < c->movers.count; i++) {
        const q2_mover *m = &c->movers.movers[i];

        if (m->prim != Q2_UF_PISTON)
            continue;
        n++;
        if (m->running && running)
            (*running)++;
        if (done)
            *done += m->strokes_done;
        /* SIGNED, and that is the point: the console displaces by MINUS
         * target (0x8002CFD0 `subu v0, zero, v0`) and this port used to
         * displace by plus it, so the sum's sign alone says which way the
         * three slabs of a pumping station are travelling. */
        if (offset)
            *offset += m->offset;
    }

    return n;
}

/*
 * How many of this creature's moves before `mv` have the same length, so a move
 * can pick the matching one when several clips share a length. Both lists are
 * walked in their own order, which is the same technique the move NAMES use.
 */
/* The module's own name for a move, or NULL. Mirrors client_move_ordinal, and
 * exists because the engine reaches an animation by name (0x8006D330). */
static const char *client_move_name(const q2_monster *m, const q2_mmove *mv)
{
    const q2_cre_bind *b = q2_cre_bind_for(m);
    u32 i;

    if (!b || !mv || !b->move_name)
        return NULL;

    for (i = 0; i < b->move_count && i < b->move_name_count; i++)
        if (&b->move[i] == mv)
            return b->move_name[i];

    return NULL;
}

static u32 client_move_ordinal(const q2_monster *m, const q2_mmove *mv)
{
    const q2_cre_bind *b = q2_cre_bind_for(m);
    s32 len;
    u32 i, n = 0;

    if (!b || !mv)
        return 0;

    len = mv->last_frame - mv->first_frame + 1;
    for (i = 0; i < b->move_count; i++) {
        if (&b->move[i] == mv)
            break;
        if (b->move[i].last_frame - b->move[i].first_frame + 1 == len)
            n++;
    }

    return n;
}

/*
 * The tie-break the spawn selector asks for. The original's is the engine's own
 * RNG; any source does here, because it is only consulted when two spawn points
 * are exactly equally far from everybody, and it must not be a constant or the
 * same point wins every draw.
 */
static u32 client_mp_rng(void *user)
{
    client *c = (client *)user;

    /* Numerical Recipes' LCG. The value is used modulo a small count. */
    c->mp_rng_state = c->mp_rng_state * 1664525u + 1013904223u;
    return c->mp_rng_state >> 16;
}

/*
 * One frame of the multiplayer session — the per-frame hook QMULTI.C installs
 * into the engine's level slot, which nothing in this port had ever called.
 *
 * The clock is the engine's at 0x800AEBAC and advances by the frame's dt, both
 * in the sim's units, because that is what the time limit is compared against:
 * `level_time > minutes * 18000`, and 18000 units is sixty seconds at 300 to
 * the second.
 */
static void client_mp_tick(client *c, s32 ticks)
{
    q2_mp_request req;

    if (!c->mp_enabled || c->mp_last_request != Q2_MP_REQ_NONE)
        return;                     /* the match is over and asked for a screen */

    if (ticks <= 0)
        return;
    c->mp_level_time = c->sim[0].level_time;

    req = q2_mp_frame(&c->mp, c->mp_level_time, ticks);

    /* The frame that ends it, announced once. */
    if (c->mp.end != Q2_MP_RUNNING && !c->mp_reported) {
        c->mp_reported = true;
        Q2_INFO("multiplayer: %s at %d dt (%d s) — banner '%s'",
                c->mp.end == Q2_MP_END_TIME_UP     ? "time limit reached" :
                c->mp.end == Q2_MP_END_FRAG_LIMIT  ? "frag limit reached" :
                c->mp.end == Q2_MP_END_ROUND_OVER  ? "round over"         :
                c->mp.end == Q2_MP_END_MATCH_OVER  ? "match over"         :
                                                     "round drawn",
                c->mp_level_time, c->mp_level_time / 300,
                q2_mp_banner(&c->mp) ? q2_mp_banner(&c->mp) : "(none)");
    }

    if (req == Q2_MP_REQ_NONE)
        return;

    /* The main loop consumes round reloads after this frame releases the
     * current level; results take ownership of subsequent input frames. */
    c->mp_last_request = q2_mp_take_request(&c->mp);

    /* State 11 is "load MPResults". The port shows the scoreboard rather than
     * loading QMRESULT's own level, because what that level draws is its
     * module's business and what it draws it FROM is the session. */
    if (c->mp_last_request == Q2_MP_REQ_RESULTS)
        c->mp_scoreboard = true;

    {
        int w = q2_mp_find_winner(&c->mp);
        char buf[64];

        Q2_INFO("multiplayer: request %d (%s); winner %d — %s",
                (int)c->mp_last_request,
                c->mp_last_request == Q2_MP_REQ_RESULTS ? "load MPResults"
                                                        : "restart the round",
                w, q2_mp_winner_text(&c->mp, w, NULL, buf, sizeof(buf)));
        Q2_INFO("multiplayer: %s — %s, HUD set %s",
                q2_mp_score_title(c->mp.mode), q2_mp_mode_name(c->mp.mode),
                q2_mp_hud_image(true, c->mp.player_count));
    }
}

/*
 * The things player `who` can hurt: every creature, plus every OTHER player.
 *
 * A player's own hurt-actor lives in the sim — the live one's in `combat.self`,
 * a parked one's in `pcombat[i].self` — so the pointers are stable and the list
 * is rebuilt per player rather than per frame. Registering a player against
 * themselves would let a blaster bolt hit its own muzzle, which is why `who` is
 * skipped.
 *
 * Nothing registered players before this: `combat.targets` held creatures only,
 * so in a deathmatch every shot passed straight through everybody.
 */
/* The fade and dissolve thinks (0x8005B358 / 0x8005B39C) no longer append
 * to the damage sweep. DYING and DOWN corpses still do, so they can be gibbed. */
static bool client_player_targetable(const client *c, int pi)
{
    q2_pdeath_stage stage = c->death[pi].stage;
    return stage == Q2_PDEATH_ALIVE || stage == Q2_PDEATH_DYING ||
           stage == Q2_PDEATH_DOWN;
}

static u32 client_targets_for(client *c, int who)
{
    u32 n = 0, i;

    if (c->creatures_ready && c->cre_target)
        for (i = 0; i < c->creatures.set.count && n < Q2_CLIENT_MAX_TARGETS; i++)
            c->mp_target[n++] = c->cre_target[i];

    if (c->mp_enabled)
        for (i = 0; i < Q2_MP_MAX_PLAYERS && n < Q2_CLIENT_MAX_TARGETS; i++) {
            if ((int)i == who || (i > 0 && !c->sim_ready[i]) ||
                !client_player_targetable(c, (int)i))
                continue;
            /*
             * ALWAYS the parked slot, never `combat.self`.
             *
             * The list is built before `q2_sim_advance_player` swaps, and the
             * swap moves the live player's half OUT of `combat` and the target
             * player's IN. So a pointer to `combat.self` chosen here for "the
             * player who is live right now" points at somebody else by the time
             * the shot is traced — player 1 was firing 301 shots at its own
             * actor and nobody was ever hit.
             *
             * During `who`'s tick every OTHER player is parked, so
             * `pcombat[i].self` is exactly right for all of them, and `who`
             * itself is skipped above.
             */
            c->mp_target[n++] = &c->sim[0].pcombat[i].self;
        }

    q2_sim_set_targets(&c->sim[0], c->mp_target, n);

    /*
     * And the world's list, which is every player and every creature with
     * nobody left out — what a projectile in flight can hit. Built once here
     * because it does not depend on who is shooting.
     */
    if (c->mp_enabled) {
        u32 w = 0, k;

        if (c->creatures_ready && c->cre_target)
            for (k = 0; k < c->creatures.set.count &&
                        w < Q2_CLIENT_MAX_TARGETS; k++)
                c->mp_world_target[w++] = c->cre_target[k];

        for (k = 0; k < Q2_MP_MAX_PLAYERS && w < Q2_CLIENT_MAX_TARGETS; k++) {
            if (k > 0 && !c->sim_ready[k])
                continue;
            q2_actor *actor = k == (u32)c->sim[0].cur_player
                                  ? &c->sim[0].combat.self
                                  : &c->sim[0].pcombat[k].self;
            actor->takedamage = client_player_targetable(c, (int)k)
                                   ? Q2_DAMAGE_AIM : Q2_DAMAGE_NO;
            if (actor->takedamage) c->mp_world_target[w++] = actor;
        }

        q2_sim_set_world_targets(&c->sim[0], c->mp_world_target, w);
    }

    /* Once, so a run says plainly how many things a player can hit. */
    if (!c->mp_targets_logged) {
        c->mp_targets_logged = true;
        Q2_INFO("multiplayer: player %d has %u targets (%u creatures, "
                "%d other players)", who, n,
                c->creatures_ready ? c->creatures.set.count : 0,
                (int)n - (int)(c->creatures_ready ? c->creatures.set.count : 0));
    }

    return n;
}

/*
 * Any player whose health has crossed zero scores a frag for whoever did it.
 *
 * This is the engine's own hook at 0x800396AC — `(*module)->[4](killer,
 * victim)` — with the killer taken from the actor's `last_attacker`, which is
 * the byte the original keeps at entity+222. `q2_mp_killer_field` is that byte
 * as the handler reads it, and whether it counts is the hook's own gate: a
 * creature's kill (4) is nobody's frag and costs the victim nothing.
 */
static void client_score_deaths(client *c)
{
    int i;

    if (!c->mp_enabled || c->mp.end != Q2_MP_RUNNING)
        return;

    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++) {
        const q2_actor *a = (i == c->sim[0].cur_player)
                                ? &c->sim[0].combat.self
                                : &c->sim[0].pcombat[i].self;

        if (i > 0 && !c->sim_ready[i])
            continue;
        const q2_inventory *inv = i == c->sim[0].cur_player
                                      ? &c->sim[0].combat.inv
                                      : &c->sim[0].pcombat[i].inv;
        if (inv->health > 0) {
            c->mp_dead[i] = false;
            continue;
        }
        if (c->mp_dead[i])
            continue;                /* already counted this death */

        c->mp_dead[i] = true;
        {
            /*
             * THE BYTE THE HANDLER READS, not the scoring fold. 0x800396EC
             * `lb s1, 222(s0)` after the acid/lava override, and the frag hook
             * is called only when it is below 4 (0x80039774 `slti s1, 4`).
             * q2_mp_attribute_kill folded a 4 — a creature's hit, or the spawn
             * sentinel — into -1, which q2_mp_player_killed charges to the
             * victim as a suicide; the console, handed a 4, calls nothing.
             * q2_mp_player_killed's own `killer >= Q2_MP_MAX_PLAYERS` return is
             * that `slti`, and its `killer < 0 -> victim` covers the -1 the
             * override writes. The log below now shows the raw byte (4, 23 or
             * an index) where it showed -1.
             */
            int killer = q2_mp_killer_field(a->last_attacker, a->last_mod);

            q2_mp_player_killed(&c->mp, killer, i);
            c->mp_deaths++;
            Q2_INFO("multiplayer: player %d killed by %d — frags %d %d %d %d",
                    i, killer, c->mp.frags[0], c->mp.frags[1],
                    c->mp.frags[2], c->mp.frags[3]);
        }
    }
    if (c->mp.mode == Q2_MP_VERSUS) {
        q2_mp_player_view players[Q2_MP_MAX_PLAYERS] = {0};
        for (i = 0; i < c->mp.player_count; i++) {
            const q2_inventory *inv = i == c->sim[0].cur_player
                                          ? &c->sim[0].combat.inv
                                          : &c->sim[0].pcombat[i].inv;
            players[i].alive = (i == 0 || c->sim_ready[i]) && inv->health > 0;
        }
        q2_mp_versus_check(&c->mp, players, (u32)c->mp.player_count);
    }

}

/* ------------------------------------------------------------------------- */
/*
 * THE CAROUSEL'S WRITE AT AN EVENT THIS SIDE SEES — statusbar.h, `strip`.
 *
 * Three of the six writers are things the client does itself, and each makes
 * the pair of 0x80050758 calls whatever it changed:
 *
 *   the select       0x8004ED3C's delay slot sets a2 = 1 on both arms, and
 *                    0x8004ED8C tests only a2, so the pair is written at
 *                    0x8004EDB4/0x8004EDBC even when the step found nothing;
 *   the auto-select  0x8004FB60 calls 0x800506C4 and falls straight into the
 *                    pair (0x8004FB8C/0x8004FB98), picked or not;
 *   the spawn        0x8003D4FC's tail (0x8003D610/0x8003D614), reached from
 *                    0x8003B250 when a player entity is built.
 *
 * The draw's latch (q2_statusbar_weapon_slots_track) reads events off their
 * marks, and those three can leave none — the same weapon, the same owned set,
 * no pool higher — so the writer is called here, for player `pi`'s own bar
 * with that player's inventory and weapon, the way the draw picks them.
 */
enum { CLIENT_SLOTS_SELECT, CLIENT_SLOTS_AUTOSELECT, CLIENT_SLOTS_SPAWN };

static void client_sbar_write_slots(client *c, int pi, int why)
{
    const q2_inventory *inv;
    int weapon;

    if (!c || pi < 0 || pi >= Q2_MP_MAX_PLAYERS)
        return;

    if (pi == c->sim[0].cur_player) {
        inv    = &c->sim[0].combat.inv;
        weapon = c->sim[0].combat.weapon_id;
    } else {
        inv    = &c->sim[0].pcombat[pi].inv;
        weapon = c->sim[0].pcombat[pi].weapon_id;
    }
    q2_statusbar_weapon_slots(&c->sbar[pi], inv, weapon);
    if (why >= 0 && why < 3)
        c->slots_written[why]++;
}

/* ------------------------------------------------------------------------- */
/* The player death chain                                                     */
/* ------------------------------------------------------------------------- */
/*
 * Put a dead player back — 0x8003DDF8, which is the ONLY thing on the console
 * that respawns anybody, and which the engine itself never calls: its one
 * caller is 0x8003DECC, the mode gate `q2_mp_may_respawn` already carries, and
 * QMULTI.C reaches that through slot 12 of the engine block. The engine's own
 * death chain animates the body, waits its 1500 and dissolves it, and stops.
 *
 * 0x8003DDF8 picks a MultiSpawn through 0x80071004, builds a new player entity
 * at it (0x8003B250: health 100, the client record cleared at 0x8003B2BC and
 * the spawn loadout 0x8003D4FC run; then 0x8003DE34 puts entity+222 at the
 * "not a player" sentinel) and installs the Stand move. The pad and menu gates
 * the engine also applies (0x8001FC50, 0x800AE8B4) belong to the caller.
 */
static bool client_mp_respawn(client *c, int pi)
{
    q2_mp_player_view pv[Q2_MP_MAX_PLAYERS];
    s32               feet[3];
    int               pick, i, players;

    if (!c->mp_enabled || !c->mp_spawn_count || !c->mp_start_valid)
        return false;
    if (pi < 0 || pi >= Q2_MP_MAX_PLAYERS)
        return false;

    /* The selector takes the spawn FARTHEST from everybody already standing
     * somewhere, so a respawn arrives away from the fight rather than in it. */
    memset(pv, 0, sizeof(pv));
    players = c->mp.player_count;
    if (players < 1)
        players = 1;
    for (i = 0; i < players && i < Q2_MP_MAX_PLAYERS; i++) {
        if (i == pi || c->death[i].stage != Q2_PDEATH_ALIVE)
            continue;
        pv[i].alive  = true;
        pv[i].pos[0] = c->sim[0].player[i].pos[0];
        pv[i].pos[1] = c->sim[0].player[i].pos[1];
        pv[i].pos[2] = c->sim[0].player[i].pos[2];
    }

    pick = q2_mp_select_spawn(c->mp_spawns, pv, (u32)players,
                              client_mp_rng, c);
    if (pick < 0)
        return false;

    feet[0] = c->mp_spawns[pick].pos[0];
    feet[1] = c->mp_spawns[pick].pos[1];
    feet[2] = c->mp_spawns[pick].pos[2];

    c->mp_view_pos[pi][0] = feet[0];
    c->mp_view_pos[pi][1] = feet[1];
    c->mp_view_pos[pi][2] = feet[2];
    c->mp_view_yaw[pi]    = c->mp_spawns[pick].angle;
    c->mp_view_valid[pi]  = true;

    {
        int saved = c->sim[0].cur_player;
        q2_sim_select_player(&c->sim[0], pi);
        c->sim[0].combat.inv = c->mp_start_inv;
        q2_sim_spawn(&c->sim[0], feet, c->mp_view_yaw[pi]);
        q2_sim_player_loadout(&c->sim[0], pi, &c->mp_start_inv,
                              c->mp_start_weapon);
        q2_sim_select_player(&c->sim[0], saved);
        if (pi == 0) c->cam.roll = 0;
    }

    /* And the carousel, from the loadout just handed back: 0x8003B250 runs
     * the spawn loadout 0x8003D4FC, whose tail writes the pair. */
    client_sbar_write_slots(c, pi, CLIENT_SLOTS_SPAWN);

    q2_player_death_init(&c->death[pi]);
    client_reset_view_model(c, pi);
    c->mp_dead[pi] = false;
    c->death_respawns++;
    Q2_INFO("multiplayer: player %d respawned at MultiSpawn %d", pi, pick);
    return true;
}

/*
 * A script CALL reached a rotation primitive: ask that node's rotator to take
 * one step.
 *
 * The event runtime reports a CALL without interpreting it, because which
 * index is SIMROT is a per-map question only the map's UserFuncs answers. What
 * the operands mean is `rotator.[ch]`'s business, beside the builder that
 * reads the same offsets.
 */
static bool client_load_zone(client *c, const char *map, int index);

/* Deferred script work must retain the entity passed to the CALL dispatcher.
 * Several players can enter different teleport volumes on the same frame. */
static void client_queue_teleport(client *c, const q2_start_pos *sp)
{
    int pi = c->sim[0].cur_player;
    c->pending_teleport[pi] = *sp;
    c->pending_teleport_have[pi] = true;
}

/* Selects the loaded level's music. Defined beside the rest of the music code;
 * declared here because client_load_zone ends by calling it. `zone_gate` is
 * that caller's `same_map_transition` — the console runs a different level load
 * for one of those, and it treats the playlist differently. */
static void client_music_for_level(client *c, bool force, bool zone_gate);

/*
 * How long a headless run holds each of the three screens a transition puts up
 * — the arrival briefing, the unit tally and the end-of-mission placard —
 * before going on. Long enough that a `--shot` lands on one, short enough that
 * a scripted run through several levels does not spend its whole frame budget
 * on intermissions. Windowed, only the briefing releases itself; the other two
 * wait for the press their prompt asks for, as the console does.
 */
#define Q2_INTERMISSION_HEADLESS 45

/*
 * `Q2_INTERMISSION_WINDOW` used to sit here — ten seconds, a port constant,
 * invented so a player who pressed nothing was not stranded on the tally board
 * and inherited by the arrival briefing when the two shared a release. Both
 * reasons are gone. The tally waits for the press its prompt asks for, as
 * `0x80018ED8` does, and there IS no arrival briefing: the panel is the
 * script's pop-up, on the fifteen seconds `0x800213B0` is passed at every
 * raise in the executable and that `briefing.h` already carries as
 * Q2_BRIEFING_SECONDS.
 */

/*
 * The beat between a difficulty being confirmed and the opening reel starting.
 *
 * 150, in the console's own 1/300 s units — armed by 0x80101E4C in a delay slot
 * and counted down by 0x80101CD0, which subtracts the FRAME DELTA rather than
 * one. That is what makes it half a second of real time whatever the frame rate
 * is, and why it is kept in those units and drained with `Q2_DT_HZ` rather than
 * turned into a frame count: a frame count would be 0.3 s at the PAL field rate
 * and 0.25 s at the NTSC one, for a number the disc states exactly.
 *
 * READ, not chosen: see `start_beat` for the writer, for its three callers, and
 * for why the reel it arms was never the title screen's attract loop.
 */
#define Q2_START_BEAT_UNITS      150

/*
 * The last unit on this disc. A MISCOMPLETE here ends the GAME — the outer
 * state machine's answer 5, which loads `Extro FMV` — rather than the mission.
 */
#define Q2_LAST_UNIT             5

/* How many of the RESIDENT zone's creatures are dead, right now. */
static u32 client_zone_dead(const client *c)
{
    u32 i, d = 0;

    /*
     * AND THE NUMERATOR REFUSES ONE CLASS, NOT THREE. ProcessQuakeDeath
     * (QENTS.C 0x8007CD24) is the only writer that ever raises 0x800B29E8, and
     * its filter is a single compare:
     *
     *     8007CD5C  addiu v1, zero, 34
     *     8007CD60  beq   v0, v1, <past the increment>
     *
     * so shooting a wounded marine moves neither half, while shooting a Rider2
     * moves the numerator and was never in the denominator. That is the
     * console's own inconsistency and it is left alone rather than tidied into
     * agreement with `q2_mission_class_counts` — see mission.h for both
     * derivations. The class comes from ent+0xD2, or from ent+0xDA when
     * ent+0xD2 is already 47, so a detached body is still judged by the row it
     * died as; `pop_class_id` is that row on both a creature and its corpse.
     */
    if (c->creatures_ready)
        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            if (m->dead && q2_mission_class_kills(m->pop_class_id))
                d++;
        }

    return d;
}

/*
 * Put the zone being LEFT into its slot, before the creature set that holds
 * its answer is freed.
 *
 * Called at the top of every `client_load_zone`, so it runs on a zone gate, a
 * level change, a restart and a save restore alike. Only the gate's answer
 * ends up being used: the two clears in the load below throw the array away
 * whenever the level itself changes, exactly where `secrets_found` is thrown
 * away and for the same reason.
 *
 * An index past the array is dropped rather than clamped — the disc's highest
 * zone file is ZONE5 and the array holds eight, so this cannot happen on a
 * real level, and folding a stray zone into somebody else's slot would be a
 * wrong number rather than a missing one.
 *
 * THE SLOT ONLY EVER RISES. Walking back through a gate into a zone already
 * visited reloads that zone's creature set from COMMON.DAT with every `dead`
 * flag cleared — the port carries a zone's script latches across a gate
 * (`carry_events`) and not its casualties — so a plain assignment here would
 * hand the level's kill count back to zero the second time the player stood
 * in the room they cleared first. The console's counter cannot do that:
 * `0x8007CD84` only ever adds one to `0x800B29E8`, and nothing inside a level
 * subtracts from it. Keeping the larger of the two is how a port that rebuilds
 * the set reproduces that, and it is a deviation only in mechanism.
 */
static void client_zone_stash(client *c)
{
    int z = c->zone_index;
    u32 dead;

    if (!c->creatures_ready || z < 0 || z >= Q2_SAVE_LEVEL_ZONES)
        return;

    dead = client_zone_dead(c);
    if (dead > c->zone_dead[z])
        c->zone_dead[z] = dead;
    if (c->cre_in_zone > c->zone_placed[z])
        c->zone_placed[z] = c->cre_in_zone;
}

/*
 * How this level is doing, as the two pairs the tally shows.
 *
 * Kills come from the creature world rather than from a counter the client
 * keeps, because the world already knows both halves: how many creatures the
 * map placed and how many of them are dead. Counting deaths as they happen
 * would drift the moment a creature is removed for any other reason.
 *
 * The DENOMINATOR is the count this zone placed, taken at load — not the number
 * that happen to be live now. A creature held back for a CREBATCH has `in_use`
 * clear exactly as an out-of-zone one does, so counting live bodies made the
 * total grow as the player sprang each ambush: "3/3 kills" on a level with
 * nine.
 *
 * ...and the level's figure is every zone's, not the resident one's. A row is
 * a LEVEL's — the console keys it by `MapTitle` and a map has one whatever
 * zone is loaded — and its kills word survives a gate because the console
 * never rebuilds the creature set inside a level. The port does rebuild, so
 * the zones already visited are read out of their slots and only the resident
 * zone's pair is computed from the live set. Its own slot is stale between
 * gates and is skipped.
 *
 * NOT the sum of the map's placed records, which would be the other wrong
 * number: on BASE2 that is 36 while the two zones the gates reach hold 18 and
 * 5, so thirteen creatures the player can never meet would sit in the
 * denominator.
 */
static void client_level_tally(const client *c, u32 *dead, u32 *placed)
{
    u32 i, d = 0, p = 0;
    u32 res_dead   = client_zone_dead(c);
    u32 res_placed = c->creatures_ready ? c->cre_in_zone : 0;
    int z = c->zone_index;

    for (i = 0; i < Q2_SAVE_LEVEL_ZONES; i++) {
        if ((int)i == z)
            continue;
        d += c->zone_dead[i];
        p += c->zone_placed[i];
    }

    /* The resident zone's own slot is not stale — it is what was stashed the
     * last time the player left this zone, and on a return visit the reloaded
     * set is alive again. The larger of the two is the one that does not
     * un-kill anything; see `client_zone_stash`. */
    if (z >= 0 && z < Q2_SAVE_LEVEL_ZONES) {
        if (c->zone_dead[z] > res_dead)
            res_dead = c->zone_dead[z];
        if (c->zone_placed[z] > res_placed)
            res_placed = c->zone_placed[z];
    }

    *dead   = d + res_dead;
    *placed = p + res_placed;
}

/*
 * The pause page's status row. The same two pairs the level tally shows, so a
 * player can ask mid-level how they are doing — which is what the row is for,
 * and which is why it asks `client_level_tally` for them rather than counting
 * the resident zone itself: the row used to go backwards the moment the
 * player walked through a gate and paused again.
 */
static void client_menu_fill_stats(client *c)
{
    u32 dead, placed;

    if (!c)
        return;

    client_level_tally(c, &dead, &placed);
    q2_menu_set_stats(&c->menu, (int)dead, (int)placed,
                      (int)c->secrets_found, (int)c->secrets_total);
}

/*
 * ENTERING a level: take this level's row in the mission table.
 *
 * The row is claimed on ARRIVAL, not on departure, and it is keyed by the
 * level's name rather than by visit order — both because that is what the
 * console does. Every map's `LevelBin` init looks its `MapTitle` up and hands
 * the string straight to the engine export at `+0x474`, which is
 * `0x800222B8`: find the row of six whose name matches, else take the first
 * whose name is empty, and stamp the live counters into it. BASE1's module
 * does it in five instructions at `80100434`..`80100448`.
 *
 * Registering on arrival is what makes re-entering a level keep its row rather
 * than take a second one, and it is what lets the counters be written into the
 * row as they move — which is the other half of the console's model
 * (`0x800223A8` for a secret, `0x80022420` for a kill).
 *
 * **THE TABLE IS EMPTIED BETWEEN UNITS, and this comment used to deny it.**
 * What was read here was `0x80022498` — a six-iteration loop with no body,
 * sitting in the MISCOMPLETE arm between the board's setup and its spin — and
 * the conclusion drawn from it, that a per-unit clear had been written and then
 * gutted, was wrong. That loop is not the clear. There are TWO clears:
 *
 *     0x8003DDB8             memset(0x8009B550, 0, 150), the `else` of
 *                            `0x8003D62C`'s restore — taken when the lookup of
 *                            "PlayerSave" through `0x8007FBEC` finds no block.
 *                            That is the NEW GAME clear, and it is the one this
 *                            comment found.
 *     0x80021A0C             ClearLevelIntermission, and it is
 *                            `GameInterMission`'s LAST act (`jal` at
 *                            0x8001902C, after the spin). It opens the
 *                            "PlayerSave0" block and memsets 150 bytes at
 *                            block+228 — which is precisely where the six
 *                            records live, since the save arm writes row i to
 *                            `block + 16 + 212 + i*25` (0x8003D764, 0x8003DAB4)
 *                            and the load arm reads it back (0x8003DCC8).
 *
 * The live array is untouched by the second, and does not need to be:
 * `InitialiseQuakePlayer` calls `GamePlayerLoadSave(player, 0)` at 0x8003B4B0
 * on every level entry, and that arm either copies the block's six records back
 * into 0x8009B550 or, with no block at all, falls into the memset above.
 *
 * So the board lists a UNIT's levels, `-1` is a seventh level inside one unit
 * and not a seventh level of the campaign, and the port's old consequence —
 * unit 2 showing unit 1's rows, and the Secrets and Kills counters silently
 * stopping from unit 2 onward because every row was taken — was this port's
 * defect rather than the original's. The boundary clear is where the console
 * puts it, in the mission_after_map arm; see `q2_mission_clear_rows`.
 *
 * The unit still tracks the map, from `Unit<N>Miss1`, because the title needs
 * it: the disc's maps group by it exactly as the game does — Base 1, Jail and
 * Security 2, Power and Waste 3, Lab/Command/BigGun 4, the bosses 5.
 */
static void client_mission_enter(client *c)
{
    /* The level's OWN name, not its directory: `MapTitle` says "Outer Base"
     * where the folder says BASE1, and the console's Location column is the
     * former — it is the same string the module registers. Falling back to the
     * directory keeps a map with no Strings chunk from drawing a blank row,
     * which would be skipped. */
    const char *name = c->map_title[0] ? c->map_title : c->map;

    if (c->map_unit > 0)
        c->mission.unit = c->map_unit;

    c->mission_row = q2_mission_register(&c->mission, name);
    if (c->mission_row < 0)
        Q2_WARN("mission: no row for %s — this unit's six are taken, which "
                "is what the console does too", name);
    else
        Q2_INFO("mission: %s takes row %d of unit %d",
                name, c->mission_row, c->mission.unit);

    /*
     * And the board's two centred body lines, which used to draw blank because
     * what fed them had not been read. `0x80021FD8` builds `"Unit%dMiss1"` with
     * the unit at `0x800B2E20` and hands the lookup to the wrapper — the same
     * key this map's briefing already reads as its Mission Objective.
     */
    q2_mission_set_objective(&c->mission, c->briefing.objective);
}

/*
 * The live half: put this level's counters into the row it holds.
 *
 * The console writes them at the moment they move — `INSECRET`'s exec calls
 * `0x800223A8` and a creature's death calls `0x80022420`, each stamping the
 * one counter it changed. Doing it once a frame is the same table with fewer
 * hooks, and it matters that it is live rather than deferred to the level's
 * end: a save taken mid-level carries the mission table, so a row that is only
 * written on the way out would save as zeroes.
 */
static void client_mission_update(client *c)
{
    u32 dead, placed;

    if (!c || c->mission_row < 0)
        return;

    client_level_tally(c, &dead, &placed);
    q2_mission_set_counts(&c->mission, c->mission_row,
                          (int)c->secrets_found, (int)c->secrets_total,
                          (int)dead, (int)placed);
}

/*
 * Change level, at the arrival point the script names.
 *
 * Two things about the arrival are the operand table's, not this file's
 * invention (userfuncs.c): the start-position name resolves against the TARGET
 * map's spawns rather than the map the item lives in — 129 of 129 against the
 * target and only 104 of 135 against the container — and the spawn record
 * carries the ZONE it belongs to, so the destination zone is the arrival
 * point's, not zero. A transition that assumed zone 0 would drop the player at
 * the level's own start on every LOADMAP that lands in a later zone.
 *
 * When the name resolves to nothing the load still happens, at the target's
 * zone 0, because losing a level transition is a worse failure than arriving
 * in the wrong doorway — and the warning says which.
 *
 * **The map name is a DISPLAY name and is resolved through the level table**,
 * which is what `0x8007C54C` does with the twelve bytes the outer state machine
 * hands it: walk the 56-byte records comparing `+0` and take `+0x0C` as the
 * directory. The port used to use the operand as a directory outright, which
 * happens to work for every single-player transition on this disc — all
 * thirteen name a map whose display name is its directory in a different case
 * — and would silently fail on any record where the two differ, of which the
 * table has plenty (`COLD STORAGE` is `MATRIX6`). It is also what the console
 * would do: a name the table does not carry reaches `0x8007C684`, an
 * unconditional branch to itself. This warns and tries the name as a directory
 * instead, because hanging is not a behaviour worth reproducing.
 */
static bool client_find_arrival(client *c, const char *map, const char *start,
                                q2_start_pos *out)
{
    char   path[256];
    q2_buf buf;
    bool   found = false;

    if (!start || !start[0])
        return false;
    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(c->disc, path, &buf) == Q2_OK) {
        q2_common_file probe;

        if (q2_common_open(&probe, &buf) == Q2_OK) {
            q2_start_pos_list spawns;

            found = q2_start_pos_parse(&spawns, &probe) == Q2_OK &&
                    q2_start_pos_find(&spawns, start, out);
            q2_common_close(&probe);
        } else {
            q2_buf_free(&buf);
        }
    }
    return found;
}

/* The zone a fresh start of MAP loads: its "Default" arrival point's, as the
 * console's own fresh start resolves it, or zone 0 for a map without one. */
static int client_default_zone(client *c, const char *map)
{
    q2_start_pos sp;

    snprintf(c->arrival, sizeof(c->arrival), "Default");
    return client_find_arrival(c, map, c->arrival, &sp) ? sp.zone : 0;
}

static bool client_change_map(client *c, const char *map, const char *start)
{
    int  zone = 0;
    bool have_arrival = false;
    q2_start_pos arrival;

    memset(&arrival, 0, sizeof(arrival));

    if (c->level_table_ready && map && map[0]) {
        const q2_level_entry *e = q2_level_find_display(&c->level_table, map);

        /* A name that is already a directory is a caller's shorthand rather
         * than a fault — `--map` takes one, and so does a save. */
        if (!e)
            e = q2_level_find(&c->level_table, map);

        if (e && !e->is_placeholder && e->directory[0])
            map = e->directory;
        else if (!e)
            Q2_WARN("LOADMAP: '%s' is in no level table record; taking it as a "
                    "directory", map);
    }

    if (client_find_arrival(c, map, start, &arrival)) {
        zone         = arrival.zone;
        have_arrival = true;
    }
    snprintf(c->arrival, sizeof(c->arrival), "%s", start ? start : "");

    /* A level change is a transition, so the player keeps what they are
     * carrying; the clock is a new level's, so the powerup deadlines are
     * rebased rather than kept. See `carry_player`. */
    c->carry_player   = true;
    c->carry_same_map = false;   /* a new coordinate space: do NOT carry the
                                  * position — this level has its own arrival */
    c->move_reason    = "LOADMAP (level change)";

    if (!client_load_zone(c, map, zone)) {
        Q2_WARN("LOADMAP: %s zone %d would not load", map, zone);
        c->carry_player   = false;
        c->carry_same_map = false;   /* both, or the next carry takes the
                                      * one-level clock branch on a stale
                                      * absolute level_time */
        return false;
    }

    /* `client_load_zone` spawns at the first StartPos in the zone; the script
     * named a particular one, so it wins. */
    if (have_arrival) {
        c->cam.pos[0] = arrival.x;
        c->cam.pos[1] = arrival.y;
        c->cam.pos[2] = arrival.z;
        c->cam.yaw    = arrival.angle;
        q2_sim_spawn(&c->sim[0], c->cam.pos, c->cam.yaw);
    } else if (start && start[0]) {
        Q2_WARN("LOADMAP: %s has no start position '%s'; using its own start",
                map, start);
    }

    /*
     * A new level starts with a clean overlay. The notifications carry a
     * lifetime on the LEVEL clock, and a level change restarts that clock, so
     * without this the "You have found a secret." from the level you just left
     * is still sitting over the new one's first frames — which is exactly what
     * the first capture of the arrival briefing showed.
     */
    if (c->hud_ready)
        q2_hud_init(&c->hud[0], &c->hud_tables, 1);

    c->map_changes++;
    Q2_INFO("LOADMAP -> %s zone %d%s%s", map, zone,
            have_arrival ? " at " : "", have_arrival ? arrival.name : "");
    return true;
}

/* Defined with the other menu state below; the arrival raises it. */
static void client_save_prompt_open(client *c);

/*
 * Perform the queued transition, and everything that goes with arriving.
 *
 * Three callers reach it — a plain LOADMAP, the tally board being dismissed,
 * and the end-of-mission placard being dismissed — and they must not differ in
 * what happens on the far side, which is why it is one function.
 */
static void client_change_map_and_brief(client *c)
{
    if (!client_change_map(c, c->pending_map, c->pending_start))
        return;

    /*
     * AND THE GAME STOPS AND ASKS WHETHER TO SAVE.
     *
     * GameCheckPaused (0x8003E538, QUAKE.C) runs every frame, and on the first
     * one after a level change it takes 0x8003E6FC..0x8003E75C:
     *
     *   8003E700  lw   v1, 0x800AEBD0       LevelChange, incremented once per
     *                                       level load at 0x80018C4C
     *   8003E704  lw   v0, gp+700           its own latch (0x800AE8BC), read
     *                                       here and written at 0x8003E728 and
     *                                       nowhere else in the image
     *   8003E70C  beq  v1, v0 -> 0x8003E770 already asked for this load
     *   8003E718  lw   v0, 0x800AEBCC       LevelIsMultiPlayer
     *   8003E720  bne  v0, zero -> skip
     *   8003E728  sw   v1, gp+700           latch, whatever the exit code is
     *   8003E730  lw   v1, 0x800B271C       the exit code the transition carried
     *   8003E738  beq  v1, 2 -> raise       LOADMAP (screen.h)
     *   8003E740  bne  v1, 4 -> skip        4 has no dispatch arm of its own
     *   8003E750  sh   zero, 0x800B2E50     the CD volume target to nothing
     *   8003E754  sw   1, gp+692            Paused
     *   8003E758  sw   zero, gp+696         Pauser = player 0
     *   8003E75C  jal  0x800203C4           InGame_GOTO_WANT_TO_SAVE
     *
     * This function is what a LOADMAP arrives through — a plain one, the tally
     * board being dismissed, and the end-of-mission placard, which is the whole
     * of exit code 2 and of the 7 that drops to it. Exit 4 is not raised here
     * because nothing in this port produces it and screen.h leaves it
     * unidentified rather than guessing.
     *
     * THE LATCH IS STRUCTURAL. The console asks from a per-frame checker and
     * needs 0x800AE8BC to stop it asking a second time; there is one call here
     * per level change, so there is nothing for a latch to suppress.
     */
    if (q2_mcard_save_prompt_due(c->level_change_serial,
                                 &c->save_prompt_latch, c->mp_enabled,
                                 Q2_SCREEN_EXIT_CHANGE_MAP))
        client_save_prompt_open(c);

    /*
     * AND NO ARRIVAL BRIEFING, which this used to raise here.
     *
     * There is no such screen. The port had two state machines around one
     * console screen: `briefing_open`, raised by the transition, and `popup`,
     * raised by the script — and both draw through `q2_briefing_build_ot`
     * because they are the same panel. The console has only the second.
     * `0x80021250` sets the two fields and `0x800213B0` raises them, and every
     * caller of either is a script primitive (`0x80023894`, `0x8002BBF4`) or
     * the pause menu's MISSION row (`0x800203AC`). Nothing in the transition
     * path touches them: what the outer state machine does run on a new
     * level's first frame is `0x800203C4`, and that installs two overlay
     * tables through `0x800B2FE4+512` rather than raising a panel.
     *
     * So the panel a player sees just after arriving is a trigger volume near
     * the spawn calling HELPCOMPUTER, and on a map that has none it does not
     * appear — the two fields are global (`0x800B27A4`/`0x800B27A8`,
     * "deliberately not per level"), so the orders simply stand until
     * something changes them. Measured across ten maps with no trigger fired:
     * BASE0, POWER1 and LAB raise it at level start on their own and the other
     * seven do not.
     */

    /* Re-arm, so one `--fire-triggers` walks the game rather than one level.
     * Without this a scripted run stops at the first boundary, having proved
     * only that the first boundary works. */
    if (c->fire_interval > 0) {
        c->fire_triggers = true;
        c->fire_at_frame = (long)c->frame_index + c->fire_interval;
    }
}

/*
 * Case-insensitive name compare. Map names reach this from two places that do
 * not agree on case — the executable's level table, which LOADMAP names, and
 * the ISO directory the loader walks — so comparing them exactly would make
 * every transition look like a move to a different map, including the ones
 * that name the map you are already standing in.
 */
static bool client_name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (int)(unsigned char)*a++;
        int cb = (int)(unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

/*
 * A script reached a MOVER item: open that door.
 *
 * The runtime reports the item and this maps it to the movers built from it —
 * plural, because MOVER_C is a double door and one item builds both leaves.
 */
static void client_event_mover(void *user, const q2_event_item *item)
{
    client *c = (client *)user;

    if (!c || !c->movers_ready || !item)
        return;

    c->mover_triggers += q2_movers_trigger_item(&c->movers, item->offset);
}

/*
 * Apply a set of node visibility changes to the zone's hide array.
 *
 * `node_hidden` is this side's because the array has a second writer — the
 * script's OBJDRAWOFF — so the sim and the explosives hand back lists and this
 * is the one place that turns them into bytes. Scene.flags08 bit 15 is what the
 * console writes; world.c honours the array in the same place it honours the
 * bit.
 */
static void client_apply_node_vis(client *c, const q2_explosive_result *vis)
{
    u32 i;

    if (!c || !vis || !c->node_hidden)
        return;

    for (i = 0; i < vis->hide_count; i++) {
        s16 n = vis->hide[i];

        if (n >= 0 && (u32)n < c->node_hidden_count && !c->node_hidden[n]) {
            c->node_hidden[n] = 1;
            c->explosive_vis++;
        }
    }
    for (i = 0; i < vis->show_count; i++) {
        s16 n = vis->show[i];

        if (n >= 0 && (u32)n < c->node_hidden_count && c->node_hidden[n]) {
            c->node_hidden[n] = 0;
            c->explosive_vis++;
        }
    }
}

/*
 * A script reaching a `func_explosive`. The console destroys it on the spot —
 * the dispatch arm passes damage zero and the handler's first branch falls
 * straight through to the destruction (explosive.h).
 */
static void client_event_explosive(void *user, const q2_event_item *item)
{
    client *c = (client *)user;

    if (!c || !c->explosives_ready || !item)
        return;

    /*
     * THE EVE_ REPLAY DESTROYS IT IN SILENCE. The exec tests gp+16948 at
     * 0x80026924 and, while the replay has it up, branches past the Explosion
     * (0x8005A778), the report (0x80073704) and the debris (0x80064558 at
     * 0x80026970) to the swap — 0x80068818 hides each intact node and frees
     * its box, and the loop after it shows the rubble. So a group the script
     * had already blown up comes back blown up, not blowing up again. The
     * sim's entry has no such input; the set's own does (`suppress`,
     * explosive.h), and the swap and the dead boxes are applied here the way
     * q2_sim_explosive_trigger_item applies them.
     */
    if (c->sim[0].event_rt.initial_pass && c->sim[0].explosives) {
        q2_explosive_result res;
        u32 i;

        if (!q2_explosive_trigger_item(c->sim[0].explosives, item->offset,
                                       true, c->sim[0].breakable_scene,
                                       &res))
            return;
        client_apply_node_vis(c, &res);
        for (i = 0; i < c->sim[0].breakable_count; i++)
            if (c->sim[0].breakable[i].kind == Q2_BREAKABLE_FXGROUP &&
                c->sim[0].breakable[i].item_offset == item->offset)
                c->sim[0].breakable[i].broken = true;
        c->sim[0].explosive_destroyed++;
        c->explosive_scripted++;
        return;
    }

    if (q2_sim_explosive_trigger_item(&c->sim[0], item->offset))
        c->explosive_scripted++;
}

/*
 * A SCRIPTED LINE GOES ON THE CENTRE LINE, and every one of them did not.
 *
 * PSXSTRING resolves its twelve-byte key through GetStringFromStringtable and
 * hands the text to `centreprintf` (0x80043570) with player 0's window context
 * — the `jal` at 0x8002B930 — and the secret handler does exactly the same at
 * 0x80028F90. centreprintf parks the icon CLUT at 72 (0x800435A4), formats the
 * body into 0x800C8DDC and calls DoAddCentredSpriText (0x80042E14): the ONE
 * buffer at ctx+0x240, prefixed `*7`, placed at `width/2 - 4*len` by
 * `height/2 + 20`, with its timer reset to zero so it holds for 1200.
 *
 * The notification ring is DoAddSpriText, 0x80042D24 — a different entry, and
 * the right one for pickups and the weapon caption. Routing the level text
 * there, which is what this did, put "Access granted" in the top-left corner at
 * x = 24, unboxed, stacked under up to three earlier lines and retiring on the
 * ring's 60-tick clock rather than held in front of the player for 1200 with
 * the backdrop fading out from under the words.
 *
 * DEVIATION, and it is the one the key prompt already carries: the line is laid
 * out in the console's own 512x248 space, which is what the single-screen
 * overlay draws in. A split-screen view centres its own half instead; the
 * console has neither a second screen nor a second string table.
 */
static void client_centre_line(client *c, const char *text)
{
    q2_hud_ctx ctx;

    if (!c || !text || !text[0] || !c->hud_ready)
        return;

    /*
     * 0x80042E54: `lh v1, [0x800B2834]; bne v1, zero` — the centre line itself
     * refuses everything while the engine's initial-state pass runs, the same
     * gp+16948 flag STRING and INSECRET already test at their top
     * (client_event_call). Asked here too because it is 0x80042E14's own gate
     * and so covers every caller, not only the two that test it first.
     */
    if (c->sim[0].event_rt.initial_pass)
        return;

    q2_hud_ctx_default(&ctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
    q2_hud_centre(&c->hud[0], &c->hud_tables, &ctx, text);
    c->centre_lines++;
}

static void client_event_call(void *user, const q2_event_item *item,
                              u8 call_index)
{
    client *c = (client *)user;

    if (!c || !c->sim[0].userfuncs_ready)
        return;

    /*
     * gp+16948 (events_rt.h, `initial_pass`), up while the EVE_ replay runs:
     * these five primitives test it before anything else and return at once
     * — STRING 0x8002B880, INSECRET 0x80028EC4, HELPCOMPUTER 0x8002BAEC and
     * SIMPLESOUND 0x8002D318 as their first instruction, CREBATCH 0x8002B96C
     * after its length check (userfuncs.h, "Two engine-wide pass flags"). A
     * replayed batch does not spawn twice and a replayed secret is not found
     * twice. Keyed on the primitive, as the console's gate is on the handler.
     *
     * GLASS reads the flag too, at 0x8002A3C4 — after its hit burst
     * (0x8002A384), so a replayed pane still puffs once but skips the
     * shatter and the sound. That gate lives in q2_sim_breakable_call
     * (simcombat.c), which reads the same flag off the runtime; the client
     * plays no sound for a scripted GLASS, so there is nothing to gate here.
     * Six spendable records on the disc carry a GLASS call.
     */
    if (c->sim[0].event_rt.initial_pass) {
        q2_uf_prim prim = q2_userfuncs_prim(&c->sim[0].userfuncs, call_index);

        if (prim == Q2_UF_STRING || prim == Q2_UF_CREBATCH ||
            prim == Q2_UF_HELPCOMPUTER || prim == Q2_UF_SIMPLESOUND ||
            prim == Q2_UF_INSECRET)
            return;
    }

    if (c->rotators_ready)
        c->rot_steps += q2_rotators_call(&c->rotators,
                                         &c->sim[0].userfuncs,
                                         item, call_index);

    /*
     * ONKEYDO — the key gate, and the reason it matters more now than it did
     * an hour ago.
     *
     * It is a PREDICATE: it tests the player's key bits and, when they do not
     * satisfy it, aborts the rest of the record it sits in. Nothing acted on
     * it, so every gated script ran for free — which was invisible while the
     * things they gate did nothing, and is not invisible now that the same
     * records open doors and lifts.
     *
     * The four tests are `userfuncs.c`'s, and a zero operand disables its own
     * test rather than requiring nothing to be set. The bitfield is the
     * inventory's low twelve bits (inventory.h), the same field the movers'
     * `key_mask` is checked against.
     */
    {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_ONKEYDO) {
            u32 all_set = 0, any_set = 0, all_clear = 0, any_clear = 0;
            u16 keys = (u16)(c->sim[0].combat.inv.flags & 0x0FFFu);
            bool pass = true;

            q2_uf_operand_u32(&call, 0, 0, &all_set);
            q2_uf_operand_u32(&call, 1, 0, &any_set);
            q2_uf_operand_u32(&call, 2, 0, &all_clear);
            q2_uf_operand_u32(&call, 3, 0, &any_clear);

            if (all_set   && (keys & all_set)   != all_set)   pass = false;
            if (any_set   && (keys & any_set)   == 0)         pass = false;
            if (all_clear && (keys & all_clear) != 0)         pass = false;
            if (any_clear && (keys & any_clear) == any_clear) pass = false;

            if (!pass) {
                c->sim[0].event_rt.abort_record = true;
                c->script_gated++;
            }
        }
    }

    /*
     * TELEPORT, SETWIBBLE and HELPCOMPUTER — three more the histogram named.
     *
     * TELEPORT's `start_pos` resolves 28 of 28 disc-wide against the map's own
     * spawns. Placement and any zone switch are deferred until the frame has
     * finished reading the current zone, with the triggering player retained.
     *
     * SETWIBBLE writes the low four bits of its operand into `flags08` bits
     * 10..13, and bits 10-11 are the DRAW VARIANT: variant 3 links nothing,
     * which is the other way a script hides a surface group (world.c). Only
     * that case is acted on, because the other three variants are subdivision
     * choices the port makes per quad rather than per node. Its operand is a
     * Scene NODE index and takes no rebase — `userfuncs.c` is explicit that the
     * constructor only restores bytes and never rewrites them.
     *
     * HELPCOMPUTER carries two Strings keys and shows them; the port puts them
     * on the overlay, which is where its own notifications go. Its third
     * operand selects a screen this port does not have.
     */
    {
        q2_uf_call call;
        char key[Q2_UF_NAME_LEN + 1];

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK) {
            if (call.prim == Q2_UF_TELEPORT &&
                q2_uf_operand_name(&call, 0, key) && key[0]) {
                q2_start_pos_list spawns;
                q2_start_pos sp;

                if (q2_start_pos_parse(&spawns, &c->common) == Q2_OK &&
                    q2_start_pos_find(&spawns, key, &sp)) {
                    /*
                     * "Switches zone first if the target is in another one,
                     * then sets entity position" — so a cross-zone teleport is
                     * a zone load with an arrival point on the end, which is
                     * the zone gate's own path. QUEUED rather than done here
                     * for the reason every other transition is: this CALL is
                     * running inside the script a zone load would free.
                     */
                    client_queue_teleport(c, &sp);
                    c->script_teleports++;
                    Q2_INFO("TELEPORT to '%s' (zone %d)", sp.name,
                            (int)sp.zone);
                }
            }

            if (call.prim == Q2_UF_SETWIBBLE && item->len >= 8 &&
                item->payload && c->node_hidden) {
                const u8 *p    = item->payload - 2;
                s16       node = q2_rd_s16(p + 4);
                u16       wib  = q2_rd_u16(p + 6);

                /* Bits 10-11 of flags08 are the variant; 3 links nothing. */
                if (node >= 0 && (u32)node < c->node_hidden_count &&
                    (wib & 3u) == 3u && !c->node_hidden[node]) {
                    c->node_hidden[node] = 1;
                    c->script_hidden++;
                }
            }

            if (call.prim == Q2_UF_HELPCOMPUTER) {
                const char *text[2] = { NULL, NULL };
                u32 param = 0;
                s32 delay;
                int k;

                /*
                 * 0x80021250. The two keys go through the level's Strings and
                 * miss to the shipped defaults; the third operand is the
                 * DELAY, clamped UP to a minimum of 5 (`slti v0,s3,5` at
                 * 0x8002136C); and the screen is then raised for 15 seconds.
                 *
                 * These used to be posted to the HUD's notification overlay,
                 * which is where the port's own messages go — so the game's
                 * orders scrolled past as two lines of chatter and the screen
                 * that exists to hold them was never raised.
                 */
                /* The keys are kept, not just their answers: they are what
                 * `HelpComputer_GetMessages` puts in the save block, and what
                 * a restore re-resolves. See q2_briefing_popup. */
                char keys[2][Q2_UF_NAME_LEN + 1];

                keys[0][0] = keys[1][0] = '\0';
                for (k = 0; k < 2; k++) {
                    if (!q2_uf_operand_name(&call, (u32)k, key) || !key[0])
                        continue;
                    snprintf(keys[k], sizeof(keys[k]), "%s", key);
                    if (c->leveltext_ready)
                        text[k] = q2_leveltext_find(&c->leveltext, key);
                    if (text[k])
                        c->script_strings++;
                }

                q2_briefing_popup_set(&c->popup, keys[0], text[0],
                                      keys[1], text[1]);

                (void)q2_uf_operand_u32(&call, 2, 0, &param);
                delay = (s32)param;
                if (delay < Q2_BRIEFING_DELAY_MIN)
                    delay = Q2_BRIEFING_DELAY_MIN;

                /*
                 * THE AUTHORED DELAY, INCLUDING AT SPAWN — and it is meant to
                 * be felt.
                 *
                 * BASE0's board is raised by a HELPCOMPUTER whose volume
                 * contains the spawn point, so the call fires on the level's
                 * very first tick. This briefly forced `delay = 0` for that
                 * case, which put the board up on frame 0 — and that is not
                 * retail either: the console gives you just long enough to see
                 * the blaster come up before the screen arrives. The operand is
                 * the delay, and honouring it is what produces that beat.
                 *
                 * What was really wrong was the arithmetic, and that is fixed
                 * elsewhere: `sim->cur_dt` is now assigned at the TOP of the
                 * tick, so a trigger firing on the first tick no longer arms its
                 * countdown with zero, and the raise no longer doubles the
                 * operand (briefing.c). With those two right the authored
                 * delay lands where the console puts it and needs no override.
                 */
                q2_briefing_popup_raise(&c->popup, delay,
                                        Q2_BRIEFING_SECONDS,
                                        c->sim[0].level_time,
                                        c->sim[0].cur_dt);
                c->popup_raises++;
                /* The sound plays at the RAISE, not at the open (0x800213B0
                 * calls it before it looks at the delay). */
                client_play_sound(c, "msc_comp_up");
                Q2_INFO("help computer: \"%s\" / \"%s\" in %d",
                        c->popup.orders, c->popup.objective, (int)delay);
            }
        }
    }

    /*
     * MISCOMPLETE — the unit is over.
     *
     * `0x8002DC68` is four instructions of substance: it copies the fixed
     * string `"Default"` into the arrival-point buffer at `0x800C8CD0` and
     * writes **7** into the game-state word at `0x800B2E28`. Nothing else.
     *
     * Exit 7 is `0x80018ED8`, which `screen.h` had listed by number with no
     * name. Reading it names it: it tears the level's two module images down,
     * runs the outer state machine until it answers, and then either loads
     * `"Extro FMV"` on answer 5 — the ending — or `"EndMission N"` with the
     * digit at index 11 patched from `0x800B2E20` (`lbu`/`addu`/`sb` at
     * `0x8001900C`..`0x80019028`). Those are the `QENDMIS1`..`QENDMIS5` maps
     * the level table carries as `EndMission 1`..`EndMission 5`.
     *
     * So a MISCOMPLETE ends a UNIT rather than a level, and the port does what
     * it can read: raise the mission screen — which already says
     * "Mission N - Complete" — and go to that unit's end-of-mission map,
     * resolved by DISPLAY name because "EndMission N" is a display name and
     * not a directory.
     *
     * The outer state machine is not reconstructed, so the choice between
     * `EndMission N` and `Extro FMV` is made here from the unit the map
     * declares rather than from that machine's answer. Stated, because it is
     * the one invented step: unit 5 is the last on this disc.
     *
     * BOTH BRANCHES NOW EXIST. The `Extro FMV` half used to be a comment about
     * a path the port did not take — the last unit went to `EndMission 5` like
     * every other, and the outro was started from there because that was the
     * only place a film could be started from. `Extro FMV` is a level table
     * record (index 11) resolving to QFMV, QFMV plays the film its module names
     * for that screen, and so the campaign now ends the way 0x80018ED8 ends it.
     */
    /*
     * ONCE A UNIT IS OVER, a second MISCOMPLETE has nothing to add. The exec
     * only writes "Default" at 0x800C8CD0 and 7 at 0x800B2E28
     * (0x8002DC68..0x8002DCB4), so running it twice is the same as running it
     * once — and this arm was not: it took the destination it had queued
     * itself as the LOADMAP to continue to, and the unit ended on its own
     * EndMission forever. The EVE_ replay re-runs a spent MISCOMPLETE record
     * after a zone gate that lands in the frame the unit ended (a trigger
     * sweep reaches both), which is how it showed.
     */
    if (!c->unit_over) {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_MISCOMPLETE && c->level_table_ready) {
            char want[32];
            const q2_level_entry *e;
            /* Answer 5: the last unit on this disc ends the game rather than
             * the mission. */
            bool ending = c->map_unit >= Q2_LAST_UNIT;

            if (ending)
                snprintf(want, sizeof(want), "Extro FMV");
            else
                snprintf(want, sizeof(want), "EndMission %d",
                         c->map_unit > 0 ? c->map_unit : 1);
            e = q2_level_find_display(&c->level_table, want);

            /* Which of QFMV's two films to play, carried across the load —
             * the screen name IS the selector (see `film_screen`). */
            if (ending)
                snprintf(c->film_screen, sizeof(c->film_screen), "%s", want);

            if (e && !e->is_placeholder && e->directory[0]) {
                /*
                 * A MISCOMPLETE OVERWRITES a LOADMAP queued the same frame,
                 * and this is the console's order rather than a tie-break.
                 * `0x80018ED8` runs the tally board and only THEN writes
                 * `"EndMission N"` over `0x800E46C0` and `"Default"` over
                 * `0x800C8CD0` — whatever `0x8002DCE0` had put there. The port
                 * used to let the LOADMAP win, which is why every scripted run
                 * walked past the unit boundaries without ever seeing one.
                 *
                 * A unit's last level carries BOTH — BASE2 has three LOADMAPs
                 * and the unit-1 MISCOMPLETE — and a player fires one of them
                 * by walking into one volume. `--fire-triggers` fires every
                 * volume at once, so within that artificial batch the order
                 * means nothing; the unit end is the one that must survive it.
                 *
                 * The LOADMAP's destination is not thrown away: it is where
                 * this unit's last level was going, and it is what the port
                 * continues to once the end-of-mission screen is dismissed.
                 */
                if (c->map_change_pending && c->pending_map[0]) {
                    snprintf(c->unit_next_map, sizeof(c->unit_next_map), "%s",
                             c->pending_map);
                    snprintf(c->unit_next_start, sizeof(c->unit_next_start),
                             "%s", c->pending_start);
                    Q2_INFO("MISCOMPLETE: holding %s '%s' for after the "
                            "end-of-mission screen",
                            c->unit_next_map, c->unit_next_start);
                }

                /*
                 * The DISPLAY name, not the directory: `0x80018ED8` writes
                 * `"EndMission N"` into `0x800E46C0`, which is the same buffer
                 * a LOADMAP writes and the same one `0x8007C54C` resolves. The
                 * table lookup above is the existence check, not the
                 * resolution — doing it here as well would put a directory in
                 * a field that holds display names.
                 */
                snprintf(c->pending_map, sizeof(c->pending_map), "%s", want);
                snprintf(c->pending_start, sizeof(c->pending_start),
                         "Default");
                c->map_change_pending = true;
                c->unit_over          = true;
                c->script_units++;
                Q2_INFO("MISCOMPLETE: unit %d over -> %s (%s)",
                        c->map_unit, want, e->directory);
            } else {
                Q2_WARN("MISCOMPLETE: no level named '%s'", want);
            }
        }
    }

    /*
     * TIMER — a repeating window over the rest of the record.
     *
     * All four operands, which is the half this arm did not have. `0x80026FEC`
     * claims one of the eight slots at 0x800C6F74 and fills it from the item:
     *
     *   +4/+6   base and range -> the period, 0x800270D0..0x80027104:
     *           `(base + ((range * rand()) >> 15)) * 30`, and the 30 is not
     *           the 300 every other time on this clock uses — `userfuncs.c`
     *           calls that out and it is the sort of thing that is silently
     *           four-fifths wrong if assumed.
     *   +8      the FIRE COUNT (0x800270A4 -> slot+8), 0 meaning forever.
     *           11 of the disc's 18 TIMERs are authored that way and every one
     *           of them used to run once and stop.
     *   +10     the ITEM WINDOW (0x800270B4 -> slot+6): how many items one
     *           deadline runs. BOSS2 0x184 is TIMER + eight TIMEDLIGHTs with
     *           window 1, so the console cycles the eight one at a time and
     *           this port flashed all eight together.
     *
     * The runtime rolls only after accepting the slot and again at each
     * re-arm, using the shared BIOS-compatible gameplay random stream.
     */
    {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_TIMER) {
            u32 fires = 0, window = 0;

            q2_uf_operand_u32(&call, 2, 0, &fires);
            q2_uf_operand_u32(&call, 3, 0, &window);

            {
                c->sim[0].event_rt.defer_timer  = true;
                c->sim[0].event_rt.defer_fires  = (u16)fires;
                c->sim[0].event_rt.defer_window = (u16)window;
                c->script_timers++;
            }
        }
    }

    /*
     * DISABLEME — the record retiring itself.
     *
     * `0x8002EAA8` ORs 0x80 into the running record's header byte at +3, which
     * is the DISABLED bit the dispatcher tests at 0x8002799C before it runs
     * anything. Two calls on the disc and neither had a consumer, so a record
     * that is meant to fire once could fire every time the player walked back
     * into its volume. It does NOT stop the record: the primitive sets the bit
     * and returns, so the rest of the record still runs this once.
     */
    if (q2_userfuncs_prim(&c->sim[0].userfuncs, call_index) == Q2_UF_DISABLEME) {
        c->sim[0].event_rt.disable_self = true;
        c->script_disabled++;
    }

    /*
     * CREBATCH — the ambush arriving.
     *
     * 89 of the disc's 92 calls name a group that exists and 58 of those name a
     * group claiming no zone, which is a batch rather than a level's own
     * population. Every one of their creatures AND place records used to be
     * standing in the room from the moment the level loaded. The two halves
     * share the Population group name and their own one-shot latches.
     */
    {
        q2_uf_call call;
        char group[Q2_UF_NAME_LEN + 1];

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_CREBATCH &&
            q2_uf_operand_name(&call, 0, group) && group[0]) {
            u32 woke = c->creatures_ready
                     ? q2_creature_world_summon(&c->creatures, group) : 0;
            u32 placed = q2_sim_activate_item_group(&c->sim[0], group);

            if (woke)
                c->script_summoned += woke;
            if (woke || placed)
                Q2_INFO("CREBATCH '%s' woke %u, placed %u",
                        group, woke, placed);
        }
    }

    /*
     * OBJDRAWOFF — a script hiding geometry.
     *
     * `flags08` bit 15 is the hide flag and the zone draw has always honoured
     * it; it is clear on every node on the disc because this primitive sets it
     * at RUN TIME. Six calls a trigger volume reaches. The slots are Scene node
     * indices and take the #56 rebase, as every other object slot does.
     */
    {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_OBJDRAWOFF && item->len >= 12 &&
            item->payload && c->node_hidden) {
            const u8 *p = q2_uf_operand_at(&c->ev_operands,
                                           item->payload - 2, 12);
            int k;

            for (k = 0; k < 4; k++) {
                s16 node = q2_rd_s16(p + 4 + 2 * k);

                if (node < 0)
                    continue;            /* negative terminates, per the table */
                if ((u32)node >= c->node_hidden_count)
                    continue;
                q2_sim_scene_solids_hide(&c->sim[0], node);
                if (c->sim[0].event_rt.object_flags &&
                    item->offset < c->sim[0].event_rt.item_flags_size)
                    c->sim[0].event_rt.object_flags[item->offset] = 1;
                if (!c->node_hidden[node]) {
                    c->node_hidden[node] = 1;
                    c->script_hidden++;
                }
            }
        }
    }

    /* A LIFT1 call is both the constructor and the trigger: the same item that
     * built the mover is what asks it to move. */
    if (c->movers_ready)
        c->mover_triggers += q2_movers_trigger_item(&c->movers, item->offset);

    /*
     * And the breakables. GLASS is the other primitive whose operand is an
     * object slot, and a script CALL is enough to run it: the handler passes
     * no damage, so the hit-point test at 0x8002A390 is skipped and the pane
     * shatters where it stands. Same operand rebase as the rotators — 4 of
     * the disc's 10 breakable calls are only reachable through it.
     */
    {
        u32 pieces = q2_sim_breakable_call(&c->sim[0], &c->zone.scene,
                                           &c->ev_operands, item, call_index);
        if (pieces) {
            c->glass_calls++;
            c->glass_pieces += pieces;
        }
    }

    /*
     * STRING and SIMPLESOUND — what a script says and what it plays.
     *
     * Both were decoded and neither was acted on, and both are things a player
     * meets constantly: sweeping every trigger volume on the disc runs 33 of
     * the 68 STRING calls and 33 of the 33 SIMPLESOUND ones.
     *
     * STRING's key resolves against the MAP's own `Strings` chunk, and
     * userfuncs.c already records that a miss is normal rather than a fault —
     * 165 of 363 uses resolve disc-wide — so a key with no text is silence and
     * not a warning.
     *
     * SIMPLESOUND carries an ABSOLUTE world position and a bank name. The
     * position is not used: this port's mixer has no positional path, so the
     * sound plays flat. That is a stated shortfall rather than a silent one.
     */
    {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK) {
            char key[Q2_UF_NAME_LEN + 1];

            if (call.prim == Q2_UF_STRING && c->leveltext_ready &&
                q2_uf_operand_name(&call, 0, key) && key[0]) {
                const char *text = q2_leveltext_find(&c->leveltext, key);

                if (text) {
                    /* 0x8002B930's own destination — see client_centre_line. */
                    client_centre_line(c, text);
                    c->script_strings++;
                    Q2_INFO("script says: \"%s\"", text);
                }
            }

            if (call.prim == Q2_UF_SIMPLESOUND &&
                q2_uf_operand_name(&call, 2, key) && key[0]) {
                s32 at[3];
                bool ok;

                /* Its first operand is an absolute world position
                 * (userfuncs.c), which is exactly what a positional voice
                 * wants — and what the port used to throw away. */
                if (q2_uf_operand_vec3(&call, 0, at))
                    ok = client_play_sound_at(c, key, at);
                else
                    ok = client_play_sound(c, key);
                if (ok)
                    c->script_sounds++;
            }
        }
    }

    /*
     * MISEVENT — the map's named mission events.
     *
     * The engine's half is two lines: park the twelve-byte name in a global
     * (0x8006D2EC writes 0x800DD950) and call the handler the namespace gave
     * back. Both namespaces are read here — the executable's three-record table
     * and the map's own, recovered from its LevelBin (levelbin.h) — and 20 of
     * the disc's 20 keys resolve in one or the other.
     *
     * Most handlers are still MIPS in the module and are named/countable rather
     * than executable here. BASE0's DOCRATES is reconstructed below: it is the
     * crate conveyor, not a generic train, and its four runtime-object writes
     * now have an exact native counterpart in mover.c.
     */
    {
        q2_uf_call call;
        char key[Q2_UF_NAME_LEN + 1];

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_MISEVENT &&
            q2_uf_operand_name(&call, 0, key) && key[0]) {
            const q2_misevent *exe = q2_misevent_find(key);
            u32 k;

            c->misevents++;
            snprintf(c->misevent_last, sizeof(c->misevent_last), "%s", key);

            if (exe) {
                c->misevent_exe++;
            } else {
                for (k = 0; k < c->misevent_count; k++)
                    if (client_name_eq(c->misevent[k].name, key)) {
                        c->misevent_map++;
                        break;
                    }
                if (k == c->misevent_count)
                    c->misevent_unknown++;
            }

            if (c->movers_ready && strcmp(key, "DOCRATES") == 0) {
                u32 moved = q2_movers_step_crates(&c->movers,
                                                   &c->sim[0].events,
                                                   &c->zone.scene,
                                                   c->sim[0].cur_dt);
                c->conveyor_steps += moved;
                c->mover_moved    += moved;
            }
        }
    }

    /*
     * INSECRET — the mission screen's Secrets column.
     *
     * Counted once per item offset. The runtime fires a trigger volume on the
     * edge rather than every tick, so re-entering one would otherwise raise
     * the count again; a secret is found once. See `secrets_found`.
     */
    {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_INSECRET && item->payload) {
            u32 off = (u32)(size_t)(item->payload - c->ev_operands.base_a);
            u32 k;
            bool seen = false;

            for (k = 0; k < c->secret_seen_count; k++)
                if (c->secret_seen[k] == off) { seen = true; break; }

            if (!seen) {
                if (c->secret_seen_count <
                    sizeof(c->secret_seen) / sizeof(c->secret_seen[0]))
                    c->secret_seen[c->secret_seen_count++] = off;
                c->secrets_found++;

                /*
                 * The map's own words, on the overlay — which is what makes
                 * "counter++" a thing the player can see happen — AND THE
                 * CHIME, which it never did.
                 *
                 * Both live inside this guard because 0x80028F80's
                 * `beq v0, zero, 0x80028FA8` jumps over the text at 0x80028F90
                 * AND the sound at 0x80028F98-0x80028FA0 together: a map whose
                 * `FoundASecret` key does not resolve gets neither. Only the
                 * counter is past the join, which is why it stays outside.
                 *
                 * POSITION. 0x80028FA0 passes `s2 + 84`, the handler entity's
                 * feet. The port's on_call hook carries no entity, so the
                 * player's feet are used — not the eye, which sits 576 above
                 * them. Both are within a body's width of the volume that
                 * fired, so the pan and attenuation come out the same.
                 *
                 * INHERITED DEVIATION: the dedup above is the port's, not the
                 * console's. Re-entering a secret volume chimes again on the
                 * disc; here it will stay silent, for the same reason the
                 * counter stays put.
                 *
                 * COUNTED ON BANK PRESENCE, not on a voice starting, because
                 * client_play_sound_at returns false whenever `c->audio` is
                 * NULL — every headless run — and a counter on that would be
                 * measuring the absence of an audio device. Same trap, and the
                 * same answer, as the explosive report.
                 */
                if (c->secret_message[0]) {
                    q2_vag vag;

                    /* 0x80028F90 is `jal 0x80043570` with the same player-0
                     * context PSXSTRING passes: the centre line, not the ring. */
                    client_centre_line(c, c->secret_message);

                    if (client_find_sound(c, Q2_SECRET_SOUND, &vag))
                        c->secret_sounds++;
                    client_play_sound_at(c, Q2_SECRET_SOUND,
                                         c->sim[0].player[0].pos);
                }

                Q2_INFO("secret found: %u of %u — \"%s\" (%u chimed)",
                        c->secrets_found, c->secrets_total,
                        c->secret_message[0] ? c->secret_message : "(no text)",
                        c->secret_sounds);
            }
        }
    }

    /*
     * LOADMAP — the level-to-level transition, and the reason a session could
     * never leave the map it booted into.
     *
     * The primitive has been decoded in `userfuncs.c` for a long time with
     * nothing acting on it: `map` is a 12-byte name at +4 against the
     * executable's level table, and `start_pos` a 12-byte name at +16 resolved
     * against the TARGET map's spawns rather than this one's. Sweeping every
     * trigger volume on the disc runs **28 of 28** of them, so this is not a
     * corner the scripts rarely reach — it is how the game advances.
     *
     * `0x800E46B4` holds the current map's name and the handler compares
     * against it, so a LOADMAP naming the map you are already in is a no-op
     * rather than a reload. That matters here because several maps carry one.
     *
     * Queued, not loaded: see `map_change_pending`.
     */
    {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_LOADMAP) {
            char map[Q2_UF_NAME_LEN + 1];
            char start[Q2_UF_NAME_LEN + 1];

            /* Against the DISPLAY name, because that is what `0x800E46B4`
             * holds and what the operand is. `client_name_eq` is
             * case-insensitive because the two spellings of a single-player
             * map differ only in case; a map whose display name is nothing
             * like its directory would compare wrong against the folder. */
            if (q2_uf_operand_name(&call, 0, map) && map[0] &&
                !client_name_eq(map, c->map_display) &&
                !client_name_eq(map, c->map)) {
                if (!q2_uf_operand_name(&call, 1, start))
                    start[0] = '\0';

                if (c->unit_over) {
                    /*
                     * A MISCOMPLETE has already claimed this frame. On the
                     * console the two are in different volumes and a player
                     * fires one; `--fire-triggers` fires both, and a unit end
                     * that a sweep walks straight past would be worth nothing.
                     * So the destination is remembered as the continuation
                     * rather than taken — the same slot the MISCOMPLETE arm
                     * fills when the order is the other way round.
                     */
                    snprintf(c->unit_next_map, sizeof(c->unit_next_map), "%s",
                             map);
                    snprintf(c->unit_next_start, sizeof(c->unit_next_start),
                             "%s", start);
                } else {
                    snprintf(c->pending_map, sizeof(c->pending_map), "%s", map);
                    snprintf(c->pending_start, sizeof(c->pending_start), "%s",
                             start);
                    c->map_change_pending = true;
                }
            }
        }
    }

    /*
     * TIMEDLIGHT — a script-placed light that FADES, and the last piece of the
     * fifteen `0x80075C34` call sites that this port could reach without
     * tracing a runtime value.
     *
     * THE HALFWORD AT +18 IS NOT A RADIUS, and this comment used to say it was
     * ("radius at +18 tripled before the call"). PSXTLIGHT is 0x8002E97C and
     * its five operand reads are unambiguous:
     *
     *   8002E99C  lhu   a3, 18(s0)      \
     *   8002E9B4  sll   t0, a3, 1        |  a3 = item[+18] * 3   -> TICKS
     *   8002E9BC  addu  a3, t0, a3      /
     *   8002E9C4  lhu   v1, 20(s0)      \  a2 = item[+20]
     *   8002E9A0  lhu   a2, 22(s0)       |     | item[+22] << 16
     *   8002E9B8  sll   a2, a2, 16       |
     *   8002E9D4  or    a2, v1, a2      /
     *   8002E9D0  jal   0x80075D14         AddTimedLight(pos, colour, a2, a3)
     *
     * and AddTimedLight reads that a2 back as two SIGNED halfwords — `lh v0,
     * 8(sp)` at 0x80075D90 is the low half and squares it into slot+0x14, `lh
     * v0, 10(sp)` at 0x80075DA4 is the high half and squares it into slot+0x18
     * — which is inner radius and outer radius. The triple goes into slot+0x20
     * and +0x22, the countdown and its divisor (0x80075DD8/0x80075DDC).
     *
     * So the record carries three numbers where this port read one, and the one
     * it read was the wrong one: every TIMEDLIGHT was a full-brightness
     * one-frame flash of radius 3*ticks instead of a light of the authored size
     * fading over 3*item[+18] frames. It is the only primitive on the disc that
     * reaches AddTimedLight from a script — the other three call sites are the
     * shotgun, the super shotgun and 0x80058758.
     *
     * The colour's own consumer is still 0x80075D14; the low three bytes are
     * taken as r, g, b here, which is what every other packed colour on this
     * path does.
     *
     * A seventeenth live light is DROPPED rather than replacing anything
     * (0x80075D48..0x80075D58), so the return value is deliberately ignored
     * here and the script call is counted either way — the console's script
     * does not learn about the drop either.
     */
    /*
     * FLKLIGHT — registered once and then blinked by q2_flklights_tick. Origin
     * at +4, light_id at +16, colour bytes at +18/+19/+20 (userfuncs.c). It
     * needs phase, which is why it is a set rather than a transient like
     * TIMEDLIGHT below.
     */
    if (q2_userfuncs_prim(&c->sim[0].userfuncs, call_index) == Q2_UF_FLKLIGHT
        && item->len >= 24 && item->payload) {
        const u8 *p = item->payload - 2;
        s32 at[3];
        u8  rgb[3];

        at[0]  = (s32)q2_rd_u32(p + 4);
        at[1]  = (s32)q2_rd_u32(p + 8);
        at[2]  = (s32)q2_rd_u32(p + 12);
        rgb[0] = q2_rd_u8(p + 18);
        rgb[1] = q2_rd_u8(p + 19);
        rgb[2] = q2_rd_u8(p + 20);

        /*
         * A transient, like TIMEDLIGHT — NOT a phased set. The exec at
         * 0x800287A0 loops its objects, adds one dynamic light each, and
         * returns; there is no on/off state anywhere in it. What makes a
         * flicker flicker is that both radii are redrawn from rand() on every
         * call, so the same script record gives a different-sized light each
         * time it runs. An invented on/off phase would be a rhythm the console
         * does not have.
         */
        {
            s32 inner = q2_flklight_inner_radius(
                            q2_rng_next(&c->sim[0].combat.rng));
            s32 outer = q2_flklight_outer_radius(
                            q2_rng_next(&c->sim[0].combat.rng));

            q2_ent_light_at(&c->sim[0].ent_world.events, at, rgb, inner, outer);
            c->script_lights++;
        }
    }

    if (q2_userfuncs_prim(&c->sim[0].userfuncs, call_index) == Q2_UF_TIMEDLIGHT
        && item->len >= 28 && item->payload) {
        const u8 *p = item->payload - 2;
        s32 at[3];
        u8  rgb[3];
        u32 packed;
        s32 ticks, inner, outer;

        at[0]  = (s32)q2_rd_u32(p + 4);
        at[1]  = (s32)q2_rd_u32(p + 8);
        at[2]  = (s32)q2_rd_u32(p + 12);
        ticks  = (s32)q2_rd_u16(p + 18) * 3;   /* 0x8002E9B4 / 0x8002E9BC */
        inner  = (s32)q2_rd_u16(p + 20);       /* a2's low half,  0x8002E9C4 */
        outer  = (s32)q2_rd_u16(p + 22);       /* a2's high half, 0x8002E9A0 */
        packed = q2_rd_u32(p + 24);
        rgb[0] = (u8)(packed & 0xFF);
        rgb[1] = (u8)((packed >> 8) & 0xFF);
        rgb[2] = (u8)((packed >> 16) & 0xFF);

        /*
         * No `if` on anything. AddTimedLight is called unconditionally at
         * 0x8002E9D0, and a zero tick count simply parks a slot whose counter
         * already reads free — which is the original's own way of saying
         * "nothing", not a guard this port adds. The truncation of all three to
         * halfwords is the store inside q2_light_add_timed, exactly as the
         * console's `sh` is.
         */
        q2_light_add_timed(&c->light_world, at, rgb, inner, outer, ticks);
        c->script_lights++;
    }
}


/* ------------------------------------------------------------------------- */
/* The EVE_<map> blocks — what a map remembers about its own script            */
/* ------------------------------------------------------------------------- */
/*
 * A LEVEL CHANGE RUNS THE SAME LEAVE SEQUENCE A ZONE GATE DOES, and this port
 * had the second half of that and not the first.
 *
 * `OnLeavingLevel` (0x8007CB8C, LOADLEV.C) has exactly one instruction in its
 * body: `jal 0x80078D1C` at 0x8007CB94, which is `OnLeavingZone` — the same
 * function the zone-change callback calls at 0x80079080. So the `EVE_<map>`
 * write at 0x80078E70 happens on a LOADMAP exactly as it happens on a gate,
 * and coming back `RestoreSavedData` (0x8007CBAC, from InitLevel at
 * 0x80070F50) replays the block named after the level being entered. The
 * store is q2_event_store (events_rt.h); what follows is the client's two
 * ends of it.
 */


/* ------------------------------------------------------------------------- */
static bool client_load_zone(client *c, const char *map, int index)
{
    q2_world_zone loaded;
    s32 wmin[3], wmax[3];
    bool placed = false;
    bool same_map_transition = c->carry_player && c->carry_same_map &&
                               c->map[0] && client_name_eq(c->map, map);
    /*
     * IS THE DIRECTORY ABOUT TO BE READ A DIFFERENT ONE?
     *
     * That is the level screen's question, and the one `ProcessGame` is
     * deciding when it calls TestIt at 0x80018C88 -- GetLevelData has resolved
     * the name at 0x80018C6C and LoadLevel follows at 0x80018C90.
     *
     * It is NOT `!same_map_transition`. That negation also catches a death, a
     * restart, an arena round reset and a memory-card restore, none of which
     * changes the directory and none of which goes near the screen.
     */
    bool map_change  = !(c->map[0] && client_name_eq(c->map, map));

    /*
     * The outgoing zone's kill tally, before anything below frees the set that
     * holds it. First statement of the load for that reason.
     */
    client_zone_stash(c);

    /* KillDualShocks (0x800411F0), which OnLeavingZone calls at 0x80078D24:
     * every pattern slot freed, the little motor's accumulator left. */
    {
        int p;

        for (p = 0; p < Q2_SIM_MAX_PLAYERS; p++)
            q2_shock_kill(&c->shock[p]);
    }

    /*
     * LevelChange (0x800AEBD0), bumped at 0x80018C4C by ProcessGame — the
     * outer state machine that loads a LEVEL. A zone gate never reaches it,
     * because a gate stays inside one level; everything else does, including a
     * restart and a save being restored. What reads it is the SAVE? prompt's
     * once-per-load gate (q2_mcard_save_prompt_due).
     */
    if (!same_map_transition)
        c->level_change_serial++;

    /*
     * AND WHICH SCREEN GOES UP, WHICH IS TWO QUESTIONS AND NOT ONE.
     *
     * THIS BLOCK USED TO ARGUE THE OPPOSITE AND IT WAS WRONG. It reasoned:
     * `xrefs 0x800A3314` finds the {"LOADING",256,124} record materialised by
     * exactly one instruction, inside 0x80079178; `xrefs 0x80079178` finds two
     * callers, the ZONEGATE opcode (0x80027828) and the TELEPORT primitive
     * (0x80028ACC); both are zone changes inside one map; therefore a LEVEL
     * change raises no loading screen. Every one of those facts is true, and
     * all 21 callers of the page-enter 0x8001A384 really do pass something
     * other than 46 except that one. The conclusion is still wrong, because
     * THE LEVEL SCREEN IS NOT A MENU PAGE and a search for menu pages could
     * never have found it.
     *
     * The executable's own symbol table names both (SLUS-00757 /MAIN.SYM,
     * through the PAL->USA relocation; docs/FORMATS.md §9.13):
     *
     *   0x80079178  MaybeLoadZoneName   LOADLEV.C  -- the ZONE screen
     *   0x8006E150  TestIt              TITLE.C    -- the LEVEL screen
     *
     * `ProcessGame` (0x80018A10, MAIN.C) is the only thing that changes level,
     * and its body is: 0x80018C6C `jal 0x8007C54C` GetLevelData, 0x80018C88
     * `jal 0x8006E150` TestIt, 0x80018C90 `jal 0x8007CA44` LoadLevel,
     * 0x80018C98 `sw zero, 0x800B2D94`, 0x80018CA8 MainGameLoop. TestIt clears
     * both ordering tables (0x8006E188, 0x8006E194), calls InitLoadingAnim
     * (0x80038DFC) and InitialiseVBlankLoading (0x8006DFB8), and that last
     * function ends at 0x8006E144 with `sw 0x8006E288, 0x800B2D94` -- which
     * installs VBlankLoading as the VERTICAL-BLANK HOOK the ISR calls at
     * 0x8001908C. Every vblank from then until FadeOutLoading (0x8007C8F8)
     * spin-waits for it at 0x8007C914 and unhooks at 0x8007C92C draws a whole
     * frame -- DoubleTrebleBuffer, PutDispEnv, DrawOTag, ClearOTag,
     * VBlankMainLoop -- while the CD read blocks. A level change on this
     * console shows an ANIMATED screen for several seconds.
     *
     * MaybeLoadZoneName's is the other one, and this port SHOWS NOTHING FOR IT.
     * On the console it is a still that covers a CD read: enter page 46,
     * present one frame, and let the deferred load at 0x8007901C run after it,
     * during which nothing is drawn at all. Here a zone load is a handful of
     * milliseconds off a file on disk with no frame to cover, so putting the
     * word up buys nothing and costs the one thing a zone seam must not cost
     * -- a visible break. A zone change is seamless.
     *
     * It also had a fault that made the case for the rule. The zone still was
     * raised with `timed = false`, and `q2_loading_step` only takes a screen
     * down when a hold runs out; nothing else called `q2_loading_hide` on the
     * success path. So the word went up at the first gate and STAYED up, over
     * a world that was still running underneath it -- a bar that never
     * finished, which is what a frozen game looks like from the chair.
     *
     * A load that changes no directory -- a death, a restart, an arena round
     * reset, a memory-card restore -- raises nothing either, which is what the
     * console does with those too.
     *
     * Raising it does not draw anything. It arms the screen, and the main loop
     * owns every frame that follows -- deliberate: presenting from inside a
     * load would swap the buffers under a frame that has not begun, and a
     * headless capture numbers its shots by frame.
     *
     * `c->running` stays, and still means the load that STARTS the run: main
     * sets it on the line before the frame loop, so `--map`, `--zone-probe`
     * and the front end being opened at startup are setup rather than
     * transitions, and `--frames 1 --shot` photographs the level rather than
     * the screen (AGENTS.md).
     *
     * NOT REPRODUCED: the level screen's animation. The console runs a whole
     * scene graph off the vertical blank for the length of the read; this port
     * puts up the same word and the same turning logo for a fixed hold,
     * because its own load is milliseconds and there is nothing to fill.
     */
    if (c->running && map_change) {
        q2_loading_raise(&c->loading);
        c->loading_raises++;
    }

    /*
     * EVERY load announces itself, because a load is the only thing that can
     * move a player without the sim knowing, and "which call was it" is the
     * question the report comes down to. `move_reason` is whatever the caller
     * set on the way in; an empty one is a load nothing claimed.
     */
    if (c->zone_trace)
        Q2_INFO("[zone] f%-6u LOAD %s zone %d  (was %s zone %d)  carry=%d/%d"
                "  caller: %s",
                c->trace_frame, map, index,
                c->map[0] ? c->map : "(none)", c->zone_index,
                (int)c->carry_player, (int)c->carry_same_map,
                c->move_reason ? c->move_reason : "(unclaimed)");

    /* Nothing of this level is on screen yet — see the field's note. */
    if (!same_map_transition)
        c->level_frames_drawn = false;

    /*
     * And no mission row until this map claims one. A map with no unit of its
     * own — QENDMIS, QFMV, the front end — must not keep writing the counters
     * of the level it came from into that level's row.
     */
    if (!same_map_transition)
        c->mission_row = -1;

    if (q2_world_load_zone(&loaded, c->disc, map, index) != Q2_OK) {
        /* The client counts a map's zones by probing until one is absent, so
         * "no zone N" is how the count ENDS, not a fault. It is a warning only
         * when the caller asked for a specific zone. */
        if (index == 0)
            Q2_WARN("no zone 0 in %s", map);
        else
            Q2_INFO("%s has %d zone%s", map, index, index == 1 ? "" : "s");

        /*
         * A load that never touched the sim must not leave a transition
         * ARMED. `carry_player` is only cleared by the successful restore, so
         * a failed zone gate left it up and the next load — a restart, a new
         * game — took the carry path and imported whatever the sim happened to
         * hold. client_change_map already does this on its own failure.
         */
        c->carry_player   = false;
        c->carry_same_map = false;
        c->gate_name[0]   = '\0';

        /*
         * AND THE SCREEN COMES BACK DOWN, because nothing loaded.
         *
         * The raise above is unconditional and happens before the read, which
         * is right — it is the console's order. What was missing is the other
         * half: `0x80079178` only reaches `0x80079364`, the `jal 0x8001A384`
         * that enters page 46, AFTER its two refusals, so a transition the
         * console declines never puts a screen up at all. Here the decline is
         * later — it is the read failing rather than a name compare — so the
         * screen is already up and has to be taken down again.
         *
         * Without this, a request for a zone the map does not have is half a
         * second of LOADING over a level that never went anywhere, which is
         * indistinguishable, from the player's chair, from the game hanging.
         */
        if (c->running) {
            q2_loading_hide(&c->loading);
            if (c->loading_raises)
                c->loading_raises--;
        }
        c->loads_failed++;
        return false;
    }

    /*
     * Take the player's own state off the sim FIRST, while the outgoing zone's
     * sim is still standing. Before the placement block, not after it: the
     * placement is the code that has to decide whether to move the player, and
     * it cannot decide that against a position it has not been handed yet. Read
     * late, `carry_pos_valid` was still false on the first gate of a session —
     * so the carry lost every time and the player was dropped at the zone's
     * first StartPos, which is the reported teleport surviving its own fix.
     */
    if (c->carry_player) {
        const q2_player *pl = &c->sim[0].player[0];

        c->carry_inv        = c->sim[0].combat.inv;
        c->carry_weapon_id  = c->sim[0].combat.weapon_id;
        c->carry_chaingun   = c->sim[0].combat.chaingun_bullets;
        c->carry_level_time = c->sim[0].level_time;

        /*
         * Position only within one coordinate space; a level change starts
         * somewhere else entirely and has its own arrival point.
         *
         * The motion comes with it. A doorway can be crossed at a run or in
         * mid-jump, and a transition that kept the position but zeroed the
         * velocity would stop the player dead on the threshold — the same
         * discontinuity as the teleport, one frame long instead of permanent.
         */
        c->carry_pos[0]     = pl->pos[0];
        c->carry_pos[1]     = pl->pos[1];
        c->carry_pos[2]     = pl->pos[2];
        c->carry_vel[0]     = pl->vel[0];
        c->carry_vel[1]     = pl->vel[1];
        c->carry_vel[2]     = pl->vel[2];
        c->carry_yaw        = pl->yaw;
        c->carry_pitch      = pl->pitch;
        c->carry_ground_y   = pl->ground_y;
        c->carry_on_ground  = pl->on_ground;
        c->carry_pos_valid  = c->carry_same_map;
        c->carry_motion     = *pl;
        c->carry_next_fire  = c->sim[0].combat.next_fire;
        c->carry_fire_kick[0] = c->sim[0].combat.kick[0];
        c->carry_fire_kick[1] = c->sim[0].combat.kick[1];
        c->carry_fire_kick[2] = c->sim[0].combat.kick[2];
    }

    /*
     * And the script's latches. Taken HERE, not where the sim is freed:
     * COMMON is re-read below and the old copy closed (q2_common_close), and
     * the outgoing runtime borrows its Events chunk. The console never
     * reloads that chunk on a zone change — gp+372 is stored only by the
     * level load (0x8007AD54) — and its zone-change callback writes the spent
     * bits out (0x80029078) before the load and replays them after
     * (0x8002936C); events_rt.h, q2_event_carry.
     *
     * NOT `same_map_transition`, WHICH IS WHAT THIS USED TO TEST. 0x80029078
     * is reached from OnLeavingZone, and OnLeavingZone has two callers: the
     * zone-change callback at 0x80079080 and OnLeavingLevel at 0x8007CB94,
     * whose whole body is that one call. A LOADMAP writes the block exactly
     * as a gate does — under the map being left, because the name is built
     * from 0x800E46B4 at 0x800290A4 — and the block outlives the level. The
     * fetch below is the other half: RestoreSavedData (0x8007CBAC) reads back
     * the block named after the map being ENTERED.
     *
     * Retail replays outside deathmatch only (0x80079128, 0x8007CBB8), and
     * OnLeavingZone runs only on a real level (+0x20 of the level record,
     * 0x80078D44) — so an arena neither writes nor reads one.
     */
    if (!c->restoring_save && !c->mp_enabled && c->map[0] &&
        c->sim[0].events_ready) {
        q2_event_carry *slot = q2_event_store_open(&c->event_store, c->map);

        if (slot) {
            u32 i;
            q2_event_rt_carry_out(&c->sim[0].event_rt, slot);
            for (i = 0; c->explosives_ready && i < c->explosives.count; i++) {
                const q2_explosive *e = &c->explosives.items[i];
                if (e->destroyed && e->item_offset < slot->size)
                    slot->destroyed_items[e->item_offset] = 1;
            }
        }

        /* The doors on this side of the seam, for the arrival line to be read
         * against: everything the outgoing zone's script had set moving. */
        if (c->zone_trace && c->movers_ready) {
            u32 mi;

            for (mi = 0; mi < c->movers.count; mi++) {
                const q2_mover *m = &c->movers.movers[mi];

                if (m->offset == 0 && m->state == Q2_MV_IDLE)
                    continue;
                Q2_INFO("[zone]        leaving: mover %u (item +0x%x, %u "
                        "part%s): state %u, offset %d of %d", mi,
                        m->item_offset, m->part_count,
                        m->part_count == 1 ? "" : "s", m->state, m->offset,
                        m->target);
            }
        }
    }

    /*
     * AND WHAT THE MAP BEING ENTERED LEFT BEHIND LAST TIME.
     *
     * RestoreSavedData (0x8007CBAC), which InitLevel calls at 0x80070F50,
     * runs RestoreEvents against the block named after the level it is
     * ARRIVING at — the block just written when this is a gate inside one map,
     * and the block from the previous visit when it is not. A map with no
     * block gets nothing: 0x80029498 falls straight out when OpenSaveMemory
     * returns zero, and `size` 0 is how that is said here.
     *
     * NOT MODELLED: OnLeavingZone also declines to write when the game-state
     * word 0x800B2E28 reads 3 (0x80078D58), which is the arm the menu takes at
     * 0x8001F994 when the parked code is 3 — an autosave reload rather than a
     * transition. RESTART LEVEL parks 21 and is NOT that arm, so it writes the
     * block like any other exit; the port has no autosave reload to hang the
     * exclusion on, so it is left out rather than guessed at.
     */
    c->carry_events.size = 0;
    if (!c->mp_enabled) {
        const q2_event_carry *prev = q2_event_store_find(&c->event_store, map);

        if (prev)
            c->carry_events = *prev;
    }

    /* MOVE, never assign: q2_zone_file.chunk points into the archive directory
     * stored inline in `loaded`. A struct copy leaves every pointer aimed at
     * this function's stack frame and made later chunk reads (including
     * --zone-probe) intermittent use-after-return accesses. */
    if (q2_world_move_zone(&c->zone, &loaded) != Q2_OK) {
        Q2_ERROR("cannot take ownership of %s zone %d", map, index);
        c->carry_player   = false;
        c->carry_same_map = false;
        c->gate_name[0]   = '\0';
        return false;
    }

    /*
     * The zone's draw order. Borrowed from the zone file, so it is taken after
     * the move above and dropped whenever the zone is replaced; the cell hint
     * restarts because the hull it indexes has just been replaced too.
     */
    c->sort_ready = (q2_sortdata_parse(&c->sortdata, &c->zone.zone) == Q2_OK &&
                     c->sortdata.data && c->sortdata.size > 0);
    c->sort_cell  = -1;
    c->zone.sort  = NULL;
    if (c->sort_ready)
        Q2_INFO("sort order: %u bytes, %u streams",
                c->sortdata.size, q2_sortdata_enumerate(&c->sortdata, NULL, 0));
    c->zone_index = index;
    snprintf(c->map, sizeof(c->map), "%s", map);

    /* And the name the table shows it by — 0x800E46B4's copy. A directory with
     * no record keeps its own name, which is what the compare then has. */
    snprintf(c->map_display, sizeof(c->map_display), "%s", map);
    if (c->level_table_ready) {
        const q2_level_entry *e = q2_level_find(&c->level_table, map);

        if (e && e->display[0])
            snprintf(c->map_display, sizeof(c->map_display), "%s", e->display);
    }

    /*
     * Prefer a real spawn point in this zone. StartPos records carry the zone
     * they belong to, so a map's spawns are not all valid here — filtering by
     * zone is the difference between starting in the level and starting inside
     * a wall somewhere else.
     */
    {
        char path[256];
        q2_buf buf;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);

        if (disc_read_file(c->disc, path, &buf) == Q2_OK) {
            q2_common_file common;

            if (q2_common_open(&common, &buf) == Q2_OK) {
                q2_start_pos_list spawns;

                if (q2_start_pos_parse(&spawns, &common) == Q2_OK) {
                    u32 i;

                    /*
                     * A deathmatch starts at a MultiSpawn, chosen the way the
                     * original chooses one — the farthest from everybody who is
                     * already standing somewhere, with ties broken by the RNG.
                     * The eight names are fixed (`MultiSpawn0`..`MultiSpawn7`)
                     * and only an arena carries any.
                     */
                    if (c->mp_enabled) {
                        q2_mp_spawn ms[Q2_MP_MAX_SPAWNS];
                        u32 n = 0;

                        memset(ms, 0, sizeof(ms));
                        for (i = 0; i < spawns.count && n < Q2_MP_MAX_SPAWNS; i++) {
                            q2_start_pos sp;

                            if (!q2_start_pos_get(&spawns, i, &sp))
                                continue;
                            if (sp.zone != index)
                                continue;
                            if (strncmp(sp.name, "MultiSpawn", 10) != 0)
                                continue;

                            ms[n].pos[0]  = sp.x;
                            ms[n].pos[1]  = sp.y;
                            ms[n].pos[2]  = sp.z;
                            ms[n].angle   = sp.angle;
                            ms[n].present = true;
                            n++;
                        }

                        c->mp_spawn_count = n;
                        /* Kept, because a respawn needs somewhere to put the
                         * player back and `ms` is a local. */
                        memcpy(c->mp_spawns, ms, sizeof(c->mp_spawns));
                        if (n) {
                            q2_mp_player_view pv[Q2_MP_MAX_PLAYERS];
                            int players = c->mp.player_count;
                            int pi;

                            memset(pv, 0, sizeof(pv));
                            if (players < 1)
                                players = 1;

                            /*
                             * Every player, not just the local one, and each
                             * placed AGAINST the ones already placed — which is
                             * what the selector is for: it takes the spawn
                             * farthest from everybody standing somewhere, so
                             * four players spread out instead of piling onto
                             * whichever point happens to be first.
                             */
                            for (pi = 0; pi < players; pi++) {
                                int pick = q2_mp_select_spawn(ms, pv,
                                                              (u32)pi,
                                                              client_mp_rng, c);

                                if (pick < 0)
                                    break;

                                c->mp_view_pos[pi][0] = ms[pick].pos[0];
                                c->mp_view_pos[pi][1] = ms[pick].pos[1];
                                c->mp_view_pos[pi][2] = ms[pick].pos[2];
                                c->mp_view_yaw[pi]    = ms[pick].angle;
                                c->mp_view_valid[pi]  = true;

                                pv[pi].alive  = true;
                                pv[pi].pos[0] = ms[pick].pos[0];
                                pv[pi].pos[1] = ms[pick].pos[1];
                                pv[pi].pos[2] = ms[pick].pos[2];

                                if (pi == 0) {
                                    c->cam.pos[0] = ms[pick].pos[0];
                                    c->cam.pos[1] = ms[pick].pos[1];
                                    c->cam.pos[2] = ms[pick].pos[2];
                                    c->cam.yaw    = ms[pick].angle;
                                    placed = true;
                                }
                                Q2_INFO("deathmatch: player %d at MultiSpawn %d",
                                        pi, pick);
                            }
                            Q2_INFO("deathmatch: %u MultiSpawn points on %s",
                                    n, map);
                        } else {
                            Q2_WARN("deathmatch: %s zone %d has no MultiSpawn "
                                    "points — this is not an arena", map, index);
                        }
                    }

                    /*
                     * A ZONE GATE DOES NOT MOVE THE PLAYER AT ALL.
                     *
                     * This is the reported teleport, and both previous answers
                     * to it were wrong in the same way: they placed the player
                     * somewhere. First at the zone's first StartPos, then — on
                     * the theory that a gate "really does have to move the
                     * player" because zones occupy different regions of one
                     * coordinate space — at a named 'InZone<N>' entry point.
                     * Neither cured it, because the premise was never tested.
                     *
                     * It is false. `--zone-probe` takes every trigger volume
                     * whose script reaches a ZONEGATE, takes the volume's own
                     * centre, and asks the DESTINATION zone's movement hull
                     * which cell holds it. Across BASE1, BASE2, BASE3, LAB,
                     * SECURITY, POWER1, POWER2, JAIL2, JAIL5 and BIGGUN, all
                     * ONE HUNDRED gates land in a real cell of the zone they
                     * name, and most land in a real cell of BOTH zones at once
                     * — BASE1's trigger 4, for one, resolves in zone 0 and in
                     * zone 1's cell 216.
                     *
                     * A gate is a DOORWAY. The volume straddles the seam
                     * between two adjacent regions of one space, and crossing
                     * it streams the next zone in around a player who has not
                     * moved and does not stop walking. The evidence for the old
                     * premise was a single measurement of BASE1's zone-0 SPAWN
                     * point against zone 1 — the START of zone 0, twenty-seven
                     * thousand units from any gate, which of course resolves
                     * nowhere and never meant anything.
                     *
                     * So the position carries. It is taken off the sim before
                     * the load frees it (see `carry_pos`) rather than from
                     * `cam.pos`, which holds the eye rather than the feet.
                     */
                    if (c->carry_same_map && c->carry_pos_valid) {
                        c->cam.pos[0] = c->carry_pos[0];
                        c->cam.pos[1] = c->carry_pos[1];
                        c->cam.pos[2] = c->carry_pos[2];
                        c->cam.yaw    = c->carry_yaw;
                        c->cam.pitch  = c->carry_pitch;
                        placed        = true;
                        Q2_INFO("zone gate: carried through at (%d,%d,%d)",
                                c->carry_pos[0], c->carry_pos[1],
                                c->carry_pos[2]);
                    }

                    /*
                     * The arrival point first, when it is in this zone. Every
                     * single-player map carries a "Default", and it is NOT
                     * always the first StartPos of zone 0: COMMAND's is in
                     * zone 2, while its zone 0 opens on 'Mapzone0', a ledge
                     * the player falls from and dies within two seconds.
                     */
                    if (!placed && c->arrival[0]) {
                        q2_start_pos sp;

                        if (q2_start_pos_find(&spawns, c->arrival, &sp) &&
                            sp.zone == index) {
                            c->cam.pos[0] = sp.x;
                            c->cam.pos[1] = sp.y;
                            c->cam.pos[2] = sp.z;
                            c->cam.yaw    = sp.angle;
                            placed = true;
                            Q2_INFO("spawned at arrival '%s' (%d,%d,%d)",
                                    sp.name, sp.x, sp.y, sp.z);
                        }
                    }

                    for (i = 0; !placed && i < spawns.count; i++) {
                        q2_start_pos sp;
                        if (!q2_start_pos_get(&spawns, i, &sp))
                            continue;
                        if (sp.zone != index)
                            continue;

                        c->cam.pos[0] = sp.x;
                        c->cam.pos[1] = sp.y;
                        c->cam.pos[2] = sp.z;
                        c->cam.yaw    = sp.angle;
                        placed = true;
                        Q2_INFO("spawned at '%s' (%d,%d,%d)",
                                sp.name, sp.x, sp.y, sp.z);
                        /*
                         * FIRST-MATCH IS THE FRESH-START PATH. Reaching it on
                         * a same-map transition means the carried position was
                         * missing, and the player has just been dropped at
                         * whichever StartPos the file happens to list first —
                         * which is the reported teleport exactly.
                         */
                        if (c->zone_trace && c->carry_same_map)
                            Q2_WARN("[zone]        *** NO CARRIED POSITION for"
                                    " gate '%s' into zone %d - dropped at the"
                                    " FIRST StartPos, '%s'",
                                    c->gate_name[0] ? c->gate_name : "(unnamed)",
                                    index, sp.name);
                        break;
                    }
                }
                /*
                 * `--at` overrides whatever placed the player, which is the
                 * only way to photograph a part of a level a spawn point does
                 * not look at. It is a capture tool and nothing else: the sim
                 * is spawned at the overridden position, so the player really
                 * is standing there and everything downstream — visibility,
                 * the zone's own script, the creatures' interest — is the
                 * game's, not a floating camera's.
                 *
                 * It applies to the FIRST load only. Re-applying it after a
                 * zone gate would put the player back on the mark every time
                 * they crossed one, which makes the transition itself
                 * untestable — the thing being measured is exactly where the
                 * gate leaves them.
                 */
                if (c->at_given && !c->carry_same_map) {
                    c->cam.pos[0] = c->at[0];
                    c->cam.pos[1] = c->at[1];
                    c->cam.pos[2] = c->at[2];
                    if (c->yaw_given)
                        c->cam.yaw = c->at_yaw;
                    if (c->pitch_given) {
                        c->cam.pitch                 = c->at_pitch;
                        c->sim[0].player[0].pitch    = c->at_pitch;
                    }
                    Q2_INFO("--at (%d,%d,%d) yaw %d",
                            c->at[0], c->at[1], c->at[2], (int)c->cam.yaw);
                }

                /*
                 * AND THE CREATURE WORLD GOES FIRST, because it borrows this
                 * file too and the borrow is not a read-only curiosity — it is
                 * dereferenced between here and the reload.
                 *
                 * `q2_creature_world.pop` points into COMMON.DAT's `Population`
                 * chunk; creworld.h said it "borrows COMMON.DAT, which outlives
                 * the zone", and that was true of the zone and false of the
                 * FILE: every load re-reads COMMON, including a gate between
                 * two zones of one map. Between the close below and
                 * `client_load_creatures` several hundred lines down sits
                 * `q2_sim_settle`, which ticks the world, which runs the event
                 * runtime, which runs CREBATCH, which calls
                 * `q2_creature_world_summon` — and that walks `w->pop`.
                 *
                 * AddressSanitizer on `--map JAIL3 --zone-probe`:
                 * heap-use-after-free, 12 bytes read in `q2_pop_get_group`
                 * (population.c:56) out of the buffer `q2_common_close` frees
                 * here. It crashed four runs in five; the fifth read whatever
                 * the allocator had put back and carried on, which is the worse
                 * half of the fault.
                 *
                 * Freeing here rather than teaching the population to own a
                 * copy keeps one rule instead of two: nothing holds a pointer
                 * into COMMON.DAT across the swap. The summons those settle
                 * passes make were never reaching this zone's creatures anyway
                 * — the set they name is rebuilt afterwards — so nothing that
                 * used to happen stops happening.
                 */
                client_free_creatures(c);

                /* The sim borrows the triggers and script out of this file, so
                 * it has to outlive the zone. Release the previous map's copy
                 * and take ownership of this one. */
                q2_common_close(&c->common);
                /*
                 * MOVED, not assigned. `q2_common_file` holds pointers into its
                 * own inline directory, so `c->common = common` leaves every
                 * one of them aimed at a local that is about to die — see
                 * level.h. That is what put 0x80808080 into a StartPos size on
                 * BIGGUN.
                 */
                if (q2_common_move(&c->common, &common) != Q2_OK) {
                    Q2_ERROR("could not adopt %s's COMMON.DAT", map);
                    return false;
                }

                /*
                 * The briefing's three fields, out of the map's own `Strings`
                 * chunk (leveltext.h). `MapTitle` is the location; the orders
                 * and the objective are keyed by unit number, which the game
                 * knows and the port does not yet — so the first key that
                 * resolves is taken, which for a single-unit map is the right
                 * one and for a shared directory is the lowest unit present.
                 */
                {
                    q2_leveltext tx;

                    q2_briefing_init(&c->briefing);
                    c->leveltext_ready =
                        (q2_leveltext_open(&c->leveltext, &c->common) == Q2_OK);
                    c->script_strings = 0;
                    c->script_sounds  = 0;
                    if (q2_leveltext_open(&tx, &c->common) == Q2_OK) {
                        char key[Q2_LEVELTEXT_NAME_LEN + 1];
                        const char *s2;
                        int unit, step;

                        /*
                         * `MapTitle` is the level's OWN name — "Outer Base"
                         * where the directory says BASE1 — and it is what the
                         * mission screen's Location column wants as much as
                         * the briefing does. The level table's `display` is
                         * not it: that column reads "Base1".
                         */
                        s2 = q2_leveltext_find(&tx, "MapTitle");
                        c->map_title[0] = '\0';
                        if (s2) {
                            q2_briefing_set_location(&c->briefing, s2);
                            snprintf(c->map_title, sizeof(c->map_title),
                                     "%s", s2);
                        }

                        /*
                         * `FoundASecret` is the message INSECRET shows, and it
                         * is the map's own words rather than a string this port
                         * would otherwise have had to invent.
                         */
                        c->secret_message[0] = '\0';
                        s2 = q2_leveltext_find(&tx, "FoundASecret");
                        if (s2)
                            snprintf(c->secret_message,
                                     sizeof(c->secret_message), "%s", s2);

                        /*
                         * And the UNIT, which the mission screen's title needs
                         * ("Mission %d - Complete") and which nothing was
                         * reading. The scan below already finds it — a map
                         * carries `Unit<N>Miss1` for its own unit and no other
                         * — so it is recorded rather than discarded.
                         */
                        {
                            bool own_unit = false;

                            for (unit = 1; unit <= 9; unit++) {
                                q2_leveltext_key_objective(key, unit);
                                s2 = q2_leveltext_find(&tx, key);
                                if (s2) {
                                    q2_briefing_set_objective(&c->briefing, s2);
                                    c->map_unit = unit;
                                    own_unit    = true;
                                    break;
                                }
                            }
                            for (unit = 1; unit <= 9; unit++) {
                                bool got = false;
                                for (step = 0; step <= 15; step++) {
                                    q2_leveltext_key_orders(key, unit, step);
                                    s2 = q2_leveltext_find(&tx, key);
                                    if (s2) {
                                        q2_briefing_set_orders(&c->briefing, s2);
                                        got = true;
                                        break;
                                    }
                                }
                                if (got)
                                    break;
                            }

                            /*
                             * AND TAKE THIS LEVEL'S ROW IN THE MISSION TABLE,
                             * here, because this is where the level's own name
                             * and its unit are known — which is exactly the
                             * point at which the console's LevelBin init does
                             * it (`client_mission_enter`).
                             *
                             * Only a map that carries its own `Unit<N>Miss1`.
                             * `QENDMIS<N>`, `QFMV` and the front end have no
                             * unit of their own and must not take a row from
                             * the one they are sitting between.
                             */
                            if (own_unit)
                                client_mission_enter(c);
                        }
                    }
                }
            } else {
                q2_buf_free(&buf);
            }
        }
    }

    if (!placed) {
        /* No spawn for this zone — fall back to its centre so there is still
         * something on screen. */
        q2_world_bounds(&c->zone, wmin, wmax);
        c->cam.pos[0] = (wmin[0] + wmax[0]) / 2;
        c->cam.pos[1] = (wmin[1] + wmax[1]) / 2;
        c->cam.pos[2] = (wmin[2] + wmax[2]) / 2;
    }

    /* Upload this MAP's texture pages and palettes. A streamed zone of the
     * same map names the same bank, so replacing it here only stalls the seam
     * and briefly discards already resident pixels. */
    if (!same_map_transition && c->vram) {
        q2_vram_section vs;
        memset(c->vram, 0, sizeof(*c->vram));
        if (q2_vram_load(&vs, c->disc, map) == Q2_OK) {
            c->opts.textures = (q2_vram_upload(&vs, c->vram) == Q2_OK);
            Q2_INFO("textures: %u pages, %u palettes",
                    vs.texpage_count, vs.clut4_count);
            c->clut4_count_a = vs.clut4_count_a;

            /*
             * The UI's own images, into the cells their registration slots
             * name (0x8003FE20): `frontend.lbm` for the menu's 16- and
             * 32-pixel faces, `chars.lbm` for the 8-pixel face and the HUD's
             * atlas, and the icon sheet. Three maps carry no `frontend.lbm`
             * and two no `chars.lbm`, so this is allowed to come back empty —
             * the menu then has no letterforms and says so once.
             */
            c->menu_font_ready = false;
            if (c->hud_tables_ready) {
                /*
                 * WHICH icon sheet is a session question, and asking the wrong
                 * one fails silently: `q2_menu_icons_name` picks `qk_menu.lbm`
                 * in single player, `qk2_menu.lbm` for a two-player match and
                 * `qkm_menu.lbm` for three or four, and an arena carries only
                 * the multiplayer ones. The upload still returns Q2_OK because
                 * the atlases went in, so `menu_font_ready` was true, the
                 * status bar drew into an empty texture page, and every arena
                 * on the disc showed no health, no armour and no ammo. See
                 * openquestions #52.
                 */
                int hud_players = c->mp_enabled ? c->mp.player_count : 1;
                q2_result fr = q2_menu_font_upload(&c->menu_font,
                                                   &c->hud_tables, &vs,
                                                   c->vram,
                                                   c->mp_enabled, hud_players);
                c->menu_font_ready = (fr == Q2_OK);
                if (!c->menu_font_ready) {
                    /*
                     * A map with NEITHER letterform atlas has no text to draw.
                     * QFMV is the movie stub — 46 KB, no `frontend.lbm`, no
                     * `chars.lbm`, no icon sheet — and warning about its font
                     * is warning that a film has no subtitles.
                     */
                    u32 fx;

                    if (!q2_vram_find_by_name(&vs, "frontend.lbm", &fx) &&
                        !q2_vram_find_by_name(&vs, "chars.lbm", &fx))
                        Q2_INFO("%s carries no letterforms — it draws no text",
                                map);
                    else
                        Q2_WARN("%s carries no menu font", map);
                }
                /*
                 * A missing icon sheet is three different things and only one
                 * of them is a fault.
                 *
                 * The sheet is chosen by session: `qk_menu.lbm` in single
                 * player, `qk2_menu.lbm` for two, `qkm_menu.lbm` for three or
                 * four. **An arena carries only the multiplayer ones**, because
                 * an arena cannot be played in single player — so opening
                 * MATRIX1 alone finds no `qk_menu.lbm` and that is the disc
                 * being right, not the port being wrong. Loaded with `--dm` the
                 * same map resolves its sheet immediately.
                 *
                 * And the front-end maps carry no sheet at all, because they
                 * draw no status bar: QFRONT, QLOGOS, QENDMIS1..5 and the rest
                 * are screens, not levels.
                 *
                 * Warning on all 28 of those buried the fault it was there to
                 * report. So the probe asks which sheets the map DOES carry and
                 * says which of the three cases this is.
                 */
                if (!c->menu_font.icons_resident) {
                    u32 ix;
                    bool has_mp =
                        q2_vram_find_by_name(&vs, "qk2_menu.lbm", &ix) ||
                        q2_vram_find_by_name(&vs, "qkm_menu.lbm", &ix);

                    if (has_mp && !c->mp_enabled)
                        Q2_INFO("%s is an arena: it carries the multiplayer "
                                "icon sheets and no single-player one", map);
                    else if (!has_mp &&
                             !q2_vram_find_by_name(&vs, "qk_menu.lbm", &ix))
                        Q2_INFO("%s carries no icon sheet — it draws no "
                                "status bar", map);
                    else
                        Q2_WARN("%s carries no '%s' — the status bar will be "
                                "blank", map,
                                q2_menu_icons_name(c->mp_enabled,
                                                   hud_players));
                }

                /* The overlay's own view of the same atlas. It re-uploads
                 * chars.lbm, which is harmless — the same halfwords to the
                 * same place — and gives the markup layer its palettes. */
                c->hud_font_ready =
                    (q2_hud_font_upload(&c->hud_font, &c->hud_tables, &vs,
                                        c->vram) == Q2_OK);
            }

            q2_vram_free(&vs);
        } else {
            c->opts.textures = false;
            c->menu_font_ready = false;
        }
    }

    /* The same file's second section: the map's sound bank, which is where the
     * menu's five effects live.
     *
     * Every voice is silenced first. A playing voice holds a pointer INTO this
     * bank's buffer and decodes from it as it goes (vag.h), so freeing the bank
     * under one would have it reading freed memory — and a zone load is exactly
     * when something is mid-play, because the door that triggered it just made
     * a noise. */
    if (!same_map_transition) {
        client_voices_stop(c);
        c->bed_frames = 0;
        c->bed_pos    = 0;
        if (c->sfx_ready) {
            q2_sound_bank_free(&c->sfx);
            c->sfx_ready = false;
        }
        c->sfx_ready = (q2_sound_bank_load(&c->sfx, c->disc, map) == Q2_OK);
        if (c->sfx_ready)
            Q2_INFO("sound bank: %u effects", c->sfx.count);

        client_item_sounds_resolve(c);
    }

    /*
     * The gib bodies name the sim's entity set by address (modelent.h), and
     * the free below takes that set away: forgotten first, as q2_gib_attach
     * asks. Re-attaching the same address would keep them — the set pointer
     * does not change between zones — so this is the only thing that does.
     * The attach further down this load binds the new set.
     */
    q2_gib_attach(NULL);
    /* q2_sim_init memsets the struct, so the previous zone's trigger bitmap and
     * event runtime have to be released first or they leak on every zone
     * change -- and zone changes are exactly what the gates now cause. */
    q2_sim_free(&c->sim[0]);
    /*
     * The set every in-flight drop and every queued death belonged to has just
     * been freed, so their records go with it: a drop's flight record names
     * its set and slot (item.h), and a queued death would spawn into the next
     * zone's set at the last zone's coordinates.
     */
    q2_item_drop_reset();
    q2_monster_drop_reset();
    q2_sim_init(&c->sim[0], &c->zone, q2_build_tick_rate(&c->build));
    /* The client owns a view-weapon machine, so the machine decides when a
     * shot happens — the tick must not also fire from the raw trigger. */
    c->sim[0].fire_from_input = false;
    /* Re-armed after every load, because the memset above clears it. */
    c->sim[0].trace_zone      = c->zone_trace;
    c->sim[0].autoswitch      = !c->no_autoswitch;
    c->sim[0].invulnerable    = c->god;
    /*
     * And the damage effects' mesh (sim.h, `fx_mesh`): the crackle, the
     * sparks and the quad shell spawn on a creature's own posed vertices
     * (0x8006CC44), which only this side can supply because it owns the
     * models. HERE, beside the other re-arms, because q2_sim_init clears it:
     * client_load_creatures, where it used to be installed, returns before
     * its end when a map's creatures will not load. The hook refuses any
     * actor that is not a live, drawn creature, so installing it before the
     * creatures exist is safe.
     */
    c->sim[0].fx_mesh      = client_fx_mesh;
    c->sim[0].fx_mesh_user = c;
    {
        s32 feet[3];
        feet[0] = c->cam.pos[0];
        feet[1] = c->cam.pos[1];
        feet[2] = c->cam.pos[2];
        q2_sim_attach_gameplay(&c->sim[0], &c->common);

        /*
         * WHICH ZONE THE RUNTIME IS RESIDENT IN — the name 0x80079178 compares
         * a ZONEGATE's target against (0x800E465C) before it accepts one, and
         * the switch for the handler's record abort at 0x8002783C. The owner
         * has to say it after EVERY attach, because q2_event_rt_init resets it
         * to -1 ("unknown", the old accept-and-carry-on behaviour; events_rt.h).
         * c->zone_index was assigned at the top of this load.
         */
        c->sim[0].event_rt.current_zone = c->zone_index;

        if (c->restoring_save &&
            q2_save_event_carry(c->restoring_save, &c->sim[0].event_rt,
                                 &c->carry_events) != Q2_OK) {
            Q2_ERROR("save script state does not match %s", c->map);
            return false;
        }

        /* The latches, back on the fresh runtime before anything can run it.
         * They only restore state; the replay after the hooks and the movers
         * are in place is what runs script (q2_event_rt_replay). */
        q2_event_rt_carry_in(&c->sim[0].event_rt, &c->carry_events);

        /*
         * The map's model bank, and the view weapon that draws out of it. The
         * weapon starts already raised, which is what a level start does — the
         * machine's own reset lands in RAISE at frame 0.
         */
        c->model_bank_ready =
            (q2_model_bank_from_common(&c->model_bank, &c->common) == Q2_OK);
        client_bind_player_models(c);

        /*
         * The things standing in the room when you arrive.
         *
         * Population's place records are the map's items, and until now the
         * client was the one caller that never spawned them: the sim had the
         * entity set, the thinks and the touch sweep, and the set was empty, so
         * every level was a walk through an empty building.
         *
         * It goes AFTER the bank is opened because the bank is what resolves
         * each item's model at spawn, which is where the engine resolves it
         * (0x80058850) — an item whose model this map does not ship never
         * spawns at all rather than being looked up mid-frame. And after
         * q2_sim_attach_gameplay because a place list is per MAP, exactly as
         * the triggers and the script are, and both come out of the same
         * COMMON.DAT this call borrows.
         *
         * The player is registered by the attach and moved every tick, so the
         * touch sweep works from the spawn below without anything here having
         * to order the two.
         *
         * The zone goes in because Population is per MAP and a session is in
         * one ZONE: without it a map's other four zones' items stand around in
         * this one. What that can and cannot decide is q2_item_spawn_zone's.
         */
        {
            /*
             * AND IN AN ARENA, THE MODE PICKS THE BATCHES.
             *
             * QMULTI.C's init at 0x80100140 spawns the map's item batches by
             * name — "Weapons", then "Health"/"Armour"/"Ammo" unless the mode
             * is VERSUS, then "Specials" — where a single-player level lets its
             * own LevelBin say. The arena module's code names all five whatever
             * the mode, so the scan the attach does by default selected the lot
             * and VERSUS played as a deathmatch that does not respawn: the
             * front end's own rules page says "THERE ARE NO AMMO OR HEALTH
             * POWER-UPS IN THE LEVEL", and MATRIX5 placed seventeen items in
             * every mode instead of seventeen and seven.
             *
             * It is decided here, not inside the sim, because `sim.multiplayer`
             * is only written once a frame from the tick (see the note on
             * `q2_sim.multiplayer` below) and is therefore still false at every
             * zone load, this one included. `mp_enabled` and `mp.mode` are set
             * by client_mp_configure, which runs before every load that can
             * reach an arena — the CLI's, the front end's PROCEED and the
             * round restart alike.
             */
            const char *batch[Q2_MP_MAX_BATCHES];
            u32 batch_count = c->mp_enabled
                                  ? q2_mp_batches(c->mp.mode, batch) : 0;
            q2_result ir = q2_sim_attach_item_batches(
                &c->sim[0], &c->common, index,
                c->item_table_ready ? &c->item_table : NULL,
                c->model_bank_ready ? &c->model_bank : NULL,
                batch_count ? batch : NULL, batch_count);

            /*
             * BRACED, and it had to be: the `if (c->zone_trace)` below used to
             * sit unbraced between the success arm and the `else`, so the
             * `else` bound to IT instead. Every run without `--zone-trace`
             * therefore printed "places no items: ok" over the line that had
             * just said seventeen were placed — a warning about a failure that
             * had not happened, on the one path a reader would go looking at
             * when items were missing.
             */
            if (ir == Q2_OK) {
                if (batch_count) {
                    u32 bi;
                    char names[128];
                    int  at = 0;

                    for (bi = 0; bi < batch_count; bi++)
                        at += snprintf(names + at, sizeof(names) - (size_t)at,
                                       "%s%s", bi ? " " : "", batch[bi]);
                    Q2_INFO("items: %u placed (%s batches: %s)",
                            c->sim[0].entities.count,
                            q2_mp_mode_name(c->mp.mode), names);
                } else {
                    Q2_INFO("items: %u placed", c->sim[0].entities.count);
                }

                /*
                 * Where each one ended up, because "the pickup is in the
                 * floor" is a claim about three numbers — the position the
                 * hull drop settled on, the model's own vertical bias, and the
                 * draw origin built from the two — and no log carried any of
                 * them.
                 */
                if (c->zone_trace) {
                    u32 k;
                    for (k = 0; k < c->sim[0].entities.count; k++) {
                        const q2_entity *e = &c->sim[0].entities.ent[k];
                        Q2_INFO("[item] %2u model %3d bias %4d  pos %d,%d,%d"
                                "  origin y %d  feet y %d  cell %d",
                                k, (int)e->model_index, (int)e->model_offset,
                                e->pos[0], e->pos[1], e->pos[2],
                                e->origin[1], e->pos[1] + Q2_SWEEP_HALF_EXTENT,
                                (int)e->node);
                    }
                }
            } else {
                Q2_WARN("%s places no items: %s", map, q2_result_str(ir));
            }

            /*
             * And the TITLE SCREEN's objects, which are items too but are not
             * in Population — QFRONT's is empty. Its `LevelBin` names five
             * table ids and spawns them itself, which is where the logo comes
             * from and why it turns (levelbin.h). Every other map's module
             * names none, so this is a no-op everywhere else and does not have
             * to be gated on the front end.
             */
            {
                u32 sn = q2_sim_attach_scene(
                    &c->sim[0], &c->common,
                    c->item_table_ready ? &c->item_table : NULL,
                    c->model_bank_ready ? &c->model_bank : NULL);

                if (sn)
                    Q2_INFO("scene: %u objects from %s's LevelBin", sn, map);
            }
        }

        if (c->vm_ready) {
            /*
             * A zone/map transition does NOT create the weapon in the
             * player's hands again. `carry_player` means this load came from
             * a live session, and the entire view-weapon machine — state,
             * key/frame clocks, current transform, old model and pending
             * selection — belongs to that player just as much as the weapon
             * id does.
             *
             * Re-running q2_vw_init here reset it to the level-start LOWER
             * state on every streamed zone and every LOADMAP. The carry block
             * below then restored the selected weapon after this had recorded
             * the temporary spawn blaster in `vw_last_weapon`, so the first
             * new-zone frame selected the carried gun again and played a full
             * lower/raise. Retail keeps the existing machine across these
             * loads; only its CastList model pointer has to be rebound to the
             * newly loaded bank.
             *
             * A genuine fresh load — new game, restart or save restore — still
             * takes the init arm. Save restore then replaces that initial
             * state with its live weapon machine, when present in the file.
             */
            if (!c->carry_player)
                client_reset_view_model(c, 0);
            else
                client_bind_view_model(c, 0);
        }
        /* The zone number seeds the effect generator, so re-entering a zone
         * looks the same twice and two zones do not share a sequence. */
        if (c->fx_tables_ready) {
            q2_sim_attach_effects(&c->sim[0], &c->fx_tables,
                                  0x51A5E5u + (u32)index);

            /*
             * The particle quads live on `chars.lbm`'s page, so the overlay's
             * atlas is also the effect atlas. Without this they fall back to
             * flat quads; with it they are the console's own textured ones.
             */
            if (c->hud_font_ready)
                q2_fx_use_hud_atlas(&c->sim[0].fx, &c->hud_font);

            /*
             * The glint mesh is the map's own `GlintMod` chunk, and only BIGGUN
             * has one. A map without it simply has no glint.
             */
            q2_sim_attach_glint(&c->sim[0], &c->common);

            /*
             * THE DEBRIS MODELS, which nothing has ever registered.
             *
             * 0x80064F70 appends a model to the 32-slot list at 0x800D56B0
             * (`sw a0, 0(v1)` at 0x80064F9C, capped by the `slti a1, 32` at
             * 0x80064F80) and the burst picks from it uniformly — 0x80064748
             * loads the chosen word and hands it to the piece spawner
             * 0x80064398 as its model. A level fills the list from its own
             * class table, where "Debris1", "Debris2" and "Debris3" all
             * register through that function.
             *
             * This port's q2_fx_debris_register had no caller at all, so
             * `debris_model_count` was zero for the whole of every run and
             * every piece took the `model = -1` arm: a shattered pane threw
             * twenty-one chunks that moved, bounced and expired invisibly.
             *
             * BY NAME, never by bank index. Only 19 of the disc's 49 banks
             * carry the three models, and on five of those Debris1 is not at
             * index 0 — it is 19 on LAB and POWER2, 2 on SECURITY, 25 on
             * WASTE1, 20 on WASTE3, and POWER2's index 0 is `BFGBlast`. A map
             * with no Debris model registers nothing and keeps the -1 arm,
             * which is the right answer on those 30 banks.
             *
             * It has to sit after q2_sim_attach_effects: that calls
             * q2_fx_world_init, which memsets the world and would wipe the
             * list.
             */
            if (c->model_bank_ready) {
                static const char *const debris_names[] = {
                    "Debris1", "Debris2", "Debris3"
                };
                u32 dn;

                for (dn = 0; dn < Q2PSX_ARRAY_COUNT(debris_names); dn++) {
                    s32 mi = q2_model_bank_find(&c->model_bank,
                                                debris_names[dn]);

                    if (mi >= 0 &&
                        q2_fx_debris_register(&c->sim[0].fx, (s16)mi))
                        c->debris_models++;
                }
            }
        }

        /*
         * WHAT THE ITEM THINKS REACH THAT THEIR WORLD DOES NOT CARRY (item.h,
         * q2_item_env): the one particle pool the materialise burst spawns
         * into (0x800596B0), and for a creature's dropped item the toss
         * mover's hull, entity list and gravity word (0x80046DDC, 0x80053974,
         * [0x800AE924]). Console globals, bound once a load.
         *
         * `move_world` by ADDRESS for the reason the AI binds it that way:
         * q2_sim_attach_movers reallocates its target array later in this
         * load, and the struct itself is what stays put. The gravity is the
         * sim's own word, read live, so the GAME VARIABLES menu reaches a drop
         * in flight the way it reaches the player.
         */
        {
            q2_item_env ienv;

            memset(&ienv, 0, sizeof(ienv));
            ienv.fx      = c->sim[0].fx_ready ? &c->sim[0].fx : NULL;
            ienv.hull    = c->sim[0].coll_ready ? &c->sim[0].coll : NULL;
            ienv.ents    = &c->sim[0].move_world;
            ienv.gravity = &c->sim[0].gravity;
            q2_item_bind_env(&ienv);
        }

        /*
         * THE GIB WORLD (modelent.h, q2_gib_world): what 0x8007CEB4 and its
         * throwers reach as console globals — the entity pool, the CastList,
         * the particle pool, rand(), the PRIMARY hull a gib moves in
         * (0x80046CE0) and the door list its mover tries first (0x80053974)
         * — bound once a load, so the creature corpse tick and the player's
         * gib test can throw. Without it both sites dispatch into nothing and
         * no body ever comes apart. Gravity is the sim's word, read live;
         * `move_world` by address for the reason the item env gives above.
         */
        {
            q2_gib_world gw;

            memset(&gw, 0, sizeof(gw));
            gw.set      = &c->sim[0].entities;
            gw.bank     = c->sim[0].model_bank;
            gw.fx       = c->sim[0].fx_ready ? &c->sim[0].fx : NULL;
            gw.rng      = &c->sim[0].fx_rng;
            gw.coll     = c->sim[0].coll_primary_ready
                              ? &c->sim[0].coll_primary : NULL;
            gw.ents     = &c->sim[0].move_world;
            gw.gravity  = &c->sim[0].gravity;
            gw.sounds   = &c->sim[0].explosion_sounds;
            gw.describe = client_gib_describe;
            gw.user     = c;
            q2_gib_attach(&gw);
        }
        /*
         * The zone's lights: COMMON.DAT's `Lights` array and the zone's own
         * per-node index lists, which is exactly the pair `q2_light_gather`
         * wants. SpaceLights is partitioned by the SECONDARY collision node, so
         * it is opened against the hull the sim already has.
         */
        c->lights_ready = false;
        /*
         * InitLights' whole treatment of the timed table, 0x80074194: zero the
         * sixteen counters and leave everything else. It runs here rather than
         * inside the arm below because the memset there is conditional on the
         * zone having lights at all — and a TIMEDLIGHT parked by the level the
         * player just left would otherwise keep counting down over the top of
         * the next one, in a slot nothing on this level can reuse.
         */
        q2_light_timed_reset(&c->light_world);
        if (c->sim[0].coll_ready &&
            q2_lights_parse(&c->lights, &c->common) == Q2_OK &&
            q2_spacelights_open(&c->spacelights, &c->zone.zone,
                                &c->sim[0].coll) == Q2_OK) {
            memset(&c->light_world, 0, sizeof(c->light_world));
            c->light_world.statics = &c->lights;
            c->light_world.space   = &c->spacelights;
            c->lights_ready = true;
            Q2_INFO("lights: %u in the map, %u index entries",
                    c->lights.count, c->spacelights.count);

            /*
             * The colours the map actually ships, because a creature lit
             * entirely green is either standing under a green lamp or being
             * handed one that is not there. A histogram of pure single-channel
             * records answers that without arguing about any one of them: real
             * level lighting is tinted, and a chunk full of (0,255,0) is a
             * decode fault wearing a lamp's clothes.
             */
            if (c->zone_trace) {
                u32 li, pure_r = 0, pure_g = 0, pure_b = 0, grey = 0, mixed = 0;

                for (li = 0; li < c->lights.count; li++) {
                    q2_light lt;
                    if (!q2_light_get(&c->lights, li, &lt))
                        continue;
                    if (!lt.r && lt.g && !lt.b)      pure_g++;
                    else if (lt.r && !lt.g && !lt.b) pure_r++;
                    else if (!lt.r && !lt.g && lt.b) pure_b++;
                    else if (lt.r == lt.g && lt.g == lt.b) grey++;
                    else                             mixed++;
                    if (li < 8)
                        Q2_INFO("[lamp] %2u rgb %3u,%3u,%3u type %u radius %u",
                                li, lt.r, lt.g, lt.b, lt.type, lt.radius);
                }
                Q2_INFO("[lamp] %u pure red, %u pure GREEN, %u pure blue, "
                        "%u grey, %u mixed, of %u",
                        pure_r, pure_g, pure_b, grey, mixed, c->lights.count);
            }
        }

        /*
         * The map's rotating brushes. Built from the same Events and UserFuncs
         * the movers come from, and handed to the zone, which adds each node's
         * rotation when it draws it.
         */
        if (c->rotators_ready) {
            q2_rotators_free(&c->rotators);
            c->rotators_ready = false;
        }
        free(c->node_hidden);
        c->node_hidden       = NULL;
        c->node_hidden_count = 0;
        c->zone.node_hidden  = NULL;
        c->zone.node_hidden_count = 0;

        /* The explosive set names Scene nodes of the zone being replaced, and
         * the sim borrows the pointer — drop both before either goes stale. */
        if (c->explosives_ready) {
            q2_explosives_free(&c->explosives);
            c->explosives_ready = false;
        }
        c->sim[0].explosives = NULL;

        if (c->movers_ready) {
            q2_movers_free(&c->movers);
            c->movers_ready = false;
            c->zone.movers  = NULL;
        }
        {
            q2_events    ev;
            q2_userfuncs uf;

            q2_events zev;
            bool have_zev = (q2_events_parse_zone(&zev, &c->zone.zone) == Q2_OK);

            if (q2_events_parse_common(&ev, &c->common) == Q2_OK &&
                q2_userfuncs_parse(&uf, &c->common) == Q2_OK) {
                /*
                 * A rotation CALL's object slots are read from the ZONE's Events
                 * chunk at the same offset, not from COMMON's -- 0x800285F4
                 * rebases the cursor into gp+376, which the zone loader fills at
                 * 0x8007C234. Reading COMMON's alone sees the -1 the engine
                 * stamps there as it consumes each slot, which left most of the
                 * disc's rotating geometry inert. See openquestions #56.
                 */
                c->ev_operands.base_a = ev.data;
                c->ev_operands.base_b = have_zev ? zev.data : NULL;
                c->ev_operands.b_size = have_zev ? zev.size : 0;

                /*
                 * The breakables, now that the rebase is in hand: each pane's
                 * Scene node resolved to the COLLISION node it sits in, which
                 * is the identity the weapon code matches on (#67). Registered
                 * here rather than in attach_gameplay because it needs the
                 * zone — both halves of the rebase and the hull.
                 */
                {
                    u32 n = q2_sim_attach_breakables(&c->sim[0],
                                                     &c->zone.scene,
                                                     &c->ev_operands);
                    u32 bi;

                    for (bi = 0; bi < n; bi++) {
                        const q2_breakable *b = &c->sim[0].breakable[bi];

                        Q2_INFO("breakable %u: %s node %d, %d hp, %u+%u"
                                " pieces, box (%d,%d,%d)-(%d,%d,%d)",
                                bi, b->kind == Q2_BREAKABLE_SHOOTTHEN
                                        ? "SHOOTTHEN" : "GLASS",
                                b->scene_node, b->health,
                                b->count_a, b->count_b,
                                b->bmin[0], b->bmin[1], b->bmin[2],
                                b->bmax[0], b->bmax[1], b->bmax[2]);
                    }
                }

                /*
                 * The `func_explosive` groups — opcode 0x08, and the biggest
                 * family of destroyable geometry on the disc by a wide margin.
                 *
                 * Built from the same COMMON chunk with the same rebase, for
                 * the same reason: the eight node slots and the reveal node are
                 * OBJSLOTs and read -1 out of COMMON's own copy.
                 *
                 * The initial visibility has to be applied here and not left to
                 * the first destruction: the constructor HIDES every wreckage
                 * node at load (0x80026C60), so a map whose author put rubble
                 * behind a wall would otherwise show both at once.
                 */
                if (c->explosives_ready) {
                    q2_explosives_free(&c->explosives);
                    c->explosives_ready = false;
                }
                c->explosive_boxes = 0;
                if (q2_explosives_build(&c->explosives, &ev, &c->ev_operands,
                                        &c->zone.scene) == Q2_OK) {
                    c->explosives_ready = true;
                    c->explosive_boxes =
                        q2_sim_attach_explosives(&c->sim[0], &c->explosives,
                                                 &c->zone.scene);
                }
                /* The initial visibility is applied further down, once the hide
                 * array exists — see the `node_hidden` allocation. */

                /*
                 * How many secrets this map HAS: every INSECRET call item in
                 * its script. Counted here rather than asked of the sim,
                 * because it is a property of the level and not of the play —
                 * the denominator of the mission screen's Secrets column.
                 */
                /*
                 * The FOUND count survives a zone gate; only the total is
                 * recounted.
                 *
                 * A map's zones share one mission row, and this ran on every
                 * zone load — so walking through a gate reset the player's
                 * secrets to zero and the tally reported what they had found
                 * since the last gate. `carry_same_map` is exactly "this is the
                 * same level, a zone boundary", which is the case that must not
                 * clear it.
                 */
                c->secrets_total     = 0;
                if (!c->carry_same_map) {
                    c->secrets_found     = 0;
                    c->secret_seen_count = 0;
                    /* The kill slots are the same question about the other
                     * column: a zone gate must not throw away the kills made
                     * before it, and a level change must. See `zone_dead`. */
                    memset(c->zone_dead,   0, sizeof(c->zone_dead));
                    memset(c->zone_placed, 0, sizeof(c->zone_placed));
                    c->secret_sounds     = 0;
                }
                {
                    q2_event_record rec;

                    if (q2_events_first_record(&ev, &rec)) {
                        do {
                            u32 it;

                            for (it = 0; it < rec.n_items; it++) {
                                q2_event_item item;
                                q2_uf_call    call;

                                if (!q2_events_get_item(&ev, &rec, it, &item))
                                    break;
                                if ((item.opcode & Q2_EVOP_MASK) !=
                                    Q2_EVOP_CALL)
                                    continue;
                                if (q2_uf_decode_call(&call, &uf, &item) != Q2_OK)
                                    continue;
                                if (call.prim == Q2_UF_INSECRET)
                                    c->secrets_total++;
                            }
                        } while (q2_events_next_record(&ev, &rec, &rec));
                    }
                }

                if (have_zev)
                    q2_rotators_set_operand_source(&c->rotators, ev.data,
                                                   zev.data, zev.size);
                /*
                 * The movers, from the same chunk. They take the disc's own
                 * Scene node indices — mover.h deliberately does not
                 * reproduce the console's load-time rewrite into runtime
                 * object indices — so the payload is the only input and no
                 * operand rebase is needed here.
                 */
                /* One byte per Scene node, for the nodes a script hides. */
                c->node_hidden_count = c->zone.scene.node_count;
                c->node_hidden = (u8 *)calloc(c->node_hidden_count
                                              ? c->node_hidden_count : 1, 1);
                c->script_hidden = 0;
                if (c->node_hidden) {
                    c->zone.node_hidden       = c->node_hidden;
                    c->zone.node_hidden_count = c->node_hidden_count;
                }

                /*
                 * THE EXPLOSIVES' LOAD-TIME VISIBILITY, and it has to be here
                 * rather than where they are built: the constructor at
                 * 0x80026A20 hides every wreckage node and the `reveal` node
                 * (0x80026ACC, 0x80026C60) and shows the intact ones, and the
                 * array it writes into is the one allocated three lines up.
                 *
                 * Applied before the first frame, so a map whose author stacked
                 * the intact geometry and its rubble in the same place never
                 * shows both at once.
                 */
                c->explosive_vis = 0;
                if (c->explosives_ready) {
                    u32 ei;

                    for (ei = 0; ei < c->explosives.count; ei++) {
                        q2_explosive_result vis;

                        q2_explosive_initial_vis(&c->explosives, ei, &vis);
                        client_apply_node_vis(c, &vis);
                    }

                    if (c->explosives.count)
                        Q2_INFO("explosives: %u groups, %u shootable parts,"
                                " %u nodes hidden at load",
                                c->explosives.count, c->explosive_boxes,
                                c->explosive_vis);
                }

                /*
                 * A QFMV or QENDMIS map is not a level. It is a container for
                 * the MOVIE PLAYER (levelbin.h) — so a run that reaches one and
                 * shows two quads on a black field looks exactly like a crash.
                 *
                 * The two are not the same container, and reading the table is
                 * not enough to tell them apart: the shared module is carried by
                 * every screen map on the disc, QLOGOS and QDUMMY included, so
                 * "this map has a movie table" says almost nothing. What decides
                 * is the NAME the level table gives it — `Intro FMV` and
                 * `Extro FMV` both resolve to QFMV, and `EndMission N` to
                 * QENDMIS<N> — which is also what the module compares against
                 * to choose its film.
                 */
                {
                    const dat_chunk *vlb =
                        c->common.chunk[Q2_COMMON_LEVEL_BIN];
                    bool is_fmv    = client_name_eq(c->map, "QFMV");
                    bool is_endmis = strlen(c->map) == 8 &&
                                     c->map[7] >= '1' && c->map[7] <= '9' &&
                                     strncmp(c->map, "QENDMIS", 7) == 0;

                    c->movie_count = 0;
                    c->endmission  = false;
                    if (vlb && vlb->data && vlb->size) {
                        u32 got = q2_levelbin_movies(vlb->data, vlb->size,
                                                     c->movies, 4);
                        c->movie_count = got > 4 ? 4 : got;
                    }

                    /*
                     * QFMV: the screen name picks the film, exactly as the
                     * module picks it, and the film is the whole map. Nothing
                     * else about this "level" is drawn — it has no geometry, no
                     * letterforms and no icon sheet — and when the film ends the
                     * queued destination takes over (`film_next_map`).
                     */
                    if (is_fmv && c->movie_count) {
                        const char *want = c->film_screen[0] ? c->film_screen
                                                             : "Intro FMV";
                        const char *film = NULL;
                        u32 mq;

                        for (mq = 0; mq < c->movie_count; mq++) {
                            Q2_INFO("movie: '%s' plays %s",
                                    c->movies[mq].screen, c->movies[mq].file);
                            if (client_name_eq(c->movies[mq].screen, want))
                                film = c->movies[mq].file;
                        }
                        /* A screen this table does not carry is a caller's
                         * mistake, not a disc fact — say so and take the first
                         * record rather than loading a level that draws
                         * nothing at all. */
                        if (!film) {
                            Q2_WARN("movie: QFMV has no '%s' record; "
                                    "playing '%s'", want,
                                    c->movies[0].screen);
                            film = c->movies[0].file;
                        }

                        c->endmis_open     = false;
                        c->briefing_open   = false;
                        c->leveltext_ready = false;
                        if (!client_film_start(c, film))
                            Q2_WARN("movie: QFMV could not play %s", film);
                        /* Consumed. A later entry with nobody setting it is an
                         * Intro, not whatever the last one happened to be. */
                        c->film_screen[0] = '\0';
                    } else if (is_endmis && c->movie_count) {
                        u32 mq;
                        char line[Q2_BRIEFING_FIELD_MAX];
                        const char *film = NULL;

                        c->endmission = true;
                        for (mq = 0; mq < c->movie_count; mq++) {
                            Q2_INFO("movie: '%s' plays %s",
                                    c->movies[mq].screen, c->movies[mq].file);
                            /* The END of the campaign is the Extro. */
                            if (!film ||
                                client_name_eq(c->movies[mq].screen,
                                               "Extro FMV"))
                                film = c->movies[mq].file;
                        }

                        /*
                         * The unit is the map's own digit. `QENDMIS<N>` is what
                         * the level table calls `EndMission <N>` and reaching
                         * it is how unit N ends (#88), so the name IS the
                         * number — `map_unit` is a gameplay map's field and is
                         * not set here.
                         */
                        {
                            const char *nm = c->map;
                            int unit = 0;
                            size_t ln = strlen(nm);

                            if (ln && nm[ln - 1] >= '1' && nm[ln - 1] <= '9')
                                unit = nm[ln - 1] - '0';

                            q2_endmission_init(&c->endmis);
                            c->endmis_unit = unit;
                            if (unit)
                                snprintf(line, sizeof(line),
                                         "MISSION %d COMPLETE", unit);
                            else
                                snprintf(line, sizeof(line),
                                         "MISSION COMPLETE");
                        }

                        /*
                         * The film, if this map names one — and the placard
                         * only if it does not.
                         *
                         * A FALLBACK, and it did not used to be. The console
                         * does not end the campaign here at all: the outer
                         * state machine answers 5 and loads `Extro FMV`, which
                         * is QFMV, and QFMV is what plays OUTRO1P — so the
                         * campaign now goes there (see MISCOMPLETE) and reaches
                         * QENDMIS5 only if someone asks for it by name. Units
                         * 1..4 end on something their own module draws, which
                         * this port does not run; playing OUTRO1P on QENDMIS1
                         * would be a confident wrong answer, so those still get
                         * the placard.
                         */
                        if (film && c->endmis_unit == 5 &&
                            client_film_start(c, film)) {
                            c->endmis_open   = false;
                            c->briefing_open = false;
                            c->leveltext_ready = false;
                        } else {
                            char body[Q2_BRIEFING_FIELD_MAX * 2];

                            if (film)
                                snprintf(body, sizeof(body),
                                         "The sequence here is a full-motion "
                                         "video: %s. This unit's ending is "
                                         "drawn by its own LevelBin module, "
                                         "which this port reads but does not "
                                         "run.", film);
                            else
                                snprintf(body, sizeof(body),
                                         "The sequence here is drawn by this "
                                         "map's own LevelBin module, which "
                                         "this port reads but does not run.");

                            q2_endmission_set(&c->endmis, line, body);

                            c->endmis_open     = true;
                            /* Dismissing it is what continues the campaign,
                             * when the MISCOMPLETE that got here had a LOADMAP
                             * beside it to carry — see `unit_next_map`. */
                            c->endmis_await    = (c->unit_next_map[0] != '\0');
                            c->briefing_open   = false;
                            c->leveltext_ready = false;
                            q2_prompt_show(&c->prompts, Q2_PROMPT_BACK, 216);
                        }
                    }
                }

                /*
                 * And the map's mission-event table, read out of the same
                 * module the group selection comes from. Zero load base: the
                 * chunk is unrelocated here and its handler words are still
                 * the module-relative offsets the fixups would resolve.
                 */
                {
                    const dat_chunk *mlb =
                        c->common.chunk[Q2_COMMON_LEVEL_BIN];

                    c->misevent_count = 0;
                    if (mlb && mlb->data && mlb->size) {
                        u32 got = q2_levelbin_misevents(mlb->data, mlb->size, 0,
                                                        c->misevent, 32);
                        c->misevent_count = got > 32 ? 32 : got;
                        if (c->misevent_count) {
                            char list[512];
                            u32 mq;
                            size_t at = 0;

                            list[0] = '\0';
                            for (mq = 0; mq < c->misevent_count; mq++) {
                                int w = snprintf(list + at, sizeof(list) - at,
                                                 "%s%s", at ? ", " : "",
                                                 c->misevent[mq].name);
                                if (w <= 0 || (size_t)w >= sizeof(list) - at)
                                    break;
                                at += (size_t)w;
                            }
                            Q2_INFO("misevents: %u named by this map's "
                                    "LevelBin: %s", c->misevent_count, list);
                        }
                    }
                }

                if (q2_laserbeams_build(&c->lasers, &ev, &uf,
                                        &c->ev_operands,
                                        c->sim[0].coll_primary.node_count
                                            ? &c->sim[0].coll_primary
                                            : NULL))
                    Q2_INFO("lasers: %u beam%s raised%s",
                            c->lasers.count,
                            c->lasers.count == 1 ? "" : "s",
                            c->lasers.declined ? " (some declared dark)" : "");

                /*
                 * The teleport pads, registered from the SAME pass and against
                 * COMMON's Events because Event_GlobalEVOFFSET is measured
                 * against gp+372 (0x80026E84) and every TrigBounds link on the
                 * disc names a COMMON record.
                 */
                if (q2_teleports_build(&c->teleports, &ev, &uf,
                                       c->sim[0].triggers_ready
                                           ? &c->sim[0].triggers : NULL)
                    || c->teleports.unmarked)
                    Q2_INFO("teleports: %u pad%s registered, %u TELEPORT"
                            " call%s with no volume of their own",
                            c->teleports.count,
                            c->teleports.count == 1 ? "" : "s",
                            c->teleports.unmarked,
                            c->teleports.unmarked == 1 ? "" : "s");

                if (q2_movers_build(&c->movers, &ev, &c->ev_operands) == Q2_OK) {
                    u32 opcode_built = c->movers.count;

                    /* And the lifts a CALL builds rather than an opcode: same
                     * set, same tick, same draw offset (mover.h). */
                    q2_movers_build_calls(&c->movers, &ev, &uf,
                                          &c->ev_operands, &c->zone.scene);

                    c->movers_ready = true;
                    c->zone.movers  = &c->movers;
                    Q2_INFO("movers: %u doors and lifts (%u from MOVER opcodes,"
                            " %u from LIFT1 calls)",
                            c->movers.count, opcode_built,
                            c->movers.count - opcode_built);

                    /*
                     * And make them SOLID. Until this call the mover's
                     * displacement reached the renderer and nothing else, so a
                     * closed door was a picture of a door.
                     *
                     * After q2_sim_attach_gameplay, which is what builds the
                     * volume half of the target array this appends to.
                     */
                    if (q2_sim_attach_movers(&c->sim[0], &c->movers,
                                             &c->zone.scene) == Q2_OK &&
                        c->sim[0].mover_count)
                        Q2_INFO("movers: %u part boxes in the collision world",
                                c->sim[0].mover_count);

                    /*
                     * Every box the movement sweep can hit, because "there is
                     * an invisible clip here" is a claim that one of them is
                     * somewhere it should not be, and nothing listed them.
                     *
                     * The live box and the swept ENVELOPE are printed side by
                     * side: the envelope only ever grows and covers a lift's
                     * whole travel, so a clip the size of a shaft is what
                     * confusing the two would look like (trace.h).
                     */
                    if (c->zone_trace && c->sim[0].volumes) {
                        u32 t, mi, p, out = 0;

                        for (mi = 0; mi < c->movers.count; mi++) {
                            const q2_mover *m = &c->movers.movers[mi];
                            for (p = 0; p < m->part_count &&
                                        out < c->sim[0].mover_count;
                                 p++, out++) {
                                const q2_move_target *mt =
                                    &c->sim[0].volumes[out];
                                Q2_INFO("[clip] %2u  mover %2u part %u"
                                        " node %d axis %u target %d speed %d"
                                        "  box (%d..%d, %d..%d, %d..%d)",
                                        out, mi, p, (int)m->node[p], m->axis,
                                        (int)m->target, (int)m->speed,
                                        mt->min[0], mt->max[0],
                                        mt->min[1], mt->max[1],
                                        mt->min[2], mt->max[2]);
                            }
                        }
                        /* `volume_count` is the TOTAL: mover parts first, then
                         * authored volumes (sim.h). Treating it as the latter
                         * count walked `mover_count` entries past the allocation
                         * whenever --zone-trace was enabled and printed heap
                         * bytes as bogus inactive clip boxes. */
                        for (t = c->sim[0].mover_count;
                             t < c->sim[0].volume_count; t++) {
                            const q2_move_target *mt = &c->sim[0].volumes[t];
                            Q2_INFO("[clip] %2u  volume mask %04X %s"
                                    "  box (%d..%d, %d..%d, %d..%d)",
                                    t, mt->mask, mt->active ? "on " : "off",
                                    mt->min[0], mt->max[0],
                                    mt->min[1], mt->max[1],
                                    mt->min[2], mt->max[2]);
                        }
                    }
                }

                if (q2_rotators_build(&c->rotators, &ev, &uf,
                                      &c->zone.scene) == Q2_OK) {
                    c->rotators_ready = true;
                    c->zone.rotators  = &c->rotators;
                    Q2_INFO("rotators: %u (operands from %s)",
                            c->rotators.count,
                            have_zev ? "the zone's Events" : "COMMON only");
                }
                if (q2_sim_attach_scene_solids(&c->sim[0], &c->zone.scene,
                                                &c->ev_operands,
                                                &c->rotators) != Q2_OK) {
                    Q2_ERROR("could not build scene collision boxes");
                    return false;
                }
                Q2_INFO("scene solids: %u hatch, explosive and script boxes",
                        c->sim[0].scene_solid_count);
            }
        }

        /*
         * And the other half of a rotator: the step request. Building the set
         * only says which nodes CAN turn — every kind sits still until a script
         * CALL asks for a step (rotator.c, 0x8002F1B8), which is why the set
         * built last round reported `rot moved 0` on every map.
         */
        c->sim[0].event_rt.on_call       = client_event_call;
        c->sim[0].event_rt.on_call_user  = c;
        c->sim[0].event_rt.timer_period  = q2_event_timer_period;
        c->sim[0].event_rt.on_mover      = client_event_mover;
        c->sim[0].event_rt.on_mover_user = c;
        c->sim[0].event_rt.on_explosive      = client_event_explosive;
        c->sim[0].event_rt.on_explosive_user = c;

        /*
         * THE LOAD-TIME SCRIPT PASS — the named event "STARTLEV", which the
         * zone loader runs at EVERY level and zone load and which this port
         * never ran at all.
         *
         * 0x8007C290 `addiu t0,a0,-10920` materialises the twelve bytes at
         * 0x800AD558 — `53 54 41 52 54 4C 45 56 00 00 00 00`, "STARTLEV" — and
         * 0x8007C324 `jal 0x80027CC4` runs it, inside the zone loader
         * 0x8007B3F8 that 0x800794D8 calls on every load. 0x80027CC4 is the
         * engine's own named-event runner and a transitive drain: it seeds the
         * queue at 0x800C6F24 (0x80027D60..0x80027D74) and runs everything the
         * chain TRIGGERs through the usual gate and latch (0x80027DA0..
         * 0x80027DC8), which q2_event_rt_update's drain_pending reproduces.
         *
         * The pass is bracketed by two flags, `addiu v0,zero,1` stored into
         * 0x800B281C (0x8007C2A4) and 0x800B2834 (0x8007C2AC) and cleared at
         * 0x8007C330/0x8007C338. The second is `initial_pass`, and every hook
         * that must go quiet during a rebuild already honours it.
         *
         * IT IS NOT GATED ON THE CARRY and it comes BEFORE the replay:
         * 0x80079114 calls the loader, and only then does 0x80079120..
         * 0x80079130 test 0x800AEBCC and call the replay. So a spent ENABLE
         * the replay re-dispatches correctly re-opens what STARTLEV has just
         * closed, and deathmatch — which skips the replay — still gets the
         * pass. Skipping it on the carry path would put all 22 records back on
         * their feet at every zone seam: q2_event_rt_init reseeds the flags
         * from disc and the carry only restores records whose bits are
         * (DISABLED|HASRUN) both set, which a STARTLEV disable is not.
         *
         * WHAT IT DOES ON THIS DISC, walked from each COMMON script's STARTLEV
         * directory entry: 18 of the 49 scripts have one, and their TRIGGER
         * closures DISABLE 22 records on eight maps — LAB alone shuts twelve,
         * eleven of them named by trigger volumes the player walks through, so
         * five doors, two button-and-lift pairs, a CREBATCH ambush and a
         * message were all live from frame 0. The other half of the pass is
         * the raising: 13 TIMEDLIGHT, 8 TIMER, 2 SIMROT2, 2 LASERWALL, 1
         * LIFT1, 1 PLATFORM, 1 MISEVENT and 72 LASERBEAM. Not one MOVER_A/B/C
         * and not one ZONEGATE in any of the eighteen, so the pass cannot move
         * a door or ask for a zone while the level is still being built.
         *
         * The 72 LASERBEAM and 2 LASERWALL calls are no-ops in
         * client_event_call, which implements neither: the beams are already
         * raised by q2_laserbeams_build, which is where the console's own
         * registration pass raises them (levelbin.h), and nothing here can
         * double-register one or deal LASERWALL damage at load.
         */
        c->sim[0].event_rt.initial_pass = true;          /* 0x8007C2AC */
        if (q2_event_rt_trigger_named(&c->sim[0].event_rt, "STARTLEV")) {
            const q2_event_rt *rt = &c->sim[0].event_rt;
            u32 before = rt->ran_count, dead = 0, mi;

            q2_event_rt_update(&c->sim[0].event_rt);     /* 0x8007C324 */

            for (mi = 0; rt->flags && mi < rt->record_count; mi++)
                if (rt->flags[mi] & Q2_EVREC_DISABLED)
                    dead++;
            Q2_INFO("STARTLEV: %u record%s run at load, %u of %u now "
                    "disabled (the pass's own one-shots included)",
                    rt->ran_count - before,
                    rt->ran_count - before == 1 ? "" : "s",
                    dead, rt->record_count);
        }
        c->sim[0].event_rt.initial_pass = false;         /* 0x8007C338 */

        /*
         * THE EVE_ REPLAY — for a gate inside one map AND for a map walked
         * back into, because the block is named after the level and lives
         * in save memory (events_rt.h,
         * q2_event_rt_replay): every spent record and spent one-shot item the
         * carry kept is dispatched again, so a door, a lift or a hidden node
         * the script had changed comes back as the script left it rather than
         * rebuilt at rest. Without it the carry alone would be worse than no
         * carry: the record stays spent and its door stays shut for good.
         * 0x80079128 runs it only outside deathmatch (`bne` on 0x800AEBCC),
         * after the zone load (0x80079114) and before the player moves.
         *
         * THEN THE SEVEN PASSES, 0x800296D4..0x8002974C: each stores 30000 in
         * the dt global 0x800B2DB4 and calls the think at +44 of each of the
         * 48 runtime objects at 0x800D6BB0 — here the movers and the rotators
         * — so a replayed door ARRIVES open rather than starting to. The
         * port's arithmetic survives dt = 30000 and gives the console's
         * answer. The delay and wait arms are `lhu`/`subu`/`sll 16`/`blez`
         * there (0x800258C8..0x800258E0, 0x80025978..0x8002599C) and
         * `(s16)(timer - dt) > 0` in mover.c; the travel is a 32-bit `mult` by
         * the `lw` dt (0x80025AA4..0x80025AB8), clamped to the target, in
         * both; the rotators read dt with `lw` (0x8002B468, 0x8002C058,
         * 0x8002F1C8) as rotator.c takes it. One state step per pass, as on
         * the console, so a door with wait 0xFF is open after the seven and
         * one whose wait is under 30000 ticks has opened and shut again inside
         * them. The pusher each mover think calls (0x80051EC0) is
         * q2_sim_movers_update, once a pass; nothing is swept against the
         * player, who is placed below. The movers' start sounds stay pending
         * and play on the first tick — 0x80025A5C does not read the pass flag.
         *
         * NOT REPRODUCED: each pass also adds 30000 to the level clock
         * 0x800AEBAC (0x800296F8/0x80029700). The carried clock is put back
         * below as it stood, because what of that advance survives into the
         * next frame is not traced: 0x800183D4..0x8001841C reloads the clock
         * from 0x800B29D4 while 0x800B2E0C is up, and the zone loader sets
         * that word at 0x8007B588.
         */
        if (c->carry_events.size && !c->mp_enabled) {
            const q2_event_rt *rt = &c->sim[0].event_rt;
            const u16 keys = (u16)(c->carry_inv.flags & 0x0FFFu);
            const u8  spent_bits = (u8)(Q2_EVREC_DISABLED | Q2_EVREC_HASRUN);
            u32 moved = 0, spent = 0, mi;
            int k;

            q2_event_rt_replay(&c->sim[0].event_rt, &c->carry_events);
            for (k = 0; k < 7; k++) {
                if (c->movers_ready) {
                    q2_movers_tick(&c->movers, 30000, keys);
                    q2_sim_movers_update(&c->sim[0], &c->movers);
                }
                if (c->rotators_ready)
                    q2_rotators_tick(&c->rotators, 30000);
            }

            /* What 0x80029078 would write a bit for: (flags & 0x81) == 0x81. */
            for (mi = 0; rt->flags && mi < rt->record_count; mi++)
                if ((rt->flags[mi] & spent_bits) == spent_bits)
                    spent++;

            for (mi = 0; c->movers_ready && mi < c->movers.count; mi++) {
                const q2_mover *m = &c->movers.movers[mi];

                if (m->offset == 0)
                    continue;
                moved++;
                if (c->zone_trace)
                    Q2_INFO("[zone]        replayed mover %u (item +0x%x, "
                            "%u part%s): state %u, offset %d of %d",
                            mi, m->item_offset, m->part_count,
                            m->part_count == 1 ? "" : "s", m->state,
                            m->offset, m->target);
            }
            Q2_INFO("arrival: %u spent records carried, %u script items "
                    "replayed; %u doors and lifts stand displaced after the "
                    "seven passes", spent, rt->replayed_count, moved);
        }
        c->carry_events.size = 0;
        q2_sim_scene_solids_update(&c->sim[0], &c->rotators);

        q2_sim_spawn(&c->sim[0], feet, c->cam.yaw);
        c->sim[0].player[0].ground_y = feet[1];

        /*
         * SETTLE IS FOR A START POSITION, NOT FOR A DOORWAY.
         *
         * A StartPos is an authored mark rather than a standing position, so a
         * fresh spawn drops onto the floor before the first frame rather than
         * showing the fall (sim.c). A player crossing a zone gate is ALREADY
         * standing — settling them would search downward for a floor and, if
         * they crossed at a jump or over a drop, plant them somewhere they were
         * not. The whole point of the carry is that nothing moves.
         */
        if (!c->carry_pos_valid)
            q2_sim_settle(&c->sim[0]);
        c->sim[0].combat.self.owner  = 0;

        /*
         * The rest of what a carried player was doing. `q2_sim_spawn` takes a
         * position and a yaw and resets everything else, so the pitch, the
         * motion and the footing are restored on top of it — walking through a
         * doorway must not straighten your neck or stop you dead.
         */
        if (c->carry_pos_valid) {
            q2_player *pl = &c->sim[0].player[0];
            q2_move_ent destination_ent = pl->ent;

            /* The player object itself survives a retail zone stream. Restore
             * it wholesale, then keep the one field family that genuinely was
             * rebuilt: the cached cell/contact state of the destination hull. */
            *pl     = c->carry_motion;
            pl->ent = destination_ent;

            c->sim[0].combat.next_fire = c->carry_next_fire;
            c->sim[0].combat.kick[0]   = c->carry_fire_kick[0];
            c->sim[0].combat.kick[1]   = c->carry_fire_kick[1];
            c->sim[0].combat.kick[2]   = c->carry_fire_kick[2];

            c->carry_pos_valid = false;          /* one-shot, like the rest */

            {
                s32 origin[3];
                s32 cell;

                origin[0] = pl->pos[0];
                origin[1] = q2_sim_origin_y(pl->pos[1]);
                origin[2] = pl->pos[2];
                cell = q2_coll_find_node(&c->sim[0].coll, origin, -1, true);

                if (c->zone_trace)
                    Q2_INFO("[zone]        arrived in zone %d at (%d,%d,%d),"
                            " cell %d%s", index,
                            pl->pos[0], pl->pos[1], pl->pos[2], (int)cell,
                            cell < 0 ? "   *** NO CELL HOLDS THIS POINT ***"
                                     : "");

                /*
                 * A NET UNDER THE CARRY, and one that should never take weight.
                 *
                 * Every gate on the disc is a doorway a player walks through,
                 * so the position they walk in with is by construction inside
                 * the zone they walk into — measured, 100 of 100. But a gate is
                 * an event, and an event can in principle be raised by a script
                 * rather than by the volume, from anywhere on the map. Carried
                 * blind, that leaves the player in a coordinate no cell holds:
                 * no floor, no collision, nothing drawn, and no way out.
                 *
                 * So the arrival is checked, and only a FAILED one is placed —
                 * loudly, because reaching this means a gate fired somewhere it
                 * has no doorway and that is worth knowing about on its own.
                 */
                if (cell < 0) {
                    q2_start_pos_list rescue;

                    Q2_WARN("zone gate carried the player to (%d,%d,%d), which"
                            " no cell of zone %d holds — the gate fired away"
                            " from its doorway",
                            pl->pos[0], pl->pos[1], pl->pos[2], index);

                    if (q2_start_pos_parse(&rescue, &c->common) == Q2_OK) {
                        u32 k;

                        for (k = 0; k < rescue.count; k++) {
                            q2_start_pos sp;
                            s32 to[3];

                            if (!q2_start_pos_get(&rescue, k, &sp))
                                continue;
                            if (sp.zone != index)
                                continue;

                            to[0] = sp.x; to[1] = sp.y; to[2] = sp.z;
                            q2_sim_spawn(&c->sim[0], to, sp.angle);
                            q2_sim_settle(&c->sim[0]);
                            c->cam.pos[0] = sp.x;
                            c->cam.pos[1] = sp.y;
                            c->cam.pos[2] = sp.z;
                            c->cam.yaw    = sp.angle;
                            c->move_reason = "zone gate rescue (no cell)";
                            Q2_WARN("...placed at '%s' (%d,%d,%d) instead",
                                    sp.name, sp.x, sp.y, sp.z);
                            break;
                        }
                    }
                }
            }
        }

        /*
         * And give the player back what they walked in with. After the spawn,
         * because `q2_sim_spawn` is a level start and resets the loadout — the
         * whole point of a transition is that this one is not.
         */
        if (c->carry_player) {
            c->carry_player = false;

            c->sim[0].combat.inv              = c->carry_inv;
            c->sim[0].combat.weapon_id        = c->carry_weapon_id;
            c->sim[0].combat.chaingun_bullets = c->carry_chaingun;

            if (c->carry_same_map) {
                /* One level, one clock: the deadlines stay absolute. */
                c->sim[0].level_time = c->carry_level_time;
            } else {
                /* A new level restarts the clock, so a deadline expressed
                 * against the old one has to be re-expressed against this one
                 * or a quad picked up at 4,000 ticks would run for another
                 * 4,000 after the door. Remaining time is what carries. */
                q2_inventory *inv = &c->sim[0].combat.inv;
                s32 *deadline[5];
                int  k;

                deadline[0] = &inv->quad_until;
                deadline[1] = &inv->invuln_until;
                deadline[2] = &inv->enviro_until;
                deadline[3] = &inv->breather_until;
                deadline[4] = &inv->mega_health_next;

                for (k = 0; k < 5; k++) {
                    s32 left = *deadline[k] - c->carry_level_time;
                    *deadline[k] = (*deadline[k] && left > 0) ? left : 0;
                }
                inv->item_name_until = 0;
            }

            Q2_INFO("carried through: %d hp, %d armour, weapon %d, weapons %04X",
                    c->sim[0].combat.inv.health, c->sim[0].combat.inv.armour,
                    c->sim[0].combat.weapon_id, c->sim[0].combat.inv.weapons);
        } else if (c->launch_loadout_valid && !c->mp_enabled &&
                   client_name_eq(c->launch_loadout_map, map)) {
            /* Nothing carried in, but this is the level the session was
             * launched into: arrive with what the campaign would have given,
             * holding the gun the console's own preference list picks. */
            c->sim[0].combat.inv = c->launch_loadout;
            q2_sim_autoselect_weapon(&c->sim[0]);
            Q2_INFO("launch loadout: %d hp, %d armour, weapon %d, weapons %04X",
                    c->sim[0].combat.inv.health, c->sim[0].combat.inv.armour,
                    c->sim[0].combat.weapon_id, c->sim[0].combat.inv.weapons);
        }

        /*
         * ONE-SHOT, like `carry_player`, and for the same reason now that it
         * decides POSITION as well as the clock. Left standing it would make
         * the next load that does not set it explicitly keep a position from
         * the wrong level. `client_change_map` already clears it on the way in,
         * so this only closes the paths that do not.
         */
        c->carry_same_map = false;

        /* Extra players share this world's items, script and clock. Each
         * starts at its own MultiSpawn with a separate inventory and pad. */
        if (c->mp_enabled) {
            int pi;

            for (pi = 1; pi < c->mp.player_count &&
                         pi < Q2_MP_MAX_PLAYERS; pi++) {
                s32 pfeet[3];

                if (!c->mp_view_valid[pi])
                    continue;

                pfeet[0] = c->mp_view_pos[pi][0];
                pfeet[1] = c->mp_view_pos[pi][1];
                pfeet[2] = c->mp_view_pos[pi][2];

                /*
                 * Into player 0's sim, as player `pi` — one world, four
                 * players. Each used to get a q2_sim of its own, which meant
                 * four copies of the map's items and four scripts, and only
                 * player 0's was ever read or drawn. Now they share the world
                 * they are standing in, which is what lets them collect the
                 * same pickup and see the same doors.
                 */
                {
                    /*
                     * Through the sim's own spawn, not by copying player 0.
                     * Copying carried player 0's collision node across, and a
                     * node is where you ARE — so a player placed somewhere else
                     * with someone else's node fell out of the world. Two of
                     * four ended a capture at y 64847.
                     */
                    int saved = c->sim[0].cur_player;
                    q2_inventory start = c->sim[0].combat.inv;
                    q2_sim_select_player(&c->sim[0], pi);
                    c->sim[0].combat.inv = start;
                    q2_sim_spawn(&c->sim[0], pfeet, c->mp_view_yaw[pi]);
                    c->sim[0].player[pi].ground_y = pfeet[1];
                    q2_sim_select_player(&c->sim[0], saved);
                }
                q2_sim_player_reset_combat(&c->sim[0], pi);
                c->sim[0].pcombat[pi].self.owner = (s8)pi;
                c->sim[0].player_count = pi + 1;
                c->sim_ready[pi] = true;
                client_reset_view_model(c, pi);
            }
        }

        /* Last, because it wakes the AI onto the player and therefore needs
         * the player to already be standing somewhere. */
        {
            s32 eye[3];
            q2_sim_eye(&c->sim[0], eye);
            client_load_creatures(c, eye);
        }
    }

    /*
     * EVERYBODY IS ALIVE AGAIN, and this is what a life starts with.
     *
     * 0x8003B250 is the console's player spawn: a new entity at 100 health
     * with entity+222 at the "not a player" sentinel and the Stand move
     * installed. A zone load is that for all four, and a respawn hands the
     * loadout captured here back out.
     */
    if (!same_map_transition) {
        int pi;

        for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++)
            q2_player_death_init(&c->death[pi]);
        c->death_abandon   = 0;
        c->mp_start_inv    = c->sim[0].combat.inv;
        c->mp_start_weapon = c->sim[0].combat.weapon_id;
        c->mp_start_valid  = true;

        /*
         * And each player's carousel, from the loadout they now hold:
         * 0x8003B250 ends in the spawn loadout 0x8003D4FC, whose tail writes
         * the pair (0x8003D610/0x8003D614). Not at a gate inside one map, where
         * this block does not run either: the player object survives a retail
         * zone stream (the carry above), and client+96/+100 are in it. Whether
         * the console's level change restores the pair with the rest of the
         * carried record is not traced; the write here, with the carried
         * inventory already in place, is INFERRED.
         */
        client_sbar_write_slots(c, 0, CLIENT_SLOTS_SPAWN);
        for (pi = 1; c->mp_enabled && pi < Q2_MP_MAX_PLAYERS; pi++)
            if (c->sim_ready[pi])
                client_sbar_write_slots(c, pi, CLIENT_SLOTS_SPAWN);
    }

    Q2_INFO("%s: %u nodes, %u vertices",
            c->zone.name, c->zone.scene.node_count, c->zone.points.count);

    /*
     * And this level's music. Here rather than in `main()` because a session
     * loads many zones and only the first one used to be asked what it sounded
     * like. A gate between zones of the same map resolves to the same level
     * record and does NOT restart the playlist — but it does not leave the
     * stream alone either; see client_music_for_level.
     */
    client_music_for_level(c, false, same_map_transition);

    /* `client_input_simulated` normally publishes the eye and composed view
     * after a player tick. A gate is processed later in that same frame, so a
     * load that leaves `cam.pos` at the carried FEET renders one transition
     * frame Q2_EYE_BASE units too low before the next tick repairs it. Publish
     * the rebuilt player's current eye immediately; no synthetic tick and no
     * one-frame camera drop. */
    {
        s32 eye[3], view[3];

        q2_sim_eye(&c->sim[0], eye);
        q2_sim_view_angles(&c->sim[0], view);
        c->cam.pos[0] = eye[0];
        c->cam.pos[1] = eye[1];
        c->cam.pos[2] = eye[2];
        c->cam.pitch  = view[0];
        c->cam.yaw    = view[1];
        c->cam.roll   = view[2];
    }

    c->loads_done++;
    return true;
}

/* ------------------------------------------------------------------------- */
/*
 * Music.
 *
 * The XA streams run at 37800 Hz, which no sound card wants. Rather than write
 * a resampler, hand SDL an audio stream declared as 37800 Hz stereo and let it
 * convert — the conversion is not part of the console's character, so there is
 * nothing to be gained by reproducing it by hand.
 *
 * Sectors are decoded on demand rather than up front: one .XAI channel is
 * several minutes of audio, and the original streamed it off the disc for
 * exactly the same reason.
 */
static bool client_music_start(client *c, char letter, u8 channel)
{
    if (q2_xa_track_open(&c->music, c->disc, letter, channel) != Q2_OK) {
        Q2_WARN("no music track QUAKE_%c channel %u", letter, channel);
        return false;
    }

    q2_xa_decoder_reset(&c->music_dec);
    c->music_cursor = 0;
    c->music_open   = true;
    c->music_tracks++;

    Q2_INFO("music: QUAKE_%c channel %u", letter, channel);
    return true;
}

/*
 * Play a track id, which is the unit the game thinks in.
 *
 * An id below 2 is silence — the table's `file` is negative there and the
 * player's own `bltz` at 0x80071778 takes a different arm entirely — so this
 * stops rather than pretending.
 */
/* The console's own numbers: the fallback duration and the fade's width. */
#define Q2_MUSIC_FALLBACK_TENTHS 300   /* 0x80071878, 30.0 s   */
#define Q2_MUSIC_FADE_TICKS       64   /* 0x8007195C / 0x8007198C */

static bool client_music_play_id(client *c, int id)
{
    const q2_music_entry *e = q2_music_get(&c->music_table, id);

    if (!c->music_table_ready || !e || e->file < 0 ||
        e->file >= Q2_MUSIC_FILES) {
        c->music_open = false;
        return false;
    }

    /*
     * THE DURATION IS A COUNTDOWN, and the engine acts on it.
     *
     * `0x80071898` reads the table's `tenths`, `0x800718C0` multiplies it by
     * five into `0x800B2710` and `0x800718C8` copies the same value to
     * `0x800B2708` — tenths -> 50 Hz ticks, with a 300-tenths (30.0 s) fallback
     * at `0x80071878` for an entry that names none. A is what remains, B is the
     * total, and neither is a length the stream has to agree with: at
     * `0x80071A58` a countdown of zero ADVANCES THE PLAYLIST CURSOR, and an end
     * of list restarts it at `0x80071B6C`.
     *
     * That is what #14 was asking. The engine does not loop a track; it plays
     * each for its table duration and moves on, which is why the one entry that
     * measures 1.0 s LONGER than its table value is not an error — the last
     * second is simply never heard.
     *
     * The five is PAL's. The NTSC build multiplies by SIX at the same place
     * (0x800719E4 in SLUS-00757) and counts the result at 60 Hz, so a track
     * lasts the same seconds and its 64-tick fades are a fifth quicker.
     */
    c->music_id     = id;
    c->music_total  = (e->tenths ? e->tenths : Q2_MUSIC_FALLBACK_TENTHS) *
                      q2_video_fields_per_tenth(c->build.video);
    c->music_left   = c->music_total;
    c->music_clock  = 0.0;

    return client_music_start(c, q2_music_files[e->file][6],
                              (u8)e->channel);
}

/*
 * THE MUSIC FOR THE LEVEL THAT IS NOW LOADED.
 *
 * This used to be a straight-line block in `main()`, run once, just after the
 * boot map was loaded — so the map the session STARTED on chose the music for
 * the whole session. Walking Strogg Outpost to Boss2 played BASE1's playlist
 * over all eleven levels, because nothing on the LOADMAP path ever looked at
 * the level record again. The track was being selected correctly and then never
 * re-selected.
 *
 * `0x8007C584` is the name lookup that resolves a level record, and the
 * console's single call to the playlist-start entry point hangs off it, so this
 * belongs at the end of a successful zone load and nowhere else.
 *
 * A ZONE GATE IS NOT A LEVEL CHANGE. Both resolve to the same level record, so
 * the record is compared and an unchanged one does not go back to entry 0 of
 * the playlist. `force` is for the one caller that needs the restart anyway:
 * coming back off a film, where the drive was taken away.
 *
 * BUT AN UNCHANGED RECORD IS NOT "LEAVE THE STREAM ALONE" EITHER, which is what
 * this used to say and what it did. ReloadLevelData — the zone load — is
 * branchless from 0x800793D4 to its tail, and 0x800794F8 writes ZERO to the
 * countdown at 0x800B2710 on the way out while 0x800794F0 puts the playing flag
 * back up. Nothing on that path touches the playlist cursor at 0x800AEC00: the
 * only writer that resets the cursor is 0x80071B34, whose one caller is
 * LoadLevelData at 0x800796C8, i.e. the whole-level load. So the next
 * CdCheckMusic falls through the `bgtz` at 0x80071A60 with the countdown at
 * zero, steps the cursor at 0x80071A70 and starts the NEXT entry from its first
 * sector with a fresh duration and the 64-field fade-in.
 *
 * Measured before: BASE1 played entry 0 (id 8, QUAKE_B channel 2) across both
 * of its zones and for the whole 154.8 s that entry lasts, so a seven-track
 * playlist was heard at roughly a seventh of the rate the console works
 * through it.
 */
static bool client_music_play_id(client *c, int id);

static void client_music_for_level(client *c, bool force, bool zone_gate)
{
    const q2_level_entry *lv;

    if (!c->level_table_ready || !c->music_table_ready)
        return;

    /*
     * HELD, which is not the same as "this level has no playlist".
     *
     * See `music_held`: the boot chain's QFRONT load happens before the logo
     * screens rather than after them, and its playlist must not start until the
     * front end it is standing on is actually showing. `c->level` is left alone
     * so the forced call that lifts the hold still sees a change.
     */
    if (c->music_held) {
        c->music_open = false;
        return;
    }

    lv = q2_level_find(&c->level_table, c->map);
    if (lv == c->level && !force) {
        /*
         * Same level record. A ZONE GATE is ReloadLevelData, and 0x800794F8 is
         * the countdown to zero and nothing else — the advance itself belongs
         * to the next CdCheckMusic, which here is the frame loop's countdown
         * block, so the cursor steps there exactly as it does when a track runs
         * its duration out.
         *
         * Everything ELSE that resolves the same record — a restart, a death,
         * an arena round reset, a memory-card restore — is left playing as it
         * was. Which of the console's two level loads those run is not traced
         * (ReloadLevelData has one caller, LoadZone at 0x80079114, and
         * LoadLevelData has one, 0x80070D44), and the difference decides
         * whether the playlist advances or goes back to entry 0. Guessing
         * would be inventing a behaviour, so they keep what they had.
         */
        if (zone_gate)
            c->music_left = 0;
        return;
    }

    c->level           = lv;
    c->music_cursor_at = -1;

    if (!lv) {
        /*
         * Not a missing playlist — a map the LEVEL TABLE does not name. Four
         * directories on the disc are in that position (FRAGTOWE, QSTARTUP,
         * QINTER, QMAGINTR), and the retail game never reaches them through
         * the table, so they have no playlist to have.
         */
        Q2_INFO("%s is not in the level table, so it has no playlist", c->map);
        c->music_open = false;
        return;
    }

    {
        int id = q2_level_playlist_next(lv, &c->music_cursor_at);

        if (id < 0 || !client_music_play_id(c, id))
            c->music_open = false;
        Q2_INFO("music: %s plays %d first", c->map, id);
    }
}

/*
 * The next track in this level's playlist, by the engine's own walk. Called
 * when a stream runs out, which is the only thing that advances it.
 */
static void client_music_advance(client *c)
{
    int id;

    if (!c->level || !c->music_table_ready) {
        /* No playlist: loop what is playing, which is what the port did before
         * any of this was decoded. */
        c->music_cursor = 0;
        q2_xa_decoder_reset(&c->music_dec);
        return;
    }

    id = q2_level_playlist_next(c->level, &c->music_cursor_at);
    if (id < 0) {
        /* A list that ends rather than looping: restart it. */
        c->music_cursor_at = -1;
        id = q2_level_playlist_next(c->level, &c->music_cursor_at);
    }

    if (id < 0 || !client_music_play_id(c, id)) {
        c->music_cursor = 0;
        q2_xa_decoder_reset(&c->music_dec);
    }
}

/*
 * `Paused` — 0x800AE8B4, and GameCheckPaused's own arms are the only things
 * that raise it. The DEATH page is not one of them: page 41 is built by
 * 0x8001D774 over a level that is still running, and it writes neither Paused
 * nor the CD volume target, so it is excluded here by name rather than falling
 * out of `menu.open`. The front end is QFRONT with no level under it and
 * GameCheckPaused never runs there at all.
 */
static bool client_music_paused(const client *c)
{
    /* And the SAVE? prompt, which is the third `sh zero, 0x800B2E50` above:
     * 0x8003E750 writes the target in the same breath as `sw 1, Paused`. */
    if (c->save_prompt_open)
        return true;

    return c->menu.open && !c->in_front_end &&
           c->menu.page_id != Q2_PAGE_DEATH;
}

/*
 * `targetcdvol` (0x800B2E50) — where the CD volume is being ASKED to go.
 *
 * Thirteen instructions in the executable write it, and this reproduces the
 * state they leave between them, because each of them is a level rule said as
 * an edge. `xrefs 0x800B2E50` lists all thirteen:
 *
 *   0 on a pause raise      GameCheckPaused 0x8003E888 (the START press, which
 *                           goes on to InGame_GOTO_PAUSED), 0x8003E854 (a pad
 *                           pulled out) and 0x8003E750 (the level-change SAVE?
 *                           prompt) each execute `sh zero, 0x800B2E50` in the
 *                           same breath as `sw 1, Paused`.
 *   MUSIC*2 on the way down InGame_GOTO_RESUME 0x800201AC and
 *                           InGame_GOTO_MISSION 0x800203A8, both of which
 *                           clear Paused in the same block, and 0x8001F134.
 *   MUSIC*2 after a load    MainGameLoop 0x80018430, off the `JustLoaded` flag
 *                           at 0x800B2E0C and only on the not-paused arm.
 *   MUSIC*2 at boot         FrontEndResetSoundDefaults 0x8001FA74, seeding
 *                           96 — the default slider of 48, doubled.
 *   the SOUND page's rule   CHECK_FOR_SLIDER (0x8001BF1C), installed as page
 *                           29's per-frame hook at 0x80020268. It records in
 *                           s7 the row whose slider pointer is &musicvol
 *                           (0x800B3348, the compare at 0x8001C1EC), leaves s7
 *                           at -1 when the page carries no such row
 *                           (0x8001BF2C) and then writes nothing at all
 *                           (0x8001C220); otherwise MUSIC*2 while that row is
 *                           the cursor row (0x8001C24C) and ZERO when it is
 *                           not (0x8001C25C). That is an AUDITION: on
 *                           OPTIONS -> SOUND the track plays only while the
 *                           cursor stands on MUSIC, so what you are setting is
 *                           what you can hear, and SOUND FX, STEREO and RESET
 *                           TO DEFAULTS are silent rows.
 *   and on the way out      InGame_GOTO_OPTIONS2 0x800205C0 repeats the pause
 *                           choice as TRIANGLE leaves that page: 0 while
 *                           Paused is still set (0x800205DC), MUSIC*2 when it
 *                           is not (0x800205FC).
 *
 * So the SOUND page decides while it is up, the pause decides otherwise, and
 * MUSIC*2 is everything else. The one frame the two readings differ is the
 * press of RESET TO DEFAULTS on the SOUND page, where 0x8001FA74 writes MUSIC*2
 * and the page hook writes 0 straight back over it; one frame moves the live
 * value by four, so this takes the hook's answer and says so.
 */
static s16 client_music_target(const client *c)
{
    s16 full = (s16)(c->settings.v[Q2_SET_MUSIC] * 2);

    /*
     * The EXECUTABLE's SOUND page. Not QFRONT's: the front end is a separate
     * image whose pages are built by its own module code, and whether that
     * module installs the same audition hook is NOT traced. This port shares
     * one page table between the two, so the front end keeps the plain slider
     * rather than an untraced behaviour borrowed from the in-game page.
     */
    if (c->menu.open && !c->in_front_end) {
        int audition = q2_menu_music_audition(&c->menu, full);

        if (audition >= 0)
            return (s16)audition;
    }

    return client_music_paused(c) ? 0 : full;
}

/*
 * CdCheckMusic's first act: walk the live value at 0x800B2E74 toward the target
 * at 0x800B2E50 by four (0x800718F8..0x8007194C). The step itself is
 * `q2_xa_cd_volume_step`, in xa.c beside the stream it governs.
 *
 * The console's gate on all of this is `UseCdMusic` (0x800AE8D4, tested at
 * 0x800718F0 BEFORE the ramp, so a build with the CD off never moves the live
 * value at all). This port has no equivalent — a disc that carries the XA
 * streams always plays them — so there is nothing here to reproduce it with.
 */
static void client_music_ramp(client *c)
{
    c->music_level = q2_xa_cd_volume_step(c->music_level, c->music_target);
}

/*
 * The music volume for the sector about to be handed out — the LIVE value, and
 * the fade the two duration globals exist for.
 *
 * This used to read `settings.v[Q2_SET_MUSIC] * 2` — the target — directly, and
 * so had no pause duck, no audition and no ramp: the slider doubled is what
 * 0x800205F4 and its six siblings write into 0x800B2E50, but 0x80071958 loads
 * 0x800B2E74 and it is that halfword the fades scale. See `music_level`.
 *
 * `0x80071954` then scales by `remaining / 64` while the countdown is under 64
 * ticks and `0x80071980` by `elapsed / 64` while the elapsed count is — so a
 * track fades in over its first 64 ticks and out over its last 64. At 50 Hz
 * that is 1.28 seconds each way, and it is the reason the durations are restart
 * points rather than lengths. The NTSC build keeps the 64 and ticks at 60 Hz:
 * 1.07 seconds.
 *
 * NOT reproduced, because the retail build cannot reach it: 0x800719AC scales
 * the result again by `trkvolboost` (0x800B2E6C) and clamps it to 255, but the
 * only instruction in the executable that writes that halfword is 0x8007175C,
 * `sh zero` — so the branch at 0x800719B4 is always taken and the multiplier
 * never applies.
 */
static s32 client_music_volume(const client *c)
{
    s32 vol = c->music_level;

    /* A film carries its own track and has no countdown to fade against. */
    if (!c->film_open && c->music_total > 0) {
        s32 elapsed = c->music_total - c->music_left;

        if (c->music_left < Q2_MUSIC_FADE_TICKS && c->music_left >= 0)
            vol = (vol * c->music_left) / Q2_MUSIC_FADE_TICKS;
        if (elapsed < Q2_MUSIC_FADE_TICKS && elapsed >= 0)
            vol = (vol * elapsed) / Q2_MUSIC_FADE_TICKS;
    }

    if (vol < 0)
        vol = 0;
    return vol;
}

/*
 * Decode one more sector of bed — the film's track if one is running, otherwise
 * the level's music. False means neither had anything, and the caller mixes the
 * voices into silence instead.
 */
static bool client_bed_refill(client *c)
{
    const u32 cap = (u32)(sizeof(c->bed) / sizeof(c->bed[0]));
    u32 n = 0;

    c->bed_frames = 0;
    c->bed_pos    = 0;

    if (c->film_open) {
        n = q2_movie_audio(&c->film, c->bed, cap);
    } else if (c->music_open) {
        n = q2_xa_track_read(&c->music, &c->music_dec, &c->music_cursor,
                             c->bed, cap);
        if (n == 0) {
            /*
             * End of stream: the playlist advances. The engine's walk is what
             * decides what comes next, and for every real level it eventually
             * jumps back and starts the seven again.
             *
             * This is the SECOND thing that advances it. The first is the
             * countdown in the frame loop, which is the console's own trigger;
             * a stream that runs out early still has to move on.
             *
             * One retry, so the new track starts in this call rather than a
             * frame later — and only one, so a playlist of empty tracks cannot
             * spin here.
             */
            client_music_advance(c);
            if (c->music_open)
                n = q2_xa_track_read(&c->music, &c->music_dec, &c->music_cursor,
                                     c->bed, cap);
        }
    }

    /* Less than one frame is not a bed. Guarding it here is what stops
     * `client_bed_read` spinning on a refill that succeeds and yields nothing. */
    if (n < XA_CHANNELS)
        return false;

    {
        s32 vol = client_music_volume(c);

        /*
         * AND STEREO OFF FOLDS THE MUSIC TO MONO, which this used not to do at
         * all: it scaled both channels independently and never consulted the
         * setting. 0x800719EC branches on 0x800B333E and writes volume/2 into
         * all four mix bytes when it is clear, so each output carries the sum
         * of both XA channels at half level — see `q2_xa_cd_mix`.
         *
         * This is the CD half of the setting; the SPU half already worked. The
         * same halfword is read at 0x80072C48 as a one-or-two channel count,
         * which is `client_voices_update` collapsing an effect's panning to
         * 0x80/0x80 — so turning STEREO off centred every sound effect and left
         * the music alone in full stereo.
         */
        q2_xa_cd_mix(c->bed, n, vol, c->settings.v[Q2_SET_STEREO] != 0);
    }

    c->bed_frames = n / XA_CHANNELS;
    return true;
}

/* Hand out `frames` of bed, padding with silence when nothing is playing. */
static void client_bed_read(client *c, s16 *dst, u32 frames)
{
    u32 done = 0;

    while (done < frames) {
        u32 avail = c->bed_frames - c->bed_pos;
        u32 take;

        if (avail == 0) {
            if (!client_bed_refill(c))
                break;
            continue;
        }

        take = frames - done;
        if (take > avail)
            take = avail;

        memcpy(dst + done * XA_CHANNELS,
               c->bed + c->bed_pos * XA_CHANNELS,
               (size_t)take * XA_CHANNELS * sizeof(s16));
        done       += take;
        c->bed_pos += take;
    }

    if (done < frames)
        memset(dst + done * XA_CHANNELS, 0,
               (size_t)(frames - done) * XA_CHANNELS * sizeof(s16));
}

/*
 * Sum one voice into `dst`.
 *
 * The sample's rate is 11025 or 22050 and the stream's is 37800, so the cursor
 * steps through the source by a fraction of a sample per output frame and the
 * pair either side of it are interpolated. That is not the SPU's own four-tap
 * gaussian, and it is not pretending to be: what matters here is that an effect
 * plays at its own pitch rather than at 1.7 to 3.4 times it, which is what
 * handing mono 11025 to a stereo 37800 stream did.
 *
 * Mono goes to both channels. There is no panning: the events carry a world
 * position and the port has no listener basis wired to it (openquestions #60),
 * so centring is the honest reading rather than a guessed pan.
 */
static void client_voice_mix(client_voice *v, s16 *dst, u32 frames)
{
    u32 i;

    for (i = 0; i < frames; i++) {
        u32 idx;
        s32 a, b, s, l, r;

        /* Keep two samples ahead of the cursor, so the pair to interpolate is
         * always there. Compacting first means `have` is at most 1 when the
         * next block lands, so the 2-block buffer cannot overflow. */
        while ((v->pos >> 16) + 1 >= v->have) {
            u32 shift = v->pos >> 16;
            u32 n;

            if (shift > 0) {
                memmove(v->buf, v->buf + shift,
                        (size_t)(v->have - shift) * sizeof(s16));
                v->have -= shift;
                v->pos  -= shift << 16;
            }

            n = q2_spu_voice_block(&v->dec, v->buf + v->have);
            if (n == 0) {
                v->active = false;
                return;
            }
            v->have += n;
        }

        idx = v->pos >> 16;
        a   = v->buf[idx];
        b   = v->buf[idx + 1];
        s   = a + (((b - a) * (s32)(v->pos & 0xFFFF)) >> 16);
        s   = (s * v->vol) / 127;

        /* Level and pan are the console's two per-voice bytes; both are 0..255
         * so the pair is a 16-bit scale. */
        l = dst[i * XA_CHANNELS]     + ((s * v->level * v->pan_l) >> 16);
        r = dst[i * XA_CHANNELS + 1] + ((s * v->level * v->pan_r) >> 16);
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        dst[i * XA_CHANNELS]     = (s16)l;
        dst[i * XA_CHANNELS + 1] = (s16)r;

        v->pos += v->step;
    }
}

/*
 * Feed the device: bed, plus every live voice, pushed once per chunk.
 *
 * THE TARGET IS A LATENCY, not just a stutter margin, and that is new. It used
 * to be a quarter second because nothing but music went through here and a
 * quarter second of music is simply a quarter second of lead. Now a footstep
 * waits behind whatever is queued before it is heard, so the queue is held near
 * a sixteenth of a second — about 63 ms, still two frames of headroom at 30 fps
 * and short enough that a shot does not lag the muzzle flash.
 *
 * The chunk is much smaller than a sector for the same reason: topping up a
 * sector at a time would overshoot the target by up to 53 ms every time.
 */
#define CLIENT_AUDIO_TARGET_FRAMES (XA_SAMPLE_RATE / 16)
#define CLIENT_AUDIO_CHUNK_FRAMES  256

static void client_audio_pump(client *c)
{
    s16 mix[CLIENT_AUDIO_CHUNK_FRAMES * XA_CHANNELS];
    const int target = (int)(CLIENT_AUDIO_TARGET_FRAMES * XA_CHANNELS *
                             sizeof(s16));
    const int chunk  = (int)(CLIENT_AUDIO_CHUNK_FRAMES * XA_CHANNELS *
                             sizeof(s16));
    int queued;

    if (!c->audio)
        return;

    queued = SDL_GetAudioStreamQueued(c->audio);

    while (queued < target) {
        u32 i;

        client_bed_read(c, mix, CLIENT_AUDIO_CHUNK_FRAMES);

        for (i = 0; i < CLIENT_VOICES; i++)
            if (c->voice[i].active)
                client_voice_mix(&c->voice[i], mix, CLIENT_AUDIO_CHUNK_FRAMES);

        SDL_PutAudioStreamData(c->audio, mix, chunk);
        queued += chunk;
    }
}

/* Silence every voice. Called before the sound bank is freed, because a voice
 * borrows the bank's ADPCM and would otherwise read a freed buffer. */
static void client_voices_stop(client *c)
{
    u32 i;

    for (i = 0; i < CLIENT_VOICES; i++)
        c->voice[i].active = false;

    /*
     * And drop every handle into those voices. The serial test would already
     * refuse a stale one, but a handle left pointing at a silenced slot on the
     * way into a new level is a gate that nothing can clear — the quad would be
     * suppressed until that slot happened to be reused.
     */
    c->voice_last         = NULL;
    c->voice_last_serial  = 0;
    c->quad_voice         = NULL;
    c->quad_voice_serial  = 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Simulated movement: gather the pad state, hand it to the game tick, and read
 * the camera back out of the player. The simulation runs at its own fixed rate
 * regardless of how fast we render, which is the whole point of it owning the
 * clock rather than the frame loop doing so.
 */
/*
 * The pad, when the client is driving itself.
 *
 * A demo is not a recording — nothing on the disc is being replayed — it is a
 * fixed button script, so that `--headless --demo` produces the same frames on
 * every machine and a captured frame can be compared against the last one. The
 * cycle walks, shoots, turns and jumps, because those are the four things that
 * reach the most systems: the mover and the hull trace, the weapon state
 * machine and the projectile list, the view's own kick decay, and the ground
 * projection that fall damage is measured off.
 */
#define CLIENT_DEMO_PERIOD 150

static u16 client_demo_pad(long frame)
{
    long t = frame % CLIENT_DEMO_PERIOD;
    u16  pad = 0;

    if (t >=  15 && t <  75) pad |= Q2_PAD_UP;       /* walk forward     */
    if (t >=  45 && t <  56) pad |= Q2_PAD_CROSS;    /* and shoot        */
    if (t >=  78 && t <  90) pad |= Q2_PAD_RIGHT;    /* turn             */
    if (t >=  60 && t <  75) pad |= Q2_PAD_R2;       /* strafe, for the  */
                                                     /* lean it rolls    */
    if (t >=  95 && t <  98) pad |= Q2_PAD_TRIANGLE; /* next weapon      */
    if (t >= 112 && t < 122) pad |= Q2_PAD_CROSS;    /* shoot again      */
    if (t >= 130 && t < 134) pad |= Q2_PAD_SQUARE;   /* a tap is a jump  */

    return pad;
}

/*
 * The pad this frame. The keyboard is wired to PAD BUTTONS, not to the input
 * record, and the mapping from those to the record is 0x80019154's — see pad.h.
 *
 * This is not ceremony. Three things the player feels are decided in there
 * rather than here: full deflection is 127 and not 128, so the walk speed is
 * the console's 2778 and not 2800; jump and swim-up come out of ONE button, a
 * tap for the former and a hold for the latter; and the configured style
 * decides whether the look rate is eased or set, which is the difference
 * between a view that glides and one that snaps.
 */
/*
 * ---------------------------------------------------------------------------
 * And why the keys are bound to MEANINGS rather than to buttons
 * ---------------------------------------------------------------------------
 * This used to be STANDARD A's arm of that table written out by hand — W on
 * Q2_PAD_UP, A on L2, the arrows on LEFT/RIGHT and the shoulders. That is fine
 * until the style changes, and then every key still works and none of them
 * means what it did: under RIGHT MOUSE, L2 is the PREVIOUS WEAPON, so strafing
 * left cycled the inventory.
 *
 * So the keyboard is bound through `q2_pad_style_bindings`, which is the same
 * table read backwards. One key mapping, nine styles, and selecting a style on
 * the CONTROLLER page moves what a key means instead of breaking it. Every key
 * below keeps the button it had under STANDARD A, because under STANDARD A the
 * lookup returns exactly the constants that used to be written here.
 */
static u16 client_pad_mask(const client *c, const q2_pad_bindings *b)
{
    const bool *keys;
    u16 pad = 0;

    if (c->demo)
        return client_demo_pad(c->frame_index);
    if (c->headless)
        return 0;

    keys = SDL_GetKeyboardState(NULL);
    if (!keys)
        return 0;

    if (keys[SDL_SCANCODE_W])     pad |= (u16)b->forward;
    if (keys[SDL_SCANCODE_S])     pad |= (u16)b->back;
    if (keys[SDL_SCANCODE_A])     pad |= (u16)b->strafe_left;
    if (keys[SDL_SCANCODE_D])     pad |= (u16)b->strafe_right;
    if (keys[SDL_SCANCODE_LEFT])  pad |= (u16)b->turn_left;
    if (keys[SDL_SCANCODE_RIGHT]) pad |= (u16)b->turn_right;

    /* R1 looks down and L1 up under the digital styles, and holding BOTH is the
     * chord that walks the pitch back to level — the console's own recentre,
     * which is why there is no separate key for it. Both are zero under a mouse
     * or stick style, which has no look buttons at all; the arrows drive the
     * look AXIS there instead (see client_input_simulated). */
    if (keys[SDL_SCANCODE_DOWN])  pad |= (u16)b->look_down;
    if (keys[SDL_SCANCODE_UP])    pad |= (u16)b->look_up;

    if (keys[SDL_SCANCODE_SPACE]) pad |= (u16)b->jump;    /* jump/swim */
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_F])
        pad |= (u16)b->fire;

    if (keys[SDL_SCANCODE_RIGHTBRACKET] || keys[SDL_SCANCODE_E])
        pad |= (u16)b->weapon_next;
    if (keys[SDL_SCANCODE_LEFTBRACKET] || keys[SDL_SCANCODE_Q])
        pad |= (u16)b->weapon_prev;

    /*
     * The two mouse buttons, onto the same two masks the keys use. Under the
     * mouse styles those masks ARE the mouse's buttons — 0x80019224 reads L3
     * and R3, which is where the console merges them — so MOUSE1 fires and
     * MOUSE2 jumps because that is what RIGHT MOUSE says they do, not because
     * this line chose it.
     */
    if (c->mouse_left)  pad |= (u16)b->fire;
    if (c->mouse_right) pad |= (u16)b->jump;

    return pad;
}

/*
 * One notch of the wheel, or 0: +1 up, -1 down.
 *
 * Every bit a notch feeds is tested as a press EDGE — the weapon bits are 26
 * and 27 out of the pad's shared tail, and the menu's cursor is `cur & ~prev` —
 * so a notch has to be one frame ON and one frame OFF however fast the wheel is
 * spun. That is what the gap flag is: without it a flick of the wheel is one
 * long hold and switches one weapon.
 */
static int client_wheel_notch(client *c)
{
    int n = 0;

    if (c->wheel_gap) {
        c->wheel_gap = false;
        return 0;
    }

    if (c->wheel_queue > 0)      { n =  1; c->wheel_queue--; }
    else if (c->wheel_queue < 0) { n = -1; c->wheel_queue++; }

    if (n)
        c->wheel_gap = true;
    return n;
}

/* True while a debug key that stands in for a level's own volume is held. The
 * demo never holds one, and a headless run has no keyboard to ask. */
static bool client_key_down(const client *c, SDL_Scancode a, SDL_Scancode b)
{
    const bool *keys;

    if (c->demo || c->headless)
        return false;

    keys = SDL_GetKeyboardState(NULL);
    return keys && (keys[a] || keys[b]);
}

/*
 * ---------------------------------------------------------------------------
 * MOUSELOOK — pixels of motion per unit of pad deflection
 * ---------------------------------------------------------------------------
 * The whole chain, at the default MOUSE SPEED of 64:
 *
 *     in.yaw      = (lx * (speed + 32)) >> 4          = lx * 6     (0x80019230)
 *     yaw_rate    = (in.yaw * 3) >> 2                              (0x8003A67C)
 *     yaw        += yaw_rate * dt / 10                             (0x8003A9C0)
 *
 * so feeding `lx = pixels * 10 / (DIV * dt)` — which is what the code below
 * does — turns the player by `pixels * 18 / (4 * DIV)` angle units, and the
 * circle is 4096. With DIV at 4 that is 3641 pixels for a full turn: about
 * 4.5 inches on an 800 DPI mouse, which is an ordinary sensitivity. The slider
 * spans roughly 13.6 inches at 0 to 3 inches at 127.
 *
 * The `10 / dt` is not a tuning constant, it is the compensation: the angle
 * integrates a RATE over the tick's own step, and a frame at 60 fps carries
 * half the motion into a step half as long. Without it the same physical
 * movement would turn the player half as far at 60 fps as at 30, and a
 * "sensitivity" that moves with the frame rate is the single thing that makes
 * ported mouselook feel wrong.
 */
#define CLIENT_MOUSE_DIV 4

/*
 * `--watch`'s choice of creature, shared by the aim before the tick and the
 * framing after it so the two cannot disagree.
 *
 * Normally the nearest live one with a model. With `--watch-hold N` a
 * creature that dies while framed stays framed for N more frames — long
 * enough to see what a death leaves behind, a drop landing or gibs
 * scattering, which the plain `--watch` cut away from on the frame it
 * happened. `*holding` says the creature is dead, so the caller stops
 * steering the player at it. `advance` is true at exactly one call a
 * frame, which is the one that spends the hold. A harness, not gameplay.
 */
static const q2_monster *client_watch_pick(client *c, const s32 eye[3],
                                           bool advance, bool *holding)
{
    const q2_monster *best = NULL;
    s64 best_d = 0;
    u32 i, best_i = 0;

    *holding = false;

    if (c->watch_slot && c->watch_slot <= c->creatures.set.count) {
        const q2_monster *m = &c->creatures.set.monsters[c->watch_slot - 1];

        if ((m->dead || !m->in_use) && c->watch_hold_left) {
            if (advance)
                c->watch_hold_left--;
            *holding = true;
            return m;
        }
    }

    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        s64 dx, dy, dz, d;

        if (!m->in_use || m->dead || !c->cre_model_ok[i])
            continue;

        dx = m->pos[0] - eye[0];
        dy = m->pos[1] - eye[1];
        dz = m->pos[2] - eye[2];
        d  = dx * dx + dy * dy + dz * dz;
        if (!best || d < best_d) { best = m; best_d = d; best_i = i; }
    }

    if (advance) {
        c->watch_slot      = best ? best_i + 1 : 0;
        c->watch_hold_left = c->watch_hold;
    }
    return best;
}

/* ------------------------------------------------------------------------- */
/* The DualShock — SetShockData, ProcessShockData and PadSetAct               */
/* ------------------------------------------------------------------------- */

/* MaxWindows (0x800B2C2C): one window per player in this level. */
static int client_shock_windows(const client *c)
{
    int n = c->mp_enabled ? c->mp.player_count : 1;

    return n < 1 ? 1 : (n > Q2_SIM_MAX_PLAYERS ? Q2_SIM_MAX_PLAYERS : n);
}

/*
 * SetShockData (0x80040800). `player` is a window, or Q2_SHOCK_ALL_PLAYERS
 * for every one — the train's call. `pos` NULL is a request that is not
 * positional, which is every weapon's; otherwise each window measures from
 * its own view position (GameWindows+0), which here is that player's eye.
 */
static void client_shock(client *c, int player, int id, s32 strength,
                         const s32 pos[3], s16 near_r, s16 far_r)
{
    int p, first = player, last = player;

    if (player == Q2_SHOCK_ALL_PLAYERS) {
        first = 0;
        last  = client_shock_windows(c) - 1;
    }

    for (p = first; p <= last; p++) {
        s32 ear[3];

        if (p < 0 || p >= Q2_SIM_MAX_PLAYERS)
            continue;
        c->shock_requests++;
        if (id >= 0 && id <= Q2_SHOCK_PATTERNS)
            c->shock_by_id[id]++;
        if (pos)
            q2_sim_player_eye(&c->sim[0], p, ear);
        if (q2_shock_set(&c->shock[p], id, strength, pos, pos ? ear : NULL,
                         near_r, far_r))
            c->shock_taken++;
    }
}

/*
 * The game library's own SetShockData callers, through the sink shock.h
 * keeps: a hit on a player that survived (combat.c, 0x8005859C), every
 * QuakeGenExplosion that bound its model (modelent.c, 0x8005AA7C) and the
 * Tank Commander's footstep (cre_tankcomm.c, its import +0x12C). Each one
 * arrives with SetShockData's own six arguments, so it is the same call the
 * weapons and the train make from here.
 */
static void client_shock_sink(void *user, int player, int id, s32 strength,
                              const s32 pos[3], s16 near_r, s16 far_r)
{
    client_shock((client *)user, player, id, strength, pos, near_r, far_r);
}

/* What PadSetAct sends, as SDL's two motors: the big one is the low-frequency
 * weight, the little on/off one the high-frequency. The duration is a dead
 * man's handle — renewed every tick while the motors run, so a host that
 * stops calling cannot leave a pad buzzing. */
#define Q2_SHOCK_HOLD_MS 120u

static void client_shock_send(client *c, int p)
{
    SDL_Gamepad *pad = (p >= 0 && p < Q2_SIM_MAX_PLAYERS)
                     ? c->gamepads.device[p] : NULL;
    u8 little = c->shock_out[p][0], big = c->shock_out[p][1];

    if (!pad)
        return;
    if (!little && !big && !c->shock_sent[p][0] && !c->shock_sent[p][1])
        return;

    SDL_RumbleGamepad(pad, (Uint16)(big * 257u), little ? 0xFFFFu : 0u,
                      (little || big) ? Q2_SHOCK_HOLD_MS : 0u);
    c->shock_sent[p][0] = little;
    c->shock_sent[p][1] = big;
}

/*
 * PadDualShock (0x80031728) for every window, once per field of sim time.
 *
 * Its gates, in its order: nothing is processed while Paused or in self-play
 * (the port reaches here only when the world is running, so both hold), nor
 * with that player's VIBRATION off (Engine+728 + 34*player, 0x8003179C) — the
 * slots freeze rather than play out. Then ProcessShockData, which silences a
 * dead window itself; and with three or more players in a multiplayer level
 * the odd and even pads take alternate frames (0x800317B4..0x80031808), which
 * reads the game's frame counter and so takes the host's here.
 */
static void client_shock_tick(client *c, s32 ticks)
{
    s32 field = c->sim[0].dt_per_field > 0 ? c->sim[0].dt_per_field : 6;
    int n = client_shock_windows(c), p;

    c->shock_clock += ticks;

    while (c->shock_clock >= field) {
        c->shock_clock -= field;

        for (p = 0; p < n; p++) {
            u8 out[2] = { 0, 0 };

            if (c->settings.v[Q2_SET_VIBRATION]) {
                const bool local = (p == c->sim[0].cur_player);
                const s16 health = local ? c->sim[0].combat.inv.health
                                         : c->sim[0].pcombat[p].inv.health;

                q2_shock_process(&c->shock[p], field, health > 0, out);
                if (n >= 3 && c->mp_enabled &&
                    (c->frame_index & 1) != (p & 1))
                    out[0] = out[1] = 0;
            }

            /* PadSetAct's order: act[0] the little motor, act[1] the big. */
            c->shock_out[p][0] = out[1];
            c->shock_out[p][1] = out[0];

            if (p == 0) {
                c->shock_fields++;
                if (out[0]) c->shock_big_fields++;
                if (out[1]) c->shock_little_fields++;
                Q2_DEBUG("shock: field %u t%d big %u little %u",
                         c->shock_fields, (int)c->sim[0].level_time,
                         (unsigned)out[0], (unsigned)out[1]);
            }
        }
    }

    for (p = 0; p < n; p++)
        client_shock_send(c, p);
}

/* A frame on which the world did not run — a menu, the pop-up, a board — has
 * Paused set on the console, and PadDualShock zeroes the motors without
 * touching the slots. */
static void client_shock_hold(client *c)
{
    int p;

    for (p = 0; p < Q2_SIM_MAX_PLAYERS; p++) {
        c->shock_out[p][0] = c->shock_out[p][1] = 0;
        client_shock_send(c, p);
    }
}

/* Each view-weapon entity owns its animation, selection and shot cursors.
 * The caller selects the same owner's combat block before entering here. */
static void client_advance_view_weapon(client *c, bool attack, float dt)
{
    int pi = c->sim[0].cur_player;

    if (!c->death[pi].linked_weapon || c->sim[0].combat.inv.health <= 0)
        return;

    if (c->vm_ready) {
        s32 ticks = (s32)((double)dt * 300.0 + 0.5);
        bool swapped;

        /*
         * HOW MANY TIMES THE FIRE FUNCTION RAN THIS FRAME, AND HOW MANY BULLETS
         * CAME OUT OF IT.
         *
         * The console raises its sounds INSIDE the fire function, so a frame
         * that ran it twice made two bangs; this port raised one per change of
         * `shot_serial`, which is one per frame however many calls there were.
         * On a wound-up chaingun that is the difference between a rip and a
         * tap: measured 573 fire calls against 95 sounds on a 300-frame run.
         */
        u32 fire_calls = 0, fire_bullets = 0;

        if (ticks < 1) ticks = 1;
        if (ticks > Q2_SCREEN_DT_MAX) ticks = Q2_SCREEN_DT_MAX;

        /*
         * THE OVERLAY LINE IS NOT POSTED HERE. It used to be, on any change of
         * `combat.weapon_id` — but statusbar.h:665-672 lists six writers of the
         * held weapon (a weapon pickup 0x80037E28, an ammo pickup 0x80037ECC,
         * the ALL WEAPONS variable 0x8003B040, the spawn loadout 0x8003D4FC,
         * the cycle 0x8004ECB4 and the refire pass's auto-select 0x8004F87C)
         * and the console prints from exactly one of them: the `jal 0x800434B8`
         * at 0x8004EDF0 is inside the cycle routine, 0x1C before the next
         * function begins at 0x8004EE0C. Posting from here named the weapon on
         * spawn, on every pickup and after every dry-weapon auto-select, none
         * of which the console announces. It lives in client_cycle_input now,
         * which is where the console keeps it.
         */
        if (c->sim[0].combat.weapon_id != c->vw_last_weapon[pi]) {
            q2_vw_select(&c->vw[pi], c->sim[0].combat.weapon_id);
            c->vw_last_weapon[pi] = c->sim[0].combat.weapon_id;
        }

        /*
         * WHAT THE SIM'S SHOT ACTUALLY DID.
         *
         * This used to report Q2_VW_FIRED whenever the trigger was down and the
         * weapon was not dry — which is every frame of a held trigger, refire
         * gate or no refire gate. So the fire clip played for shots that never
         * happened, and it played at RENDER rate: q2_vw_advance runs every
         * frame while the sim ticks every second frame in the headless step.
         *
         * The three outcomes are distinct and only one of them is a shot:
         *
         *     dry     -> Q2_VW_FIRE_DENIED, the empty-gun pass that makes the
         *                machine switch you off the weapon
         *     fired   -> Q2_VW_FIRED
         *     neither -> the refire gate said no; the machine must not be told
         *                anything, or the clip restarts on a tick that did not
         *                fire
         *
         * THE SERIAL IS WHAT MAKES IT AN EVENT, and this used to gate on `ticks`
         * instead — which is clamped to a minimum of 1 fifty lines above and is
         * therefore always true. It gated nothing. `last_shot` is a latch that
         * holds the last ATTEMPT, so on the frames between ticks it still reads
         * `fired`, and the third outcome above was unreachable from here: a held
         * trigger re-reported one shot on every rendered frame, and the clip
         * restarted as fast as the machine's latch could clear. That is the very
         * defect the comment above claimed the gate was preventing, and it is
         * the same one the sound below had.
         *
         * `shot_serial` is bumped once per fire attempt (sim.h), so a value this
         * client has not seen means a NEW attempt and nothing else does. The
         * trigger drops out of the condition with it: the sim only attempts a
         * shot on a tick whose input had `attack` set, so a fresh serial already
         * says the trigger was down when it mattered — which is not the same
         * question as whether it is still down on this frame.
         *
         * `attack` is still passed to the machine, because that argument is
         * the TRIGGER, not the report.
         */
        {
            q2_vw_fire_result report = Q2_VW_FIRE_NONE;

            /*
             * THE MACHINE OWNS THE TRIGGER. `q2_vw_wants_fire` is this port's
             * name for the console's own condition — IDLE with the dry latch
             * clear — and it sat here with no caller anywhere for as long as
             * the sim fired on its own tick.
             *
             * With the invented 30-tick gate gone (weapon.c), the rate of fire
             * is the fire CLIP, which is what the console does: its IDLE arm
             * calls the fire function and then enters FIRE. The three weapons
             * that need to be faster get it from their own frame driver, which
             * is drained just below.
             */
            if (ticks > 0 && attack && q2_vw_wants_fire(&c->vw[pi])) {
                q2_fire_result_v2 r = q2_sim_fire(&c->sim[0]);

                if (r.fired) {
                    fire_calls++;
                    fire_bullets += r.shot_count;
                    /* The fire function's own SetShockData, at its end. */
                    if (r.shock)
                        client_shock(c, pi, r.shock, Q2_SHOCK_FULL,
                                     NULL, 0, 0);
                }
            }

            if (c->sim[0].combat.shot_serial != c->shot_serial_shown[pi]) {
                c->shot_serial_shown[pi] = c->sim[0].combat.shot_serial;

                if (c->sim[0].combat.last_shot.dry) {
                    report = Q2_VW_FIRE_DENIED;
                    if (pi == 0) c->shots_dry++;
                    c->mp_dry[pi]++;
                } else if (c->sim[0].combat.last_shot.fired) {
                    report = Q2_VW_FIRED;
                    if (pi == 0) c->shots_fired++;
                    c->mp_shots[pi]++;
                    /*
                     * AND THE NOISE IT MADE. PlayerNoise (0x80062C74) is what
                     * puts a gunshot on the level's `sound_entity`, and
                     * FindTarget's second arm looks for exactly that. Without
                     * it the player could empty a magazine in a corridor and
                     * wake nobody — measured at 126 shots, 0 hunting.
                     */
                    if (pi == 0 && c->creatures_ready)
                        q2_creature_world_player_noise(&c->creatures, true);
                }
            }

            /*
             * Is the quad up? The four fire sites read the deadline out of the
             * player's own block (combat+172, which is `quad_until`) and
             * compare it against the level clock at 0x800AEBAC — so the
             * comparison is the console's and only the plumbing is this
             * caller's. `q2_vw_advance` raises the sound off it.
             */
            c->vw[pi].quad_active =
                (c->sim[0].level_time <
                 c->sim[0].combat.inv.quad_until);

            /*
             * AND THE BARREL'S OWN PITCH, client+0x46. PrimaryQuakePlayer eases
             * it toward client+0x44 once a tick at 0x8003A44C..0x8003A460 —
             * `Seek(&client+0x46, client+0x44, [0x800B2DB4] * 4)` — and
             * 0x8004F3F0 adds the result to the clip rotation's X before the
             * weapon model's matrix is built, so the gun tips onto a target
             * above or below you while the view stays level. `ticks` is the
             * same 1/300 s frame delta the console passes, which is what
             * q2_vw_advance is already given.
             *
             * THE TARGET IS ZERO AND THAT IS THE HALF THAT IS MISSING, said
             * plainly rather than hidden behind a call that looks wired. The
             * elevation comes from FindPlayerTarget (QPLAYER.C 0x80039DDC),
             * which picks one of eleven candidates and writes the ratan2 of the
             * offset into client+0x44 at 0x8003A138 — and writes ZERO at
             * 0x8003A190 when it finds nobody. Nothing in this port searches
             * for that target yet, so what runs here is the no-target arm: the
             * console's own answer when there is nothing in front of you, and
             * the reason `view.aim_assist` reads zero in every report today.
             * When the search lands, only this argument changes.
             */
            q2_vw_seek_aim_assist(&c->vw[pi], 0, ticks);

            swapped = q2_vw_advance(&c->vw[pi], ticks, attack, report);
        }
        if (swapped)
            client_bind_view_model(c, pi);

        /* Grenade3 is a hidden entity attached to the view weapon until the
         * model timeline crosses 411. Retail copies viewmodel+0xA4 every think
         * (0x8004A414), charges only while position 380 is pinned, and lets an
         * elapsed fuse force fire frame 2. Feed that entity after advancing the
         * model so both the attachment and its crossing are from this frame. */
        if (c->vw[pi].weapon == Q2_WID_HAND_GRENADE ||
            q2_projectile_hand_held_index(&c->sim[0].combat.projectiles, pi) >= 0) {
            s16 aim[3], kick[3];
            s32 summed[3], attached[3];
            s32 cook_ticks = q2_vw_take_hand_grenade_cook(&c->vw[pi]);
            bool release = q2_vw_take_hand_grenade_release(&c->vw[pi]);
            q2_hand_grenade_update state;

            q2_sim_view_angles(&c->sim[0], summed);
            aim[0]  = (s16)c->sim[0].player[pi].pitch;
            aim[1]  = (s16)c->sim[0].player[pi].yaw;
            aim[2]  = (s16)c->sim[0].player[pi].roll;
            kick[0] = (s16)(summed[0] - c->sim[0].player[pi].pitch);
            kick[1] = (s16)(summed[1] - c->sim[0].player[pi].yaw);
            kick[2] = (s16)(summed[2] - c->sim[0].player[pi].roll);

            q2_vw_place(&c->vw[pi], c->sim[0].player[pi].pos,
                        c->sim[0].player[pi].view_height,
                        aim, kick, attached, NULL);
            state = q2_sim_hand_grenade_update(&c->sim[0], attached,
                                                cook_ticks, release);
            if (state == Q2_HAND_GRENADE_EXPIRED)
                q2_vw_hand_grenade_expired(&c->vw[pi]);
            /* 0x8004A52C: the held entity's release shakes the thrower's pad
             * with ShockData_GrenadeThrow, after the throw sound. */
            if (state == Q2_HAND_GRENADE_RELEASED)
                client_shock(c, pi, Q2_SHOCK_GRENADE_THROW, Q2_SHOCK_FULL,
                             NULL, 0, 0);
        }

        /*
         * AND THE SHOTS THE FIRE-STATE DRIVER ASKED FOR.
         *
         * The four per-weapon arms of 0x8004FEE8 call the weapon's fire
         * function themselves, once per animation frame or once per 30 units
         * of accumulated step depending on the weapon — that is what makes a
         * held chaingun a stream rather than one shot. They are drained here
         * and turned into real shots against the sim.
         *
         * The sim's own refire gate still applies. For the four weapons that
         * have an arm the console has no gate but the animation, so the two
         * agree as long as the clip is slower than 30 ticks; where they do not,
         * the gate wins and the shot is skipped, which is the conservative
         * half. Weapon 6 raises no frame fire: its arm feeds the held entity
         * above, and its model-timeline crossing at 411 performs the throw.
         */
        {
            u32 n = q2_vw_take_frame_fires(&c->vw[pi]);
            s16 snd;

            while (n--) {
                q2_fire_result_v2 r = q2_sim_fire(&c->sim[0]);

                c->player_attacks++;
                if (r.fired) {
                    fire_calls++;
                    fire_bullets += r.shot_count;
                    if (r.shock)
                        client_shock(c, pi, r.shock, Q2_SHOCK_FULL,
                                     NULL, 0, 0);
                }

                /*
                 * AND THE ARM HEARS HOW IT WENT. Each of 0x8004FEE8's arms
                 * tests the fire function's return before anything else: a
                 * dry machinegun drops to IDLE there and then, a dry chaingun
                 * or hyperblaster raises the latch, and only a shot that
                 * fired brings the quad's sound. A shot the port's own gate
                 * refused is neither, and is not reported.
                 */
                if (r.fired || r.dry)
                    q2_vw_frame_fire_result(&c->vw[pi], r.dry);
            }

            /* And the clip's own sound at a band boundary — the chaingun's
             * spin-up, loop and spin-down, three of the eleven weapon sounds
             * the table names and nothing had ever played. */
            snd = q2_vw_take_frame_sound(&c->vw[pi]);
            if (snd >= 0 && (u32)snd < Q2_WT_SOUND_COUNT) {
                const q2_weapon_tables *wt = q2_weapon_tables_builtin();

                if (wt->sound[snd][0])
                    client_play_sound(c, wt->sound[snd]);
            }

            /*
             * AND THE QUAD'S, which is a different table.
             *
             * The four fire sites all test the level clock against the deadline
             * at combat+172 and, when it has not passed, play `[0x800B28B0]` —
             * filled at 0x80037AA0 from `itm_damage3`. That is an ITEM sound,
             * not one of the twenty-two the weapon table names, so it is played
             * through the item table's own key rather than by weapon index.
             *
             * AND ONLY IF THE LAST ONE HAS FINISHED, which this did not ask.
             *
             * 0x8004FBA4 loads the shared handle at gp+17792 (0x800B2B80),
             * 0x8004FBA8 asks 0x800739B8 whether it is still sounding and
             * 0x8004FBB0 `bne v0, zero` throws the request away when it is;
             * only an idle handle reaches 0x8004FBC4's play, which writes the
             * new handle back to the same slot (a2 = gp+17792). All four sites
             * do this, against that one global.
             *
             * `itm_damage3` is 25340 frames at 22050 Hz, 1.05 s once the 35/32
             * pitch is applied, and the fastest thing that can ask for it is a
             * spinning machinegun or hyperblaster at the 30-unit spin threshold
             * — 10 Hz. Ungated that is ten copies of one sample in phase with
             * itself; even a blaster at 2.5 shots/s stacks three. The console
             * is never more than one.
             *
             * The drain runs FIRST and unconditionally: 0x8004FBB0 discards the
             * request, it does not defer it, so a flag left raised here would
             * simply fire on the next frame instead.
             */
            if (q2_vw_take_quad_sound(&c->vw[pi])) {
                c->quad_raises++;
                if (client_voice_playing(c->quad_voice, c->quad_voice_serial)) {
                    c->quad_gated++;
                } else if (client_play_sound(c, c->item_sound[Q2_SND_QUAD])) {
                    c->quad_voice        = c->voice_last;
                    c->quad_voice_serial = c->voice_last_serial;
                }
            }
        }

        /*
         * The machine's two outputs, neither of which anything had ever
         * drained. `q2_vw_take_refire`, `q2_vw_take_event` and
         * `q2_vw_wants_fire` were all declared, implemented and never called.
         *
         * THE REFIRE PASS IS A SELECTION, NOT A CYCLE. It used to call
         * q2_sim_cycle_weapon(+1). The console's refire pass calls 0x800506C4
         * (`jal` at 0x8004FB60), which walks the fixed auto-switch preference
         * list at 0x8009DB7C and takes the first entry that is both OWNED and
         * FED, writing it to player+0x66 and the view model's +214. That is
         * idempotent: holding the best affordable weapon, it picks the same one
         * and nothing changes.
         *
         * q2_weapon_cycle is the transcription of the OTHER function,
         * 0x80050758 — the +/-1 neighbour scan the console calls twice at
         * 0x8004FB70 and 0x8004FB88 only to refill the next/previous caches at
         * player+0x64 and +0x60. It never sets the held weapon. Calling it here
         * meant every shot walked the player one step forward through the
         * carousel; invisible on BASE1, where the blaster is the only weapon
         * owned and the cycle returns "no change".
         *
         * q2_weapon_autoselect — the correct transcription — was already in the
         * tree with no production caller at all.
         */
        if (q2_vw_take_refire(&c->vw[pi])) {
            q2_sim_autoselect_weapon(&c->sim[0]);
            /* ...and the pair after it, picked or not (0x8004FB68..0x8004FB98;
             * client_sbar_write_slots). */
            client_sbar_write_slots(c, c->sim[0].cur_player,
                                    CLIENT_SLOTS_AUTOSELECT);
        }

        /*
         * The animation's own per-key event. Drained and RECORDED rather than
         * acted on, and deliberately so: the original's consumer is 0x80050454,
         * a multi-way dispatch on the id with an arm for 2 and a shared arm for
         * {3,6,7,8,11}, reading state+0x114/+0x116 and calling 0x800739B8 and
         * 0x8007270C. Nothing read so far says which id is a muzzle flash and
         * which is a shell eject, and hanging an invented meaning on a decoded
         * id is exactly the mistake this project keeps paying for. Counted so
         * the ids that actually occur can be seen; see openquestions.
         */
        {
            s16 ev;
            if (q2_vw_take_event(&c->vw[pi], &ev)) {
                c->vw_events++;
                c->vw_last_event = ev;
            }
        }

        /*
         * AND THE SHOT'S SOUND, which every one of the eleven fire functions
         * computes into `res.sound` and nothing has ever played. Firing any
         * weapon was silent, and a dry trigger did not click.
         *
         * IT IS CONSUMED, not sampled. `last_shot` is a latch that is written
         * on a trigger pull and never cleared, so `fired` stays true after the
         * trigger is released — and this read used to be gated on `ticks`,
         * which is clamped to a minimum of 1 just above and is therefore
         * always true. Every rendered frame replayed the same shot: one pull,
         * and then that shot for as long as the player stood there. Between
         * ticks it also fired several times per shot while the trigger WAS
         * held, because a fire attempt happens on a tick and this runs on a
         * frame.
         *
         * The serial is what makes it an event: it is bumped once per attempt
         * (sim.h), so a shot is heard exactly once no matter how many frames
         * pass before the next one.
         */
        if (c->sim[0].combat.shot_serial != c->shot_serial_heard[pi]) {
            const q2_fire_result_v2 *shot = &c->sim[0].combat.last_shot;

            c->shot_serial_heard[pi] = c->sim[0].combat.shot_serial;

            /*
             * THE CHAINGUN BANGS ONCE PER BULLET, AND QUIETLY.
             *
             * QPlayerFireChainGun's bullet loop runs `lhu s5, 44(s6)` times —
             * one, two or three, written by the view-model arm at 0x800501C0
             * and clamped to the ammo at 0x80050200 — and 0x8004CCB4/0x8004CCB8
             * play `SoundChaingunFire` (gp+17700) at the player's origin INSIDE
             * it, once per bullet. That handle is not the machinegun's: 0x8004E0F8
             * CLONES the wep_machgf1b request with GetSoundClone and 0x8004E2CC
             * takes the clone down by 40, so it sounds at 23 of 63 — a little
             * over a third of a machinegun shot from the same sample.
             *
             * Then, after the loop, 0x8004CCD8 plays the SEPARATE full-volume
             * request at gp+17744 (SoundMachinegunFire) once through
             * SoundByCreature. So a three-bullet chaingun tick is three quiet
             * bangs and one loud one, and this port made a single loud one.
             */
            if (fire_bullets &&
                c->sim[0].combat.weapon_id == Q2_WID_CHAINGUN) {
                const q2_weapon_tables *wt = q2_weapon_tables_builtin();
                const char *key = wt->sound[Q2_WSND_MACHINEGUN];
                u32 b;

                for (b = 0; b < fire_bullets && key[0]; b++) {
                    c->chaingun_bangs++;
                    if (client_play_sound(c, key))
                        client_sound_set_relative_volume(
                            c, Q2_SFX_VOLUME_CHAINGUN_DELTA);
                }
            }

            /*
             * ONCE PER FIRE CALL, not once per frame. Every fire function plays
             * its weapon's sound itself — the chaingun's trailing one is
             * 0x8004CCD8 — so a frame in which the frame-fire driver ran the
             * function three times made three of them, and this raised one.
             *
             * `fire_calls` counts only the calls that FIRED, which is also why
             * it is preferred to `shot->fired`: that is the LAST attempt's
             * outcome, and a frame can fire twice and then be refused.
             * A dry trigger has no call to count and still clicks once.
             */
            {
                u32 want = fire_calls ? fire_calls
                                      : ((shot->fired || shot->dry) ? 1u : 0u);
                u32 k;

                for (k = 0; k < want && shot->sound >= 0; k++) {
                    const q2_weapon_tables *wt = q2_weapon_tables_builtin();

                    c->weapon_sounds++;
                    if ((u32)shot->sound < Q2_WT_SOUND_COUNT &&
                        wt->sound[shot->sound][0] &&
                        client_play_sound(c, wt->sound[shot->sound])) {
                        /* entity+276/278: the handle the cock waits on. */
                        c->shot_voice[pi]        = c->voice_last;
                        c->shot_voice_serial[pi] = c->voice_last_serial;
                    }
                }
            }

            /* 0x8004C460. The raise is the fire function's, so it is keyed on
             * the shot having fired and on the weapon that fired it. */
            if (shot->fired && c->sim[0].combat.weapon_id == Q2_WID_SHOTGUN)
                c->cock_pending[pi] = true;
        }

        /*
         * 0x80050484..0x800504D0, the deferred half. The console asks on every
         * substep of the key loop and this asks once a frame, which moves the
         * cock by at most one frame and cannot move it before the bang ends —
         * the query is the same one.
         */
        if (c->cock_pending[pi] &&
            c->sim[0].combat.weapon_id == Q2_WID_SHOTGUN &&
            !client_voice_playing(c->shot_voice[pi],
                                  c->shot_voice_serial[pi])) {
            const q2_weapon_tables *wt = q2_weapon_tables_builtin();

            if (wt->sound[Q2_WSND_SHOTGUN_RELOAD][0])
                client_play_sound(c, wt->sound[Q2_WSND_SHOTGUN_RELOAD]);
            c->cock_pending[pi] = false;
            c->cocks_played++;
        }
    }

}

static void client_extra_input(client *c, int pi, s32 step, bool resumed,
                                q2_input *out)
{
    q2_pad_state *pad = &c->mp_pad[pi];
    q2_pad_config cfg;
    u32 raw;
    if (c->gamepads.changed[pi]) {
        memset(pad, 0, sizeof(*pad));
        c->mp_pad_pend[pi] = 0;
        c->mp_pad_resume[pi] = true;
        c->gamepads.changed[pi] = false;
    }
    pad->lx = pad->ly = pad->rx = pad->ry = 0;
    q2_pad_config_default(&cfg);
    cfg.style = c->sim[0].player[pi].look_scheme;
    cfg.swap_y = c->settings.v[Q2_SET_SWAP_Y];
    raw = c->demo ? client_demo_pad((long)c->frame_index + (long)pi * 37)
                  : q2_gamepads_read(&c->gamepads, pi, cfg.style, pad);
    c->gamepads.pressed[pi] = 0;
    c->mp_pad_resume[pi] |= resumed;
    c->mp_pad_pend[pi] |= (u16)raw;
    if (step > 0) {
        if (c->mp_pad_resume[pi])
            q2_pad_roll_resume(pad, c->mp_pad_pend[pi], raw);
        else
            q2_pad_roll(pad, c->mp_pad_pend[pi]);
        c->mp_pad_pend[pi] = 0;
        c->mp_pad_resume[pi] = false;
    }
    q2_pad_read(pad, &cfg, out);
}

/*
 * 0x8004ECB4 — the weapon next/previous routine, and the ONLY place the
 * console names a weapon on the overlay. Its tail, read out:
 *
 *     8004ED8C  beq   a2, zero, 0x8004EDF8   nothing pressed, no line
 *     8004EDB4  sh    v0, 100(s0)            the next-weapon cache
 *     8004EDBC  sh    v0, 96(s0)             the previous-weapon cache
 *     8004EDC8  lw    a0, 0(v0)              the notification target
 *     8004EDD0  beq   a0, zero, 0x8004EDF8   none bound, no line
 *     8004EDD8  lh    v1, 102(s0)            the weapon now held
 *     8004EDDC  addiu a1, a1, -13076         0x800ACCEC "Selected %s"
 *     8004EDE4  addiu a2, a2, -9076          0x8009DC8C weapon_glyph[]
 *     8004EDE8  sll / addu                   a2 += weapon_id * 3
 *     8004EDF0  jal   0x800434B8             sprintf, then post
 *
 * so the line is "Selected " followed by the weapon's GLYPH — weapon_glyph[]
 * holds "&B", "&S", "&U" and the markup layer expands an escape into a
 * pre-rendered word out of chars.lbm's icon table (hudtables.h). "Selected
 * Shotgun" on screen is nine characters and one sprite, which is why hunting
 * for the whole string never found it; the port had the prefix sitting unused
 * in q2_hud_weapon_selected and posted the bare glyph instead.
 *
 * NOT GATED ON THE CYCLE SUCCEEDING. `a2` is the "a cycle button was pressed"
 * flag and 0x8004ED40 sets it to 1 in the delay slot of the jump to that tail
 * whether or not 0x80050758 found another weapon — what a failing scan skips
 * is the store at 0x8004ED38, not the flag. So a player holding his only
 * weapon and tapping NEXT gets the line again, naming what he already has, and
 * the bool from q2_sim_cycle_weapon is deliberately not consulted.
 */
static void client_cycle_input(client *c, int pi, const q2_input *in)
{
    int saved = c->sim[0].cur_player;
    if (c->sim[0].player[pi].ent2_flags & Q2_ENT2_DEAD) return;
    if (!(in->buttons & (Q2_BTN_WEAP_NEXT | Q2_BTN_WEAP_PREV))) return;
    q2_sim_select_player(&c->sim[0], pi);
    q2_sim_cycle_weapon(&c->sim[0],
                        in->buttons & Q2_BTN_WEAP_NEXT ? 1 : -1);
    client_sbar_write_slots(c, pi, CLIENT_SLOTS_SELECT);
    /* After the two caches, as at 0x8004EDC0, and while this pad's player is
     * still the selected one. `hud[pi]` is this viewport's own overlay, which
     * is the split-screen reading of the per-client target at 0x8004EDC8.
     * q2_hud_weapon_selected does the table lookup and the blank slot 0; the
     * console range-checks nothing and slot 0 is the deliberate "  ". */
    if (c->hud_ready && c->hud_tables_ready) {
        q2_hud_weapon_selected(&c->hud[pi], &c->hud_tables,
                               c->sim[0].combat.weapon_id);
        c->weapon_lines++;
    }
    q2_sim_select_player(&c->sim[0], saved);
}

static void client_extra_camera(client *c, int pi)
{
    const q2_player *pl = &c->sim[0].player[pi];
    s32 *angles = c->mp_camera_angles[pi];
    if (pl->ent2_flags & Q2_ENT2_DEAD) {
        s32 cur = angles[2] & 4095;
        s32 d = ((-384 & 4095) - cur) & 4095;
        if (!(d & 2048)) cur += (d * 100 + 2047) >> 11;
        else cur -= ((4096 - d) * 100 + 2047) >> 11;
        angles[2] = cur & 4095;
    } else {
        angles[0] = pl->pitch;
        angles[1] = pl->yaw;
        angles[2] = pl->roll;
    }
}

static void client_input_simulated(client *c, float dt)
{
    q2_input in;
    bool view_attack[Q2_MP_MAX_PLAYERS] = { false };
    s32 eye[3], view[3];
    bool ticked;

    q2_pad_state       *pad  = &c->pad;
    u16                *pend = &c->pad_pend;  /* bits seen since the last TICK */
    /*
     * Did something else own the previous frame?
     *
     * STICKY, and that is not fussiness. This function runs on every frame but
     * the roll below only happens on the frames that TICK, which above 25 Hz
     * is a minority of them. A gap noticed on a non-ticking frame would be
     * forgotten before any roll could act on it, and the very next frame would
     * look contiguous again — leaving the defect exactly where it was, one
     * frame later and only on the boundaries that happen to land off a tick.
     *
     * So it is raised here and cleared only where it is consumed. A headless
     * run of BASE2 across the tally board and the end-of-mission placard
     * reports `pad: 2 resumes`, which is one per screen and is the count that
     * says the detection reaches them at all.
     */
    bool                resumed;
    q2_pad_config       cfg;

    if (c->pad_frame != c->frame_index - 1)
        c->pad_resume = true;
    resumed = c->pad_resume;
    /* The world runs this frame, ticked or not: the pads keep what they were
     * last given rather than being held as if Paused. */
    c->shock_live = true;
    q2_pad_bindings     bind;
    u16                 raw;
    s32                 step;
    int                 style = c->sim[0].player[0].look_scheme;

    if (!q2_pad_style_bindings(style, &bind))
        memset(&bind, 0, sizeof(bind));

    /*
     * -----------------------------------------------------------------------
     * THE PAD ROLLS ONCE PER TICK, NOT ONCE PER RENDERED FRAME
     * -----------------------------------------------------------------------
     * The console has no distinction to make: 0x80019154 has exactly one
     * caller, 0x8003A4A4 inside the player's own frame, and 0x800184D8 runs
     * that frame once per screen frame with no gate on dt at all. So a press
     * EDGE is produced and consumed in the same call, always.
     *
     * The port splits them. `q2_sim_advance` only runs a tick once the
     * accumulator reaches Q2_DT_NOMINAL, which at any frame rate above 25 Hz
     * is a minority of frames — and this rolled `prev` on EVERY frame, so an
     * edge raised on a non-ticking frame was destroyed before any tick could
     * see it. Bit 22, the jump, is the only control in the game that exists
     * solely as a single-frame edge and is consumed solely inside the tick, so
     * it is the one that broke: measured against this exact accumulator
     * arithmetic, 48 of 200 presses survived at 60 Hz and 33 at 144 Hz.
     * Holding the key longer did not help — `derive` clears the bit from the
     * second frame on, because `was` is true by then.
     *
     * So the roll is deferred to the frames that tick, and the raw pad is
     * accumulated into `pend` in between. The OR is an addition the console
     * does not have and needs a name: it catches a tap that begins and ends
     * inside one 40 ms tick interval, which retail never had to because its
     * tick rate WAS its frame rate. Without it a fast tap on a 240 Hz display
     * would still be dropped. Do not "correct" it back out.
     *
     * Everything below this that is a LEVEL rather than an edge — the look
     * axis, the mouse accumulator — stays unconditional.
     */
    step = q2_sim_next_dt(&c->sim[0], (double)dt);

    pad->lx = pad->ly = pad->rx = pad->ry = 0;
    if (c->gamepads.changed[0]) {
        *pend = 0;
        memset(pad, 0, sizeof(*pad));
        c->pad_resume = resumed = true;
        c->gamepads.changed[0] = false;
    }
    raw = client_pad_mask(c, &bind);
    if (!c->demo)
        raw |= (u16)q2_gamepads_read(&c->gamepads, 0, style, pad);
    c->gamepads.pressed[0] = 0;

    /* The wheel, onto the same two masks the weapon keys use, and into `raw`
     * so a notch is latched by the same accumulator as a key. */
    {
        int notch = client_wheel_notch(c);

        if (notch > 0)      raw |= (u16)bind.weapon_next;
        else if (notch < 0) raw |= (u16)bind.weapon_prev;
    }

    *pend |= raw;

    if (step > 0) {
        /*
         * AND THE FIRST TICK BACK IS NOT AN ORDINARY ONE.
         *
         * The roll above stalls whenever something else owns the frame — a
         * film, the menu, an intermission board, the front end — because those
         * branches do not call this function at all. The console has no such
         * gap: its pad pair rolls every frame whatever is on screen, so a
         * button held from one screen into the next is already `was` by the
         * time anything looks at it and raises no press edge.
         *
         * Without this the port manufactured one. Skipping the intro film with
         * the jump key left `prev` holding whatever was down before the film
         * and `buttons` holding the jump, and the level's first tick read that
         * pair as a fresh press: the player jumped on arrival. Bit 22 is the
         * loudest case because it exists solely as an edge, but every press
         * edge the pad derives had the same hole — fire, and both weapon
         * cycles.
         */
        if (resumed) {
            /* Said out loud when it actually swallows something, because "the
             * player jumped on arrival" and "the player did not" look the same
             * in a log otherwise. */
            if (raw)
                Q2_INFO("pad: resumed with %04X held — no press edges from it",
                        (unsigned)raw);
            c->pad_resumes++;
            c->pad_resume = false;
            q2_pad_roll_resume(pad, *pend, raw);
        } else {
            q2_pad_roll(pad, *pend);
        }
        *pend = 0;
    }
    c->pad_frame = c->frame_index;

    q2_pad_config_default(&cfg);
    cfg.style       = style;
    cfg.swap_y      = c->settings.v[Q2_SET_SWAP_Y];
    cfg.mouse_speed = c->settings.v[Q2_SET_MOUSE_SPEED];

    /*
     * The look axis, on the styles that have one. Two sources land in the same
     * pair and are summed:
     *
     *   the mouse    a displacement, converted to a rate against the step the
     *                tick is about to take
     *   the arrows   a rate already, because these styles have no look BUTTONS
     *                for the keys to press — leaving them dead would be the
     *                regression this whole arrangement exists to avoid
     */
    if (q2_pad_style_look(style) == Q2_PAD_LOOK_MOUSE) {
        int scale = (cfg.mouse_speed + 32) >> 4;   /* `step` is the pad roll's */
        int lx = 0, ly = 0;

        if (scale < 1)
            scale = 1;

        if (step > 0) {
            /* Pixels one unit of deflection is worth this tick. The remainder
             * stays in the accumulator: a fast flick that clamps at full
             * deflection is spread over the next frame or two rather than
             * thrown away. */
            double per = (double)CLIENT_MOUSE_DIV * (double)step /
                         (double)Q2_LOOK_DIV;

            if (per > 0.0) {
                int mx = (int)(c->look_acc_x / per);
                int my = (int)(c->look_acc_y / per);

                c->look_acc_x -= (double)mx * per;
                c->look_acc_y -= (double)my * per;

                lx += mx;
                ly += my;
            }
        }

        /*
         * A held arrow asks for the rate the digital styles reach: their look
         * axis saturates at Q2_PAD_FULL, and the mouse scale is applied on top
         * of this one, so dividing it out lands on the same turn.
         */
        {
            const bool *keys = (c->demo || c->headless) ? NULL : SDL_GetKeyboardState(NULL);
            int key_full = Q2_PAD_FULL / scale;

            if (key_full < 1)
                key_full = 1;

            if (keys) {
                if (keys[SDL_SCANCODE_LEFT])  lx -= key_full;
                if (keys[SDL_SCANCODE_RIGHT]) lx += key_full;
                /* The same way round as the digital styles' look buttons:
                 * SDL_SCANCODE_UP drives `look_up`, which is +Q2_PAD_FULL. */
                if (keys[SDL_SCANCODE_UP])    ly += key_full;
                if (keys[SDL_SCANCODE_DOWN])  ly -= key_full;
            }
        }

        if (lx >  Q2_PAD_FULL) lx =  Q2_PAD_FULL;
        if (lx < -Q2_PAD_FULL) lx = -Q2_PAD_FULL;
        if (ly >  Q2_PAD_FULL) ly =  Q2_PAD_FULL;
        if (ly < -Q2_PAD_FULL) ly = -Q2_PAD_FULL;

        pad->lx = (s8)lx;
        pad->ly = (s8)ly;
    }

    q2_pad_read(pad, &cfg, &in);

    /*
     * `--shoot`: hold fire. The same kind of scripted stand-in as `--demo` and
     * `--dm-stage` — a headless run cannot press a button, and "shoot the pane
     * and see it break" is not a claim a still frame can make on its own.
     */
    if (c->shoot || (c->shoot_to_kill && c->cre_die_calls == 0)) {
        in.attack   = true;
        in.buttons |= Q2_BTN_ATTACK | Q2_BTN_ATTACK_PRESS;
    }

    (void)dt;

    /*
     * Crouch is not an input on the console — INCROUCH and INLOWCROUCH are event
     * script primitives a trigger volume runs, so where you crouch is authored
     * per map. The key drives the same environment flag the dispatcher would set,
     * which is the honest way to keep a debug crouch without inventing a mechanic.
     */
    c->sim[0].env_flags &= ~(u32)(Q2_ENT_INCROUCH | Q2_ENT_INLOWCROUCH);
    if (client_key_down(c, SDL_SCANCODE_LCTRL, SDL_SCANCODE_C))
        c->sim[0].env_flags |= Q2_ENT_INLOWCROUCH;

    /*
     * Being submerged is the same kind of thing and is held the same way. The
     * map's own water volumes now work on their own — the sim resolves a
     * volume's record to its UserFuncs primitive at load — so this is no longer
     * the only source of the flag, just the one that does not need you to go
     * and find water. F3 holds it on (see the key handler), which drives both
     * the swimming physics and the water screen effect.
     */
    c->sim[0].env_flags &= ~(u32)(Q2_ENT_INWATER | Q2_ENT_UNDERWATER);
    if (c->force_underwater)
        c->sim[0].env_flags |= Q2_ENT_INWATER | Q2_ENT_UNDERWATER;

    /*
     * Weapon switching. The edge is the PAD's now — bits 26 and 27 are already
     * press edges out of q2_pad_read, so the "was it down last frame" bookkeeping
     * this used to do by hand is the shared tail's job and happens once for every
     * button rather than once per key.
     */
    /*
     * `--watch` aims the PLAYER, and it has to happen BEFORE the tick: the shot
     * is taken inside `q2_sim_advance`, so an aim written after it applies to
     * the frame after the one that fired.
     */
    if (c->watch && c->creatures_ready) {
        const q2_monster *best;
        bool holding;
        s32 eye0[3];

        q2_sim_eye(&c->sim[0], eye0);
        best = client_watch_pick(c, eye0, true, &holding);

        /* A held kill is watched, not shot at: leave the player where the
         * last live frame put them. */
        if (best && !holding) {
            s32 to[3];
            double horiz, p;

            /*
             * Stand the PLAYER in front of it as well, 700 units along the
             * creature's own facing and at head height — the same framing the
             * camera uses below.
             *
             * Without this the demo can only shoot from wherever it wandered
             * to, and on BASE1 that is a floor above: aiming correctly then
             * put every bolt into the floor between them, which is geometry
             * and not a combat fault. A test of whether the player can hurt a
             * creature has to be able to see one.
             */
            c->sim[0].player[0].pos[0] = best->pos[0] +
                ((q2_sin12(best->angles[2]) * 700) >> Q2_FRAC_12);
            c->sim[0].player[0].pos[1] = best->pos[1];
            c->sim[0].player[0].pos[2] = best->pos[2] +
                ((q2_cos12(best->angles[2]) * 700) >> Q2_FRAC_12);
            q2_sim_eye(&c->sim[0], eye0);

            to[0] = best->pos[0] - eye0[0];
            to[1] = best->pos[1] - eye0[1] - 150;
            to[2] = best->pos[2] - eye0[2];

            horiz = sqrt((double)to[0] * to[0] + (double)to[2] * to[2]);
            p = atan2((double)to[1], horiz > 1.0 ? horiz : 1.0);

            c->sim[0].player[0].yaw   = (s16)q2_vectoyaw(to);
            c->sim[0].player[0].pitch = (s16)(s32)(p * (double)Q2_ANGLE_360 /
                                             (2.0 * 3.14159265358979323846));
        }
    }

    /*
     * The weapon edges, under the SAME gate the pad roll is under, and this is
     * not optional: with `prev` frozen between ticks, `derive` recomputes the
     * same press edge on every non-ticking frame, so a consumer that runs at
     * render rate fires two or three times for one notch at 60 Hz.
     *
     * And `else if`, because 0x8004ECE0 branches to 0x8004ED00 rather than
     * falling into it — bit 26 suppresses bit 27 on a frame that somehow
     * carries both, which the wheel's own gap flag makes possible.
     */
    if (step > 0)
        client_cycle_input(c, 0, &in);

    /*
     * The creatures, published to combat as actors before the tick that may
     * shoot one, and read back after it.
     *
     * They are two structures because they are two things: `q2_monster` is what
     * the AI drives and `q2_actor` is what the damage function at 0x80057D54
     * operates on, and combat.h supplies the pair of converters precisely so
     * neither has to know about the other. Syncing on both sides of the tick is
     * what makes a monster that has walked somewhere shootable where it now is,
     * and a monster that has been shot notice.
     */
    if (c->creatures_ready && c->cre_actor) {
        u32 i;
        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            s32 push[3];

            /*
             * The damage-effect timers (entity+0x2F0..0x2F5) and the killer
             * byte (+222) outlive this rebuild inside q2_actor_from_monster
             * itself (combat.c), so this side no longer saves and restores
             * effect[] around the call.
             *
             * A BODY'S KNOCKBACK outlives it too, and that one is this side's
             * to keep, because q2_actor_init zeroes +0x2F8 and nothing on a
             * corpse ever does. T_Damage accumulates the push into a living
             * target and OVERWRITES a dead one's (0x80058188 `lh v0,264(s2)` /
             * `blez` to the stores at 0x800581E0..0x80058200). `q2psx-inspect
             * access 0x2F8` over the main executable (it does not scan the
             * relocated creature modules) finds `sh zero,760` only at
             * 0x80045F4C and 0x800461CC, inside the mover 0x8004583C, which
             * the live creature think calls (0x8007EE00, 0x8007EFD0) and the
             * corpse handler 0x8007F71C does not. So the gib dispatcher reads,
             * at 0x8007D104 and its twins, the last hit's push. Zeroed here
             * every frame it reached the throw only when the gibbing hit and
             * the 10 Hz corpse tick fell in the same frame. A living
             * creature's is still dropped: the port has no creature mover to
             * spend it, and the rebuild stands in for the consume.
             */
            memcpy(push, c->cre_actor[i].knockback, sizeof(push));
            q2_actor_from_monster(&c->cre_actor[i], m);
            if (m->health <= 0)
                memcpy(c->cre_actor[i].knockback, push, sizeof(push));
            /*
             * A FREED body — gibbed, or dissolved by 0x8005B2A8's handler —
             * has no think once 0x8006D280 has released it, so 0x8005B880
             * never presents it again, and the pool clears the record on its
             * next allocation (0x8006C18C, 768 bytes). The actor outlives the
             * monster here, so its effect bytes are cleared instead: an armed
             * effect[1] would otherwise raise its energy light where the body
             * used to be.
             */
            if (!m->in_use)
                memset(c->cre_actor[i].effect, 0,
                       sizeof(c->cre_actor[i].effect));

            /*
             * AND THE SOLID, in the same pass and for the same reason: the
             * separation the stepped mover runs (trace.h) has to be against
             * where the creature IS this frame, not where it was when the zone
             * loaded.
             *
             * `solid` is `in_use && !dead`. The console's own gate is bit
             * 0x8000 at entity+0x10C and a corpse is appended to the list
             * (0x8007F740), so this is narrower than the original — but the
             * port has no equivalent of that bit, and the alternative, leaving
             * every corpse solid forever, would wall a corridor off with the
             * things the player has killed in it. Stated rather than implied:
             * a body you cannot walk through is the console's, a corpse you
             * cannot walk through is not established.
             */
            {
                q2_move_body *b = &c->cre_body[i];
                int k;

                memset(b, 0, sizeof(*b));
                for (k = 0; k < 3; k++) {
                    b->pos[k]  = m->pos[k];
                    b->mins[k] = m->mins[k];
                    b->maxs[k] = m->maxs[k];
                }
                b->radius = Q2_BODY_RADIUS;
                /*
                 * The SWEEP's own pair, entity+0x94 and +0x96, taken from the
                 * actor rebuilt beside it exactly as 0x800544EC reads them,
                 * and a corpse's are already the wider, shorter ones.
                 *
                 * NOT from the creature's hull, which is what this said. The
                 * pair comes from the creature's MODEL now — GetCreHitSize's
                 * `lhu +0x18`/`lhu +0x1A`, bound at zone load — and only falls
                 * back to 286/600 for a creature whose model never resolved.
                 * The hull is the same 286 cube for every creature in the game,
                 * so while it was the source every creature was shot at as one
                 * cylinder.
                 */
                b->sweep_radius = c->cre_actor[i].radius;
                b->height       = c->cre_actor[i].height;
                b->id     = (s32)i;
                b->solid  = m->in_use && !m->dead;
            }
        }
        q2_sim_set_bodies(&c->sim[0], c->cre_body, c->creatures.set.count);
    }

    if (in.attack) c->player_attacks++;

    /*
     * `--dm-stage`: put the other players in front of player 0 and point
     * everyone at each other, with fire held.
     *
     * The same reason `--watch` exists. A scripted demo wanders; it does not
     * arrange a fight, and four players scattered across an arena firing
     * blindly produced no hits in 1200 frames — which says nothing about
     * whether a hit would have registered. This stages the encounter so the
     * scoring path can be exercised rather than reasoned about, and it is a
     * harness, not gameplay.
     */
    if (c->mp_stage && c->mp_enabled) {
        int pi;
        s32 eye0[3];

        q2_sim_eye(&c->sim[0], eye0);

        for (pi = 1; pi < Q2_MP_MAX_PLAYERS; pi++) {
            q2_player *pl;

            if (!c->sim_ready[pi])
                continue;

            pl = &c->sim[0].player[pi];

            /*
             * IN FRONT of player 0, along the way they are facing — not at a
             * blind diagonal offset, which is what the first version did and
             * which put a wall between them: the scan counted 44 shots stopped
             * by the world before reaching a target 339 units away. Player 0
             * walked to where they are, so the space ahead of them is space
             * they can see, the same reasoning `--watch` uses to frame a
             * creature.
             */
            {
                /* Close enough that a bolt connects often: the actors' reach
                 * is 286 + 286, so a few hundred units apart makes the target
                 * subtend a wide angle and the staged exchange conclusive in a
                 * capture short enough to run. */
                s32 fwd = 360 + 120 * pi;

                pl->pos[0] = c->sim[0].player[0].pos[0] +
                    ((q2_sin12(c->sim[0].player[0].yaw) * fwd) >> Q2_FRAC_12);
                pl->pos[1] = c->sim[0].player[0].pos[1];
                pl->pos[2] = c->sim[0].player[0].pos[2] +
                    ((q2_cos12(c->sim[0].player[0].yaw) * fwd) >> Q2_FRAC_12);
            }
            pl->ent.node = c->sim[0].player[0].ent.node;
            /*
             * Aimed at player 0's POSITION, not at the reverse of their
             * facing. The first version set `yaw + 2048`, which points a player
             * back down player 0's own line of sight and only coincides with
             * pointing AT them when player 0 happens to be looking at the
             * spot — 900 frames of that produced no hits at all.
             */
            {
                s32 v[3];

                v[0] = c->sim[0].player[0].pos[0] - pl->pos[0];
                v[1] = 0;
                v[2] = c->sim[0].player[0].pos[2] - pl->pos[2];

                pl->yaw   = q2_vectoyaw(v);
                pl->pitch = 0;
            }

            /*
             * The hurt-actor's origin is NOT set here. The sim maintains it
             * every tick, at the eye, and writing the feet over it each frame
             * put the target 572 units — two eye-heights — below the muzzle
             * and made every bolt miss. A harness that overwrites the field it
             * is measuring measures the harness.
             */
        }

        /*
         * And player 0's own aim is HELD too: fire on, no look input. Holding
         * only the extra players' aim is what made three code changes produce
         * byte-identical counters — player 0 was doing all the shooting, being
         * aimed at the top of each frame and turning away inside its own tick.
         */
        in.attack   = true;
        in.buttons |= Q2_BTN_ATTACK_PRESS;
        in.yaw      = 0;
        in.pitch    = 0;

        /* And player 0 looks back at the first of them. */
        if (c->sim_ready[1]) {
            s32 v[3];

            v[0] = c->sim[0].player[1].pos[0] - c->sim[0].player[0].pos[0];
            v[1] = 0;
            v[2] = c->sim[0].player[1].pos[2] - c->sim[0].player[0].pos[2];

            c->sim[0].player[0].yaw   = q2_vectoyaw(v);
            c->sim[0].player[0].pitch = 0;
        }
    }

    if (c->mp_enabled)
        client_targets_for(c, 0);

    /*
     * AND THE SIM IS TOLD IT IS A DEATHMATCH, which nothing ever did.
     *
     * `q2_sim.multiplayer` is 0x800AEBCC, and it is read in five places — the
     * -3072 impulse ceiling, the end-of-frame basis rebuild, and the three that
     * derive `sim->ent_world.deathmatch` from it, which is in turn what gates
     * doubled item amounts, weapons-stay and item respawn. `sync_rules` in
     * simcombat.c now takes the combat half from the same flag, so the
     * railgun's 150 and armour's 2048 bias hang off it too.
     *
     * The only writer it had was the save loader. A match started from the menu
     * left it false, so every one of those rules ran in its single-player form
     * inside a deathmatch. Written here, once a frame and before the tick,
     * because the sims are created and reset by the zone load rather than by
     * `client_mp_configure` — setting it there alone would not survive.
     */
    {
        int si;
        for (si = 0; si < Q2_MP_MAX_PLAYERS; si++)
            c->sim[si].multiplayer = c->mp_enabled;
    }

    q2_combat_scan_who = c->mp_enabled ? 0 : Q2_COMBAT_SCAN_OTHER;
    /*
     * WHETHER THE WORLD ACTUALLY MOVED. `q2_sim_advance` returns 0 when the
     * accumulated time has not reached one tick, which at any frame rate above
     * the nominal 25 Hz is most frames — and on those frames nothing in the sim
     * changed, including its event queue.
     */
    /*
     * A directory entry literally named ALWAYS is the level module's per-tick
     * hook. BASE0's runs DOCRATES; several other maps keep rotators and mission
     * checks there. Queue it only when the sim is about to take a logic tick —
     * at higher render rates q2_sim_advance legitimately does nothing on the
     * intervening frames.
     */
    if (q2_sim_next_dt(&c->sim[0], (double)dt) != 0)
        q2_event_rt_trigger_named(&c->sim[0].event_rt, "ALWAYS");

    c->mp_input[0] = in;
    ticked = (q2_sim_advance(&c->sim[0], &in, (double)dt) != 0);
    q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;
    client_score_deaths(c);

    /*
     * The other players, each on its own pad. In a headless demo run there is
     * one script, so each is given a rotated slice of it — otherwise four
     * players would walk in lockstep and a split screen would show one man
     * reflected four times, which would hide input-routing faults.
     */
    /*
     * ONE CLOCK, FOUR PLAYERS — 0x80033030 loops 0x800323EC over s0 = 0..3 and
     * every one of them reads the single dt global at 0x800B2DB4. There is no
     * per-player step.
     *
     * This recomputed a step from the RENDER frame's own dt and floored it at
     * 1, so on a frame where player 0's accumulator had not filled, players
     * 1..3 were stepped anyway on a 1-to-4 unit tick that player 0 never saw:
     * they ran ahead of the world they share, and — being the other half of
     * the bug above — they received every press edge while player 0 lost two
     * in three. Now they ride the tick player 0 actually took.
     *
     * `cur_dt` has to be captured BEFORE the loop: `q2_sim_advance_player`
     * calls `q2_sim_tick`, which overwrites it on the first iteration.
     */
    {
        int pi;
        s32 step_dt = c->sim[0].cur_dt;

        view_attack[0] = in.attack;

        for (pi = 1; pi < Q2_MP_MAX_PLAYERS; pi++) {
            q2_input pin;

            if (!c->sim_ready[pi])
                continue;

            client_extra_input(c, pi, step, resumed, &pin);
            c->mp_input[pi] = pin;
            if (ticked) client_cycle_input(c, pi, &pin);

            view_attack[pi] = c->mp_stage || pin.attack;
            if (!ticked)
                continue;

            {
                s32 ticks = step_dt;      /* the step player 0 just took */

                if (c->mp_stage) {
                    pin.attack   = true;
                    pin.buttons |= Q2_BTN_ATTACK_PRESS;

                    /*
                     * And no look input: the aim is written before the tick and
                     * `update_look` would turn them off it before the shot is
                     * taken inside the same tick. The scan counted 1988 shots
                     * with the target BEHIND the muzzle — a staged player was
                     * being aimed and then immediately turning away.
                     */
                    pin.yaw   = 0;
                    pin.pitch = 0;
                }
                client_targets_for(c, pi);
                q2_combat_scan_who = pi;
                q2_sim_advance_player(&c->sim[0], pi, &pin, ticks);
                q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;

                client_score_deaths(c);
            }
        }
    }

    /*
     * THE ACTOR-TO-MONSTER WRITE-BACK USED TO BE HERE, AND THAT IS WHY NOTHING
     * COULD BE SHOT.
     *
     * The pair is a sync, not a copy: `q2_actor_from_monster` rebuilds the
     * actor from the monster at the top of the frame (and `q2_actor_init`
     * zeroes health before reassigning it), and `q2_actor_to_monster` carries
     * the damage back. Draining it HERE — before the two `q2_sim_fire` sites
     * below — meant every hitscan hit landed on an actor that was overwritten
     * from the undamaged monster on the very next frame. Measured against
     * BASE1 creature 3: 200 machinegun shots reported 200 hits and left the
     * creature on 40 of 40 hp, with the actor holding the 24 hp nobody read.
     *
     * The blaster hid it. Projectiles are resolved by `q2_sim_combat_tick`,
     * which runs INSIDE `q2_sim_advance` — between the two syncs — so bolts
     * always counted and only the four hitscan weapons were inert.
     *
     * It is drained below instead, after the last thing in the frame that can
     * damage a creature. See client_drain_creature_damage.
     */

    /*
     * What the items did while that ran. Immediately after the tick, because
     * the event list is cleared at the top of the next one.
     *
     * AND ONLY IF ONE RAN. The list is cleared at the top of a tick and this is
     * the only thing that drains it, so a frame on which the sim did not tick
     * finds the PREVIOUS tick's events still sitting there — and used to play
     * every one of them again. At 90 fps against a 25 Hz tick that is every
     * footstep, pain grunt and pickup heard three or four times, with the count
     * wobbling as the frame rate did. It is also why the sixteen dynamic light
     * slots were being refilled from the same events several times a frame.
     */
    /*
     * AND AFTER THE WEAPON, NOT BEFORE IT. This drain used to sit above the
     * view-weapon block, and the muzzle flash never reached the light world
     * once: `q2_sim_fire` is called from inside `client_advance_view_weapon`,
     * so the flash was appended to a list that had already been walked and was
     * thrown away by the next tick's clear. The machinegun and the chaingun
     * have carried a muzzle light for as long as `q2_weapon_has_muzzle_light`
     * has existed and not one of them was ever seen — which is why the timed
     * shotgun flashes looked dormant when they were merely last in a queue
     * nobody read.
     *
     * Still immediately after the tick in the sense that matters — nothing
     * between here and there clears the list, and the next tick's clear is a
     * whole frame away.
     */
    /*
     * The weapon in the hands, advanced on the same clock. The selection comes
     * from the simulation, but the SWAP does not happen when the selection
     * changes — it happens when the lower clip has run and the 70-tick countdown
     * has expired, which is the machine's job, not this caller's.
     */
    {
        int pi;
        int saved = c->sim[0].cur_player;

        for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
            if (pi > 0 && (!c->mp_enabled || !c->sim_ready[pi]))
                continue;
            q2_sim_select_player(&c->sim[0], pi);
            if (c->mp_enabled)
                client_targets_for(c, pi);
            q2_combat_scan_who = c->mp_enabled ? pi : Q2_COMBAT_SCAN_OTHER;
            /*
             * A CORPSE HOLDS A STILL GUN. The view-weapon driver 0x8004EE0C
             * has exactly one call site in the whole image, 0x8003AD98, and
             * that is inside the player think — which `player_die` replaces
             * with the corpse think at 0x800396AC, so the driver is never
             * reached again for that entity (playerdeath.h says this of the
             * pad read in the same breath; the pad half was already honoured
             * in sim.c and this half was not).
             *
             * The attack was already gated, because the sim hands a dead
             * player a neutral input, so nothing was firing. What still ran
             * was the MACHINE: the lower clip kept playing, the 70-tick swap
             * countdown kept running, and the frame sounds kept draining, so
             * a dead player's gun went on animating in his hands and could
             * still change weapon under him.
             */
            if (!(c->sim[0].player[pi].ent2_flags & Q2_ENT2_DEAD))
                client_advance_view_weapon(c, view_attack[pi], dt);
            client_score_deaths(c);
        }
        q2_sim_select_player(&c->sim[0], saved);
        if (c->mp_enabled)
            client_targets_for(c, saved);
        q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;
    }

    if (ticked)
        client_entity_events(c);

    /* Damage tracking is per player, like the viewport's flash tile
     * (0x8003AE10 writes the same view+672 that 0x80076764 draws). */
    if (c->hud_ready) {
        int pi;
        /* [0x800B2DB4] for this drawn frame: the ring counts frames and the
         * centre line counts these (hud.h). */
        s32 frame_dt = (s32)((double)dt * 300.0 + 0.5);

        if (frame_dt < 1) frame_dt = 1;
        if (frame_dt > Q2_SCREEN_DT_MAX) frame_dt = Q2_SCREEN_DT_MAX;

        for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
            q2_hud *hud = &c->hud[pi];
            q2_inventory *inv;

            if (pi > 0 && (!c->mp_enabled || !c->sim_ready[pi]))
                continue;
            inv = pi == c->sim[0].cur_player ? &c->sim[0].combat.inv
                                            : &c->sim[0].pcombat[pi].inv;
            q2_hud_tick(hud, 1, frame_dt);

            /*
             * THE POWERUP TINT, and it goes FIRST because 0x8003A2F4 does:
             * FlashPowerUp runs near the top of the player think and the
             * health/armour compare at 0x8003AE10 runs near the bottom, both
             * writing the same view+672. A frame in which a quadded player is
             * also hurt therefore ends on the grey damage tile, and the purple
             * comes back on the next frame — which it can, because the tint is
             * re-armed every frame rather than left to count down.
             *
             * Per RENDERED frame rather than per tick, for the same reason the
             * tile is: strength 1 means one drawn frame of colour, so a raise
             * that only happened on sim ticks would strobe at whatever fraction
             * of the frame rate the accumulator happens to be running at.
             */
            {
                const s32 until[Q2_HUD_POWERUP_SLOTS] = {
                    inv->quad_until, inv->invuln_until,
                    inv->enviro_until, inv->breather_until
                };
                q2_hud_powerup_flash pf =
                    q2_hud_powerup_flash_tick(until, c->sim[0].level_time,
                                              &inv->powerup_quiet_until,
                                              &inv->powerup_warn_secs);

                if (pf.tint) {
                    q2_screen_flash_set(&c->screen, pi, pf.rgb,
                                        Q2_HUD_POWERUP_STRENGTH,
                                        Q2_HUD_POWERUP_MODE);
                    c->powerup_tints[pi]++;
                }
                if (pf.warn) {
                    /* 0x800398CC picks [0x800B28B0] for quad and 0x80039918
                     * picks [0x800B28AC] for the other three; both are item
                     * sounds, so they come out of the item table by key.
                     * 0x800399EC passes entity+0x54 — the port's player
                     * position is the FEET, the same stand-in the secret chime
                     * makes, and both are inside a body's width of the ear. */
                    const char *snd =
                        c->item_sound[pf.slot == 0 ? Q2_SND_QUAD
                                                   : Q2_SND_PROTECT];

                    c->powerup_warns[pi]++;
                    if (snd[0])
                        client_play_sound_at(c, snd, c->sim[0].player[pi].pos);
                }
            }

            if (q2_hud_track(hud, inv->health, inv->armour)) {
                q2_screen_flash_set(&c->screen, pi, hud->flash.rgb,
                                    hud->flash.strength, hud->flash.mode);
                c->hud_flashes[pi]++;
            }

            /*
             * AND THE ABSORB'S OWN WASH — 0x80057B80's green for a hit the
             * power shield ate and 0x80057D24's grey for one ordinary armour
             * took, the two SetWinFade raisers `xrefs 0x8007832C` lists that
             * nothing in this port had ever called. Without them a Power Shield
             * absorbing a hit whole gave the player no feedback at all: it
             * costs cells, not health or armour, so 0x8003AE10's compare above
             * has nothing to see either.
             *
             * DRAINED LAST, and this is a judgement rather than a transcription.
             * The two raisers fire inside T_Damage, which runs from whatever
             * hurt the player, while the two above run inside the player's own
             * think — and which of those the entity sweep reaches first is not
             * established from the addresses read here. What IS established is
             * that all four write the same view+672 and the last one wins, and
             * that the console's armour hit is a grey-128 wash over twenty
             * frames where the HUD's is grey-64 over at most five. The stronger,
             * longer one is the one a player describes, so it is the one this
             * port lets survive the frame.
             *
             * The flag is cleared on the way out for the same reason the console
             * clears it at 0x80057AB0: it is per hit, not per frame, and a fade
             * left latched would re-arm itself every frame forever.
             */
            {
                q2_actor *self = pi == c->sim[0].cur_player
                                     ? &c->sim[0].combat.self
                                     : &c->sim[0].pcombat[pi].self;

                if (self->armour_fade == Q2_ACTOR_FADE_POWER) {
                    q2_screen_flash_set(&c->screen, pi,
                                        q2_actor_fade_power_rgb,
                                        Q2_ACTOR_FADE_POWER_STRENGTH,
                                        Q2_ACTOR_FADE_MODE);
                    c->power_flashes[pi]++;
                } else if (self->armour_fade == Q2_ACTOR_FADE_ARMOUR) {
                    q2_screen_flash_set(&c->screen, pi,
                                        q2_actor_fade_armour_rgb,
                                        Q2_ACTOR_FADE_ARMOUR_STRENGTH,
                                        Q2_ACTOR_FADE_MODE);
                    c->armour_flashes[pi]++;
                }
                self->armour_fade = Q2_ACTOR_FADE_NONE;
            }
        }
    }

    /*
     * The camera is NOT the player's aim. 0x80038260 composes three decaying
     * kicks — firing over 30 ticks, damage over 150, landing over 90 — on top of
     * the aim angles, and 0x8004F41C is where the result becomes the view. Using
     * `player.pitch/yaw/roll` straight, which this did, throws all three away:
     * no recoil, no flinch, and no thump when you land.
     */
    /* `--pitch` is a capture flag and has to be re-applied every frame: the
     * pad's own pitch is zero and would otherwise level the view back out on
     * the frame after the one the load set. */
    if (c->pitch_given)
        c->sim[0].player[0].pitch = c->at_pitch;

    q2_sim_eye(&c->sim[0], eye);
    q2_sim_view_angles(&c->sim[0], view);

    /*
     * Drop a breadcrumb — 0x8007E32C is PlayerTrail_Add's only caller.
     *
     * NOT ON A TIMER. 0x8007E2C4 fetches the previous crumb and 0x8007E2D8
     * skips the add while the player can still see it, so the eight slots are
     * corner markers spanning a whole route. The ten-frame throttle stays
     * because this sits on the render frame while the console's caller sits on
     * the game frame; all it bounds now is how often the question is asked.
     *
     * AND THE CRUMB IS AN ENTITY ORIGIN, NOT THE EYE. The slots are fed
     * straight to `visible` and then to M_MoveToGoal as goal positions, both
     * of which are in the origin frame — this is the same rule the AI tick
     * below already follows. Recording the eye put every crumb
     * Q2_VIEW_STAND - Q2_EYE_BASE = 290 units out in Y, which is further than
     * a run frame's step, so ai_run's arrival test (`dist*dist >= |v|^2`)
     * could never fire and the pursuit stalled on its first waypoint.
     */
    if (c->creatures_ready && (c->frame_index % 10) == 0
        && q2_trail_needs_spot(eye)) {
        s32 crumb[3];

        crumb[0] = c->sim[0].player[0].pos[0];
        crumb[1] = q2_sim_origin_y(c->sim[0].player[0].pos[1]);
        crumb[2] = c->sim[0].player[0].pos[2];
        q2_trail_add(crumb);
    }

    /* The multiplayer session's own frame, on the same clock. */
    if (ticked) client_mp_tick(c, c->sim[0].cur_dt);

    /*
     * The level's own beams, re-queued because the transient pool empties every
     * frame — which is exactly what the console's walk at 0x8002EE38 does with
     * its own list, every frame, for ever. Not on the rotators' clock and not
     * gated on them: a zone with lasers and no rotating brush is ordinary.
     */
    c->laser_drawn = c->no_lasers
                     ? 0
                     : q2_laserbeams_draw(&c->lasers, &c->sim[0].event_rt,
                                          &c->sim[0].fx, &c->sim[0].fx_rng);

    /*
     * AND THE TELEPORT PADS, which is the third of QuakeLoop's three per-frame
     * walks — 0x80038FA4 calls DrawTelePorts immediately BEFORE DrawLaserWalls
     * and DrawLaserBeams, so this goes above the beams in the console's order
     * and the beams' own draws follow. Called unconditionally and on every map,
     * because the console's is: the fifteen shared velocities come out of the
     * generator before NumTelePorts is tested (levelbin.h).
     *
     * The light is added here rather than inside the walk because the light
     * world is the client's and `client_entity_events` has already emptied it
     * for this frame; sixteen dynamic slots is the engine's ceiling and
     * q2_light_add_dynamic drops the seventeenth by itself.
     */
    {
        q2_teleport_frame tp;
        u32 k;

        c->teleport_drawn =
            q2_teleports_draw(&c->teleports, &c->sim[0].triggers,
                              &c->sim[0].fx, &c->sim[0].fx_rng,
                              c->sim[0].level_time, &tp);

        if (c->lights_ready) {
            for (k = 0; k < tp.light_count; k++) {
                if (q2_light_add_dynamic(&c->light_world, tp.light[k].pos,
                                         tp.light[k].rgb, tp.light[k].inner,
                                         tp.light[k].outer,
                                         Q2_FX_TELEPORT_LIGHT_STYLE,
                                         Q2_FX_TELEPORT_LIGHT_SIZE))
                    c->teleport_lights++;
            }
        }
    }

    /*
     * The 1/300 s clock three separate subsystems run on, and each is now
     * gated on its OWN readiness rather than on the rotators'.
     *
     * All three used to sit inside `if (c->rotators_ready)`, which is a
     * dependency none of them has: q2_rotators_build is reached only when both
     * q2_events_parse_common and q2_userfuncs_parse succeed, so a map whose
     * rotator build failed silently stopped every door, every lift, and the
     * script's own deferred-timer clock along with them.
     */
    /*
     * AND IT IS THE SIM'S CLOCK, not the frame's.
     *
     * These used to run every rendered frame on `dt * 300`, while the player's
     * own move runs inside q2_sim_advance, which only steps once the
     * accumulator reaches Q2_DT_NOMINAL. At 60 Hz that is every second or third
     * frame, so a mover advanced two or three times between consecutive player
     * moves — and `q2_move_target.dy`, which is the vertical motion the sweep
     * makes the player RELATIVE TO, only ever carried the last slice of it.
     * That is what a lift you cannot ride feels like, and it got worse the
     * faster the machine.
     *
     * Running them on the tick the sim actually took makes the door, the script
     * clock and the player agree on how much time has passed.
     */
    if (ticked) {
        s32 ticks = c->sim[0].cur_dt;
        if (ticks < 1) ticks = 1;

        /* The rotating brushes. */
        if (c->rotators_ready)
            c->rot_moved += q2_rotators_tick(&c->rotators, ticks);
        q2_sim_scene_solids_update(&c->sim[0], &c->rotators);

        /* The script's own clock, which is what a TIMER's deadline is measured
         * against. It has no readiness flag of its own and never needed one. */
        q2_event_rt_advance(&c->sim[0].event_rt, (s32)ticks);

        /*
         * And the doors and lifts. `player_keys` gates a locked door; the
         * inventory's low twelve bits are the keys the script tests
         * (inventory.h), which is the same field ONKEYDO reads.
         *
         * The hull follows the tick immediately, because the sweep the player
         * is about to run has to see the door where it is NOW — 0x80051EC0 is
         * called from the mover's own per-frame handler for the same reason.
         */
        /*
         * The pop-up's own countdown. A HELPCOMPUTER is a DELAYED raise —
         * 0x800213B0 arms `delay * frame_dt * 2` and 0x80021830 counts it down
         * by the frame's dt — so this has to run while the world is running,
         * which is exactly when the pop-up is NOT up.
         */
        if (q2_briefing_popup_tick(&c->popup, ticks, c->sim[0].level_time,
                                   false, false))
            c->popup_opens++;

        if (c->movers_ready) {
            client_mover_ctx bctx;

            bctx.c = c;
            c->mover_moved +=
                q2_movers_tick_blocked(&c->movers, ticks,
                                       (u16)(c->sim[0].combat.inv.flags
                                             & 0x0FFFu),
                                       client_mover_blocked, &bctx);
            q2_sim_movers_update(&c->sim[0], &c->movers);

            /*
             * A SHOOTABLE LEAF whose hit points ran out. The sim queues the
             * event item; opening it is this side's business because the mover
             * set is here. `q2_movers_trigger_item` is the same entry a script
             * uses, so a shot and a switch open a door the same way — and a
             * MOVER_C's two leaves both go, which is what "every mover built
             * from this item" is for.
             */
            {
                u32 q;

                for (q = 0; q < c->sim[0].breakable_open_count; q++) {
                    u32 n = q2_movers_trigger_item(&c->movers,
                                                   c->sim[0].breakable_open[q]);
                    if (n) {
                        c->mover_triggers += n;
                        c->breakable_opened++;
                    }
                }
                c->sim[0].breakable_open_count = 0;
            }
        }

        /*
         * A `func_explosive` that came apart this tick, wherever it came from —
         * a shot, or a script item. The sim spawns the debris and the blast
         * itself; the geometry swap has to land here, because the hide array is
         * this side's.
         *
         * Outside the `movers_ready` arm above on purpose: an explosive has
         * nothing to do with a mover, and a zone with no doors still has these.
         */
        {
            s16 node;
            u8  hidden;

            while (q2_sim_next_node_vis(&c->sim[0], &node, &hidden)) {
                if (!c->node_hidden || node < 0 ||
                    (u32)node >= c->node_hidden_count)
                    continue;
                if (c->node_hidden[node] != hidden) {
                    c->node_hidden[node] = hidden;
                    c->explosive_vis++;
                }
            }
        }

        /*
         * THE EXPLOSION'S OWN BANG. Every spawn that bound the `Explosion`
         * model ends with SoundByCreature(Sound_Explosion, ent) at
         * 0x8005AA54 — `wep_grenlx1a`, at the entity, which is the point the
         * spawn was handed (modelent.h). For a rocket or a grenade it is the
         * only sound the detonation makes; for a brush it is the first of
         * two, and this drain runs first because PSXEXPLODE's spawn
         * (0x80026950) comes before its report (0x8002695C).
         *
         * Counted on the bank, not on a voice starting, for the reason the
         * report below gives.
         */
        {
            s32 at[3];

            while (q2_sim_next_explosion_sound(&c->sim[0], at)) {
                q2_vag vag;

                if (client_find_sound(c, Q2_MODEL_ENT_SOUND, &vag))
                    c->explosion_sounds++;
                else
                    c->explosion_sounds_missed++;

                client_play_sound_at(c, Q2_MODEL_ENT_SOUND, at);
            }
        }

        /*
         * AND WHAT A DETONATION SOUNDS LIKE. `wep_grenlx1` at the box centre —
         * 0x8002695C, whose handle at 0x800B27F8 this is the only reader of in
         * the whole executable. Positioned rather than flat, because the
         * console passes the same point it passes the explosion.
         */
        {
            s32 at[3];

            while (q2_sim_next_blast(&c->sim[0], at)) {
                q2_vag vag;

                /*
                 * COUNTED ON WHETHER THE BANK HAS IT, not on whether a voice
                 * started — the same trap the creature sounds fell into above.
                 * `client_play_sound_at` returns false when `c->audio` is NULL,
                 * which is every headless run, so counting its return would
                 * report the absence of an audio device as a missing sound.
                 *
                 * BASE0 carries `wep_grenlx1a22K` and the key is the truncated
                 * twelve `wep_grenlx1a`, which is exactly the prefix rule
                 * client_find_sound documents.
                 */
                if (client_find_sound(c, Q2_EXPLOSIVE_SOUND, &vag))
                    c->explosive_sounds++;
                else
                    c->explosive_sounds_missed++;

                client_play_sound_at(c, Q2_EXPLOSIVE_SOUND, at);
            }
        }

        /*
         * AND WHAT A BREAKING PANE SOUNDS LIKE. The same arrangement one
         * handler along: PSXGLASS's tail plays the handle at 0x800B27FC
         * (`Sound_GlassSmash`, the twelve-byte key `amb_brkglas` at
         * 0x800ABCC8) through 0x8002A4A4, positioned at the centre of the
         * pane's Scene node box — the same point the shatter debris comes out
         * of. The sim queues the position because it knows where the pane was;
         * the mixer and the map's bank are this side's.
         *
         * Outside `movers_ready` for the reason the blast is: a window has
         * nothing to do with a door, and a zone with no movers still has them.
         * Every breaking window in the game was silent until this drain
         * existed — the queue filled to eight and dropped the ninth, which is
         * harmless and completely invisible.
         */
        {
            s32 at[3];

            while (q2_sim_next_glass_smash(&c->sim[0], at)) {
                q2_vag vag;

                /*
                 * COUNTED ON WHETHER THE BANK HAS IT, the same trap as the
                 * blast above: `client_play_sound_at` returns false when
                 * `c->audio` is NULL, which is every headless run, so counting
                 * its return would report a missing audio device as a missing
                 * sample. BASE1's SNDVRAM entry 29 is `029_amb_brkglas`.
                 */
                if (client_find_sound(c, Q2_GLASS_SOUND, &vag))
                    c->glass_sounds++;
                else
                    c->glass_sounds_missed++;

                client_play_sound_at(c, Q2_GLASS_SOUND, at);
            }
        }

        if (c->movers_ready) {

            /*
             * AND WHAT THE TRANSITIONS SOUND LIKE. The mover set asks; this is
             * the only place that knows where a mover's box is, and the
             * console positions every one of these calls at the CENTRE of that
             * box (with the live displacement added on the closing arm), so
             * the sound follows the door.
             *
             * THE OBJECT'S OWN BOX, not the collision volume built from it.
             * Each call reads the bbox of the Scene node at obj+0x38 and halves
             * its corners (the button's at 0x80029D58..0x80029E14), and the
             * door's adds obj+0x12..0x16 — which is zero on every arm but the
             * closing one, so adding the live displacement is the console's
             * arithmetic on all of them. The sim volume is the same point for
             * a door, but not for a CAGE LIFT, whose volume is reshaped into a
             * ceiling or floor slab (sim.c), and not for a BUTTON, whose click
             * comes from where it rests, not where it has been pushed to.
             */
            {
                u32 mi;

                for (mi = 0; mi < c->movers.count; mi++) {
                    s8 which = q2_mover_take_sound(&c->movers, mi);
                    const q2_mover *m = &c->movers.movers[mi];
                    s32 at[3];
                    bool have;

                    if (which < 0 || which >= Q2_MVSND_COUNT)
                        continue;

                    have = m->part_count > 0 &&
                           client_node_centre(c, m->node[0], at);
                    if (have && !m->is_button) {
                        s32 d[3];
                        int k;

                        q2_mover_displacement(m, d);
                        for (k = 0; k < 3; k++)
                            at[k] += d[k];
                    }

                    /*
                     * NO BOX, NO SOUND.
                     *
                     * A mover whose object slots all resolved to -1 belongs to
                     * ANOTHER ZONE — it has no collision volume in this one and
                     * nothing to be heard from. Falling back to the
                     * listener-local call played it at full volume in the
                     * player's ear, so a door somewhere else in the map
                     * thudded as if it were in the room. That is what the
                     * "door sounds are wrong" report is: not the wrong sound —
                     * 0x80025A5C really does play pt1__strt for a linear door —
                     * but the right one, unattenuated, for a door that is not
                     * there.
                     */
                    if (!have)
                        continue;

                    /*
                     * COUNT THE VOICE, NOT THE TRANSITION. This used to
                     * increment unconditionally, and `client_play_sound_at`
                     * returns false whenever there is no audio device — which
                     * is EVERY headless run. So "N sounds" was reporting
                     * transitions reached, and a name missing from the map's
                     * bank looked identical to one that played.
                     */
                    if (client_play_sound_at(c, q2_mover_sound_name[which], at))
                        c->mover_sounds++;
                    else
                        c->mover_sounds_missed++;
                }
            }

            /*
             * AND WHAT A LOCKED DOOR SAYS.
             *
             * The refusal arm is not just a noise: 0x80025870 hands the door's
             * own key mask to 0x800254EC, which switches it onto one of eleven
             * names, formats "You need the %s" and puts the sentence on the
             * centre line through 0x80043570. The port played msc_keytry and
             * printed nothing, so a player at a locked door learned that it was
             * locked but never which of the disc's eleven keys opens it.
             *
             * Drained here for the same reason the sounds are: mover.c has no
             * HUD. Player 0's HUD, because player 0's inventory is what the
             * tick above gated the lock on.
             *
             * DEVIATION: the line is laid out in the console's own 512x248
             * space, which is exactly what the single-screen draw uses
             * (q2_hud_ctx_centre_in). In split-screen the overlay is built per
             * view, so a prompt raised here sits at the full-width centre
             * rather than that view's — there is no console behaviour to match,
             * since the console has one screen and one key inventory.
             */
            {
                u32 mi;
                q2_hud_ctx kctx;

                q2_hud_ctx_default(&kctx, Q2_HUD_SPACE_W, Q2_HUD_SPACE_H);
                for (mi = 0; mi < c->movers.count; mi++) {
                    u16 need = q2_mover_take_key_request(&c->movers, mi);

                    if (!need)
                        continue;
                    c->key_prompts++;
                    if (c->hud_ready && c->hud_tables_ready)
                        q2_hud_need_key(&c->hud[0], &c->hud_tables, &kctx,
                                        need);
                }
            }

            /*
             * AND THE TRAIN'S TWO, which are not sounds at all: SetShockData
             * for every window, from the centre of the platform's box plus
             * its displacement, full inside 2048 and nothing past 4096
             * (0x8002C778, 0x8002C8D8; mover.h). Taken every tick so the
             * field cannot latch.
             */
            {
                u32 mi;

                for (mi = 0; mi < c->movers.count; mi++) {
                    const q2_mover *m = &c->movers.movers[mi];
                    u8 id = q2_mover_take_travel_sound(&c->movers, mi);
                    s32 at[3], d[3];
                    s32 strength;
                    int k;

                    if (id == Q2_MOVER_TRAVEL_MOVE_ID) {
                        c->train_move_calls++;
                        strength = Q2_MOVER_TRAVEL_MOVE_VOL;
                    } else if (id == Q2_MOVER_TRAVEL_STOP_ID) {
                        c->train_stop_calls++;
                        strength = Q2_MOVER_TRAVEL_STOP_VOL;
                    } else {
                        continue;
                    }

                    if (m->part_count == 0 ||
                        !client_node_centre(c, m->node[0], at))
                        continue;
                    q2_mover_displacement(m, d);
                    for (k = 0; k < 3; k++)
                        at[k] += d[k];
                    client_shock(c, Q2_SHOCK_ALL_PLAYERS, id, strength, at,
                                 Q2_MOVER_TRAVEL_NEAR, Q2_MOVER_TRAVEL_FAR);
                }
            }

            /*
             * AND THE ROTATING HATCHES, which had no sound path at all.
             *
             * pt1__mid and pt1__end have exactly ONE reader each in the whole
             * image — 0x8002B3DC and 0x8002B534, both inside ROTHATCH's handler
             * 0x8002B250 — so the start/loop/stop trio belongs to the rotating
             * hatch alone, and this port played none of it. That is the residue
             * behind "door sounds use platform sounds": the hatches that should
             * open with a motor were silent, leaving only the linear door's
             * single pt1__strt to be heard anywhere.
             */
            {
                u32 ri;

                for (ri = 0; ri < c->rotators.count; ri++) {
                    q2_rotator *r     = &c->rotators.rotators[ri];
                    s8          which = q2_rotator_take_sound(&c->rotators, ri);
                    s8          loop;
                    s32         at[3];
                    bool        have_at;

                    /* A rotator turns a Scene node, and that node is where the
                     * sound is. An unbound one stays silent rather than falling
                     * back to a listener-local play, which is the mistake the
                     * movers above just had corrected. */
                    have_at = client_node_centre(c, r->node, at);

                    /*
                     * AND THE MOTOR, WHICH USED TO BE LEFT OUT ON PURPOSE.
                     *
                     * The reason was honest and is now gone: this mixer had no
                     * way to stop one voice, so a started `pt1__mid` would have
                     * run for the rest of the level and the silence was the
                     * lesser wrong. It has one now — client_voice_stop, the
                     * port's 0x8007398C — guarded by the same tag the console's
                     * is, so a handle whose slot has since been taken over
                     * stops nothing rather than cutting a stranger's sound.
                     *
                     * 0x8002B3DC's a2 is `s1 + 52`, a field on the rotator
                     * object itself, so the handle is kept there here too.
                     *
                     * The two arms are exclusive in practice — arrival clears
                     * `running` and only a stopped rotator can be re-triggered
                     * — so the order of this else-if is not load-bearing.
                     */
                    while ((loop = q2_rotator_take_loop(&c->rotators, ri))
                           != Q2_ROTLOOP_NONE) {
                        if (loop == Q2_ROTLOOP_STOP) {
                            client_voice_stop((client_voice *)r->loop_voice,
                                              r->loop_serial);
                            r->loop_voice  = NULL;
                            r->loop_serial = 0;
                            c->rot_loop_stops++;
                            continue;
                        }
                        if (!have_at)
                            continue;
                        {
                            q2_vag vag;

                            /* Counted on the bank having it, not on a voice
                             * starting: client_play_sound_at is false for every
                             * headless run, and a counter on that would report
                             * the absence of an audio device as a missing
                             * motor. The same trap, and the same answer, as the
                             * explosive report above. */
                            if (client_find_sound(
                                    c, q2_rot_sound_name[Q2_ROTSND_MID], &vag))
                                c->rot_loop_starts++;
                            else
                                c->mover_sounds_missed++;

                            if (client_play_sound_at(
                                    c, q2_rot_sound_name[Q2_ROTSND_MID], at)) {
                                r->loop_voice  = c->voice_last;
                                r->loop_serial = c->voice_last_serial;
                                c->rot_sounds++;
                            }
                        }
                    }

                    if (which < 0 || which >= Q2_ROTSND_COUNT || !have_at)
                        continue;

                    if (which == Q2_ROTSND_BUTTON) {
                        q2_vag vag;

                        if (client_find_sound(c, q2_rot_sound_name[which],
                                              &vag))
                            c->rot_clicks++;
                    }

                    if (client_play_sound_at(c, q2_rot_sound_name[which], at))
                        c->rot_sounds++;
                    else
                        c->mover_sounds_missed++;
                }
            }

            /*
             * NO TOUCH PASS. `client_movers_touch` used to be called here, on
             * the reading that MOVER_A's +20 halfword is "also opens on touch".
             * It is not: 0x80025E98 tests it for > 0 and installs 0x8002F050 at
             * object+0x24, and that handler subtracts an AMOUNT from it and
             * opens only when it reaches zero. It is HIT POINTS. The five
             * readers of object+0x24 (through 0x8002EF1C) all sit immediately
             * after a TRACE and read the trace result's hit-box index — so the
             * door opens by being SHOT, not by being walked into.
             *
             * Walking into the door's own leaf was also self-defeating: the
             * same boxes are solid, so the only way to overlap one is to graze
             * its edge, which is precisely the "takes some moving around"
             * the report describes. The real mechanism is the trigger volume in
             * front of the door, and that is fixed in update_triggers.
             */
        }

        /* And the pads, once this tick's requests are all in (shock.h). */
        client_shock_tick(c, ticks);
    }

    /*
     * WHAT THE FRAME'S SHOTS DID — drained here because this is below every
     * site that can damage a creature: the sim tick (projectiles, splash and
     * the creatures' own attacks), the view weapon's trigger, and the fire
     * driver's per-animation-frame shots. See the note at the old site above.
     */
    if (c->creatures_ready && c->cre_actor) {
        u32 i;
        for (i = 0; i < c->creatures.set.count; i++)
            client_cre_drain_one(c, i);
    }

    /*
     * The AI, on its own clock and looking at where the player is now — as an
     * entity ORIGIN, not an eye. `q2_visible` adds the sight client's view
     * height itself, so handing it the eye added the view height twice and put
     * the player end of every sight line 400 units into the ceiling.
     */
    {
        s32 player_origin[3];
        s32 flash_dt = (s32)((double)dt * 300.0 + 0.5);

        /*
         * 0x8007EE20: the object handler drops the muzzle bit before the think
         * that may raise it again, every frame — so a flash is the frame of
         * its round and no other. And gp+16996, the flash's turn, takes twice
         * the frame delta once per new frame (0x800310E0..0x800310F8).
         */
        q2_fx_flash_clear(&c->sim[0].fx);
        if (flash_dt < 1) flash_dt = 1;
        if (flash_dt > Q2_SCREEN_DT_MAX) flash_dt = Q2_SCREEN_DT_MAX;
        c->flash_spin += 2 * flash_dt;

        player_origin[0] = c->sim[0].player[0].pos[0];
        player_origin[1] = q2_sim_origin_y(c->sim[0].player[0].pos[1]);
        player_origin[2] = c->sim[0].player[0].pos[2];
        client_creatures_tick(c, dt, player_origin);
    }

    /*
     * THIS FRAME'S DEATH DROPS — 0x80020E24, which the frame routine calls
     * once a frame (0x80038FE0). Here because it is below every site a
     * creature can die at this frame: the sim tick, the actor sync above where
     * q2_monster_damage_reaction runs the death-use pass that records them, and
     * the AI tick. Four deaths between two flushes is the queue's whole
     * capacity (0x80020D6C), so the cadence is part of the behaviour.
     *
     * The picker reads the player's weapon (0x8002069C `lh` client+0x66,
     * 0x800206A0 `lw` client+0x68). The port's `combat.weapon_id` IS the
     * console's selected weapon, 1-based — the HUD reads it as client+102 —
     * and the bitmask is already `1 << (id - 1)`. NOT `inv.current_weapon + 1`:
     * that byte is the inventory's zero-based record, which the carousel
     * (q2_sim_cycle_weapon) never writes, so after one weapon change it names
     * the wrong gun and the Gunner's arm (0x80020730) would test it.
     */
    q2_monster_set_player_weapon((s16)c->sim[0].combat.weapon_id,
                                 (u32)c->sim[0].combat.inv.weapons);
    q2_monster_drop_flush();

    /*
     * DEATH — the whole chain now, not only the frame it starts on.
     *
     * What used to be here was the first tick of it: health crossed zero, page
     * 41 opened, and that was the end of the subject. The console has four more
     * functions after the handler and they are what a body actually does — the
     * death move, the five seconds it lies there, the fade, and the two
     * different endings single player and deathmatch give it. playerdeath.h
     * carries the addresses; this is where they are driven from.
     *
     * It is driven from the client and not the sim for the reason it always
     * was: the sim has no menu, and in single player the page IS the death
     * sequence on this console.
     *
     * THE SCORING IS NOT HERE ANY MORE. `client_score_deaths` above already
     * walks all four players and attributes each death from that player's own
     * `last_attacker`, so doing it again here was scoring a local player's
     * death TWICE — once correctly and once with a hard-coded killer of -1,
     * which also charged the victim a suicide frag for being shot.
     */
    {
        const s32 tick = (step > 0) ? step : 0;
        int       pi;

        for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
            q2_player_death *d = &c->death[pi];
            q2_player       *p = &c->sim[0].player[pi];
            const bool       local = (pi == c->sim[0].cur_player);
            const q2_actor  *a = local ? &c->sim[0].combat.self
                                       : &c->sim[0].pcombat[pi].self;
            const s16 health = local ? c->sim[0].combat.inv.health
                                     : c->sim[0].pcombat[pi].inv.health;

            if (pi > 0 && (!c->mp_enabled || !c->sim_ready[pi]))
                continue;

            /*
             * The engine's gate, 0x8003ADB8, against the chain's OWN copy of
             * entity+0x10C — which is where the corpse think raises the DEAD
             * bit. The sim keeps a second copy of the same bit for its movement
             * gates and raises it a tick earlier (see update_pain); testing
             * that one here would shut the gate before it ever opened.
             *
             * The STAGE is in front of it because the bit alone is not enough
             * on this side. The original's one-shot is the think swap: the
             * player think that runs the gate is replaced by the corpse think,
             * so it cannot run a second time whatever the bit says. Here the
             * loop is the same loop every tick, and the bit is not raised until
             * the body's first tick — which the `continue` below skips. Without
             * the stage the gate re-opened every frame: measured on POWER2, one
             * Berserk produced 300 deaths in 900 frames.
             */
            if (d->stage == Q2_PDEATH_ALIVE) {
                q2_player_death_event ev;

                if (!q2_player_should_die(health, d->ent2))
                    continue;

                q2_player_die(d, a->last_attacker, a->last_mod, pi,
                              c->mp_enabled,
                              (p->ent.flags & Q2_ENT_UNDERWATER) != 0, &ev);

                Q2_INFO("player %d died: killer %d, means %d, %s", pi,
                        (int)d->killer, (int)d->mod,
                        ev.cried_out ? (ev.drowned ? "drowned" : "cried out")
                                     : "silently");

                if (ev.death_page && !c->menu.open && !c->mcard_open) {
                    /* 0x8001D774 writes the middle row from the continues
                     * count on the way in, so it has to be there first. */
                    q2_menu_set_resupplies(&c->menu, c->continues);
                    q2_menu_open(&c->menu);
                    q2_menu_goto(&c->menu, Q2_PAGE_DEATH);
                }
                if (ev.abandon_armed)
                    c->death_abandon = Q2_PDEATH_ABANDON_TICKS;

                /* The rendered Male2 cursor raises ANIM_WRAPPED at the last
                 * death key. A map with no player model still needs the old
                 * fail-safe or a deathmatch corpse could never settle. */
                if (c->mp_enabled && !c->player_anim_base_ok)
                    q2_player_death_anim_ended(d);
                continue;          /* the killing tick is not a body tick */
            }

            {
                const q2_pdeath_stage was = d->stage;

                /* The body's effect bytes, for the dissolve gate respawn_think
                 * runs first (0x8003E244 jal 0x8005B2A8; playerdeath.h). */
                q2_player_death_tick_fx(d, health, tick, c->mp_enabled,
                                        client_mp_rng(c), a->effect);

                /* The sim keeps its own copy for the movement and camera
                 * gates; this is the chain handing it the bit. */
                p->ent2_flags |= (d->ent2 & Q2_ENT2_DEAD);

                if (was != d->stage && d->stage == Q2_PDEATH_FADING)
                    c->death_bodies++;
                if (was != d->stage && d->stage == Q2_PDEATH_GIBBED)
                    c->death_gibs++;
            }

            /*
             * And back in. Every mode but VERSUS lets a dead player return
             * (0x8003DEB4); the engine wants the fire button on a FRESH press
             * (0x8001FC50) and the menu closed (0x800AE8B4) as well, and those
             * two are the client's. The body has to have finished falling
             * first, which is the same test the corpse think makes.
             */
            if (c->mp_enabled && c->mp.end == Q2_MP_RUNNING &&
                d->stage != Q2_PDEATH_DYING &&
                q2_mp_may_respawn(&c->mp) && !c->menu.open && !c->mcard_open) {
                bool ask = ticked &&
                    (c->mp_input[pi].buttons & Q2_BTN_ATTACK_PRESS) != 0;
                if (c->demo && c->mp_stage && pi > 0)
                    ask |= d->stage == Q2_PDEATH_FADING ||
                           d->stage == Q2_PDEATH_GONE ||
                           d->stage == Q2_PDEATH_GIBBED;

                if (ask)
                    client_mp_respawn(c, pi);
            }
        }

        /*
         * 0x800B2A10, spent by 0x80041D30: a single-player death that is never
         * answered raises game-state 8, which is 0x8004149C — "load
         * MagazineExtrQFront". The console walks back to the front end on its
         * own after 1200, and nothing in this port had ever done so.
         */
        if (q2_player_abandon_tick(&c->death_abandon, tick)) {
            Q2_INFO("death: unanswered for %d ticks — back to the front end",
                    Q2_PDEATH_ABANDON_TICKS);
            /* RAISED, NOT ACTED ON. Going to the front end loads a map, and
             * the load frees the zone this function is still standing in —
             * the same reason the zone gate is deferred. The main loop takes
             * it beside the other transitions. */
            c->death_abandoned = true;
        }
    }

    if (c->mp_enabled) {
        int pi;
        for (pi = 1; pi < c->mp.player_count; pi++)
            client_extra_camera(c, pi);
    }
    c->cam.pos[0] = eye[0];
    c->cam.pos[1] = eye[1];
    c->cam.pos[2] = eye[2];

    /*
     * THE DEATH CAM. 0x80038618's other branch, which this port did not have.
     *
     * A live player's camera takes all three angles from the view. A DEAD one
     * keeps the yaw and pitch it died with — the corpse is not looking around —
     * and rolls, easing toward -384 on the 4096-step circle, about 34 degrees.
     * That roll is the whole of the effect: the horizon tips as the body goes
     * down. The position still comes from the eye, so the view also settles as
     * the corpse falls.
     *
     * The ease is the engine's short-way angle lerp (0x8006FAC8): take the
     * difference modulo the circle, and go whichever way round is shorter.
     *
     * AND ITS FRACTION IS `50 * dt`, NOT A CONSTANT. This used to pass a flat
     * 100, which is what 0x800380D8..0x800380EC gives at about 144 fps and a
     * fifth of what it gives at the console's own PAL rate (dt 12 → 600): the
     * horizon took roughly 46 frames to lie over where the console takes eight,
     * and — because the step was per RENDERED frame with no dt in it — the same
     * death rolled at a visibly different speed on a fast machine and a slow
     * one, where the console's rolls identically at both. The lerp itself was
     * already right, including the +2047 rounding and the 0x800 half-circle
     * test, so the whole of the fix is the fraction; it lives beside the rest of
     * the death chain now (playerdeath.h) rather than open-coded here.
     */
    if (c->sim[0].player[0].ent2_flags & Q2_ENT2_DEAD) {
        s32 dt_ticks = (s32)((double)dt * 300.0 + 0.5);

        if (dt_ticks > Q2_SCREEN_DT_MAX) dt_ticks = Q2_SCREEN_DT_MAX;
        /* view+16 is a halfword on the console (`lh`/`sh` at 0x800380D4 and
         * 0x80038100); the camera carries it as a word, so it is narrowed on
         * the way in and widened back out. */
        c->cam.roll = q2_pdeath_cam_roll((s16)(c->cam.roll & 0xFFF), dt_ticks);
    } else {
        /*
         * THE PLAIN ANGLES. The kick does NOT reach the camera.
         *
         * `screen.h` records why, from the other side: "the wobble is ANGULAR
         * and it moves the VIEW WEAPON, not the camera — 0x80038260 has exactly
         * one caller, 0x8004F404", which is the weapon's own angle sum. A
         * function with one caller cannot also be shaking the view.
         *
         * This used to hand the camera `q2_sim_view_angles`, which is that same
         * kick summed into the player's aim — so the port shook the WORLD where
         * the console shakes the GUN. Firing moved the horizon; taking damage
         * tilted the level. The weapon is given the kick instead, at the site
         * that draws it.
         */
        c->cam.yaw    = c->sim[0].player[0].yaw;
        c->cam.pitch  = c->sim[0].player[0].pitch;
        c->cam.roll   = c->sim[0].player[0].roll;
    }

    /*
     * `--watch`: turn the CAMERA, and only the camera, onto the nearest live
     * creature. The player still walks, shoots and is shot at; what changes is
     * where the view points, which is the one thing that decides whether a
     * creature the frame has already emitted is a creature you can see.
     *
     * It exists because "20 drawn, 4100 faces" says nothing about whether a
     * Soldier is standing in front of you: with an ordering table and no depth
     * buffer, a creature behind a wall is emitted and then painted over.
     */
    if (c->watch && c->creatures_ready) {
        bool holding;
        const q2_monster *best = client_watch_pick(c, eye, false, &holding);

        if (best) {
            s32 to[3];
            /* The framing below is measured from the creature's FEET, which is
             * where its model stands; `pos` is the entity origin. */
            s32 feet_y = best->pos[1] + Q2_EYE_BASE;

            /*
             * Stand in front of it, at head height, looking at it — the
             * inspector's `mob` framing, but of a LIVE creature: this one has
             * thought, turned, and is playing whatever move its AI put it in.
             * The camera moves and nothing else does; the player is still
             * where the simulation left them.
             */
            c->cam.pos[0] = best->pos[0] +
                            ((q2_sin12(best->angles[2]) * 700) >> Q2_FRAC_12);
            c->cam.pos[1] = feet_y - 250;
            c->cam.pos[2] = best->pos[2] +
                            ((q2_cos12(best->angles[2]) * 700) >> Q2_FRAC_12);
            eye[0] = c->cam.pos[0];
            eye[1] = c->cam.pos[1];
            eye[2] = c->cam.pos[2];

            to[0] = best->pos[0] - eye[0];
            to[1] = feet_y - eye[1] - 150;         /* look at the chest */
            to[2] = best->pos[2] - eye[2];

            double horiz = sqrt((double)to[0] * to[0] +
                                (double)to[2] * to[2]);
            double p = atan2((double)to[1], horiz > 1.0 ? horiz : 1.0);

            /*
             * The PLAYER is turned too, not just the camera. Without it the
             * demo fires on a timer into whatever it happens to be facing, so
             * a run could never show whether a shot hits a creature — measured
             * as 135 creature shots against the player and zero damage the
             * other way, which looked like a bug and was only ever the aim.
             */
            /* +Y is down, so a target below the eye needs a positive pitch. */
            c->cam.pitch = (s32)(p * (double)Q2_ANGLE_360 /
                                 (2.0 * 3.14159265358979323846));
            c->cam.roll  = 0;
        }
    }
}

/* Free-fly camera, kept for inspecting geometry without physics in the way. */
static void client_input(client *c, float dt)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    s32 speed = (s32)(4000.0f * dt);
    s32 turn  = (s32)(1500.0f * dt);
    s32 fwd[3], right[3];
    s32 sy, cy;

    if (!keys)
        return;

    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])
        speed *= 4;

    if (keys[SDL_SCANCODE_LEFT])  c->cam.yaw   -= turn;
    if (keys[SDL_SCANCODE_RIGHT]) c->cam.yaw   += turn;
    if (keys[SDL_SCANCODE_UP])    c->cam.pitch -= turn;
    if (keys[SDL_SCANCODE_DOWN])  c->cam.pitch += turn;

    /*
     * And the mouse, which looks the same way here as it does in play: forward
     * is up, which is a POSITIVE pitch (a frame rendered at pitch 500 is looking
     * at the ceiling). There is no tick to scale against in this mode — the
     * camera is moved directly rather than through a rate — so the whole
     * accumulator is spent every frame, and the divisor is the same one play
     * uses so the two feel alike.
     *
     * NOTE that the four keys above pitch the OTHER way round: this camera's
     * up arrow looks down, which it has always done and which disagrees with
     * the player's. Left alone deliberately — it is a keyboard binding, and
     * changing one was not part of adding a mouse.
     */
    {
        /* The angle units a pixel is worth in play, with the dt cancelled out:
         * ((speed + 32) >> 4) * 3 / (4 * DIV). MOUSE SPEED moves this camera
         * exactly as far as it moves the player's. */
        double gain = (double)(((c->settings.v[Q2_SET_MOUSE_SPEED] + 32) >> 4) *
                               Q2_LOOK_SCALE_NUM) /
                      (double)((1 << Q2_LOOK_SCALE_SHIFT) * CLIENT_MOUSE_DIV);

        c->cam.yaw   += (s32)(c->look_acc_x * gain);
        c->cam.pitch += (s32)(c->look_acc_y * gain);
        c->look_acc_x = 0.0;
        c->look_acc_y = 0.0;
    }

    if (c->cam.pitch >  Q2_ANGLE_90) c->cam.pitch =  Q2_ANGLE_90;
    if (c->cam.pitch < -Q2_ANGLE_90) c->cam.pitch = -Q2_ANGLE_90;

    sy = q2_sin12(c->cam.yaw);
    cy = q2_cos12(c->cam.yaw);

    /* Movement stays on the horizontal plane regardless of pitch, which is what
     * a player controller wants and what makes flying around a level bearable. */
    fwd[0]   =  sy; fwd[1]   = 0; fwd[2]   =  cy;
    right[0] =  cy; right[1] = 0; right[2] = -sy;

    if (keys[SDL_SCANCODE_W]) {
        c->cam.pos[0] += (s32)(((s64)fwd[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] += (s32)(((s64)fwd[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_S]) {
        c->cam.pos[0] -= (s32)(((s64)fwd[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] -= (s32)(((s64)fwd[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_D]) {
        c->cam.pos[0] += (s32)(((s64)right[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] += (s32)(((s64)right[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_A]) {
        c->cam.pos[0] -= (s32)(((s64)right[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] -= (s32)(((s64)right[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_E]) c->cam.pos[1] -= speed;
    if (keys[SDL_SCANCODE_Q]) c->cam.pos[1] += speed;
}

/* ------------------------------------------------------------------------- */
/*
 * The menu.
 *
 * The engine it drives is written against the console's button mask, so the
 * keyboard is translated into one rather than the menu being taught about
 * scancodes. That keeps the navigation rules — wrap, skip, press-versus-release
 * — exactly as they were read out of the executable.
 */
static u16 client_menu_pad(const client *c)
{
    const bool *k;
    u16 pad = 0;

    /*
     * A scripted run has to be able to answer a page too. Without this the
     * death screen ends the run: the world freezes behind it, the demo's pad
     * goes to the simulation which is no longer ticking, and every later frame
     * is the same picture.
     *
     * CROSS on a slow cycle is enough — it takes the row the page opens on,
     * which for the death page is RESTART LEVEL.
     */
    if (c && c->demo) {
        if ((c->frame_index % 30) >= 3)
            return 0;
        /*
         * In the FRONT END, alternate CROSS and TRIANGLE rather than pressing
         * CROSS forever. Pressing forward four times leaves the title screen
         * for a game thirty frames in, and a scripted run then never sees the
         * page graph at all — nor the logo's own two thinks, which only differ
         * once you have gone somewhere and come back (levelbin.h). Stepping in
         * and out keeps a headless capture on the screen it is capturing.
         */
        if (c->in_front_end && ((c->frame_index / 30) & 1))
            return Q2_PAD_TRIANGLE;
        return Q2_PAD_CROSS;
    }

    if (c) {
        int pi;
        for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++)
            pad |= (u16)c->gamepads.held[pi];
        if (c->headless) return pad;
    }
    k = SDL_GetKeyboardState(NULL);
    if (!k)
        return pad;

    if (k[SDL_SCANCODE_UP])        pad |= Q2_PAD_UP;
    if (k[SDL_SCANCODE_DOWN])      pad |= Q2_PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT])      pad |= Q2_PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT])     pad |= Q2_PAD_RIGHT;
    if (k[SDL_SCANCODE_RETURN] ||
        k[SDL_SCANCODE_KP_ENTER] ||
        k[SDL_SCANCODE_SPACE])     pad |= Q2_PAD_CROSS;
    if (k[SDL_SCANCODE_BACKSPACE]) pad |= Q2_PAD_TRIANGLE;

    /*
     * And the RIGHT BUTTON, which is TRIANGLE — the console's own back. It is
     * held rather than pulsed so the engine sees the same press-and-release a
     * key gives it, and it costs nothing in play because in play the same
     * button is read through a different function entirely.
     *
     * The LEFT button is not here: what a click means depends on what it landed
     * on, so client_menu_pointer decides and ORs its answer in.
     */
    if (c->mouse_right) pad |= Q2_PAD_TRIANGLE;

    return pad;
}

/* ------------------------------------------------------------------------- */
/* The pointer, over a menu                                                   */
/* ------------------------------------------------------------------------- */
/*
 * Where the console's 512x248 menu block sits inside this buffer. One function
 * because the DRAW does the same arithmetic, and a pointer that disagrees with
 * the picture by a few pixels is a menu whose rows are hit slightly above
 * themselves.
 *
 * On a 240-line NTSC buffer the block lands four lines up, and that is not an
 * approximation: it is what SLUS-00757 did to its own tables. Its page rows
 * are PAL's less four, in its executable and in its QFRONT, and so is the
 * pause screen's status line; the title follows the framebuffer height in both
 * builds, and (240 - 188) / 2 + 10 is PAL's 40 less four too. The one table it
 * left alone, the memory card's SAVE FILE screen, is put back down inside the
 * block (q2_menu_item_y).
 */
static void client_menu_origin(const client *c, int *ox, int *oy)
{
    if (ox) *ox = (c->width  - Q2_MENU_SCREEN_W) / 2;
    if (oy) *oy = (c->height - Q2_MENU_SCREEN_H) / 2;
}

/*
 * Where the finished frame goes in a window of `out_w` x `out_h`: the part of
 * the framebuffer the television shows, and where. SCREEN POSITION is already
 * in the display envs (client_frame installs it), so this only has to say what
 * "unmoved" is — the reset routine's position for this build, which the fit
 * rectangle stands for. One function because the frame and the pointer must
 * agree to the pixel.
 */
static void client_place(const client *c, int out_w, int out_h,
                         q2_screen_placement *pl)
{
    q2_menu_settings reset;
    q2_screen_range neutral, shown;

    /* 0x8001FA18: (0, 24) on PAL, and (0, 0) on the 240-line build. */
    q2_menu_reset_video_for(&reset, c->screen.disp.height);
    q2_screen_range_at(&c->screen, reset.v[Q2_SET_SCREEN_X],
                       reset.v[Q2_SET_SCREEN_Y], &neutral);
    q2_screen_shown_range(&c->screen, &shown);
    q2_screen_place(&c->screen, c->fit, out_w, out_h, &neutral, &shown, pl);
}

/*
 * The pointer in menu space, undoing the two transforms the frame applied on
 * the way out: the placement (client_place, exactly as client_frame blits it)
 * and then the menu block's origin.
 *
 * False when there is no pointer to speak of — headless, or grabbed for
 * mouselook, in which case its motion is look input and its position is
 * meaningless.
 */
static bool client_menu_pointer_pos(const client *c, int *mx, int *my)
{
    int out_w = 0, out_h = 0;
    int ox = 0, oy = 0;
    double fx = 0.0, fy = 0.0;
    q2_screen_placement pl;

    if (c->headless || !c->renderer || !c->pointer_valid || c->mouse_grabbed)
        return false;

    SDL_GetCurrentRenderOutputSize(c->renderer, &out_w, &out_h);
    client_place(c, out_w, out_h, &pl);
    if (!q2_screen_window_to_buffer(&pl, (double)c->pointer_x,
                                    (double)c->pointer_y, &fx, &fy))
        return false;

    client_menu_origin(c, &ox, &oy);

    if (mx) *mx = (int)fx - ox;
    if (my) *my = (int)fy - oy;
    return true;
}

/*
 * One frame of the pointer over `m`, returning the pad bits it is asking for.
 *
 * The division of labour is the point: everything a mouse can say that a pad
 * can also say goes back through the pad, so the navigation rules, the sounds,
 * the press-versus-release split and the page transitions are the ones read out
 * of the executable rather than a second implementation. Only the two things a
 * pad cannot say — land on that row, set that slider to that value — go through
 * menu.c's own pointer entry points.
 */
static u16 client_menu_pointer(client *c, q2_menu *m)
{
    q2_menu_hit hit = {0};
    int  mx = 0, my = 0;
    bool have_pos, have_hit, pressed;
    u16  pad = 0;

    /* A scripted run has no pointer, and letting a stray one move the cursor
     * would make its output depend on where the mouse happened to be. */
    if (!m->open || !m->page || c->demo)
        return 0;

    have_pos = client_menu_pointer_pos(c, &mx, &my);
    have_hit = have_pos && q2_menu_hit_test(m, mx, my, &hit);
    pressed  = c->mouse_left && !c->mouse_left_prev;

    /*
     * Hover, but only while nothing is held: once the button is down the press
     * belongs to the row it started on, however far the pointer then travels.
     * That is what makes a slider draggable and what stops a click that drifts
     * a pixel activating the row below.
     */
    if (have_hit && !c->mouse_left && !c->mouse_right)
        q2_menu_point_at(m, hit.index);

    if (pressed) {
        c->menu_click_index = -1;
        c->menu_click_part  = Q2_MENU_HIT_NONE;

        if (have_hit) {
            q2_menu_point_at(m, hit.index);
            c->menu_click_index = hit.index;
            c->menu_click_part  = (u8)hit.part;

            /* A slider takes its value from the press itself, so the bar jumps
             * to where it was clicked rather than only responding to a drag. */
            if (hit.part == Q2_MENU_HIT_SLIDER)
                q2_menu_set_slider(m, hit.index, hit.value);
        }
    }

    if (c->mouse_left && c->menu_click_index >= 0) {
        switch (c->menu_click_part) {
        case Q2_MENU_HIT_SLIDER: {
            int value;

            /* Tracked by x alone: see q2_menu_slider_at. */
            if (have_pos &&
                q2_menu_slider_at(m, c->menu_click_index, mx, &value))
                q2_menu_set_slider(m, c->menu_click_index, value);
            break;
        }

        /* 0x8001B720: the row reads "LABEL ON OFF" and LEFT means ON because
         * you are moving along it, so aiming at a word is the same press. */
        case Q2_MENU_HIT_ON:
        case Q2_MENU_HIT_PREV:
            pad |= Q2_PAD_LEFT;
            break;
        case Q2_MENU_HIT_OFF:
        case Q2_MENU_HIT_NEXT:
            pad |= Q2_PAD_RIGHT;
            break;

        /*
         * CROSS, HELD. The engine fires most rows on the press and some on the
         * release (0x8001A0D8), and a real click is both — so holding the bit
         * while the button is down and dropping it on release gives each kind
         * the edge it asks for without this having to know which is which.
         */
        default:
            pad |= Q2_PAD_CROSS;
            break;
        }
    }

    if (!c->mouse_left) {
        c->menu_click_index = -1;
        c->menu_click_part  = Q2_MENU_HIT_NONE;
    }

    /* The wheel walks the cursor, one row per notch. */
    {
        int notch = client_wheel_notch(c);

        if (notch > 0)      pad |= Q2_PAD_UP;
        else if (notch < 0) pad |= Q2_PAD_DOWN;
    }

    return pad;
}

/*
 * Push the settings the menu edits into the systems that consume them. The
 * original does this from the menu's own hooks; here it is one place so the
 * effect of a page is visible rather than scattered.
 */
static void client_apply_settings(client *c)
{
    q2_menu_rules rules;

    /* GAME VARIABLES only exist in a multiplayer session, which is exactly
     * when the original enables them (0x8002033C passes 1 there and 0 in
     * single player). */
    q2_menu_apply_variables(&c->settings, c->menu.multiplayer,
                            q2_build_tick_rate(&c->build), &rules);

    c->sim[0].gravity = rules.gravity;

    /*
     * BLAST FORCE, which until now stopped at the menu: the slider on four
     * pages wrote a setting nothing read, and `q2_combat_rules.knockback_mass`
     * -- the port's name for 0x800B3358 -- was never written by anything, so
     * every impulse in the game used the scale for mass 0, exactly half the
     * disc's. Unlike gravity and the tick rate this value is not transformed
     * on the way in: 0x80057F84 reads the halfword the slider stored, and
     * 0x8001C7E4 has no store to it at all, so it carries on both arms.
     */
    c->sim[0].blast_force = rules.blast_force;

    /*
     * The cheat word, which until now never left the menu: `rules.cheats` was
     * computed and dropped, and the only writer of `sim.cheats` was the save
     * loader, so every GAME VARIABLES toggle was a row that changed nothing.
     * 0x8001C698 is the one place the console folds the four toggles into the
     * halfword at 0x800B29EC, and it writes that halfword on BOTH arms: the
     * enabled arm clears it at 0x8001C6CC before ORing the toggles in, and the
     * disabled arm stores zero at 0x8001C7FC. So single player is zeroed here
     * exactly as it is on the disc, beside the gravity and tick rate that the
     * same function resets. NO FALL DAMAGE is bit 0x40 of the word
     * (0x8001C704); the sim keeps it as a flag of its own.
     */
    c->sim[0].cheats         = rules.cheats;
    c->sim[0].no_fall_damage = (rules.cheats & Q2_CHEAT_NO_FALL_DAMAGE) != 0;

    /*
     * WEAPON STAY, which had the same fate as the cheat word above and is fixed
     * the same way — the row was on six page layouts and moved nothing, so a
     * gun still vanished under the first player to touch it.
     *
     * NOT through `q2_menu_rules`. 0x8001C698 folds four toggles into the cheat
     * halfword at 0x800B29EC and never touches 0x800B3360; the GAME VARIABLES
     * row writes that halfword itself (its address is the row's own operand at
     * 0x8009A748) and only 0x8001BE5C / 0x800204B4 clear it. Routing it through
     * `enabled` would model a zeroing the console does not do — and it is not
     * needed, because both readers (0x80037E60, 0x8005988C) test deathmatch
     * first anyway.
     */
    c->sim[0].weapons_stay   = c->settings.v[Q2_SET_WEAPON_STAY] != 0;
    if (rules.tick_rate > 0)
        c->sim[0].dt_per_field = 300 / rules.tick_rate;
    if (c->sim[0].dt_per_field <= 0)
        c->sim[0].dt_per_field = 1;
}

/*
 * The CONTROLLER page, applied — which until now it was not. Nothing anywhere
 * read Q2_SET_PAD_STYLE, so the page was five rows that remembered what you
 * chose and changed nothing, and `look_scheme` sat at the STANDARD A that
 * q2_sim_init writes.
 *
 * WHICH styles the page offers is not the player's choice either: 0x8001C8A8
 * picks [0,3) for a mouse, [3,6) for an analogue pad and [6,9) otherwise, from
 * the CONTROLLER THAT IS CONNECTED. On a PC with USE MOUSE on, the mouse is the
 * connected controller — so the toggle drives the class, and the page offers
 * RIGHT MOUSE / RIGHT MOUSE 2 / HUNTER MOUSE. A gamepad assigned to player one
 * selects the analogue class and exposes styles 3..5. Extra controllers use
 * RIGHT STICK until the front end has per-player controller settings.
 *
 * Cheap enough to run every frame, which is what makes the toggle take effect
 * the moment it is flipped rather than at the next page change.
 */
static void client_apply_input(client *c)
{
    bool have_pad = c->gamepads.id[0] != 0 && !c->demo;
    bool want_mouse = !have_pad && c->settings.v[Q2_SET_USE_MOUSE] != 0;
    int  style;
    int  i;

    if (have_pad) {
        c->settings.v[Q2_SET_PAD_CLASS] = 1;
        if (c->settings.v[Q2_SET_PAD_STYLE] < Q2_PAD_STYLE_RIGHT_STICK ||
            c->settings.v[Q2_SET_PAD_STYLE] > Q2_PAD_STYLE_BOTH_STICKS)
            c->settings.v[Q2_SET_PAD_STYLE] = Q2_PAD_STYLE_RIGHT_STICK;
    } else if (want_mouse) {
        c->settings.v[Q2_SET_PAD_CLASS] = 0;
        if (c->settings.v[Q2_SET_PAD_STYLE] < 0 ||
            c->settings.v[Q2_SET_PAD_STYLE] >= Q2_PAD_STYLE_RIGHT_STICK)
            c->settings.v[Q2_SET_PAD_STYLE] = Q2_PAD_STYLE_RIGHT_MOUSE;
    } else {
        c->settings.v[Q2_SET_PAD_CLASS] = 2;
        if (c->settings.v[Q2_SET_PAD_STYLE] < Q2_PAD_STYLE_STANDARD_A ||
            c->settings.v[Q2_SET_PAD_STYLE] >= Q2_PAD_STYLE_COUNT)
            c->settings.v[Q2_SET_PAD_STYLE] = Q2_PAD_STYLE_STANDARD_A;
    }

    style = c->settings.v[Q2_SET_PAD_STYLE];
    for (i = 0; i < Q2_SIM_MAX_PLAYERS; i++)
        c->sim[0].player[i].look_scheme = i == 0 || c->demo ? style
                                                       : Q2_PAD_STYLE_RIGHT_STICK;

    /* 0x800B3342. The AUTOCENTRE row has been on this page since the menu was
     * transcribed and had nothing on the other end of it; the sim reads it now.
     * See the timer at 0x8003A4AC.
     *
     * Keep that retail pad behaviour, but do not let it fight the PC port's
     * continuous mouse-look mode.  The retail timer arms after walking for 200
     * ticks and sets `recentring`; vertical mouse motion merely pauses that
     * latch, so the pitch used to spring back as soon as the mouse stopped.  A
     * PlayStation mouse is a selectable controller style.  Here USE MOUSE means
     * the pointer is permanently captured as the view, which has the same
     * semantics as holding mlook in the PC game.
     *
     * Clear both pieces of retained state as well as gating the timer.  Without
     * that, switching USE MOUSE on while a pad recenter was already armed would
     * allow one last delayed spring. */
    c->sim[0].autocentre_setting =
        !want_mouse && c->settings.v[Q2_SET_AUTOCENTRE] != 0;
    if (want_mouse) {
        for (i = 0; i < Q2_SIM_MAX_PLAYERS; i++) {
            c->sim[0].player[i].autocentre = 0;
            c->sim[0].player[i].recentring = false;
        }
    }

    /* A scripted run has no mouse to grab, and its pad script is STANDARD A's. */
    c->mouse_look = want_mouse && !c->headless && !c->demo;
}

/*
 * Whether the pointer belongs to the world or to a screen in front of it.
 *
 * Everything listed here is something the player is meant to be able to point
 * at or dismiss, and none of them wants the pointer locked to the centre of the
 * window — so the grab follows this, and the system cursor reappears over a
 * menu without the port having to draw one of its own.
 */
static bool client_ui_open(const client *c)
{
    return c->menu.open || c->mcard_open || c->mission_open ||
           c->briefing_open || c->credits_open || c->film_open ||
           c->boot_open || c->popup.visible || c->save_prompt_open;
}

static void client_update_grab(client *c)
{
    bool want;

    if (c->headless || !c->window)
        return;

    want = c->mouse_look && !client_ui_open(c) &&
           (SDL_GetWindowFlags(c->window) & SDL_WINDOW_INPUT_FOCUS) != 0;

    if (want == c->mouse_grabbed)
        return;

    if (!SDL_SetWindowRelativeMouseMode(c->window, want)) {
        Q2_ERROR("cannot %s the mouse: %s", want ? "grab" : "release",
                 SDL_GetError());
        return;
    }

    c->mouse_grabbed = want;

    /*
     * Motion that arrived while the pointer was free is not look input, and
     * motion that arrived while it was grabbed is not a cursor position. Either
     * way the accumulator is stale across the boundary, and keeping it would
     * fling the view on the frame the menu closes.
     */
    c->look_acc_x = 0.0;
    c->look_acc_y = 0.0;

    /*
     * And the QUEUE, which is the half that actually bit: taking the pointer
     * warps it, and the warp is itself a motion event sitting in the queue
     * behind this call. The next poll would read it as look input and throw the
     * view somewhere — measured as a first frame that was already pitched at
     * the ceiling before the mouse had been touched. Clearing the accumulator
     * alone does not help, because the offending delta has not arrived yet.
     */
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);

    /* Where the pointer reappears is the platform's business, not the last
     * place it was before the grab. */
    if (!want)
        c->pointer_valid = false;
}

/* A menu owns the shared retail prompt bar while it is open. Keep the close
 * operation paired with parking that bar so a later mission/briefing prompt
 * cannot inherit SELECT, BACK or RULES from the last menu page. */
static void client_menu_close(client *c)
{
    q2_menu_close(&c->menu);
    q2_prompt_hide_all(&c->prompts);
}

/*
 * IsPlayerPadShockable (0x80033250) for every player, from the host's pads.
 *
 * The console reads bit 8 of the pad record's +0x0E, which PadDualShock raises
 * at 0x80031844 only when PadInfoAct reports two actuators and clears at
 * 0x80031878 otherwise; 0x8001C9D8 greys VIBRATION and skips the row when it is
 * clear. The port's answer is the pad's own rumble capability, and a player on
 * keyboard and mouse has no pad at all — which is the console's answer for a
 * controller with no motors, i.e. false.
 *
 * It defaults to false, so until this ran OPTIONS -> CONTROLLER greyed
 * VIBRATION for everyone, DualShock owners included. The gate is read by
 * q2_menu_goto's page hook, so calling it after the page is already open takes
 * effect on the next entry — the same granularity the console's per-frame hook
 * gives in practice, since the pad set cannot change while a page is being
 * walked.
 */
static void client_menu_bind_pads(client *c)
{
    int p;

    for (p = 0; p < Q2_MENU_MP_MAX_PLAYERS; p++) {
        bool rumble = false;

        if (p < Q2_SIM_MAX_PLAYERS && c->gamepads.device[p])
            rumble = SDL_GetBooleanProperty(
                         SDL_GetGamepadProperties(c->gamepads.device[p]),
                         SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);

        /* The card menu never reaches page 31, so only the one that can. */
        q2_menu_set_pad_shockable(&c->menu, p, rumble);
    }
}

/* Defined with the front-end state machines below; their menu requests reach
 * them here, before their full definitions in this translation unit. */
static void client_card_open(client *c, q2_save_ui_mode mode);
static void client_enter_front_end(client *c);

_Static_assert(Q2_MENU_MP_TIME_OPTIONS == Q2_MP_TIME_OPTION_COUNT,
               "QFRONT time-option count drifted from QMULTI");
_Static_assert(Q2_MENU_MP_FRAG_OPTIONS == Q2_MP_FRAG_OPTION_COUNT,
               "QFRONT frag-option count drifted from QMULTI");
_Static_assert(Q2_MENU_MP_ROUND_OPTIONS == Q2_MP_ROUND_OPTION_COUNT,
               "QFRONT round-option count drifted from QMULTI");

/*
 * Install the session globals QFRONT hands to QMULTI before an arena loads.
 * Both the command-line harness and the live front end use this path, so a
 * menu-started match cannot quietly differ in layout, clocks or stale scores.
 */
static void client_mp_configure(client *c, q2_mp_mode mode, int players,
                                s16 frag_limit, s16 time_limit,
                                s16 round_limit)
{
    q2_screen_layout layout = Q2_SCREEN_LAYOUT_ONE;
    int views;
    int pi;

    if ((u32)mode >= Q2_MP_MODE_COUNT)
        mode = Q2_MP_DEATHMATCH;
    if (players < 2) players = 2;
    if (players > Q2_MP_MAX_PLAYERS) players = Q2_MP_MAX_PLAYERS;

    c->mp_enabled = true;
    q2_menu_set_multiplayer(&c->menu, true);
    q2_mp_session_init(&c->mp, mode, players);
    c->mp.frag_limit  = frag_limit;
    c->mp.time_limit  = time_limit;
    c->mp.round_limit = round_limit;

    /*
     * And the session halfwords the pause banner is coloured from. 0x800B3354
     * is the mode, and it is only ever asked one question — 0x8001CFF0 tests it
     * against 1, so TEAM DEATHMATCH takes the TEAM halfword at config+2 and
     * every other mode takes the player-colour halfword at config+4.
     *
     * The per-player pair is fed from the session's own `team[]`, which had no
     * reader in this port before now, and from the player index for the colour.
     * That index IS the colour: FrontEndResetMultiplayerDefaults writes
     * `colour = p` at 0x8001BEDC and `team = p & 1` at 0x8001BF04, so a client
     * that never got here still has the disc's defaults from q2_menu_init.
     */
    q2_menu_set_mp_mode(&c->menu, (int)c->mp.mode);
    for (pi = 0; pi < Q2_MENU_MP_MAX_PLAYERS; pi++)
        q2_menu_set_player_colour(&c->menu, pi,
                                  pi < Q2_MP_MAX_PLAYERS ? (int)c->mp.team[pi]
                                                         : (pi & 1),
                                  pi);

    c->mp_spawn_count    = 0;
    c->mp_rng_state      = 0x13572468u;
    c->mp_level_time     = 0;
    c->mp_last_request   = Q2_MP_REQ_NONE;
    c->mp_reported       = false;
    c->mp_deaths         = 0;
    c->mp_scoreboard     = false;
    c->mp_targets_logged = false;
    memset(c->sim_ready,     0, sizeof(c->sim_ready));
    memset(c->mp_pad,        0, sizeof(c->mp_pad));
    memset(c->mp_pad_pend,   0, sizeof(c->mp_pad_pend));
    memset(c->mp_pad_resume, 0, sizeof(c->mp_pad_resume));
    memset(c->mp_input,      0, sizeof(c->mp_input));
    q2_mp_results_init(&c->mp_results);
    c->mp_results_dt_frac = 0;
    c->mp_results_entered = false;
    c->mp_rounds_restarted = 0;
    memset(c->mp_spawns,     0, sizeof(c->mp_spawns));
    memset(c->mp_view_pos,   0, sizeof(c->mp_view_pos));
    memset(c->mp_view_yaw,   0, sizeof(c->mp_view_yaw));
    memset(c->mp_view_valid, 0, sizeof(c->mp_view_valid));
    memset(c->mp_shots,      0, sizeof(c->mp_shots));
    memset(c->mp_dry,        0, sizeof(c->mp_dry));
    memset(c->mp_dead,       0, sizeof(c->mp_dead));
    for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++)
        q2_player_death_init(&c->death[pi]);

    views = c->mp.player_count;
    if (views == 2)
        layout = c->settings.v[Q2_SET_HORIZONTAL_SPLIT]
                     ? Q2_SCREEN_LAYOUT_TWO_H : Q2_SCREEN_LAYOUT_TWO_V;
    else if (views >= 3)
        layout = Q2_SCREEN_LAYOUT_QUAD;
    q2_screen_set_layout(&c->screen, layout, views);

    Q2_INFO("multiplayer: %s, %d viewport%s", q2_screen_layout_name(layout),
            c->screen.view_count, c->screen.view_count == 1 ? "" : "s");
    Q2_INFO("multiplayer: %s, %d players, frag limit %d, time limit %d min, "
            "round limit %d%s", q2_mp_mode_name(mode), c->mp.player_count,
            c->mp.frag_limit, c->mp.time_limit, c->mp.round_limit,
            q2_mp_mode_selectable(mode) ? ""
                : "  (this mode is CUT — the front end cannot select it)");
}

/* Apply queued teleports after the frame, when a zone load cannot free an
 * active script. Same-zone placements preserve each owner's inventory and
 * immediately publish the new body, eye and item-touch position. */
static bool client_apply_teleports(client *c)
{
    int pi;
    for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
        q2_start_pos sp;
        q2_sim *sim = &c->sim[0];
        s32 to[3];
        int saved;
        if (!c->pending_teleport_have[pi]) continue;
        sp = c->pending_teleport[pi];
        c->pending_teleport_have[pi] = false;
        if (pi > 0 && (!c->mp_enabled || !c->sim_ready[pi])) continue;
        if (pi == 0) c->move_reason = "TELEPORT primitive";
        if (c->zone_trace) {
            const q2_player *pl = &sim->player[pi];
            Q2_WARN("[zone] f%-6u TELEPORT player %d '%s' zone %d -> (%d,%d,%d)"
                    " from (%d,%d,%d) in zone %d", c->trace_frame, pi, sp.name,
                    (int)sp.zone, sp.x, sp.y, sp.z,
                    pl->pos[0], pl->pos[1], pl->pos[2], c->zone_index);
        }
        if (sp.zone != c->zone_index) {
            char map[sizeof(c->map)];
            memcpy(map, c->map, sizeof(map));
            c->carry_player = true;
            c->carry_same_map = true;
            if (!client_load_zone(c, map, sp.zone)) return false;
        }
        saved = sim->cur_player;
        q2_sim_select_player(sim, pi);
        to[0] = sp.x; to[1] = sp.y; to[2] = sp.z;
        q2_sim_spawn(sim, to, sp.angle);
        q2_entity_world_move_player(&sim->ent_world, pi, to);
        sim->combat.self.origin[0] = to[0];
        sim->combat.self.origin[1] = q2_sim_origin_y(to[1]);
        sim->combat.self.origin[2] = to[2];
        c->mp_camera_angles[pi][0] = sim->player[pi].pitch;
        c->mp_camera_angles[pi][1] = sim->player[pi].yaw;
        c->mp_camera_angles[pi][2] = sim->player[pi].roll;
        if (pi == 0) {
            q2_sim_player_eye(sim, pi, c->cam.pos);
            c->cam.pitch = sim->player[pi].pitch;
            c->cam.yaw = sim->player[pi].yaw;
            c->cam.roll = sim->player[pi].roll;
        }
        q2_sim_select_player(sim, saved);
        Q2_INFO("teleported player %d to '%s' in zone %d", pi, sp.name, (int)sp.zone);
    }
    return true;
}

/* A round reload happens outside the simulation frame: loading frees its
 * entity/trigger arrays. Scores and settings are engine globals on retail and
 * survive QMULTI being reloaded, while every body, item and projectile is new. */
static bool client_mp_restart_round(client *c)
{
    char arena[sizeof(c->map)];
    if (c->mp_last_request != Q2_MP_REQ_RESTART_ROUND) return true;
    memcpy(arena, c->map, sizeof(arena));
    c->carry_player = false;
    c->carry_same_map = false;
    if (!client_load_zone(c, arena, c->zone_index)) return false;
    q2_mp_round_start(&c->mp);
    c->mp_last_request = Q2_MP_REQ_NONE;
    c->mp_level_time = c->sim[0].level_time;
    c->mp_reported = false;
    memset(c->mp_dead, 0, sizeof(c->mp_dead));
    c->pad_resume = true;
    memset(c->mp_pad_resume, 1, sizeof(c->mp_pad_resume));
    c->mp_rounds_restarted++;
    Q2_INFO("multiplayer: round restarted %u; wins %d %d %d %d",
            c->mp_rounds_restarted, c->mp.team_frags[0], c->mp.team_frags[1],
            c->mp.team_frags[2], c->mp.team_frags[3]);
    return true;
}

/* The scoreboard owns input and freezes the arena. QMRESULT +0x1580 reads
 * every player's fire edge, latches READY and waits 150 ticks after all agree.
 * The port then returns to its multiplayer setup screen for the next match. */
static bool client_mp_results_input(client *c, float dt)
{
    u32 pressed = 0;
    double elapsed = c->mp_results_dt_frac + (double)dt * 300.0;
    s32 ticks = (s32)elapsed;
    int pi;
    c->mp_results_dt_frac = elapsed - ticks;
    if (ticks > Q2_DT_MAX) ticks = Q2_DT_MAX;
    for (pi = 0; pi < c->mp.player_count; pi++) {
        q2_pad_bindings bind;
        q2_pad_state axes = {0};
        int style = c->sim[0].player[pi].look_scheme;
        u32 raw;
        q2_pad_style_bindings(style, &bind);
        if (c->demo)
            raw = (c->frame_index % 30) < 3 ? bind.fire : 0;
        else {
            raw = q2_gamepads_read(&c->gamepads, pi, style, &axes);
            if (pi == 0) raw |= client_pad_mask(c, &bind);
        }
        if (c->mp_results_entered &&
            (raw & ~c->mp_results_prev[pi] & bind.fire)) pressed |= 1u << pi;
        c->mp_results_prev[pi] = raw;
        c->gamepads.pressed[pi] = 0;
    }
    if (!c->mp_results_entered) {
        q2_mp_results_init(&c->mp_results);
        c->mp_results_entered = true;
    }
    if (pressed)
        Q2_INFO("multiplayer: results ready mask %X",
                (unsigned)(c->mp_results.ready | pressed));
    return q2_mp_results_tick(&c->mp_results, c->mp.player_count, pressed, ticks);
}

static bool client_mp_start_from_menu(client *c)
{
    q2_menu_mp_setup *setup = &c->menu.mp_setup;
    q2_mp_mode mode = (q2_mp_mode)setup->mode;
    int ti = setup->time_option;
    int fi = setup->frag_option;
    int ri = setup->round_option;
    const char *arena;

    if (!q2_mp_mode_selectable(mode)) mode = Q2_MP_DEATHMATCH;
    if (ti < 0 || ti >= Q2_MP_TIME_OPTION_COUNT) ti = Q2_MP_TIME_OPTION_DEFAULT;
    if (fi < 0 || fi >= Q2_MP_FRAG_OPTION_COUNT) fi = Q2_MP_FRAG_OPTION_DEFAULT;
    if (ri < 0 || ri >= Q2_MP_ROUND_OPTION_COUNT) ri = Q2_MP_ROUND_OPTION_DEFAULT;
    if (setup->arena < 0 || setup->arena >= Q2_MENU_MP_ARENA_COUNT)
        setup->arena = 0;

    arena = q2_menu_mp_arena_directory(setup->arena);
    client_mp_configure(c, mode, setup->players,
                        q2_mp_frag_options[fi],
                        mode == Q2_MP_VERSUS ? Q2_MP_NO_LIMIT
                                             : q2_mp_time_options[ti],
                        q2_mp_round_options[ri]);

    c->in_front_end   = false;
    c->carry_player   = false;
    c->carry_same_map = false;
    c->mission_open   = false;
    c->briefing_open  = false;
    c->credits_open   = false;
    client_menu_close(c);

    Q2_INFO("front end: PROCEED — %s on %s (%s)", q2_mp_mode_name(mode),
            q2_menu_mp_arena_name(setup->arena), arena);
    if (!client_load_zone(c, arena, 0)) {
        Q2_ERROR("front end: cannot load arena %s", arena);
        c->mp_enabled = false;
        q2_menu_set_multiplayer(&c->menu, false);
        client_enter_front_end(c);
        return false;
    }

    c->sim_enabled = true;
    return true;
}

static void client_menu_requests(client *c)
{
    switch (q2_menu_take_request(&c->menu)) {
    case Q2_MREQ_RESUME:
        break;
    case Q2_MREQ_RESUPPLY:
        /*
         * A RESUPPLY SPENDS A CONTINUE, which nothing here had ever done: the
         * two rows did exactly the same thing, so the count on the page never
         * moved and the middle row could be taken for ever. 0x8001FF0C is
         * `*(u8*)0x800B335D -= 1` and it is the only write to that byte in the
         * executable — the page's own greying rule (0x8001D774) is what stops
         * it going below zero, by taking the row out of the navigation.
         */
        if (!q2_player_spend_resupply(&c->continues))
            Q2_WARN("resupply: none left — the row should have been greyed");
        q2_menu_set_resupplies(&c->menu, c->continues);
        Q2_INFO("resupply: %d left", c->continues);
        /* fall through: what it does after spending one is restart the level */
        /* FALLTHROUGH */
    case Q2_MREQ_RESTART:
        Q2_INFO("restarting %s zone %d", c->map, c->zone_index);
        /*
         * A RESTART IS NOT A TRANSITION, and the carry flags must be down
         * before the load or the player is handed back the corpse they just
         * died as. Only the successful restore clears `carry_player`, and a
         * failed zone gate leaves it up — so a player who died anywhere on a
         * map with a zone gate restarted at 0 health, died again, and looped.
         * Observed: died -> restarting -> died -> 0 hp -> restarting.
         */
        c->carry_player   = false;
        c->carry_same_map = false;
        /* And the board that was up must not survive the restart, or the
         * player comes back alive standing behind it. */
        c->mission_open   = false;
        c->briefing_open  = false;
        client_load_zone(c, c->map, c->zone_index);
        client_menu_close(c);
        break;
    case Q2_MREQ_QUIT:
        c->running = false;
        break;
    /*
     * The front end's three leaves. SINGLE PLAYER is what turns the title
     * screen into a game — but not at the moment the row is pressed, and not
     * by loading a level.
     */
    case Q2_MREQ_NEW_GAME:
        /*
         * CONFIRMING A DIFFICULTY IS WHAT ARMS THE OPENING REEL.
         *
         * The EASY, MEDIUM and HARD records all call 0x80101E4C, which stores
         * the skill, hides the five title objects and arms the half-second
         * countdown whose end plays `ROGUEINP.STX` (`start_beat`). NOTHING IS
         * LOADED HERE: the console is still standing in QFRONT with its scene
         * running and its page emptied, and that is what the beat looks like.
         *
         * Where the game itself comes from is `client_start_game`, which the
         * reel hands over to — not this row.
         */
        /* The difficulty is the AI's, and it is chosen before anything loads so
         * that the creatures spawned by that load already have it. */
        q2_cre_set_skill(c->menu.skill);
        Q2_INFO("front end: skill %d confirmed — the reel, then %s",
                c->menu.skill, c->first_map);
        /* A new game carries nothing either. */
        c->carry_player   = false;
        c->carry_same_map = false;
        client_menu_close(c);
        /*
         * And the title goes with the page. 0x80101E4C sets bit 0x80 in four
         * fields of each of the five objects `module+0x12B20` names before it
         * arms the count, so the half second is a BLANK front end rather than a
         * title screen with its rows taken away.
         */
        q2_sim_scene_page(&c->sim[0], false, false);
        c->start_beat = (double)Q2_START_BEAT_UNITS;
        /*
         * AND THE BLANK FRONT END IS NOT BLANK. It is `STARTING` / `GAME` over
         * black with the wire logo turning in the corner — QFRONT's own page at
         * module+0x0EBF4, which is the screen a retail capture of this half
         * second shows. `q2_loading_show` rather than `q2_loading_raise`,
         * because the beat above is already the clock and two countdowns for
         * one half second are two things that can drift.
         */
        q2_loading_show(&c->loading, Q2_LOADING_PAGE_STARTING);
        break;
    case Q2_MREQ_CREDITS: {
        /*
         * The credit roll, read out of the module the front end IS. It is not
         * a page array — 45 pages and 186 rows in QFRONT and only one of them
         * mentions the credits, which is the row that got us here — so the
         * words are the module's and the scroll is this port's.
         */
        const dat_chunk *lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];

        c->credits_count = 0;
        if (lb && lb->data && lb->size)
            c->credits_count = q2_levelbin_credits(lb->data, lb->size,
                                                   c->credits,
                                                   Q2_LB_CREDITS_MAX);
        if (c->credits_count) {
            c->credits_open   = true;
            c->credits_scroll = 0;
            client_menu_close(c);
            Q2_INFO("front end: credits, %u lines", c->credits_count);
        } else {
            Q2_INFO("front end: this module carries no credit roll");
            q2_menu_open(&c->menu);
            q2_menu_goto(&c->menu, Q2_PAGE_FRONT_TITLE);
        }
        break;
    }
    case Q2_MREQ_LOAD_GAME:
        client_card_open(c, Q2_SAVE_UI_LOAD);
        break;
    case Q2_MREQ_MP_PROCEED:
        (void)client_mp_start_from_menu(c);
        break;
    case Q2_MREQ_MP_LOAD_SETTINGS:
        client_card_open(c, Q2_SAVE_UI_SETTINGS_LOAD);
        break;
    case Q2_MREQ_MP_SAVE_SETTINGS:
        client_card_open(c, Q2_SAVE_UI_SETTINGS_SAVE);
        break;
    case Q2_MREQ_MISSION:
        /*
         * THE PAUSE MENU'S MISSION ROW OPENS THE OBJECTIVES POP-UP, not the
         * level-completion tally.
         *
         * 0x8002033C ends `jal 0x800213B0` with a0 = 1 and a1 = 15 — a raise
         * with a one-frame delay and a fifteen-second deadline. FORMATS.md
         * reads that call as "it leaves the menu with exit code 15"; it is not
         * an exit code, it is the two arguments. The tally screen at
         * 0x80021ADC has exactly one caller, 0x80018944, the level-end state,
         * and the pause menu never reaches it.
         *
         * The tally is still reachable — that is what the unit boundary shows
         * — but not from here.
         */
        q2_briefing_popup_raise(&c->popup, Q2_BRIEFING_MENU_DELAY,
                                Q2_BRIEFING_SECONDS,
                                c->sim[0].level_time, c->sim[0].cur_dt);
        c->popup_raises++;
        client_play_sound(c, "msc_comp_up");
        client_menu_close(c);
        break;
    default:
        break;
    }
}

/*
 * Play one of the menu's five effects.
 *
 * The engine names them by their bank keys — `msc_menu2` on a cursor move,
 * `msc_menu1` on an activation, `msc_menu3` on back, `itm_pkup` on a toggle,
 * `msc_comp_up` while a slider moves (FORMATS.md §10.3) — so playing one is a
 * lookup by name in the map's own bank, a decode, and a push into the same
 * stream the music uses. Mixed in rather than replacing: the console has an SPU
 * with 24 voices and the effect does not stop the track.
 *
 * The SFX slider scales it. STEREO is not consulted because this path is mono
 * and panning a UI sound centre is what stereo would do anyway.
 */
/* The bank's names are ASCII and the engine's keys are lower case; compare
 * without dragging in a locale-aware `stricmp`. */
static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* The same comparison, stopping after `n` characters — `pre` is a prefix of
 * `s`. See client_find_sound for why that is a thing worth having. */
static int name_is_prefix(const char *pre, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int ca = (pre[i] >= 'A' && pre[i] <= 'Z') ? pre[i] + 32 : pre[i];
        int cb = (s[i]   >= 'A' && s[i]   <= 'Z') ? s[i]   + 32 : s[i];
        if (ca != cb)
            return 0;
    }
    return 1;
}

/*
 * Find one effect in the map's bank by the name a table gave.
 *
 * Exact first, and then the truncation rule — because a table's name field is
 * TWELVE BYTES (itemtable.h) and several of the bank's names are longer than
 * that. The disc carries `msc_ar2_pkup22k`; the item table can only hold
 * `msc_ar2_pkup`. So a key that FILLS the field may be a truncation and has to
 * be matched as a prefix, while one that does not fill it was not truncated and
 * must match exactly — otherwise a short key would collide with anything that
 * merely begins with it.
 *
 * That distinction is not a guess. Across all 49 banks on the disc, every one of
 * the eleven item names shorter than twelve characters matches exactly in every
 * bank that carries it, and every one that is exactly twelve — the three health
 * names and both armour names — matches nowhere exactly and everywhere as a
 * prefix of the same name with the sample rate appended. No name is ambiguous
 * under this rule. Five of the eleven are unreachable without it.
 */
static bool client_find_sound(client *c, const char *want, q2_vag *out)
{
    size_t len;
    u32 i, pass;

    if (!c->sfx_ready || !want || !want[0])
        return false;

    len = strlen(want);

    for (pass = 0; pass < 2; pass++) {
        /* The second pass only applies to a key that filled the field. */
        if (pass == 1 && len < Q2_ITEM_MODEL_LEN)
            return false;

        for (i = 0; i < c->sfx.count; i++) {
            if (!q2_sound_bank_get(&c->sfx, i, out))
                continue;
            if (pass == 0 ? name_eq(out->name, want)
                          : name_is_prefix(want, out->name, len))
                return true;
        }
    }

    return false;
}

/*
 * Play one effect out of the map's own bank, by name.
 *
 * The menu names its five and the item table names its eleven, and both are
 * keys into the same per-map bank, so there is one decoder here rather than
 * two. Returns false when the map does not carry the name, which is a thing
 * that happens and is not an error — three maps ship no `frontend.lbm` either.
 */
static bool client_play_sound(client *c, const char *want)
{
    client_voice *v = NULL;
    q2_vag vag;
    u32 i, rate;
    s32 vol;

    if (!c->audio || !client_find_sound(c, want, &vag))
        return false;

    if (!vag.body || vag.data_size < SPU_BLOCK_SIZE)
        return false;

    for (i = 0; i < CLIENT_VOICES; i++) {
        if (!c->voice[i].active) {
            v = &c->voice[i];
            break;
        }
    }

    /*
     * All twenty-four busy. The console has no twenty-fifth either, so this
     * drops rather than stealing one — a stolen voice cuts a sound already
     * being heard, which is a louder mistake than a sound never started. It is
     * counted, because a client that keeps hitting this ceiling is a client
     * raising more events than the hardware ever could.
     */
    if (!v) {
        c->voice_dropped++;
        return false;
    }

    /* 0..127 from the slider, and the console doubles the music one but not
     * this (0x800205F4 is the music path alone). */
    vol = c->settings.v[Q2_SET_SFX];
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;

    /* 11025 and 22050 are the only two the disc carries (vag.h); anything else
     * would be a header this reader misparsed, so it is not trusted. */
    rate = vag.sample_rate;
    if (rate < 1000 || rate > XA_SAMPLE_RATE)
        rate = 22050;

    memset(v, 0, sizeof(*v));
    /* AFTER the memset, or it is wiped: this is the slot's new tag, the port's
     * equivalent of the halfword 0x80073734 reads back from voice_record+52. */
    v->serial = ++c->voice_serial_next;
    q2_spu_voice_start(&v->dec, vag.body, vag.data_size);
    /* Listener-local until a caller says otherwise; client_voices_update
     * recomputes both the moment it has a position. */
    v->level = CLIENT_SFX_LOCAL;
    v->pan_l = 0xFF;
    v->pan_r = 0xFF;
    /*
     * THE PITCH MODIFIER, which this played without.
     *
     * The console derives the SPU pitch from the VAG's header rate the same way
     * and then multiplies by a per-sound modifier whose DEFAULT is 35/32 —
     * 1.09375, about one and a half semitones. Playing at the bare header rate
     * makes every effect in the game 9.375% flat.
     *
     * Q2_SFX_PITCH_DEFAULT is the default only. Individual retail request
     * records can carry ranges and choose a randomising or direct start. Those
     * byte-state primitives are transcribed in vag.c; this generic name-based
     * caller cannot select a request role safely yet, so it uses the default.
     */
    v->step   = q2_sfx_step_16_16(rate, Q2_SFX_PITCH_DEFAULT,
                                  XA_SAMPLE_RATE);
    v->vol    = vol;
    /* A fresh request, so 0x80071C5C's 63. A caller that wants the chaingun's
     * clone overwrites it through client_sound_set_relative_volume below. */
    v->req_vol = (u8)Q2_SFX_VOLUME_DEFAULT;
    v->active = true;
    c->voice_started++;
    c->voice_last        = v;
    c->voice_last_serial = v->serial;

    return true;
}

/*
 * SoundSetRelativeVolume (0x80072BB4), applied to the voice this port has just
 * started rather than to a request record it does not keep. The byte's own
 * wrap-then-clamp is q2_sfx_volume_add, beside the same record's pitch byte.
 */
static void client_sound_set_relative_volume(client *c, s32 delta)
{
    if (!c->voice_last)
        return;

    c->voice_last->req_vol = q2_sfx_volume_add(c->voice_last->req_vol, delta);
}

/*
 * IS THE VOICE A CALLER STARTED STILL THE ONE IN THAT SLOT, AND STILL RUNNING?
 *
 * 0x800739E4, behind 0x800739B8. The console checks the index against 24 and
 * compares the handle's tag with the slot's own; a mismatch means the slot has
 * been taken over since and the answer is no. `serial` is that tag here, and a
 * zero serial is a handle that was never filled.
 */
static bool client_voice_playing(const client_voice *v, u32 serial)
{
    return v && serial != 0 && v->active && v->serial == serial;
}

/*
 * And stop it — 0x80073924, behind 0x8007398C.
 *
 * The same tag test, then the stop; a stale handle silences nothing rather than
 * cutting whatever sound happens to be in that slot now. That guard is the
 * whole reason the rotator's motor loop can be started at all (rotator.c).
 */
static void client_voice_stop(client_voice *v, u32 serial)
{
    if (client_voice_playing(v, serial))
        v->active = false;
}

/*
 * The same sound, but somewhere in the world.
 *
 * The console's own split: `0x80072E24` takes the listener-local arm when
 * `0x800AE8B4` is set and the positional one otherwise, and everything the
 * player themselves makes goes through the first. Here the caller says which,
 * because the port has no equivalent global and the distinction is per sound
 * rather than per state.
 */
static bool client_play_sound_at(client *c, const char *want, const s32 at[3])
{
    if (!client_play_sound(c, want))
        return false;

    if (c->voice_last && at) {
        c->voice_last->positional  = true;
        c->voice_last->pos_world[0] = at[0];
        c->voice_last->pos_world[1] = at[1];
        c->voice_last->pos_world[2] = at[2];
    }
    return true;
}

/*
 * Attenuate and pan every live voice against where the listener is NOW.
 *
 * `level = 63 * (12288 - dist) / 4096` on the horizontal distance, and the pan
 * index is the source's sideways offset in the camera's own basis over that
 * distance, fifteen either side of centre. Both are recomputed every frame
 * rather than latched at the start, because the console updates its voices per
 * frame — which is what makes a rocket's flight sweep across the stereo field
 * instead of being stamped where it was launched.
 *
 * With STEREO off the pan table is not consulted at all and both channels take
 * half, which is the else-branch of the console's own CdMix.
 */
static void client_voices_update(client *c)
{
    s32 eye[3], fwd[3], right[3];
    bool stereo = c->settings.v[Q2_SET_STEREO] != 0;
    u32 i;

    q2_sim_eye(&c->sim[0], eye);
    {
        s32 yaw = c->cam.yaw;
        s32 sy = q2_sin12(yaw), cy = q2_cos12(yaw);

        fwd[0]   =  sy; fwd[1]   = 0; fwd[2]   =  cy;
        right[0] =  cy; right[1] = 0; right[2] = -sy;
    }

    for (i = 0; i < CLIENT_VOICES; i++) {
        client_voice *v = &c->voice[i];
        s32 d[3], along, side, dist, level, idx;

        if (!v->active)
            continue;

        if (!v->positional) {
            /*
             * Scaled by the request's own volume, because on the console that
             * byte IS the voice's volume field and the listener-local path does
             * not touch it either. 63/63 leaves every existing caller exactly
             * where it was; the chaingun's 23/63 is the one ratio that moves.
             */
            v->level = (CLIENT_SFX_LOCAL * (s32)v->req_vol) / CLIENT_SFX_REQ_VOL;
            v->pan_l = v->pan_r = stereo ? 0xFF : 0x80;
            continue;
        }

        d[0] = v->pos_world[0] - eye[0];
        d[1] = 0;                        /* horizontal only, as the console is */
        d[2] = v->pos_world[2] - eye[2];

        along = (s32)(((s64)d[0] * fwd[0]   + (s64)d[2] * fwd[2])   >> 12);
        side  = (s32)(((s64)d[0] * right[0] + (s64)d[2] * right[2]) >> 12);

        {
            s64 len2 = (s64)along * along + (s64)side * side;
            s64 lo = 0, hi = 0x7FFFFFFF, len = 0;

            while (lo <= hi) {
                s64 mid = lo + (hi - lo) / 2;
                if (mid * mid <= len2) { len = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
            dist = (s32)(len > 0 ? len : 1);
        }

        /* The base was this number under another name: the request's volume
         * byte, 63 by default. Read it off the voice now that a request can
         * carry something else. */
        level = ((s32)v->req_vol * (CLIENT_SFX_REACH - dist)) / 4096;
        if (level < 0)   level = 0;
        if (level > 255) level = 255;
        v->level = level;

        if (!stereo) {
            v->pan_l = v->pan_r = 0x80;
            continue;
        }

        idx = 15 + (15 * side) / dist;
        if (idx < 0)                  idx = 0;
        if (idx >= CLIENT_PAN_STEPS)  idx = CLIENT_PAN_STEPS - 1;

        /* The table is one side of the curve: the far channel reads it
         * backwards, which is what makes the pair constant-power. */
        v->pan_l = k_pan[idx];
        v->pan_r = k_pan[CLIENT_PAN_STEPS - 1 - idx];
    }
}

static void client_play_menu_sound(client *c, q2_menu_sound snd)
{
    const char *want = q2_menu_sound_name(snd);

    if (!client_play_sound(c, want) && want && want[0])
        Q2_DEBUG("menu sound '%s' is not in %s's bank", want, c->map);
}

/* ------------------------------------------------------------------------- */
/*
 * What the tick asked to be heard.
 *
 * A think has no audio path of its own — it records what it would have played
 * and the caller drains it (entity.h) — and so does the player's own frame. This
 * is the other end of both: the queue is cleared at the TOP of a tick precisely
 * so the caller can drain it afterwards (sim.c), and the drain cannot miss one,
 * because q2_sim_advance runs the world exactly once per frame with a variable
 * dt rather than sub-stepping (sim.h).
 *
 * The LIGHT and BURST events are not consumed. Both are real and both are
 * dropped rather than faked: a glow light wants a q2_light_world for the entity
 * draw to gather from and the client has none yet, and the pickup burst is a
 * particle effect whose emitter is not reconstructed. An item therefore glows
 * through its own tint (entitydraw.c) and vanishes without sparks, which is less
 * than the console does rather than something the console does not do.
 */
/*
 * THE ELEVEN ITEM SOUNDS, and the SUBSTITUTIONS a map that is missing one gets.
 *
 * 0x800374BC resolves the eleven names into gp+17032..gp+17072 once per map,
 * and 0x80037B24 then PATCHES the slots that came back null. This port resolved
 * by name at every play instead, so there was nowhere for a patched slot to
 * live and a map whose bank lacks a name simply played nothing.
 *
 * The chain, read at 0x80037B24..0x80037BCC. Slots are four bytes apart from
 * gp+17032, so 17044/17048/17052 are slots 3/4/5 and 17056/17060 are 6/7:
 *
 *   17048 || 17052 null -> BOTH take the OR, i.e. whichever one loaded
 *                          (`or v1,v1,v0` / `or v0,v0,v1` at 0x80037B4C)
 *   17044       null    -> takes 17048
 *   17056 || 17060 null -> BOTH take the OR, the same idiom
 *   17056 still null    -> BOTH take 17032
 *
 * The other five get no fallback and stay silent, which is also the console's
 * behaviour rather than an omission here.
 *
 * Held as NAMES rather than bank indices because everything downstream plays by
 * name; a patched slot is simply the donor's name copied into it.
 */
static void client_item_sounds_resolve(client *c)
{
    const q2_item_table *t;
    q2_vag scratch;
    bool   have[Q2_CLIENT_ITEM_SOUNDS];
    u32    i;

    if (!c)
        return;

    t = c->item_table_ready ? &c->item_table : q2_item_table_builtin();

    for (i = 0; i < Q2_CLIENT_ITEM_SOUNDS; i++) {
        snprintf(c->item_sound[i], sizeof(c->item_sound[i]), "%s", t->sound[i]);
        have[i] = c->item_sound[i][0] &&
                  client_find_sound(c, c->item_sound[i], &scratch);
    }

    /* 4 and 5: the large and normal health pickups. */
    if (!have[4] || !have[5]) {
        int donor = have[4] ? 4 : (have[5] ? 5 : -1);
        if (donor >= 0) {
            int k;
            for (k = 4; k <= 5; k++) {
                snprintf(c->item_sound[k], sizeof(c->item_sound[k]),
                         "%s", c->item_sound[donor]);
                have[k] = true;
            }
        }
    }

    /* 3: the mega health, which falls back to the large one. */
    if (!have[3] && have[4]) {
        snprintf(c->item_sound[3], sizeof(c->item_sound[3]),
                 "%s", c->item_sound[4]);
        have[3] = true;
    }

    /* 6 and 7: the two armour pickups. */
    if (!have[6] || !have[7]) {
        int donor = have[6] ? 6 : (have[7] ? 7 : -1);
        if (donor >= 0) {
            int k;
            for (k = 6; k <= 7; k++) {
                snprintf(c->item_sound[k], sizeof(c->item_sound[k]),
                         "%s", c->item_sound[donor]);
                have[k] = true;
            }
        }
    }

    /* ...and if neither armour sound exists, both become the generic pickup. */
    if (!have[6] && have[0]) {
        int k;
        for (k = 6; k <= 7; k++) {
            snprintf(c->item_sound[k], sizeof(c->item_sound[k]),
                     "%s", c->item_sound[0]);
            have[k] = true;
        }
    }

    {
        u32 patched = 0;
        for (i = 0; i < Q2_CLIENT_ITEM_SOUNDS; i++)
            if (t->sound[i][0] && strcmp(c->item_sound[i], t->sound[i]) != 0)
                patched++;
        if (patched)
            Q2_INFO("item sounds: %u of %u substituted for this bank",
                    patched, (u32)Q2_CLIENT_ITEM_SOUNDS);
    }
}

static const char *client_ent_sound_name(const client *c, u32 which)
{
    const q2_item_table *t = c->item_table_ready ? &c->item_table
                                                 : q2_item_table_builtin();

    /*
     * Q2_SND_TELEPORT is the materialise effect and is deliberately NOT in the
     * eleven-name table at 0x800AC240 — the materialise block names it inline
     * (FORMATS.md §"Materialise"), so it is named inline here too.
     */
    if (which == Q2_SND_TELEPORT)
        return "msc_tele1";

    /*
     * The PLAYER's own sounds — footsteps, the landing thump, the four pain
     * grunts — which share this queue because it is the one a headless caller
     * can already drain (entity.h).
     *
     * These are NOT in the eleven-name table either: the executable holds them
     * as resolved sound POINTERS at `0x800B28EC` and the seven beside it. The
     * names are recoverable anyway, because the initialiser that fills those
     * pointers looks each one up by name — `0x8003B900`…`0x8003C590`, a run of
     * `find_sound(name)` / `sw v0, gp+N` pairs against the string pool at
     * `0x800AC458`.
     *
     * The one trap in reading it: the compiler hoists the NEXT name's setup
     * above the current store, so the `addiu t0, "pla_step2"` sitting one
     * instruction before `sw v0, gp+17172` belongs to the following entry and
     * not to that one. Pair them off by one and every sound here is wrong by
     * exactly one slot, which sounds plausible and is not.
     */
    switch (which) {
    case Q2_SND_FOOTSTEP_A:   return "pla_step1";    /* 0x800B2914 */
    case Q2_SND_FOOTSTEP_B:   return "pla_step2";    /* 0x800B2918 */
    case Q2_SND_FOOTSTEP_WET: return "pla_wade3";    /* 0x800B292C */
    case Q2_SND_LAND:         return "pla_fall2";    /* 0x800B28EC */
    case Q2_SND_PAIN_25:      return "mal_pn25_1";   /* 0x800B294C */
    case Q2_SND_PAIN_50:      return "mal_pn50_1";   /* 0x800B2950 */
    case Q2_SND_PAIN_75:      return "mal_pn75_1";   /* 0x800B2954 */
    case Q2_SND_PAIN_100:     return "mal_pn100_1";  /* 0x800B2958 */
    case Q2_SND_DEATH:        return "pla_death4";   /* 0x800B28E4 */
    case Q2_SND_DROWN:        return "pla_drown1";   /* 0x800B28E8 */
    case Q2_SND_JUMP:         return "pla_jump1";    /* 0x800B2900 */
    case Q2_SND_LAND_SOFT:    return "pla_land1";    /* 0x800B2904 */
    case Q2_SND_WATER_IN:     return "pla_watr_in";  /* 0x800B2938 */
    case Q2_SND_WATER_OUT:    return "pla_watr_out"; /* 0x800B293C */
    case Q2_SND_WATER_UNDER:  return "pla_watr_un";  /* 0x800B2940 */
    case Q2_SND_GASP:         return "pla_gasp1";    /* 0x800B28F8 */
    default: break;
    }

    /* The RESOLVED slot, which is the raw name unless this map's bank was
     * missing it and the chain substituted a donor. See
     * client_item_sounds_resolve. */
    if (which < Q2_CLIENT_ITEM_SOUNDS && c->item_sound[which][0])
        return c->item_sound[which];

    if (which < sizeof(t->sound) / sizeof(t->sound[0]))
        return t->sound[which];

    return NULL;
}

/*
 * The TITLE SCREEN's lighting — `module+0x2BD8`, the second of the two hooks
 * the menu's own frame calls (`0x8001A200`). The derivation is in levelbin.h;
 * what is here is the wiring, and it is deliberately not inside
 * `client_entity_events`: that runs off the sim tick and the front end does not
 * tick. The console's hook runs off the MENU frame, so this does too.
 *
 * The clear is the load-bearing half. Sixteen dynamic slots, five lights a
 * frame, and `0x80075C34` drops the seventeenth silently — without it the rig
 * fills the list in four frames and the logo is then lit by a frozen snapshot
 * of frame four for the rest of the session.
 */
static void client_scene_lights(client *c)
{
    q2_lb_light light[Q2_LB_LIGHT_MAX];
    u32 n, i;

    if (!c->lights_ready || !c->sim[0].scene_ready)
        return;

    q2_light_world_begin_frame(&c->light_world);

    n = q2_levelbin_scene_lights(light, c->scene_wander, &c->sim[0].combat.rng);
    for (i = 0; i < n; i++)
        q2_light_add_dynamic(&c->light_world, light[i].pos, light[i].rgb,
                             light[i].inner, light[i].outer, 0, 0);
}

static void client_entity_events(client *c)
{
    const q2_ent_events *ev = q2_sim_entity_events(&c->sim[0]);
    u32 i;

    /*
     * Empty last frame's runtime lights first — 0x80075B94 does this at the top
     * of every frame and nothing in this port was calling it. Without it the
     * sixteen dynamic slots fill on the first frames a projectile flies and stay
     * full forever: a 500-frame BASE3 capture added 16 lights and dropped 481.
     */
    if (c->lights_ready)
        q2_light_world_begin_frame(&c->light_world);



    if (!ev)
        return;

    for (i = 0; i < ev->count; i++) {
        const char *name;

        /*
         * Only SOUND is acted on. The entity world also raises _LIGHT (a
         * dynamic light of `glow` and `radius`) and _BURST (the pickup particle
         * burst at 0x8005B6C0), and both are dropped here — counted rather than
         * silently ignored, because "the client handles entity events" was true
         * of one kind in three and nothing said so.
         *
         * Neither is guessed at: the port has no preset for a pickup burst —
         * its seven are explosion, blood, BFG, gib, scripted, spark and laser
         * end — and choosing one of those would invent an effect rather than
         * reconstruct it. See openquestions #60.
         */
        if (ev->e[i].kind == Q2_ENT_EVENT_LIGHT) {
            /*
             * Fed to the light world rather than dropped. The event carries the
             * colour and the outer radius the projectile sweep reads out of
             * 0x800AE954; the inner comes from the same preset. Sixteen dynamic
             * lights is the engine's own ceiling (lighting.h) and the
             * seventeenth is dropped there, so a busy frame still counts what it
             * could not take.
             */
            /*
             * The inner radius is chosen from the outer the event carries,
             * because the event has no room for both: the BFG's 1400 pairs with
             * 1000 and every other bolt's 800 pairs with 300. Both pairs are
             * read from 0x800AE9C0 and 0x800AE958 -- see projectile.h.
             */
            /* The event carries both radii now; 0 means the raiser had no
             * inner to give, and the projectile's is the sane default. */
            s32 inner = ev->e[i].inner_radius ? ev->e[i].inner_radius
                                              : Q2_PROJ_LIGHT_INNER;

            if (c->zone_trace)
                Q2_INFO("[dyn] glow %3u,%3u,%3u at (%d,%d,%d) r %d/%d",
                        ev->e[i].glow[0], ev->e[i].glow[1], ev->e[i].glow[2],
                        ev->e[i].pos[0], ev->e[i].pos[1], ev->e[i].pos[2],
                        inner, ev->e[i].radius);

            /*
             * AddLight OR AddTimedLight, and the event says which. A `ticks`
             * of zero is 0x80075C34, one frame on the world list; anything
             * above is 0x80075D14, a slot in the sixteen-entry table at
             * 0x800E1218 that re-emits the light at a falling fraction of its
             * radii for that many frames. The shotgun, the super shotgun and
             * the energy pulse are the three raisers that take the second path
             * (simcombat.c), and all three raised nothing at all until now.
             * Both refuse a seventeenth, and the drop is counted the same way.
             */
            if (!c->lights_ready ||
                (ev->e[i].ticks > 0
                     ? !q2_light_add_timed(&c->light_world, ev->e[i].pos,
                                           ev->e[i].glow, inner,
                                           ev->e[i].radius, ev->e[i].ticks)
                     : !q2_light_add_dynamic(&c->light_world, ev->e[i].pos,
                                             ev->e[i].glow, inner,
                                             ev->e[i].radius, 0, 0)))
                c->ent_light_dropped++;
            else
                c->ent_light_added++;
            continue;
        }
        if (ev->e[i].kind == Q2_ENT_EVENT_BURST) {
            client_pickup_burst(c, ev->e[i].pos, ev->e[i].model_index);
            continue;
        }
        if (ev->e[i].kind != Q2_ENT_EVENT_SOUND)
            continue;

        name = client_ent_sound_name(c, ev->e[i].sound);
        if (name) {
            bool ok;

            /*
             * The PLAYER'S own are listener-local — footsteps, the landing
             * thump, the pain grunts — and everything else is where the event
             * says. That is the console's own split at 0x80072E24, made per
             * sound because this port has no "a menu owns the frame" global to
             * branch on.
             */
            if (ev->e[i].sound == Q2_SND_FOOTSTEP_A ||
                ev->e[i].sound == Q2_SND_FOOTSTEP_B ||
                ev->e[i].sound == Q2_SND_FOOTSTEP_WET ||
                ev->e[i].sound == Q2_SND_LAND ||
                ev->e[i].sound == Q2_SND_LAND_SOFT ||
                ev->e[i].sound == Q2_SND_JUMP ||
                ev->e[i].sound == Q2_SND_WATER_IN ||
                ev->e[i].sound == Q2_SND_WATER_OUT ||
                ev->e[i].sound == Q2_SND_PAIN_25 ||
                ev->e[i].sound == Q2_SND_PAIN_50 ||
                ev->e[i].sound == Q2_SND_PAIN_75 ||
                ev->e[i].sound == Q2_SND_PAIN_100)
                ok = client_play_sound(c, name);
            else
                ok = client_play_sound_at(c, name, ev->e[i].pos);

            /*
             * WHICH PLAYER EVENTS RAISE AI NOISE, and this was backwards in
             * both directions.
             *
             * `xrefs 0x80062B80` gives PlayerNoise fourteen callers. Ten are
             * weapons. The other four are water entry (0x8003D2B8), the breath
             * (0x8003D3FC), water exit (0x8003D460) and the JUMP (0x8003E208).
             * The footstep block at 0x8003AA3C..0x8003AB04 contains no `jal`
             * except its own sound play, and the fall handler makes only
             * 0x8007270C and 0x80057D54. So retail raises no noise for walking
             * or landing at all, and raises one for the jump — the exact
             * opposite of what was here.
             *
             * And the pair was wrong too. All four player-body callers pass
             * type 0, and 0x80062C68's `sltiu v0, s4, 2` sends anything below 2
             * to level.sound_entity at 0x800E46EC. That is the `true` branch of
             * this helper. `false` writes sound2_entity, which an ambush
             * creature ignores — so even the noise that was being raised went
             * to the pair least likely to be heard.
             *
             * Expect this to read as a regression at first: creatures stop
             * noticing you walk. They did not notice on the console either.
             */
            if (c->creatures_ready &&
                (ev->e[i].sound == Q2_SND_JUMP ||
                 ev->e[i].sound == Q2_SND_WATER_IN ||
                 ev->e[i].sound == Q2_SND_WATER_OUT))
                q2_creature_world_player_noise(&c->creatures, true);

            if (!ok)
                Q2_DEBUG("sound '%s' is not in %s's bank", name, c->map);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Saving and loading                                                         */
/*                                                                            */
/* The three function pointers the front end calls (`0x800B3234` poll,        */
/* `0x800B3238` request, `0x800B324C` act on a row) are filled straight from   */
/* saveui.h, which has their exact signatures — so the reconstruction drives   */
/* the port's save system without either side knowing about the other.        */
/* ------------------------------------------------------------------------- */

/*
 * Everything a save has to contain that does not live in the sim: the mission
 * tallies, which the HUD owns, and the menu settings, which the pause menu
 * edits and which state 14 "applies" on the console (memcard.h).
 */
static bool client_capture(client *c)
{
    q2_result rc;

    q2_save_free(&c->snapshot);

    rc = q2_save_capture(&c->snapshot, &c->sim[0], NULL, c->build.serial,
                         c->map, c->zone_index);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot capture a save: %s", q2_result_str(rc));
        return false;
    }

    rc = q2_save_capture_event_history(&c->snapshot, &c->event_store);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot capture script history: %s", q2_result_str(rc));
        q2_save_free(&c->snapshot);
        return false;
    }

    q2_save_capture_mission(&c->snapshot, &c->mission);
    /*
     * ...and the help computer's current message pair, as the two Strings KEYS
     * it was resolved from. `HelpComputer_GetMessages` (0x800218A8) copies
     * `LastOrders` and `LastObjective` into the unit's save block on the save
     * arm of every `LoadSaveHub`, so which board the player last read is part
     * of a saved game and not session scratch. Without it a reload showed the
     * shipped "Awaiting Orders." however far into the unit the save was taken.
     */
    q2_save_set_help_keys(&c->snapshot, c->popup.orders_key,
                          c->popup.objective_key);
    /*
     * ...and the live counters the row's first column is a copy of. The row
     * alone is not enough to resume a level: it holds one clamped byte per
     * counter and nothing about WHICH secrets have counted or WHERE the kills
     * were made. See `q2_save_level_counters`.
     */
    {
        q2_save_level_counters lc;
        u32 i;
        int z = c->zone_index;

        memset(&lc, 0, sizeof(lc));
        lc.secrets_found     = c->secrets_found;
        lc.secret_seen_count = c->secret_seen_count;
        if (lc.secret_seen_count > Q2_SAVE_SECRETS_SEEN)
            lc.secret_seen_count = Q2_SAVE_SECRETS_SEEN;
        for (i = 0; i < lc.secret_seen_count; i++)
            lc.secret_seen[i] = c->secret_seen[i];

        for (i = 0; i < Q2_SAVE_LEVEL_ZONES; i++) {
            lc.zone_dead[i]   = c->zone_dead[i];
            lc.zone_placed[i] = c->zone_placed[i];
        }
        /* The resident zone's slot is only written when the zone is left, so
         * take its answer from the live set the way the tally does — the
         * larger of the two, for the reason `client_zone_stash` gives. */
        if (z >= 0 && z < Q2_SAVE_LEVEL_ZONES) {
            u32 live = client_zone_dead(c);

            if (live > lc.zone_dead[z])
                lc.zone_dead[z] = live;
            if (c->creatures_ready && c->cre_in_zone > lc.zone_placed[z])
                lc.zone_placed[z] = c->cre_in_zone;
        }

        q2_save_set_level_counters(&c->snapshot, &lc);
    }
    /* The doors, which the client owns rather than the sim. Without them a
     * reload shuts every one the player opened AND cannot reopen it: the
     * script flags are carried, so the record that opened it has run. */
    if (c->movers_ready)
        q2_save_capture_movers(&c->snapshot, &c->movers);
    if (c->rotators_ready &&
        q2_save_capture_rotators(&c->snapshot, &c->rotators) != Q2_OK) {
        Q2_ERROR("cannot capture rotating brushes");
        return false;
    }
    /* And who is dead: without this a save reloads into a room the player has
     * already cleared, full again. */
    if (c->creatures_ready) {
        q2_save_capture_creatures(&c->snapshot, &c->creatures.set);
        if (q2_save_capture_ai(&c->snapshot, &c->creatures,
                (u32)(c->ai_accum * 1000000.0 + 0.5)) != Q2_OK) {
            Q2_ERROR("cannot capture creature behavior");
            return false;
        }
    }
    q2_save_set_settings(&c->snapshot, c->settings.v, Q2_SET_COUNT);
    if (c->vm_ready && q2_save_capture_viewweapon(&c->snapshot, &c->vw[0],
            c->vw_last_weapon[0], c->shot_serial_shown[0],
            c->shot_serial_heard[0], c->cock_pending[0]) != Q2_OK) {
        Q2_ERROR("cannot capture view weapon");
        return false;
    }
    return true;
}

/*
 * Put a loaded save back into the running game.
 *
 * The zone is reloaded first and unconditionally, even when the map and zone
 * already match. Applying onto whatever the player happened to be standing in
 * would leave anything the save does not cover — the models bound to this map,
 * the effect generator's attachment, the spawn the mover cached — carrying over
 * from a session that is being discarded. A load IS a level load; treating it
 * as one is both simpler and correct.
 */
static bool client_apply_save(client *c, const q2_save *s)
{
    q2_result rc;
    s32 eye[3];
    bool was_front_end = c->in_front_end;
    q2_event_store saved_history = {0}, previous_history;
    s32 previous_skill = q2_cre_skill(), saved_skill;

    /* Refuse the wrong disc before loading anything or changing the screen. */
    if (s->serial[0] && strcmp(s->serial, c->build.serial) != 0) {
        Q2_ERROR("save is for build %s, this disc is %s",
                 s->serial, c->build.serial);
        return false;
    }
    rc = q2_save_restore_event_history(s, &saved_history);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot restore script history: %s", q2_result_str(rc));
        return false;
    }
    previous_history = c->event_store;
    c->event_store = saved_history;
    c->restoring_save = s;
    c->carry_player = false;
    c->carry_same_map = false;
    c->carry_inv = s->inventory;  /* Mover settling during replay uses saved keys. */
    /* SpawnGeneral filters Population before the creature set is built. */
    if (q2_save_get_skill(s, &saved_skill))
        q2_cre_set_skill(saved_skill);

    /* Game saves are single-player. LOAD GAME can be entered from QFRONT, so
     * clear that screen's state before the map load chooses fonts, HUD and
     * input policy for the restored level. */
    c->in_front_end = false;
    c->mp_enabled   = false;
    q2_menu_set_multiplayer(&c->menu, false);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);

    if (!client_load_zone(c, s->map, s->zone)) {
        Q2_ERROR("the save names %s zone %d, which will not load",
                 s->map, (int)s->zone);
        c->in_front_end = was_front_end;
        c->restoring_save = NULL;
        q2_event_store_free(&c->event_store);
        c->event_store = previous_history;
        q2_cre_set_skill(previous_skill);
        return false;
    }
    c->restoring_save = NULL;
    q2_event_store_free(&previous_history);

    rc = q2_save_apply(s, &c->sim[0], NULL, c->build.serial, c->map);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot apply the save: %s", q2_result_str(rc));
        return false;
    }

    /* BRKS restores hitboxes, EXPL restores group health/destruction. Apply
     * the same geometry swap as a live hit, without a new blast, sound or
     * script continuation. Some shootable crates have no one-shot script bit
     * at all, so EVE_ replay alone cannot reconstruct them. */
    if (c->explosives_ready) {
        u32 i;
        for (i = 0; i < c->explosives.count; i++) {
            q2_explosive *e = &c->explosives.items[i];
            q2_explosive_result vis;
            if (!s->explosive_count) {
                u32 j;
                /* Older files still carry broken shootable boxes in BRKS. */
                for (j = 0; j < c->sim[0].breakable_count; j++) {
                    const q2_breakable *b = &c->sim[0].breakable[j];
                    if (b->kind == Q2_BREAKABLE_FXGROUP &&
                        b->item_offset == e->item_offset && b->broken)
                        e->destroyed = 1;
                }
            }
            if (e->destroyed) {
                e->destroyed = 0;
                q2_explosive_trigger_item(&c->explosives, e->item_offset,
                                            true, &c->zone.scene, &vis);
            } else {
                /* The constructor already showed intact geometry. Repeating
                 * it here would undo an OBJDRAWOFF from the script replay. */
                continue;
            }
            client_apply_node_vis(c, &vis);
        }
    }

    /* The settings travel with the save, and applying them is what states 14
     * and 16 do on the console (0x8001C698, the GAME VARIABLES application). */
    {
        s16 v[Q2_SET_COUNT];
        u32 n = q2_save_get_settings(s, v, (u32)Q2_SET_COUNT);
        u32 k;
        for (k = 0; k < n; k++)
            c->settings.v[k] = v[k];
    }
    client_apply_settings(c);

    q2_save_apply_mission(s, &c->mission);
    /*
     * ...and find this level's row again in the table that just replaced the
     * one it was registered into. The load runs the map first, so the row this
     * client is holding is an index into the table the save has now
     * overwritten — pointing, on a save made in a later level, at somebody
     * else's counters. Re-registering resolves it by name, which is the only
     * key the table has.
     */
    if (c->map_unit > 0)
        client_mission_enter(c);

    /*
     * THE HELP COMPUTER'S PAIR, re-resolved rather than restored as text.
     *
     * The restore arm of every unit hub hands the two saved keys back to
     * `HelpComputer_SetMessages` (0x800229C8 and its four siblings), which does
     * the 0x800701B4 lookups again — so the pair is resolved against the level
     * being resumed, not against the one it was saved in. That is why the save
     * carries keys. A key that misses here falls to the shipped default at
     * 0x8009B520/0x8009B534, exactly as a live HELPCOMPUTER whose key is not in
     * this level's Strings does.
     *
     * A file older than version 8 has no chunk; the session's pair stands,
     * which is the closest thing to "this save predates the question".
     */
    {
        q2_save_help hk;

        if (q2_save_get_help_keys(s, &hk)) {
            const char *ot = NULL, *bt = NULL;

            if (c->leveltext_ready) {
                if (hk.orders_key[0])
                    ot = q2_leveltext_find(&c->leveltext, hk.orders_key);
                if (hk.objective_key[0])
                    bt = q2_leveltext_find(&c->leveltext, hk.objective_key);
            }
            q2_briefing_popup_set(&c->popup, hk.orders_key, ot,
                                  hk.objective_key, bt);
        }
    }

    if (c->movers_ready)
        q2_save_apply_movers(s, &c->movers);
    if (q2_save_apply_rotators(s, &c->rotators) != Q2_OK) {
        Q2_ERROR("save rotators do not match this map");
        return false;
    }
    q2_sim_scene_solids_update(&c->sim[0], &c->rotators);
    {
        u32 phase_us = 0;
        if (c->creatures_ready) {
            q2_save_apply_creatures(s, &c->creatures.set);
            client_cre_area_forget(c);
        }
        if (q2_save_apply_ai(s, &c->creatures, &phase_us) != Q2_OK) {
            Q2_ERROR("save creature behavior does not match this map");
            return false;
        }
        c->ai_accum = (double)phase_us / 1000000.0;
    }
    if (q2_save_get_skill(s, &saved_skill)) c->menu.skill = saved_skill;

    /*
     * AND THE LIVE COUNTERS, which the restore used to destroy.
     *
     * `0x8003DD9C` calls `0x80022210` on the level being resumed, and that is
     * the only caller in the executable: four `lbu`s off the restored row into
     * the four live words. The port had the table and not the words —
     * `client_load_zone` above has just zeroed `secrets_found` (this is not a
     * `carry_same_map` transition), and `client_mission_update` writes the
     * client's counters into the row every frame, so the next frame put the
     * zero it had just been handed back into the row the save had restored.
     * A game saved with two secrets found reloaded as 0/2 and then saved as
     * 0/2, and every secret in the level could be walked into again.
     *
     * Version 7 carries the client's own state, which is a superset: the same
     * found count plus the dedupe list and the per-zone kill slots, neither of
     * which the console needs (see `q2_save_level_counters`). An older file
     * has no such chunk and falls back to the console's own move, the row.
     */
    {
        q2_save_level_counters lc;

        if (q2_save_get_level_counters(s, &lc)) {
            u32 i;

            c->secrets_found     = lc.secrets_found;
            c->secret_seen_count = lc.secret_seen_count;
            if (c->secret_seen_count >
                sizeof(c->secret_seen) / sizeof(c->secret_seen[0]))
                c->secret_seen_count =
                    sizeof(c->secret_seen) / sizeof(c->secret_seen[0]);
            for (i = 0; i < c->secret_seen_count; i++)
                c->secret_seen[i] = lc.secret_seen[i];

            /*
             * The zone slots go back whole. The resident zone's is skipped by
             * the tally in favour of the live set, which `q2_save_apply_
             * creatures` has just restored above — so it does not matter that
             * the slot and the set agree, only that the zones already left do.
             */
            for (i = 0; i < Q2_SAVE_LEVEL_ZONES; i++) {
                c->zone_dead[i]   = lc.zone_dead[i];
                c->zone_placed[i] = lc.zone_placed[i];
            }
        } else if (c->mission_row >= 0) {
            int secrets = 0;

            /*
             * 0x80022210, on a file from before the chunk existed. Only
             * `secrets` is taken back: the other three counters the console
             * reloads are derived here rather than stored — `secrets_total`
             * is recounted from the map's INSECRET items by the load above,
             * and both kill figures come from the creature world — so pulling
             * them out of the row would be overwritten on the same frame by
             * `client_mission_update` and could disagree with the creature set
             * the save restored.
             *
             * DEVIATION, stated because it is visible: the seen-list cannot be
             * recovered from a number, so a pre-version-7 save that is
             * reloaded standing inside a secret volume can count that secret a
             * second time and read one over the map's total. That is still the
             * better of the two answers available — the alternative is the
             * zero this whole block exists to stop — and it cannot happen to a
             * save this build writes.
             */
            if (q2_mission_get_counts(&c->mission, c->mission_row,
                                      &secrets, NULL, NULL, NULL))
                c->secrets_found = (u32)secrets;
        }
    }

    /* Resume the live machine, including a selection that has not yet reached
     * the hands. Legacy saves only recorded the selected weapon. */
    if (c->vm_ready) {
        client_reset_view_model(c, 0);
        if (q2_save_apply_viewweapon(s, &c->vw[0], &c->vm_tables) != Q2_OK) {
            Q2_ERROR("save has an incompatible view-weapon animation");
            return false;
        }
        if (s->viewweapon_present) {
            c->vw_last_weapon[0] = s->viewweapon_selected;
            c->shot_serial_shown[0] = s->shot_serial_shown;
            c->shot_serial_heard[0] = s->shot_serial_heard;
            c->cock_pending[0] = s->cock_pending;
            client_bind_view_model(c, 0);
        }
    }
    q2_save_apply_game_rng(s);

    /* A restored game is a played game, so it resumes under the simulation
     * rather than in the free-fly camera. */
    c->sim_enabled = true;

    q2_sim_eye(&c->sim[0], eye);
    c->cam.pos[0] = eye[0];
    c->cam.pos[1] = eye[1];
    c->cam.pos[2] = eye[2];
    c->cam.yaw    = c->sim[0].player[0].yaw;
    c->cam.pitch  = c->sim[0].player[0].pitch;

    Q2_INFO("loaded %s zone %d at %d:%02d",
            s->map, (int)s->zone,
            (int)(s->level_time / 300 / 60), (int)(s->level_time / 300 % 60));
    return true;
}

static void client_notify(client *c, const char *text)
{
    Q2_INFO("%s", text);
    if (c->hud_ready)
        q2_hud_message(&c->hud[0], text);
}

/* ------------------------------------------------------------------------- */
/* The front end                                                              */
/* ------------------------------------------------------------------------- */
#define Q2_MP_SETTINGS_SCHEMA 1
#define Q2_MP_SETTINGS_HEADER_WORDS 2
#define Q2_MP_SETTINGS_SETUP_WORDS  6
_Static_assert(Q2_MP_SETTINGS_HEADER_WORDS + Q2_SET_COUNT +
                   Q2_MP_SETTINGS_SETUP_WORDS <= Q2_SETTINGS_VALUE_MAX,
               "menu settings no longer fit the card settings payload");

static void client_settings_pack(const client *c, q2_settings_blob *out)
{
    u32 i, at;

    memset(out, 0, sizeof(*out));
    out->value[0] = Q2_MP_SETTINGS_SCHEMA;
    out->value[1] = Q2_SET_COUNT;
    for (i = 0; i < Q2_SET_COUNT; i++)
        out->value[Q2_MP_SETTINGS_HEADER_WORDS + i] = c->settings.v[i];

    at = Q2_MP_SETTINGS_HEADER_WORDS + Q2_SET_COUNT;
    out->value[at++] = c->menu.mp_setup.mode;
    out->value[at++] = c->menu.mp_setup.players;
    out->value[at++] = c->menu.mp_setup.arena;
    out->value[at++] = c->menu.mp_setup.time_option;
    out->value[at++] = c->menu.mp_setup.frag_option;
    out->value[at++] = c->menu.mp_setup.round_option;
    out->count = at;
}

static bool client_settings_unpack(client *c, const q2_settings_blob *in)
{
    q2_menu_mp_setup setup;
    u32 stored, copy, at, i;

    if (!in || in->count < Q2_MP_SETTINGS_HEADER_WORDS ||
        in->value[0] != Q2_MP_SETTINGS_SCHEMA || in->value[1] < 0)
        return false;

    stored = (u32)in->value[1];
    if (stored > Q2_SETTINGS_VALUE_MAX ||
        in->count < Q2_MP_SETTINGS_HEADER_WORDS + stored +
                    Q2_MP_SETTINGS_SETUP_WORDS)
        return false;

    copy = stored < Q2_SET_COUNT ? stored : Q2_SET_COUNT;
    for (i = 0; i < copy; i++)
        c->settings.v[i] = in->value[Q2_MP_SETTINGS_HEADER_WORDS + i];

    at = Q2_MP_SETTINGS_HEADER_WORDS + stored;
    setup.mode         = in->value[at++];
    setup.players      = in->value[at++];
    setup.arena        = in->value[at++];
    setup.time_option  = in->value[at++];
    setup.frag_option  = in->value[at++];
    setup.round_option = in->value[at++];

    if (setup.mode != Q2_MENU_MP_DEATHMATCH &&
        setup.mode != Q2_MENU_MP_TEAM_DEATHMATCH &&
        setup.mode != Q2_MENU_MP_VERSUS)
        setup.mode = Q2_MENU_MP_DEATHMATCH;
    if (setup.players < 2) setup.players = 2;
    if (setup.players > Q2_MENU_MP_MAX_PLAYERS)
        setup.players = Q2_MENU_MP_MAX_PLAYERS;
    if (setup.arena < 0 || setup.arena >= Q2_MENU_MP_ARENA_COUNT)
        setup.arena = 0;
    if (setup.time_option < 0 ||
        setup.time_option >= Q2_MENU_MP_TIME_OPTIONS)
        setup.time_option = Q2_MENU_MP_TIME_DEFAULT;
    if (setup.frag_option < 0 ||
        setup.frag_option >= Q2_MENU_MP_FRAG_OPTIONS)
        setup.frag_option = Q2_MENU_MP_FRAG_DEFAULT;
    if (setup.round_option < 0 ||
        setup.round_option >= Q2_MENU_MP_ROUND_OPTIONS)
        setup.round_option = Q2_MENU_MP_ROUND_DEFAULT;

    c->menu.mp_setup = setup;
    client_apply_settings(c);
    return true;
}

static void client_card_return(client *c)
{
    int page = c->card_return_page;

    c->card_return_page = Q2_PAGE_NONE;
    if (!c->in_front_end || page == Q2_PAGE_NONE)
        return;

    q2_menu_open(&c->menu);
    q2_menu_goto(&c->menu, page);
}

static void client_card_open(client *c, q2_save_ui_mode mode)
{
    q2_settings_blob settings;

    if (mode == Q2_SAVE_UI_SAVE && !client_capture(c)) {
        client_notify(c, "CANNOT SAVE");
        return;
    }

    c->card_return_page = Q2_PAGE_NONE;
    if (c->in_front_end) {
        if (mode == Q2_SAVE_UI_SETTINGS_SAVE ||
            mode == Q2_SAVE_UI_SETTINGS_LOAD)
            c->card_return_page = Q2_PAGE_FRONT_MULTI;
        else if (mode == Q2_SAVE_UI_LOAD)
            c->card_return_page = Q2_PAGE_FRONT_NEWLOAD;
        else if (c->menu.open)
            c->card_return_page = c->menu.page_id;
    }

    c->card_mode = mode;
    if (mode == Q2_SAVE_UI_SAVE)
        q2_save_ui_open_save(&c->save_ui, &c->snapshot);
    else if (mode == Q2_SAVE_UI_LOAD)
        q2_save_ui_open_load(&c->save_ui);
    else if (mode == Q2_SAVE_UI_SETTINGS_SAVE) {
        client_settings_pack(c, &settings);
        q2_save_ui_open_settings_save(&c->save_ui, &settings);
    } else {
        q2_save_ui_open_settings_load(&c->save_ui);
    }

    /* A fresh session: the cursor, the pad edge and the screen all start
     * clean, so the button that opened the front end cannot also pick a row. */
    q2_mcard_init(&c->mcard, &c->mcard_host);
    c->card_screen  = Q2_MCARD_NONE;
    c->card_menu.page = NULL;
    c->mcard_open   = true;

    client_menu_close(c);
    c->mission_open = false;
}

static void client_card_close(client *c)
{
    q2_save_ui_close(&c->save_ui);
    c->mcard_open = false;
    client_card_return(c);
}

/*
 * The row text, and the one place the port has to put something on screen that
 * the console's own screen gets from the card's directory.
 *
 * An empty slot is the EMPTY STRING when loading, which is exactly right: the
 * selection bar tests the label against the empty string, so the row draws
 * nothing and cannot be aimed at (memcard.h). When SAVING it cannot be empty,
 * because a player has to be able to pick a free slot to write into — so it
 * carries the slot number and nothing else, which is the least this port can
 * invent and still work.
 */
static void client_card_rows(client *c)
{
    int i;

    for (i = 0; i < Q2_SAVE_SLOTS && i < Q2_MENU_MAX_ITEMS - 1; i++) {
        const q2_save_info *info = &c->save_ui.info[i];
        char *dst = c->card_menu.text[i + 1];

        if (info->used) {
            snprintf(dst, Q2_MENU_TEXT_MAX, "%s", q2_save_ui_row(&c->save_ui, i));
            c->card_menu.disabled[i + 1] = 0;
        } else if (c->card_mode == Q2_SAVE_UI_SAVE ||
                   c->card_mode == Q2_SAVE_UI_SETTINGS_SAVE) {
            snprintf(dst, Q2_MENU_TEXT_MAX, "%d", i + 1);
            c->card_menu.disabled[i + 1] = 0;
        } else {
            dst[0] = '\0';
            c->card_menu.disabled[i + 1] = 1;
        }
    }
}

/* Point the shadow menu at the screen the current state shows, and fill in
 * whatever that screen composes at run time. */
static void client_card_sync(client *c)
{
    q2_mcard_screen want = q2_mcard_screen_for_state_port(c->save_ui.state);
    const q2_menu_page *page;

    if (want != c->card_screen) {
        c->card_screen = want;
        page = q2_mcard_page(want);

        memset(c->card_menu.text, 0, sizeof(c->card_menu.text));
        memset(c->card_menu.disabled, 0, sizeof(c->card_menu.disabled));
        c->card_menu.page   = page;
        c->card_menu.cursor = page ? (int)page->first : 0;
        c->card_menu.open   = (page != NULL);
        c->mcard.cursor     = 0;
    }

    page = c->card_menu.page;
    if (!page)
        return;

    if (c->card_screen == Q2_MCARD_SAVE_FILE) {
        client_card_rows(c);
    } else if (c->card_screen == Q2_MCARD_LOAD_MESSAGE) {
        /*
         * The screen whose text the runtime composes — which is why state 13
         * maps to it (memcard.h). BOTH of its rows are placeholders, and both
         * have to be written: an empty override falls back to the table's own
         * label, so leaving the second alone leaves the word HERE on screen.
         * A single space is what blanks a line the report does not need.
         */
        snprintf(c->card_menu.text[0], Q2_MENU_TEXT_MAX, "%s",
                 c->save_ui.message);
        snprintf(c->card_menu.text[1], Q2_MENU_TEXT_MAX, "%s",
                 c->save_ui.detail[0] ? c->save_ui.detail : " ");
    }
}

/* What the front end left behind when it closed. */
static void client_card_finish(client *c)
{
    q2_save loaded;
    q2_settings_blob settings;
    bool settings_mode = c->card_mode == Q2_SAVE_UI_SETTINGS_SAVE ||
                         c->card_mode == Q2_SAVE_UI_SETTINGS_LOAD;

    switch (c->save_ui.status) {
    case Q2_SAVE_UI_SAVED:
        client_notify(c, settings_mode ? "SETTINGS SAVED" : "GAME SAVED");
        break;

    case Q2_SAVE_UI_LOADED:
        if (settings_mode &&
            q2_save_ui_take_settings(&c->save_ui, &settings)) {
            bool ok = client_settings_unpack(c, &settings);
            client_notify(c, ok ? "SETTINGS LOADED" : "LOAD FAILED");
        } else if (!settings_mode &&
                   q2_save_ui_take_loaded(&c->save_ui, &loaded)) {
            bool ok = client_apply_save(c, &loaded);
            q2_save_free(&loaded);
            client_notify(c, ok ? "GAME LOADED" : "LOAD FAILED");
        }
        break;

    case Q2_SAVE_UI_FAILED:
        client_notify(c, c->save_ui.message[0] ? c->save_ui.message
                                               : "SAVE FAILED");
        break;

    default:
        break;
    }

    c->mcard_open = false;
    client_card_return(c);
}

static void client_card_frame(client *c)
{
    u16 pad;
    const q2_menu_page *page;
    q2_menu_sound snd;

    /*
     * Last frame's work first. The read or write is deferred by exactly one
     * frame so the busy screen is actually drawn — which is what the console
     * has a DO NOT POWER-OFF screen for, and what a save that completes inside
     * the same frame never shows.
     */
    q2_save_ui_update(&c->save_ui);

    client_card_sync(c);
    page = c->card_menu.page;

    /*
     * The pointer, over the same shadow menu the screens are drawn from — so
     * the card front end is clickable for exactly the same reason it is
     * navigable, which is that its screens ARE menu pages.
     */
    pad  = client_menu_pointer(c, &c->card_menu);
    pad |= client_menu_pad(c);

    /* TRIANGLE backs out. The console's own arms do not handle it — they are
     * four instructions long and test CROSS only — so this is the port's, and
     * without it a front end with no live arm would be a trap. The right mouse
     * button is folded into TRIANGLE by client_menu_pad, so it backs out here
     * too. */
    if ((pad & Q2_PAD_TRIANGLE) && !(c->card_menu.pad_prev & Q2_PAD_TRIANGLE)) {
        c->card_menu.pad_prev = pad;
        client_card_close(c);
        return;
    }

    /* Navigation, through the real menu engine so the wrap and skip rules are
     * the ones read out of the executable. */
    q2_menu_advance(&c->card_menu, pad);

    snd = q2_menu_take_sound(&c->card_menu);
    if (snd != Q2_MSND_NONE)
        client_play_menu_sound(c, snd);

    /* The front end reads the cursor POSITIONALLY, as `cursor - first`
     * (0x800B32AC minus 0x800B32AE). */
    if (page) {
        int rel = c->card_menu.cursor - (int)page->first;
        c->mcard.cursor = rel < 0 ? 0 : rel;
    }

    if (q2_mcard_advance(&c->mcard, pad)) {
        /* The accept arm applies the game variables and leaves (0x8001F0A4). */
        client_apply_settings(c);
    }

    /*
     * State 13 is live and has no arm of its own, so the press that dismisses
     * the report is the port's — see q2_save_ui_acknowledge.
     */
    if (c->mcard.fired && c->save_ui.state == Q2_SAVEUI_STATE_REPORT)
        q2_save_ui_acknowledge(&c->save_ui);

    if (!c->save_ui.open) {
        client_card_finish(c);
        return;
    }

    /*
     * Again, because the arms above may have changed the state and this frame
     * still has to be DRAWN. Without it the busy screen would be skipped
     * entirely: the work happens at the top of the next frame, so the frame in
     * between is the only one that can show it.
     */
    client_card_sync(c);
}

/* ------------------------------------------------------------------------- */
/* "DO YOU WANT TO SAVE?" — InGame_GOTO_WANT_TO_SAVE, 0x800203C4               */
/* ------------------------------------------------------------------------- */
/*
 * 0x800203C4 is five things in twenty instructions:
 *
 *   800203D0  jal 0x8001A384(39)      enter page 39
 *   800203E0  a0 = 0x8009B3B4, a1 = 16 install the caption table — one record,
 *                                     "DO YOU WANT TO SAVE?" (0x800AB18C) at
 *                                     (256, 98)
 *   800203F8  sh 1, 0x800B348A        the flag GameCheckPaused tests at
 *                                     0x8003E6A0 so START cannot dismiss it
 *   80020400  a0 = 0x8009B3E4, a1 = 16 install the answers — "YES" (0x800AE644)
 *                                     at (256, 124) with action 0x80020428,
 *                                     "NO" (0x800AE640) at (256, 150) with
 *                                     action 0x80020140 (InGame_GOTO_RESUME)
 *   80020414  sw zero, 0x800B3270
 *
 * The port already carries both tables, transcribed in memcard.c as
 * Q2_MCARD_SAVE_PROMPT with `first` = 1, so YES is the row the cursor starts
 * on and NO is the one below it. YES is the only record in the whole card
 * front end with a real action, and 0x80020428 is what this port's
 * `client_card_open(Q2_SAVE_UI_SAVE)` already stands in for — it is the same
 * call F7 makes. NO is the ordinary resume.
 *
 * The world does not tick behind it: `Paused` is up (0x8003E754) and
 * MainGameLoop's GameIsPaused check at 0x800183C4 zeroes the frame delta, so
 * the arm in the frame loop below runs this and nothing else.
 */

/*
 * HEADLESS PICKS "NO", AND THIS IS THE PORT'S, NOT THE CONSOLE'S.
 *
 * The console has no timeout on this screen at all: it waits for a press, and
 * a windowed player here waits with it. A scripted run cannot press anything,
 * and a prompt that waited would stop every sweep this project takes at the
 * first level boundary — which is the same problem the tally board has, and
 * this is the same answer it already got (Q2_INTERMISSION_HEADLESS). Two
 * frames, so the page is composed and drawn at least once and a capture taken
 * across a level change still shows it.
 */
#define Q2_SAVE_PROMPT_HEADLESS 2

static void client_save_prompt_close(client *c)
{
    c->save_prompt_open      = false;
    c->save_prompt_menu.open = false;
    c->save_prompt_menu.page = NULL;
}

static void client_save_prompt_open(client *c)
{
    const q2_menu_page *page = q2_mcard_page(Q2_MCARD_SAVE_PROMPT);

    /* 0x8003E718: never in deathmatch. An arena has no save to write and
     * nothing to pause for. */
    if (!page || c->mp_enabled || c->in_front_end)
        return;

    memset(c->save_prompt_menu.text, 0, sizeof(c->save_prompt_menu.text));
    memset(c->save_prompt_menu.disabled, 0,
           sizeof(c->save_prompt_menu.disabled));
    c->save_prompt_menu.page   = page;
    c->save_prompt_menu.cursor = (int)page->first;   /* YES */
    c->save_prompt_menu.open   = true;
    c->save_prompt_menu.pad_prev = 0;
    c->save_prompt_pad     = 0;
    c->save_prompt_frames  = 0;
    c->save_prompt_open    = true;
    c->save_prompts++;

    /* The prompt owns the screen. Nothing else may be standing behind it —
     * 0x800203D0 enters a page, which replaces whatever was up. */
    client_menu_close(c);
    c->mission_open  = false;
    c->briefing_open = false;

    Q2_INFO("SAVE?: %s asks whether to save", c->map);
}

static void client_save_prompt_frame(client *c)
{
    const q2_menu_page *page = c->save_prompt_menu.page;
    q2_menu_sound snd;
    u16 pad, released;
    int row;

    if (!page) {
        client_save_prompt_close(c);
        return;
    }

    c->save_prompt_frames++;

    pad  = client_menu_pointer(c, &c->save_prompt_menu);
    pad |= client_menu_pad(c);

    /*
     * ESCAPE AND TRIANGLE DO NOTHING HERE, which is the point of 0x800B348A:
     * 0x800203F8 raises it and GameCheckPaused's START arm refuses to unpause
     * while it is up (0x8003E6A0). One of the two answers is the only way out,
     * so the key handler in the frame loop leaves this screen alone as well.
     */
    q2_menu_advance(&c->save_prompt_menu, pad);

    snd = q2_menu_take_sound(&c->save_prompt_menu);
    if (snd != Q2_MSND_NONE)
        client_play_menu_sound(c, snd);

    /*
     * THE TWO ANSWERS ARE TAKEN FROM DIFFERENT PLACES, because on the console
     * they ARE different things.
     *
     * NO is 0x80020140, InGame_GOTO_RESUME — an ordinary menu action, which
     * this port already carries as Q2_ACT_RESUME on that record, so the engine
     * has run it by the time we get here and left the request behind. Taking
     * it from the request rather than from the cursor matters: Q2_ACT_RESUME
     * calls q2_menu_close, and a reading that went back to the cursor
     * afterwards would depend on what a close happens to leave there.
     *
     * YES is 0x80020428, which is not a page transition in this engine's sense
     * — it is the card front end, and the client owns that — so it is read
     * positionally, exactly as the card screens read their own rows
     * (`cursor - first`, 0x800B32AC minus 0x800B32AE).
     */
    if (q2_menu_take_request(&c->save_prompt_menu) == Q2_MREQ_RESUME) {
        client_save_prompt_close(c);
        return;
    }

    if (c->headless && c->save_prompt_frames >= Q2_SAVE_PROMPT_HEADLESS) {
        Q2_INFO("SAVE?: headless — taking NO");
        client_save_prompt_close(c);
        return;
    }

    /*
     * The release rule for YES. Both records carry bit 13 (memcard.c's
     * ANSWER), which is the on-release idiom the confirmations use, and the
     * edge starts empty so the button that was down when the level changed
     * cannot answer the question it raised.
     */
    released = (u16)(c->save_prompt_pad & ~pad);
    c->save_prompt_pad = pad;

    if ((pad & Q2_PAD_CROSS) != 0 || (released & Q2_PAD_CROSS) == 0)
        return;

    row = c->save_prompt_menu.cursor - (int)page->first;
    if (row == 0) {
        client_save_prompt_close(c);
        client_card_open(c, Q2_SAVE_UI_SAVE);
    }
}

/* ------------------------------------------------------------------------- */
/* Quick save and quick load — slot 1, no screens.                            */
/*                                                                            */
/* Entirely the port's: the console has no such thing, and it is here because  */
/* a four-screen front end is the wrong tool for "try that jump again". It     */
/* goes through exactly the same capture, file and apply paths, so it cannot   */
/* drift from what the front end writes.                                      */
/* ------------------------------------------------------------------------- */
static void client_quick_save(client *c)
{
    q2_result rc;

    if (!client_capture(c)) {
        client_notify(c, "CANNOT SAVE");
        return;
    }

    rc = q2_save_slot_write(&c->snapshot, 0);
    if (rc != Q2_OK) {
        Q2_ERROR("quick save failed: %s", q2_result_str(rc));
        client_notify(c, "SAVE FAILED");
        return;
    }

    client_notify(c, "QUICK SAVED");
}

static void client_quick_load(client *c)
{
    q2_save s;
    q2_result rc = q2_save_slot_read(&s, 0);

    if (rc != Q2_OK) {
        Q2_ERROR("quick load failed: %s", q2_result_str(rc));
        client_notify(c, rc == Q2_ERR_NOT_FOUND ? "NO QUICK SAVE"
                                                : "LOAD FAILED");
        return;
    }

    client_notify(c, client_apply_save(c, &s) ? "QUICK LOADED" : "LOAD FAILED");
    q2_save_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * A screenshot, of the console's framebuffer rather than of the window.
 *
 * Entirely the port's, and the distinction is the point: the window is an
 * upscale of a 512x248 buffer, so grabbing it back off the desktop resamples
 * the very pixels — the dither pattern, the vertex snapping — that the whole
 * renderer exists to get right. This writes the buffer the frame was composed
 * into, at its own size, through the same P6 writer the offline tools use.
 */
static void client_screenshot(client *c)
{
    static int n = 0;
    char path[64];
    q2_result rc;

    snprintf(path, sizeof(path), "q2psx-%03d.ppm", n);

    rc = psx_fb_write_ppm(q2_screen_front(&c->screen), path);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot write %s: %s", path, q2_result_str(rc));
        return;
    }

    n++;
    Q2_INFO("screenshot: %s", path);
}

/* `--shot`'s numbering, shared with `--shot-display`. */
static void shot_path_for(const char *base, long frame, bool numbered,
                          char *out, size_t out_sz)
{
    if (numbered) {
        const char *dot = strrchr(base, '.');
        size_t stem = dot ? (size_t)(dot - base) : strlen(base);

        if (stem > out_sz - 32)
            stem = out_sz - 32;
        memcpy(out, base, stem);
        snprintf(out + stem, out_sz - stem, "_%04ld%s",
                 frame, dot ? dot : ".ppm");
    } else {
        snprintf(out, out_sz, "%s", base);
    }
}

/*
 * `--shot-display`: the finished frame as the television is shown it.
 *
 * `--shot` writes what the console DREW, which is the right thing for every
 * pixel comparison in this project — but it is taken before presentation, so
 * nothing in it can show what the display envs do. This runs the real
 * placement, q2_screen_place over the live and neutral ranges exactly as
 * client_present blits them, and samples the front buffer through it: the
 * cropped rows are absent, SCREEN POSITION has moved the picture, and the
 * border is the black the window is cleared to.
 *
 * Nearest-neighbour on purpose. The point is to see WHERE the picture is and
 * how much of it there is, and an interpolated edge would blur the one thing
 * the capture is for.
 */
static void client_write_shot_display(client *c, bool numbered)
{
    char path[512];
    psx_framebuffer out;
    q2_screen_placement pl;
    int win_w = 0, win_h = 0, x, y;
    q2_result rc;

    if (!c->shot_display_path)
        return;

    /* Headless has no renderer to ask, so the window is the one `--scale`
     * would have opened. */
    q2_screen_window_size(&c->screen, c->fit, c->win_scale > 0 ? c->win_scale : 3,
                          &win_w, &win_h);
    if (win_w < 1 || win_h < 1)
        return;

    client_place(c, win_w, win_h, &pl);

    memset(&out, 0, sizeof(out));
    out.width  = win_w;
    out.height = win_h;
    out.px     = calloc((size_t)win_w * (size_t)win_h, sizeof(u16));
    if (!out.px) {
        Q2_ERROR("cannot allocate a %dx%d display capture", win_w, win_h);
        return;
    }

    if (pl.src_w > 0 && pl.src_h > 0 && pl.dst_w > 0.0f && pl.dst_h > 0.0f) {
        const psx_framebuffer *front = q2_screen_front(&c->screen);

        for (y = 0; y < win_h; y++) {
            for (x = 0; x < win_w; x++) {
                double bx, by;
                int sx, sy;

                if (!q2_screen_window_to_buffer(&pl, (double)x + 0.5,
                                                (double)y + 0.5, &bx, &by))
                    continue;
                sx = (int)bx;
                sy = (int)by;
                if (bx < 0.0 || by < 0.0 ||
                    sx < pl.src_x || sx >= pl.src_x + pl.src_w ||
                    sy < pl.src_y || sy >= pl.src_y + pl.src_h ||
                    sx >= front->width || sy >= front->height)
                    continue;       /* border: leave it the cleared colour */
                out.px[(size_t)y * (size_t)win_w + (size_t)x] =
                    front->px[(size_t)sy * (size_t)front->width + (size_t)sx];
            }
        }
    }

    shot_path_for(c->shot_display_path, c->frame_index, numbered,
                  path, sizeof(path));
    rc = psx_fb_write_ppm(&out, path);
    free(out.px);

    if (rc != Q2_OK) {
        Q2_ERROR("cannot write %s: %s", path, q2_result_str(rc));
        return;
    }
    Q2_INFO("display %ld -> %s  (%dx%d window, %d x %d rows from %d)",
            c->frame_index, path, win_w, win_h, pl.src_w, pl.src_h, pl.src_y);
}

/*
 * The capture a scripted run writes.
 *
 * `--shot out.ppm` alone writes the last frame to that name; with `--shot-every`
 * it becomes a stem — `out.ppm` -> `out_0000.ppm`, `out_0030.ppm` — so a run
 * produces a strip that can be flipped through. The framebuffer is written, not
 * the window: these are the console's own 512x248 pixels, which is what every
 * comparison in this project is made in. `--shot-display` writes the window
 * alongside it.
 */
static void client_write_shot(client *c, bool numbered)
{
    char path[512];
    q2_result rc;

    /* Either capture can be asked for on its own; `shots_written` counts
     * frames captured, so a display-only run still tells the final-frame
     * check that this frame is done. */
    client_write_shot_display(c, numbered);

    if (!c->shot_path) {
        if (c->shot_display_path)
            c->shots_written++;
        return;
    }

    shot_path_for(c->shot_path, c->frame_index, numbered, path, sizeof(path));

    rc = psx_fb_write_ppm(q2_screen_front(&c->screen), path);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot write %s: %s", path, q2_result_str(rc));
        return;
    }


    c->shots_written++;
    Q2_INFO("frame %ld -> %s", c->frame_index, path);
    Q2_INFO("  eye %d %d %d  yaw %d pitch %d roll %d  cell %d  "
            "%u/%u quads, %u nodes, sort entities %u/%u (%u collapsed), "
            "near %u back %u bounds %u flat %u depth %u..%u ot %u",
            c->cam.pos[0], c->cam.pos[1], c->cam.pos[2],
            c->cam.yaw, c->cam.pitch, c->cam.roll, c->sim[0].current_node,
            c->shot_stats.quads_emitted, c->shot_stats.quads_total,
            c->shot_stats.nodes_visited,
            c->shot_stats.sort_entities_visible,
            c->shot_stats.sort_entities,
            c->shot_stats.sort_entities_degenerate,
            c->shot_stats.quads_rejected_near,
            c->shot_stats.quads_rejected_back,
            c->shot_stats.quads_rejected_bounds,
            c->shot_stats.quads_rejected_flat,
            c->shot_stats.depth_min, c->shot_stats.depth_max,
            c->shot_stats.ot_overflow);

    /*
     * The lens flares. `lit` can say how many flare-carrying lights a cell
     * holds; only this can say how many of them reached the screen, because the
     * near cull and the attenuation both happen after the gather. A line of
     * zeroes where the cell has flares means the pass is not running at all,
     * which is a different fault from one where they are all culled.
     */
    Q2_INFO("  flares    %u lights, %u styled, %u too near, %u dark, "
            "%u drawn, %u prims",
            c->shot_stats.flare_lights, c->shot_stats.flare_styled,
            c->shot_stats.flare_near,   c->shot_stats.flare_dark,
            c->shot_stats.flare_drawn,  c->shot_stats.flare_prims);

    /*
     * The weapon in the hands against the shots the sim actually took. These two
     * figures are the whole point of the line: the machine is told about a shot
     * once per fire ATTEMPT, so a run where the clips outnumber the shots is one
     * where the client is sampling `last_shot` instead of consuming its serial —
     * the fire clip restarting at render rate, which is what it used to do.
     */
    if (c->vm_ready)
        Q2_INFO("  view weapon: %u fire clips, %u shots, %u dry, %u keys",
                c->vw[0].fires_started, c->shots_fired, c->shots_dry,
                c->vw[0].keys_played);

    /* How many times a screen owned the frame and the pad pair had to be
     * reseeded instead of rolled. Zero would mean the detection never fired. */
    Q2_INFO("  pad: %u resume%s", c->pad_resumes,
            c->pad_resumes == 1 ? "" : "s");

    /* The carousel's writes (client_sbar_write_slots, and the draw's latch).
     * A bar recomputed every drawn frame, which is what it used to be, would
     * be one write per bar per frame; the console writes at events alone. */
    Q2_INFO("  carousel  %u writes at a select, %u at an auto-select, "
            "%u at a spawn; %u inferred by the latch",
            c->slots_written[CLIENT_SLOTS_SELECT],
            c->slots_written[CLIENT_SLOTS_AUTOSELECT],
            c->slots_written[CLIENT_SLOTS_SPAWN], c->slots_inferred);

    if (c->mp_enabled) {
            int pi;

            for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
                const q2_combat_scan_stats *sc0 = &q2_combat_scan_by[pi];

                if (pi > 0 && !c->sim_ready[pi])
                    continue;
                if (pi == 0)
                    Q2_INFO("  scan[0]: %u tested, %u behind, %u beyond world,"
                            " %u off axis, %u hit",
                            sc0->tested, sc0->behind, sc0->beyond_world,
                            sc0->off_axis, sc0->hit);
                Q2_INFO("  player %d view weapon %d, %u prims, %u fire clips, linked %d",
                        pi, c->vw[pi].weapon, c->vw_drawn[pi],
                        c->vw[pi].fires_started, (int)c->death[pi].linked_weapon);
                Q2_INFO("  player %d damage flashes %u", pi, c->hud_flashes[pi]);
                Q2_INFO("  player %d shots %u, dry %u", pi,
                        c->mp_shots[pi], c->mp_dry[pi]);
                if (pi == 1)
                    Q2_INFO("  proj: %u launched, %u stepped, %u expired, "
                            "%u hit; near %u (past end %u), closest^2 %lld, "
                            "seg^2 %d", q2_sim_proj_scan.launched,
                            q2_sim_proj_scan.stepped, q2_sim_proj_scan.expired,
                            q2_sim_proj_scan.hit, q2_sim_proj_scan.near_miss,
                            q2_sim_proj_scan.past_end,
                            (long long)q2_sim_proj_scan.closest_sq,
                            q2_sim_proj_scan.seg_len);
                if (pi == 1)
                    Q2_INFO("  closest: owner %d, bolt at [%d %d %d], "
                            "target origin [%d %d %d]",
                            q2_sim_proj_scan.closest_owner,
                            q2_sim_proj_scan.closest_from[0],
                            q2_sim_proj_scan.closest_from[1],
                            q2_sim_proj_scan.closest_from[2],
                            q2_sim_proj_scan.closest_origin[0],
                            q2_sim_proj_scan.closest_origin[1],
                            q2_sim_proj_scan.closest_origin[2]);
                {
                    const q2_combat_scan_stats *sc = &q2_combat_scan_by[pi];

                    Q2_INFO("  scan[%d]: %u tested, %u behind, %u beyond world,"
                            " %u off axis, %u hit",
                            pi, sc->tested, sc->behind, sc->beyond_world,
                            sc->off_axis, sc->hit);
                }
                Q2_INFO("  player %d at [%d %d %d] yaw %d, %d hp, moved %ld",
                        pi, c->sim[0].player[pi].pos[0],
                        c->sim[0].player[pi].pos[1],
                        c->sim[0].player[pi].pos[2],
                        c->sim[0].player[pi].yaw,
                        pi == c->sim[0].cur_player
                            ? c->sim[0].combat.inv.health
                            : c->sim[0].pcombat[pi].inv.health,
                        labs(c->sim[0].player[pi].pos[0] - c->mp_view_pos[pi][0]) +
                        labs(c->sim[0].player[pi].pos[2] - c->mp_view_pos[pi][2]));
            }
        }

    if (c->misevents || c->misevent_count)
        Q2_INFO("  misevent  %u run (%u EXE, %u this map's, %u in neither), "
                "%u in the map's table%s%s",
                c->misevents, c->misevent_exe, c->misevent_map,
                c->misevent_unknown, c->misevent_count,
                c->misevent_last[0] ? ", last " : "",
                c->misevent_last[0] ? c->misevent_last : "");

    if (c->lasers.count || c->lasers.declined)
        Q2_INFO("  lasers    %u raised in this zone, %u queued this frame, "
                "%u declared dark here; pool %u queued %u dropped, "
                "%u beam faces drawn",
                c->lasers.count, c->laser_drawn, c->lasers.declined,
                c->sim[0].fx.stats.beams_queued,
                c->sim[0].fx.stats.beams_dropped,
                c->sim[0].fx.stats.beam_faces_emitted);

    if (c->teleports.count)
        Q2_INFO("  teleports %u pad%s registered in this zone, %u column%s "
                "queued this frame, %u light%s added over the run",
                c->teleports.count, c->teleports.count == 1 ? "" : "s",
                c->teleport_drawn, c->teleport_drawn == 1 ? "" : "s",
                c->teleport_lights, c->teleport_lights == 1 ? "" : "s");

    if (c->creatures_ready && c->creatures.set.count) {
        u32 i, live = 0, hunting = 0, dead = 0;
        long hp = 0;
        s32 near_d = -1;
        long moved = 0;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            s32 dx, dz, d;

            if (!m->in_use || m->dead) { if (m->dead) dead++; continue; }
            live++;
            hp += m->health;
            if (m->enemy)
                hunting++;

            moved += c->cre_home ? (labs(m->pos[0] - c->cre_home[i*3+0]) +
                                    labs(m->pos[2] - c->cre_home[i*3+2])) : 0;

            dx = m->pos[0] - c->cam.pos[0];
            dz = m->pos[2] - c->cam.pos[2];
            d  = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (near_d < 0 || d < near_d)
                near_d = d;
        }
        Q2_INFO("  creatures %u live, %u hunting, %u drawn (%u faces), "
                "nearest %d units, moved %ld, player %d hp, "
                "%u swings %u shots (%u fire reports, %u with no figures read), "
                "%u sounds (%u not in bank, "
                "%u unnamed), %u dead of %d, "
                "%ld hp total, "
                "player attacked %u, targets %u, bolts %u (%u faces, %u dropped "
                "on a full pool), %u bodies, "
                "rot %u steps %u moved %u turned, %u calls",
                live, hunting, c->cre_drawn, c->cre_faces, near_d, moved,
                c->sim[0].combat.inv.health, c->cre_swings, c->cre_shots,
                c->cre_fire_sounds, c->cre_fire_no_figures,
                c->cre_sounds, c->cre_sound_missing, c->cre_sound_unnamed,
                dead, q2_level_state.total_monsters, hp,
                c->player_attacks,
                c->sim[0].combat.target_count,
                c->sim[0].combat.projectiles.live,
                c->proj_prims, q2_sim_proj_scan.dropped_full,
                c->cre_bodies, c->rot_steps,
                c->rot_moved, client_rot_turned(c),
                c->sim[0].event_rt.call_count);
        /* The death drops and the damage effects' mesh — see client_cre_drop
         * and client_fx_mesh. "requested" is what the queue flushed; the
         * spawner can still decline one (no record, model or entity). */
        Q2_INFO("  drops     %u requested, %u spawned, %u declined, %u in flight;"
                " fx mesh %u asked, %u posed, %u with an effect running",
                c->cre_drop_requests, c->cre_drops, c->cre_drops_declined,
                q2_item_drop_in_flight(), c->fx_mesh_asked, c->fx_mesh_given,
                c->fx_mesh_armed);
        Q2_INFO("  gibs      %u bodies, %u chunks (%u unbound, %u refused),"
                " %u trail groups, %u landed; creatures handed %u posed"
                " meshes, %u a push",
                q2_gib_counters.destroyed, q2_gib_counters.chunks,
                q2_gib_counters.unbound, q2_gib_counters.refused,
                q2_gib_counters.trails, q2_gib_counters.landed,
                c->gib_mesh_posed, c->gib_pushed);
        /* Radius damage's occlusion (sim.h, q2_sim_proj_stats): candidates
         * asked, hidden by a wall or a solid box, and waved through from a
         * blast point in no cell — the port-only state splash_clear counts. */
        Q2_INFO("  splash    %u asked, %u occluded, %u from no cell",
                q2_sim_proj_scan.splash_asked,
                q2_sim_proj_scan.splash_occluded,
                q2_sim_proj_scan.splash_unplaced);
        /* The muzzle's clip into the hull (simcombat.c, clip_muzzle). */
        Q2_INFO("  muzzle    %u pulled back out of a wall, %u from no cell",
                q2_sim_proj_scan.muzzle_clipped,
                q2_sim_proj_scan.muzzle_unplaced);
        /* The DualShock (shock.h): SetShockData calls, the ones a slot
         * took, and player 0's fields with each motor running. */
        Q2_INFO("  shock     %u requests, %u taken; %u fields, big %u, "
                "little %u%s", c->shock_requests, c->shock_taken,
                c->shock_fields, c->shock_big_fields, c->shock_little_fields,
                c->settings.v[Q2_SET_VIBRATION] ? "" : " (VIBRATION off)");
        /* ...and which patterns they asked for, `id x count`, so a hit (13,
         * 15, 16), an explosion (13) and a Tank's step (14) can be told from
         * the weapons (1..12). */
        {
            char line[192];
            int  id, n = 0;

            line[0] = '\0';
            for (id = 1; id <= Q2_SHOCK_PATTERNS; id++)
                if (c->shock_by_id[id] && n < (int)sizeof(line) - 16)
                    n += snprintf(line + n, sizeof(line) - (size_t)n,
                                  " %dx%u", id, c->shock_by_id[id]);
            if (n)
                Q2_INFO("            by id%s", line);
        }
        /* The mixer, because "sounds are broken" needs a number to argue with.
         * `dropped` is voices that found all 24 busy — a steady stream of those
         * means something is raising more than the SPU could ever have played. */
        Q2_INFO("  audio     %u voices started, %u dropped on a full 24, "
                "%d bytes queued",
                c->voice_started, c->voice_dropped,
                c->audio ? SDL_GetAudioStreamQueued(c->audio) : 0);
        Q2_INFO("            quad %u shots asked for itm_damage3, "
                "%u refused because the last was still sounding",
                c->quad_raises, c->quad_gated);
        Q2_INFO("  pose      %u by name, %u named but no position, %u unnamed",
                c->pose_by_name, c->pose_name_no_pos, c->pose_no_name);
        Q2_INFO("            %u held the timeline's last frame; of the misses "
                "%u had no such name in block D",
                c->pose_held, c->pose_name_absent);
        Q2_INFO("  breakable %u GLASS calls broke something, %u pieces thrown;"
                " %u boxes registered, %u SHOT, %u pieces, %u SHOOTTHEN records raised",
                c->glass_calls, c->glass_pieces,
                c->sim[0].breakable_count, c->sim[0].breakable_hits,
                c->sim[0].breakable_pieces, c->sim[0].breakable_fired);
        Q2_INFO("            %u panes smashed audibly, %u the bank could not"
                " name",
                c->glass_sounds, c->glass_sounds_missed);
        Q2_INFO("  explosive %u groups, %u shootable parts; %u destroyed"
                " (%u by script), %u detonated, %u node show/hide,"
                " %u reports the bank carries (%u it does not),"
                " %u Explosion models spawned",
                c->explosives_ready ? c->explosives.count : 0u,
                c->explosive_boxes, c->sim[0].explosive_destroyed,
                c->explosive_scripted, c->sim[0].explosive_blasts,
                c->explosive_vis, c->explosive_sounds,
                c->explosive_sounds_missed,
                c->sim[0].explosive_models);
        /* Rockets and grenades through QuakeGenExplosion (simcombat.c). */
        Q2_INFO("  missiles  %u rockets and grenades went off, %u Explosion"
                " models spawned", c->sim[0].projectile_blasts,
                c->sim[0].projectile_models);
        /* QuakeGenExplosion's own bang, from every spawn that bound its
         * model — brush, missile or gib arm (modelent.h). */
        Q2_INFO("            %u Explosion bangs the bank carries (%u it"
                " does not), %u dropped on a full queue",
                c->explosion_sounds, c->explosion_sounds_missed,
                c->sim[0].explosion_sounds.dropped);
        Q2_INFO("  entities  %u drawn (%u faces, %u shadows),"
                " %u could not resolve a model, %u outside the zone",
                c->ent_drawn, c->ent_faces, c->ent_shadows,
                c->ent_no_model, c->ent_outside);
        Q2_INFO("  script    %u strings, %u sounds, %u gated by ONKEYDO, "
                "%u nodes hidden, %u summoned, %u teleports, %u timers, %u resumed,"
                " %u records retired",
                c->script_strings, c->script_sounds, c->script_gated,
                c->script_hidden, c->script_summoned, c->script_teleports,
                c->script_timers, c->sim[0].event_rt.resumed_count,
                c->script_disabled);
        {
            u32 mi;
            char kinds[256];
            size_t at = 0;

            kinds[0] = '\0';
            for (mi = 0; c->movers_ready && mi < c->movers.count; mi++) {
                u8 pr = c->movers.movers[mi].prim;
                const q2_uf_prim_info *pi =
                    (pr == Q2_MOVER_PRIM_OPCODE) ? NULL
                                                 : q2_uf_info((q2_uf_prim)pr);
                const char *nm = (pr == Q2_MOVER_PRIM_OPCODE)
                                 ? "MOVER_A/B/C" : (pi ? pi->name : NULL);

                if (!nm || strstr(kinds, nm))
                    continue;
                at += (size_t)snprintf(kinds + at, sizeof(kinds) - at,
                                       "%s%s", at ? " " : "", nm);
            }
            if (at)
                Q2_INFO("  movers    built from: %s", kinds);
        }
        {
            /*
             * The trains, one line each. A PLATFORM is rare enough — one on
             * the disc — that a count would say nothing, and its path is the
             * one mover operand a reader cannot infer from the payload: the
             * direction comes from a Scene node's box centre, so it exists only
             * after the build has had the zone's Scene in hand.
             */
            u32 mi;

            for (mi = 0; c->movers_ready && mi < c->movers.count; mi++) {
                const q2_mover *m = &c->movers.movers[mi];
                s32 d[3];

                s32 home[3] = { 0, 0, 0 };

                if (!m->is_path)
                    continue;
                q2_mover_displacement(m, d);
                if (m->part_count)
                    client_node_centre(c, m->node[0], home);
                Q2_INFO("  train     %u part%s (node %d%s) from (%d,%d,%d), "
                        "path (%d,%d,%d) len %d, speed %d, "
                        "at %d/%d -> (%d,%d,%d), %u running rumbles, %u stops",
                        m->part_count, m->part_count == 1 ? "" : "s",
                        m->part_count ? m->node[0] : -1,
                        m->part_count > 1 ? ", +more" : "",
                        home[0], home[1], home[2],
                        m->dir[0], m->dir[1], m->dir[2], m->target,
                        m->speed, m->offset, m->target, d[0], d[1], d[2],
                        c->train_move_calls, c->train_stop_calls);
            }
        }
        {
            u32 mi, blocked = 0, sealed = 0;

            for (mi = 0; c->movers_ready && mi < c->movers.count; mi++) {
                if (c->movers.movers[mi].state == Q2_MV_BLOCKED) blocked++;
                if (c->movers.movers[mi].sealed)                 sealed++;
            }
            Q2_INFO("  movers    %u built, %u triggered by the script, "
                    "%u tick-moves (%u conveyor writes), %u sounds "
                    "(%u hatch, %u not started), %u shot open",
                    c->movers_ready ? c->movers.count : 0,
                    c->mover_triggers, c->mover_moved, c->conveyor_steps,
                    c->mover_sounds, c->rot_sounds, c->mover_sounds_missed,
                    c->breakable_opened);
            Q2_INFO("  movers    %u part boxes solid, %u blocked now, "
                    "%u sealing their portal",
                    c->sim[0].mover_count, blocked, sealed);
        }
        Q2_INFO("  entity ev %u lights added, %u dropped, %u bursts drawn, "
                "%u script lights",
                c->ent_light_added, c->ent_light_dropped, c->ent_bursts,
                c->script_lights);
        Q2_INFO("  burst why %u no fx, %u no table, %u no model, %u no bank, "
                "%u bad model, %u no verts",
                c->burst_no_fx, c->burst_no_table, c->burst_no_model,
                c->burst_no_bank, c->burst_bad_model, c->burst_no_verts);

        Q2_INFO("  attacks   %u checkattack (%u blind, %u decided, %u yes), "
                "%u attack calls, %u missing",
                q2_ai_stats.checkattack_calls, q2_ai_stats.checkattack_blind,
                q2_ai_stats.checkattack_decided, q2_ai_stats.checkattack_yes,
                q2_ai_stats.attack_called, q2_ai_stats.attack_missing);

        Q2_INFO("  callbacks %u pain, %u die (the module's own, not the "
                "generic fallback below)",
                c->cre_pain_calls, c->cre_die_calls);

        Q2_INFO("  moves     attack set %u / missing %u, melee %u / %u, "
                "run %u / %u, pain %u, die %u, stand %u",
                q2_cre_actions.move_via_set[6],
                q2_cre_actions.move_via_missing[6],
                q2_cre_actions.move_via_set[7],
                q2_cre_actions.move_via_missing[7],
                q2_cre_actions.move_via_set[4],
                q2_cre_actions.move_via_missing[4],
                q2_cre_actions.move_via_set[11],
                q2_cre_actions.move_via_set[12],
                q2_cre_actions.move_via_set[0]);

        {
            char buf[160];
            int  used = 0, ti;

            buf[0] = '\0';
            for (ti = 0; ti < 32; ti++)
                if (q2_cre_actions.think_hits[ti] && used < 140)
                    used += snprintf(buf + used, sizeof(buf) - (size_t)used,
                                     " %d:%u", ti,
                                     q2_cre_actions.think_hits[ti]);
            Q2_INFO("  think hit%s", buf[0] ? buf : " (none)");
        }

        /* The refusals are the decoded path's alone and partition the fire
         * calls with `sent` (crebind.h); the transcribed shots that went out
         * at nothing or at a corpse are counted inside `sent`. */
        Q2_INFO("  decoded   %u thinks (%u unbound), %u calls (%u unclassified), "
                "%u fire calls: %u sent, %u no enemy, %u dead enemy; "
                "%u shots at no enemy, %u at a corpse",
                q2_cre_actions.thinks_run, q2_cre_actions.thinks_unbound,
                q2_cre_actions.calls_seen, q2_cre_actions.calls_unclassified,
                q2_cre_actions.fire_calls, q2_cre_actions.fire_sent,
                q2_cre_actions.fire_no_enemy, q2_cre_actions.fire_dead_enemy,
                q2_cre_actions.shot_no_enemy, q2_cre_actions.shot_dead_enemy);

        Q2_INFO("  ai world  %u traces (%u unplaced, %u clear), "
                "%u bottom (%u fail), %u los (%u blocked)",
                c->ai_world.stats.traces, c->ai_world.stats.trace_unplaced,
                c->ai_world.stats.trace_clear, c->ai_world.stats.bottom_calls,
                c->ai_world.stats.bottom_fail, c->ai_world.stats.los_calls,
                c->ai_world.stats.los_blocked);

        /*
         * And how much of that was the DOORS, reported separately because
         * "creatures see through walls" and "creatures see through doors" are
         * different faults with the same symptom: the first is a hull that
         * does not describe the map, the second is a hull that describes it
         * correctly and a mover nobody asked. A run on a map with movers whose
         * `by a door` figures are all zero is the entity pass not being
         * reached.
         */
        Q2_INFO("  ai solids %u sight lines blocked by a runtime solid, "
                "%u steps stopped by one, %u corners standing on one; "
                "%u projectiles stopped by one",
                c->ai_world.stats.los_blocked_ent,
                c->ai_world.stats.trace_blocked_ent,
                c->ai_world.stats.bottom_on_ent,
                q2_sim_proj_scan.stopped_on_entity);
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Bind the model the view weapon wants.
 *
 * The clip bank names it — "Blaster G", "Supershot G" — and the map's own
 * CastList is where the geometry lives, so this runs both when a zone loads and
 * whenever the state machine finishes a swap. A weapon whose model this map
 * does not ship simply draws nothing rather than drawing the wrong thing.
 */
static void client_bind_view_model(client *c, int pi)
{
    const char *name;
    s32 index;

    c->vw_model_ready[pi] = false;
    q2_vw_set_model(&c->vw[pi], NULL);

    if (!c->vm_ready || !c->model_bank_ready)
        return;

    name = q2_vw_model_name(&c->vw[pi]);
    if (!name || !name[0])
        return;

    index = q2_model_bank_find(&c->model_bank, name);
    if (index < 0) {
        Q2_DEBUG("no view model '%s' in %s", name, c->map);
        return;
    }

    if (q2_model_get(&c->model_bank, (u32)index, &c->vw_model[pi]) != Q2_OK)
        return;

    c->vw_model_ready[pi] = true;
    q2_vw_set_model(&c->vw[pi], &c->vw_model[pi]);
    Q2_INFO("view weapon: player %d %s", pi, name);
}

static void client_reset_view_model(client *c, int pi)
{
    const q2_sim *sim = &c->sim[0];
    int weapon = pi == sim->cur_player ? sim->combat.weapon_id
                                      : sim->pcombat[pi].weapon_id;
    u32 serial = pi == sim->cur_player ? sim->combat.shot_serial
                                      : sim->pcombat[pi].shot_serial;

    c->mp_camera_angles[pi][0] = sim->player[pi].pitch;
    c->mp_camera_angles[pi][1] = sim->player[pi].yaw;
    c->mp_camera_angles[pi][2] = sim->player[pi].roll;
    if (c->hud_ready) {
        q2_hud_init(&c->hud[pi], &c->hud_tables,
                    c->mp_enabled ? c->mp.player_count : 1);
        q2_screen_flash_set(&c->screen, pi, c->hud[pi].flash.rgb, 0, 0);
    }
    if (!c->vm_ready)
        return;
    q2_vw_init(&c->vw[pi], &c->vm_tables, weapon);
    c->vw_last_weapon[pi] = weapon;
    c->shot_serial_shown[pi] = serial;
    c->shot_serial_heard[pi] = serial;
    client_bind_view_model(c, pi);
}

static void client_bind_player_models(client *c)
{
    static const char *const name[Q2_MP_MAX_PLAYERS] = {
        "Male2", "Male2red", "Male2purple", "Male2aqua"
    };
    int i;

    memset(c->player_model_ok, 0, sizeof(c->player_model_ok));
    memset(c->player_anim, 0, sizeof(c->player_anim));
    c->player_anim_base_ok = false;
    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++)
        c->player_anim[i].move = Q2_PMOVE_NONE;

    if (!c->model_bank_ready)
        return;

    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++) {
        s32 index = q2_model_bank_find(&c->model_bank, name[i]);

        if (index < 0 || q2_model_get(&c->model_bank, (u32)index,
                                      &c->player_model[i]) != Q2_OK)
            continue;
        c->player_model_ok[i] = true;
    }

    c->player_anim_base_ok = c->player_model_ok[0] &&
        c->player_model[0].hdr.num_parts <= 64;
    if (c->mp_enabled && c->player_anim_base_ok) {
        u32 n = 0;
        for (i = 0; i < Q2_MP_MAX_PLAYERS; i++)
            if (c->player_model_ok[i]) n++;
        Q2_INFO("player models: animated Male2 plus %u colour bod%s",
                n > 0 ? n - 1 : 0, n == 2 ? "y" : "ies");
    }
}

/* ------------------------------------------------------------------------- */
/* The movie player                                                           */
/* ------------------------------------------------------------------------- */
/*
 * Start a film. `name` is a bare file name out of a module's movie table
 * (`OUTRO1P.STX`) or off the command line; the directory is the disc's.
 */
static bool client_film_start(client *c, const char *name)
{
    char path[128];
    u32  limit;

    if (!name || !*name || c->film_open)
        return false;

    if (!c->film_rgb) {
        c->film_rgb = (u8 *)calloc((size_t)Q2_STX_WIDTH * Q2_STX_HEIGHT * 3, 1);
        if (!c->film_rgb)
            return false;
    }

    snprintf(path, sizeof(path), "Q2DATA/MOVIES/%s", name);
    if (!q2_movie_open(&c->film, c->disc, path)) {
        Q2_WARN("movie: cannot open %s", path);
        return false;
    }

    /*
     * The stop point the module passes the player. All three films are cut
     * short by it and the outro is cut by 59 frames, so playing a file out is
     * NOT the faithful thing — it is two and a half seconds of ending nobody
     * has ever seen. A film with no row plays out. See movie.h.
     */
    limit = q2_movie_retail_length(name);
    c->film.frame_limit = limit;

    c->film_open       = true;
    c->film_done       = false;
    c->film_have_frame = false;
    c->film_frames     = 0;

    /* The film carries its own sound. Whatever the level was playing stops,
     * the way it does on the console when the drive is handed to the film —
     * and so does every effect, since none of them belong over a cutscene. */
    c->music_open = false;
    client_voices_stop(c);
    c->bed_frames = 0;
    c->bed_pos    = 0;
    if (c->audio)
        SDL_ClearAudioStream(c->audio);

    if (limit)
        Q2_INFO("movie: playing %s, %u frames (the module's own stop point)",
                path, limit - 1u);
    else
        Q2_INFO("movie: playing %s to the end — no module names it", path);
    return true;
}

static void client_film_stop(client *c)
{
    if (!c->film_open)
        return;
    c->film_open       = false;
    c->film_done       = true;
    c->film_have_frame = false;
    client_voices_stop(c);
    c->bed_frames = 0;
    c->bed_pos    = 0;
    if (c->audio)
        SDL_ClearAudioStream(c->audio);

    /*
     * And give the level its music back. `client_film_start` clears
     * `music_open` and nothing used to set it again, so a session was silent
     * from the first cutscene onwards — including one that was skipped.
     * `force`, because the level record has not changed and the ordinary
     * early-out would decline to restart it.
     */
    client_music_for_level(c, true, false);

    Q2_INFO("movie: %u frames shown", c->film_frames);
}

/*
 * Advance the film.
 *
 * Its audio is NOT pulled here. Picture and sound are separate on the console —
 * the SPU plays whatever the drive delivered and the MDEC decodes what it can,
 * so a picture that falls behind does not take the sound with it — and they are
 * separate here for the same reason: the film's track is just another bed for
 * `client_audio_pump`, which feeds the device whether or not a frame decoded.
 */
static void client_film_tick(client *c, float dt)
{
    if (!c->film_open)
        return;

    if (q2_movie_advance(&c->film, (double)dt, c->film_rgb)) {
        c->film_have_frame = true;
        c->film_frames++;
    }

    if (q2_movie_finished(&c->film))
        client_film_stop(c);
}

/* ------------------------------------------------------------------------- */
/* The boot chain                                                             */
/* ------------------------------------------------------------------------- */
/*
 * The two logo screens, transcribed.
 *
 * Each is a level directory whose module does nothing but hold two full-screen
 * images and cross-fade between them, and each names the next thing by writing
 * the game state word. What is here is those two handlers' arithmetic:
 *
 *   QLOGOS2, 0x80101D88          QLOGOS, 0x80101FB8
 *     Legal.lbm                    IdLogo.lbm
 *       t < 8      t << 4            t < 8      t << 4
 *       t < 258    128               t < 83     128
 *       t < 266    128-((t-257)<<4)  t < 91     128-((t-82)<<4)
 *     HamLogo.lbm, from t = 258    ActLogo.lbm, from t = 83
 *       d < 8      d << 4            d < 8      d << 4
 *       d < 83     128               d < 83     128
 *       d < 91     128-((d-83)<<4)   d < 91     128-((d-83)<<4)
 *     hand off at d = 95           hand off at d = 93
 *
 * The off-by-one on each screen's FIRST image is in the module and is kept:
 * the first image reads the raw counter and the second a rebased copy, and the
 * rebase costs a frame. 128 is the GPU's neutral modulation value, so a fade is
 * eight steps of 16 rather than of 32 — half brightness at the midpoint.
 *
 * The images are 8bpp with a 256-entry CLUT apiece, 512 bytes wide and 240
 * rows, which is the whole active picture: these are not overlays on a scene,
 * they ARE the screen.
 */
typedef struct {
    const char *image;      /* the .lbm in this screen's SNDVRAM            */
    u32         start;      /* the frame its own clock starts on           */
    u32         hold_end;   /* full brightness until here, then eight down */
    u32         out_base;   /* the subtrahend the module's fade-out uses   */
} q2_boot_image;

typedef struct {
    const char    *map;         /* the level directory carrying them */
    q2_boot_image  image[2];
    u32            length;      /* frames before it hands over       */
} q2_boot_screen;

static const q2_boot_screen k_boot_screens[] = {
    { "QLOGOS2", { { "Legal.lbm",   0,   258, 257 },
                   { "HamLogo.lbm", 258,  83,  83 } }, 353 },
    { "QLOGOS",  { { "IdLogo.lbm",  0,    83,  82 },
                   { "ActLogo.lbm", 83,   83,  83 } }, 176 }
};

/*
 * The same two handlers in the NTSC disc's module, which is the same module
 * with eighteen immediates changed and nothing else: every HOLD is re-counted
 * for 60 Hz (258 -> 308, 83 -> 98, and the hand-offs 95 -> 110 and 93 -> 108)
 * and every FADE is left at eight frames. So the screens are up for the same
 * seconds on both standards, give or take a frame, and the fades are a fifth
 * quicker on NTSC. Read at module+0x1E4C..0x21B4 of SLUS-00757's QLOGOS.
 */
static const q2_boot_screen k_boot_screens_ntsc[] = {
    { "QLOGOS2", { { "Legal.lbm",   0,   308, 307 },
                   { "HamLogo.lbm", 308,  98,  98 } }, 418 },
    { "QLOGOS",  { { "IdLogo.lbm",  0,    98,  97 },
                   { "ActLogo.lbm", 98,   98,  98 } }, 206 }
};

#define Q2_BOOT_SCREENS  (sizeof(k_boot_screens) / sizeof(k_boot_screens[0]))

static const q2_boot_screen *client_boot_screen(const client *c, u32 index)
{
    return c->build.video == Q2_VIDEO_PAL ? &k_boot_screens[index]
                                          : &k_boot_screens_ntsc[index];
}

/* The brightness one image is drawn at on frame `t` of its own clock. */
static int client_boot_fade(const q2_boot_image *im, u32 frame)
{
    u32 t;

    if (frame < im->start)
        return 0;
    t = frame - im->start;

    if (t < 8)
        return (int)(t << 4);
    if (t < im->hold_end)
        return 128;
    if (t < im->hold_end + 8)
        return 128 - (int)((t - im->out_base) << 4);
    return 0;
}

static void client_boot_free(client *c)
{
    u32 i;

    for (i = 0; i < 2; i++) {
        free(c->boot_rgb[i]);
        c->boot_rgb[i] = NULL;
        c->boot_w[i]   = 0;
        c->boot_h[i]   = 0;
    }
    if (c->boot_vram_open) {
        q2_vram_free(&c->boot_vram);
        c->boot_vram_open = false;
    }
}

/*
 * Decode one screen's images out of its map's SNDVRAM.
 *
 * The SECTION only, not the map: a logo screen has no world, no player and no
 * scene to run — the console loads the whole directory because loading a
 * directory is the only thing its engine knows how to do, and what it then
 * shows is two rectangles. Reading just the images is the same picture without
 * a 100 KB level load behind it.
 */
static bool client_boot_load(client *c, const q2_boot_screen *s)
{
    u32 i, loaded = 0;

    client_boot_free(c);

    if (q2_vram_load(&c->boot_vram, c->disc, s->map) != Q2_OK) {
        Q2_WARN("boot: %s carries no image bank", s->map);
        return false;
    }
    c->boot_vram_open = true;

    for (i = 0; i < 2; i++) {
        const q2_vram_image *img;
        u16 clut[Q2_VRAM_CLUT8_ENTRIES];
        u8 *px, *rgb;
        size_t need, got = 0;
        u32 index, x, y;

        if (!s->image[i].image)
            continue;
        if (!q2_vram_find_by_name(&c->boot_vram, s->image[i].image, &index))
            continue;

        img  = &c->boot_vram.images[index];
        need = q2_vram_decoded_size(&c->boot_vram, index);
        /* One byte per texel is what makes this 8bpp; a record whose payload
         * is not width*height is a texture page or a 4bpp sheet. */
        if (!need || need != (size_t)img->width * img->height)
            continue;
        if (index < c->boot_vram.texpage_count ||
            !q2_vram_get_clut8(&c->boot_vram,
                               index - c->boot_vram.texpage_count, clut))
            continue;

        px  = (u8 *)malloc(need);
        rgb = (u8 *)malloc(need * 3);
        if (!px || !rgb) {
            free(px);
            free(rgb);
            continue;
        }
        if (q2_vram_decode(&c->boot_vram, index, px, need, &got) != Q2_OK ||
            got != need) {
            free(px);
            free(rgb);
            continue;
        }

        /* Out of the CLUT's 1.5.5.5 and into bytes, once, so the per-frame
         * blit is a multiply and a shift rather than a palette lookup. */
        for (y = 0; y < img->height; y++)
            for (x = 0; x < img->width; x++) {
                u16 e = clut[px[(size_t)y * img->width + x]];
                u8 *o = rgb + ((size_t)y * img->width + x) * 3;

                o[0] = (u8)((e         & 0x1Fu) << 3);
                o[1] = (u8)(((e >>  5) & 0x1Fu) << 3);
                o[2] = (u8)(((e >> 10) & 0x1Fu) << 3);
            }

        free(px);
        c->boot_rgb[i] = rgb;
        c->boot_w[i]   = img->width;
        c->boot_h[i]   = img->height;
        loaded++;
    }

    if (!loaded) {
        Q2_WARN("boot: %s has none of its images", s->map);
        client_boot_free(c);
        return false;
    }
    return true;
}

/*
 * Open the title screen, loading QFRONT if this run is not already standing in
 * it.
 *
 * The reload is what the boot chain needs: the intro film replaced QFRONT with
 * QFMV to play, so coming out of it there is no front end to open a page over.
 * The console's is the same load — `0x80018B54` writes `"QFront"` into the
 * next-map buffer and the dispatcher loads it like any other level.
 */
static void client_enter_front_end(client *c)
{
    c->mp_enabled   = false;
    c->in_front_end  = true;
    c->film_to_front = false;
    c->film_is_start = false;
    c->start_beat    = 0.0;
    c->mp_scoreboard = false;
    q2_loading_hide(&c->loading);
    q2_menu_set_multiplayer(&c->menu, false);
    /* The pages are QFRONT's from here on, and two of them differ from the
     * executable's — see q2_menu.front_end. */
    q2_menu_set_front_end(&c->menu, true);
    q2_screen_set_layout(&c->screen, Q2_SCREEN_LAYOUT_ONE, 1);

    if (!client_name_eq(c->map, "QFRONT") &&
        !client_load_zone(c, "QFRONT", 0)) {
        Q2_ERROR("front end: cannot load QFRONT");
        c->in_front_end = false;
        return;
    }

    /*
     * The camera is the WORLD ORIGIN, looking down +z with no rotation, and it
     * is not the spawn point. `engine+0x170` — which QFRONT's `init` calls with
     * 0 before anything else — is `0x80077D0C`, and its first act is
     * `memset(0x800D5C30, 0, 3920)`: the whole five-viewport array zeroed,
     * position and rotation included. Only then does `engine+0x174(0, 160,
     * 4000)` put the projection and far plane back.
     *
     * So the front end deliberately throws the level's `StartPos` away, and
     * this port had been leaving the camera where the spawn settle dropped it
     * — 54 units below the origin, which at z = 1700 and proj 160 is five
     * pixels of vertical error on the logo.
     */
    c->cam.pos[0] = 0;
    c->cam.pos[1] = 0;
    c->cam.pos[2] = 0;
    c->cam.yaw    = 0;
    c->cam.pitch  = 0;
    c->cam.roll   = 0;

    /*
     * And the front end's own far plane, on the VIEWPORT rather than the
     * camera — the camera reloads `view[p].far_z` every frame (see the viewport
     * note in client_frame), so writing the camera here would last exactly one
     * frame.
     *
     * This is `engine+0x174(0, 160, 4000)`, `init`'s second act, and it is a
     * straight overwrite of what `engine+0x170` — which is `0x80077D0C`, the
     * ONE layout `q2_screen_set_layout` already reproduces — had just
     * installed. Same proj, far 4000 where a session layout uses 6400.
     */
    if (c->screen.view_count > 0)
        c->screen.view[0].far_z = 4000;

    /* The title screen is the menu's page 46 over the QFRONT scene. It is
     * opened rather than drawn, so it navigates with the same engine, the same
     * selection bar and the same font as every other page. */
    q2_menu_open(&c->menu);
    q2_menu_goto(&c->menu, Q2_PAGE_FRONT_TITLE);

    /*
     * AND THE MUSIC STARTS HERE, if it was held for the boot chain.
     *
     * This is the point the console's own front-end load reaches: the title
     * screen is up, the logos are behind it and the film has been played. The
     * force is what makes the track start from the top — the load above may
     * not have re-selected anything, because coming out of the film the map is
     * QFMV and going nowhere at all leaves it QFRONT.
     */
    if (c->music_held) {
        c->music_held = false;
        client_music_for_level(c, true, false);
    }
}

/*
 * Start the boot chain, or the screen after the one that just ended.
 *
 * When it runs out, the INTRO FMV — which is what QLOGOS asks for by writing
 * state 12, and what the dispatcher answers with a load of `Intro FMV`, the
 * level table's tenth record and the map QFMV. The film hands over to the
 * front end when it ends, because by then no request flag is left standing and
 * `0x80018B54` falls through to `QFront`.
 */
static void client_boot_advance(client *c)
{
    const q2_level_entry *fmv;

    while (c->boot_index < Q2_BOOT_SCREENS) {
        const q2_boot_screen *s = client_boot_screen(c, c->boot_index++);

        if (!client_boot_load(c, s))
            continue;                    /* a disc without it just skips it */
        c->boot_frame = 0;
        c->boot_carry = 0.0;
        c->boot_open  = true;
        c->boot_skip  = false;
        Q2_INFO("boot: %s — %s, %s", s->map, s->image[0].image,
                s->image[1].image ? s->image[1].image : "(one image)");
        return;
    }

    /* Out of screens. The film, then the menu. */
    client_boot_free(c);
    c->boot_open = false;

    fmv = c->level_table_ready
        ? q2_level_find_display(&c->level_table, "Intro FMV") : NULL;

    if (fmv && !fmv->is_placeholder && fmv->directory[0]) {
        snprintf(c->film_screen, sizeof(c->film_screen), "Intro FMV");
        c->film_to_front = true;
        if (client_load_zone(c, fmv->directory, 0) && c->film_open)
            return;
        c->film_to_front = false;
        Q2_WARN("boot: no intro film — straight to the front end");
    }

    client_enter_front_end(c);
}

static void client_boot_start(client *c)
{
    c->boot_index = 0;
    client_boot_advance(c);
}

/*
 * A press ends the screen that is up.
 *
 * THE CONSOLE HAS NO SUCH THING — neither logo module reads `engine+0x2AC` or
 * any other pad word anywhere in its 30 KB — so this is the port's, and it is
 * the whole of the port's: what a screen is, how long it holds and what follows
 * it are the modules'.
 *
 * Recorded rather than acted on, because acting on it can load a map (the last
 * screen hands over to the intro film, and the film to QFRONT) and the press
 * arrives inside the event poll, with more events behind it that would then be
 * dispatched against a state two loads newer than the one they were queued for.
 * Every other screen-ending press in this loop stops something; this one starts
 * something, which is why it is the one that has to wait for the frame.
 */
static void client_boot_skip(client *c)
{
    if (c->boot_open)
        c->boot_skip = true;
}

/*
 * ON THE CONSOLE'S CLOCK, NOT THE WINDOW'S.
 *
 * `module+0x526C` and `module+0x5278` are incremented BY ONE per call of a page
 * hook the engine runs once per displayed frame — unlike the reel's beat, which
 * subtracts the frame delta and is therefore a duration. So every number in
 * `k_boot_screens` is in field-rate frames, and counting rendered frames instead
 * ties them to whatever panel is in front of the player: at vsync on a 60 Hz
 * display the logos went by half again too fast, and on a 144 Hz one the legal
 * screen would be up for four seconds instead of ten.
 *
 * So the accumulator every other clock in this port uses, carrying its
 * remainder, at the BUILD's rate — 50 on the PAL disc, which puts the two
 * screens at 7.1 s and 3.5 s, and 60 on the NTSC one, whose own counts put
 * them at 7.0 s and 3.4 s.
 */
static void client_boot_tick(client *c, float dt)
{
    const q2_boot_screen *s;
    double rate = (double)q2_build_tick_rate(&c->build);

    if (!c->boot_open)
        return;

    if (c->boot_skip) {
        c->boot_skip = false;
        Q2_INFO("boot: skipped at frame %u", c->boot_frame);
        client_boot_advance(c);
        return;
    }

    if (rate <= 0.0)
        rate = 30.0;

    s = client_boot_screen(c, c->boot_index - 1);
    c->boot_carry += (double)dt * rate;
    while (c->boot_carry >= 1.0) {
        c->boot_carry -= 1.0;
        if (++c->boot_frame >= s->length) {
            client_boot_advance(c);
            return;
        }
    }
}

/*
 * Put the screen on the display.
 *
 * Straight into the finished buffer and after the ordering table has been
 * walked, for the same reason the film is (`client_film_blit`): this is a
 * rectangle DMA'd to the frame buffer, not a primitive that sorts against
 * anything. Both images are drawn every frame — the second is fading in over
 * the first's fade out, and adding them is what makes that a cross-fade rather
 * than a cut through black.
 */
static void client_boot_blit(client *c)
{
    psx_framebuffer *fb = q2_screen_back(&c->screen);
    const q2_boot_screen *s;
    u32 i;

    if (!fb || !fb->px || !c->boot_open)
        return;

    s = client_boot_screen(c, c->boot_index - 1);
    psx_fb_clear(fb, 0);

    for (i = 0; i < 2; i++) {
        int bright = client_boot_fade(&s->image[i], c->boot_frame);
        int oy, y, step;

        if (!c->boot_rgb[i] || bright <= 0)
            continue;

        oy = (fb->height - (int)c->boot_h[i]) / 2;
        if (oy < 0) oy = 0;
        step = (int)(((u32)c->boot_w[i] << 16) /
                     (u32)(fb->width > 0 ? fb->width : 1));

        for (y = 0; y < (int)c->boot_h[i]; y++) {
            const u8 *src = c->boot_rgb[i] + (size_t)y * c->boot_w[i] * 3;
            u16 *dst;
            int bx, u = 0;

            if (oy + y >= fb->height)
                break;
            dst = fb->px + (size_t)(oy + y) * fb->width;
            for (bx = 0; bx < fb->width; bx++, u += step) {
                int x = u >> 16;
                u32 r, g, b, o;

                if (x >= (int)c->boot_w[i])
                    x = (int)c->boot_w[i] - 1;

                /* 128 is neutral, so this is the GPU's own modulation. The
                 * add is the cross-fade; both halves are already scaled. */
                r = ((u32)src[x * 3 + 0] * (u32)bright) >> 10;
                g = ((u32)src[x * 3 + 1] * (u32)bright) >> 10;
                b = ((u32)src[x * 3 + 2] * (u32)bright) >> 10;
                if (r > 31) r = 31;
                if (g > 31) g = 31;
                if (b > 31) b = 31;

                o = dst[bx];
                r += o & 0x1Fu;
                g += (o >> 5) & 0x1Fu;
                b += (o >> 10) & 0x1Fu;
                if (r > 31) r = 31;
                if (g > 31) g = 31;
                if (b > 31) b = 31;

                dst[bx] = (u16)(r | (g << 5) | (b << 10));
            }
        }
    }
}

/*
 * What the front end hands the game over to when the reel has run.
 *
 * THE FIRST MAP, and nothing in between. The reel's own tail arms the engine's
 * delayed state change —
 *
 *     80101DE4  sh 1,  706(v1)     ; engine+0x2C2, the state to enter
 *     80101DF0  sh 12, 704(v1)     ; engine+0x2C0, twelve frames from now
 *     80101E04  sw ...   0x290     ; and 0x8001F964 is what counts it
 *
 * — and `0x8001F964` is a countdown that writes `engine+0x2C2` into the game
 * state word when it runs out, which is how "STARTING" / "GAME" stays up for
 * twelve frames. The state it enters is ONE, the game. Not twelve.
 *
 * That distinction is the whole of the previous round's mistake. Twelve is the
 * state that asks for the intro FMV, and QLOGOS is what sets it — at boot,
 * before the menu exists (`boot_open`). The intro is a PRE-MENU cinematic and
 * has already played by the time anyone confirms a difficulty.
 */
static void client_start_game(client *c)
{
    c->in_front_end  = false;
    c->film_is_start = false;
    c->film_to_front = false;
    c->start_beat    = 0.0;
    q2_loading_hide(&c->loading);

    /*
     * A NEW GAME empties the mission table — `0x8003D62C`'s restore finds no
     * "PlayerSave" block and takes the `memset(0x8009B550, 0, 150)` at
     * `0x8003DDB8`. It is not the ONLY thing that does, which this comment used
     * to claim: `ClearLevelIntermission` empties it again at every unit
     * boundary. See `client_mission_enter`.
     */
    q2_mission_init(&c->mission);
    c->mission_row = -1;

    /* And the same memset takes the EVE_ blocks with it: a new game is the one
     * thing in the image that empties save memory, which is why a map walked
     * back into WITHIN a game still remembers its script. */
    q2_event_store_clear(&c->event_store);

    client_load_zone(c, c->first_map, client_default_zone(c, c->first_map));
}

/*
 * The half second between the difficulty and the reel.
 *
 * 0x80101CD0, which the three difficulty records installed as the page hook
 * when they armed `module+0x12D90` with 150. It subtracts the frame delta and
 * plays the reel the moment the store goes negative; nothing in it reads the
 * pad, so the beat CANNOT BE CANCELLED — which is why this takes no input and
 * why the reel is not skippable until it is actually running.
 *
 * See `start_beat` for the writer, its callers, and for the attract loop this
 * is not.
 */
static void client_start_beat(client *c, float dt)
{
    if (c->start_beat <= 0.0 || c->film_open)
        return;

    /* `subu v0, v0, v1` at 0x80101D00, with v1 the frame delta read through
     * `*(engine+0xD4)`. Draining it in the same units is the whole of it. */
    c->start_beat -= (double)dt * (double)Q2_DT_HZ;
    if (c->start_beat > 0.0)
        return;
    c->start_beat = 0.0;
    q2_loading_hide(&c->loading);

    /* The film the front end opens a new game with, a bare literal at QFRONT's
     * module+0xDC4 because no movie-table record could hold the name:
     * `ROGUEINP.STX` on the PAL disc, `ROGUEIN1.STX` on the NTSC one. */
    if (!client_film_start(c, q2_movie_start_reel(c->disc))) {
        Q2_WARN("front end: no opening reel — starting the game without it");
        client_start_game(c);
        return;
    }

    /*
     * The reel OWNS the screen while it runs. QFRONT stays loaded underneath
     * it, exactly as it does on the console — the front end never left the
     * level, it only hid the objects and swapped its page hook — but nothing
     * about it is drawn or ticked while the film has the screen.
     */
    c->film_is_start = true;
    c->in_front_end  = false;
}

/*
 * Put the decoded frame on the screen.
 *
 * Straight into the buffer being drawn, and not through the ordering table:
 * that is where the console puts it too. MDEC output is DMA'd to the frame
 * buffer as a rectangle; it is not a GPU primitive, it has no ordering-table
 * position, and nothing sorts against it. Anything the game wants OVER a movie
 * is drawn afterwards, which here means after this call.
 *
 * WIDTH IS NOT PIXELS. The GPU's five horizontal modes all span the same active
 * line, so 320 pixels and 512 pixels are the same picture with pixels of
 * different widths — which is why this port's PAL buffer is 512 across. The
 * console plays a 320-wide film by switching the display to a 320-wide mode,
 * and the result fills the television. Centring 320 buffer pixels inside 512
 * would show it at five-eighths size with black down both sides, so the film is
 * stretched across the whole buffer width instead. That is not a scaling
 * choice; it is the same physical picture.
 *
 * Vertically a buffer line IS a scanline, so 192 lines are placed 1:1 and
 * centred — the letterbox the film is authored with.
 */
static void client_film_blit(client *c)
{
    psx_framebuffer *fb = q2_screen_back(&c->screen);
    int oy, y, step;

    if (!fb || !fb->px || !c->film_have_frame || !c->film_rgb)
        return;

    oy = (fb->height - (int)Q2_STX_HEIGHT) / 2;
    if (oy < 0) oy = 0;

    /* 16.16 source step: one film pixel per buffer pixel on a 320-wide buffer,
     * five per eight on a 512-wide one. */
    step = (int)((Q2_STX_WIDTH << 16) / (u32)(fb->width > 0 ? fb->width : 1));

    for (y = 0; y < (int)Q2_STX_HEIGHT; y++) {
        const u8 *src = c->film_rgb + (size_t)y * Q2_STX_WIDTH * 3;
        u16 *dst;
        int bx, u = 0;

        if (oy + y >= fb->height)
            break;
        dst = fb->px + (size_t)(oy + y) * fb->width;
        for (bx = 0; bx < fb->width; bx++, u += step) {
            int x = u >> 16;

            if (x >= (int)Q2_STX_WIDTH)
                x = (int)Q2_STX_WIDTH - 1;
            /*
             * 24-bit to the framebuffer's RGB555. The console has a 24-bit
             * display mode for exactly this and the movie player uses it, so
             * the truncation here is the port's and is a stated divergence:
             * every other surface in this project is 15-bit because the GPU
             * drew it, and a movie is the one thing that was not.
             */
            u32 r = src[x * 3 + 0] >> 3;
            u32 g = src[x * 3 + 1] >> 3;
            u32 b = src[x * 3 + 2] >> 3;

            dst[bx] = (u16)(r | (g << 5) | (b << 10));
        }
    }
}

/*
 * One frame with the objectives pop-up up.
 *
 * The world is held — see the caller — so all this does is advance the screen's
 * own clock and read CROSS. The dismiss is an EDGE and the console arms it late
 * (0x80021818 only sets the arm flag on a frame where CROSS is NOT held), so
 * the press that raised the screen from the pause menu cannot also dismiss it.
 */
/*
 * One frame with an intermission board up — the level-end tally, the arrival
 * briefing or the end-of-mission placard.
 *
 * The world is FROZEN, which is the console's own behaviour: 0x80018ED8 spins
 * on the tally and zeroes the frame-delta accumulator each pass, so no game
 * time elapses. All this does is take the dismiss.
 */
/*
 * The tally board's own camera — `MoveQuakeCameraInterMission`, 0x80038460.
 *
 * `SetInterMission` installs this as view[0]'s per-view hook before anything
 * else it does (0x800224C8), and `ProcessAllCameras` runs the hook from
 * `QuakeInterMissionLoop` on every frame the board is up — so the shot is
 * re-established each frame rather than set once, and this is called from the
 * intermission frame for that reason.
 *
 * The console's guard is `lw a1, 288(view)` for the owning entity and then
 * `lh v0, 264(a1)` > 0 at 0x800384A0: a DEAD player keeps their own camera and
 * the authored one is never installed. The port's equivalent of entity+264 is
 * the combat inventory's health, which is the same number the status bar
 * draws.
 *
 * `q2_mission_camera_for_unit` says what is and is not transcribed, including
 * the one unit the table has no record for.
 */
static void client_mission_camera(client *c)
{
    q2_mission_camera cam;

    if (!c || !c->mission_open)
        return;
    if (c->sim[0].combat.inv.health <= 0)
        return;
    if (!q2_mission_camera_for_unit(c->mission.unit, &cam))
        return;

    /* view+0/+4/+8 are written outright — they are the EYE, not the feet:
     * `QuakeCamera`'s live arm reaches the same fields by adding 286 to the
     * entity's y (0x80038638), so the record is already in eye space. */
    c->cam.pos[0] = cam.pos[0];
    c->cam.pos[1] = cam.pos[1];
    c->cam.pos[2] = cam.pos[2];
    c->cam.pitch  = cam.pitch;
    c->cam.yaw    = cam.yaw;
    c->cam.roll   = cam.roll;
}

static void client_intermission_frame(client *c, float dt)
{
    u16 pad = client_menu_pad(c);
    bool cross = (pad & (Q2_PAD_CROSS | Q2_PAD_START)) != 0;

    /*
     * THE TALLY BOARD IS ITS OWN CASE, and it is the only one of the three with
     * a lock on it.
     *
     * `PadEndInterMission` (0x800224F4) runs before the draw on every tally
     * frame and will not so much as read the pad for the first 1200 ticks —
     * four seconds at this port's rate. The press it then wants is the FIRE
     * binding (`MapPadInputToFire(0)`, default mask 0x4000), on a rising edge,
     * and START is not part of it: the port used to close the board on the
     * first CROSS **or START** of the first frame, so a player who reached the
     * exit holding fire never saw the screen at all. See mission.h.
     */
    if (c->mission_open) {
        bool fire = (pad & Q2_PAD_CROSS) != 0;
        bool edge = fire && !c->mission_fire_prev;
        s32  ticks;

        /* The console's own delta, clamped exactly where 0x80018914 clamps
         * it: `slti v0, v0, 31` then 30. */
        ticks = (s32)((double)dt * 300.0 + 0.5);
        if (ticks < 1)                  ticks = 1;
        if (ticks > Q2_SCREEN_DT_MAX)   ticks = Q2_SCREEN_DT_MAX;

        /* Latched unconditionally — the pad record is updated whether or not
         * the gate let anybody look at it. */
        c->mission_fire_prev = fire;

        if (q2_mission_pad_end(&c->mission_gate, ticks, edge)) {
            c->mission_open = false;
            q2_prompt_hide_all(&c->prompts);
        }

        /* And the camera, which `ProcessAllCameras` reinstalls every frame the
         * board is up rather than once at the raise. */
        client_mission_camera(c);

        c->popup_cross_prev = cross;
        return;
    }

    (void)dt;

    /* An edge, so the press that opened the board cannot also close it. */
    if (cross && !c->popup_cross_prev) {
        if (c->endmis_open)       c->endmis_open   = false;
        else                      c->briefing_open = false;
        q2_prompt_hide_all(&c->prompts);
    }
    c->popup_cross_prev = cross;
}

static void client_popup_frame(client *c, float dt)
{
    s32  ticks = (s32)((double)dt * 300.0 + 0.5);
    bool cross;
    u16  pad;

    if (ticks < 1)
        ticks = 1;

    pad   = client_menu_pad(c);
    cross = (pad & Q2_PAD_CROSS) != 0;

    /*
     * The level clock does not advance while the world is held, so the
     * deadline is measured against a clock that has to keep moving on its own.
     * The console gets this for free — 0x800AEBAC is advanced by the display
     * loop, not by the game logic the menu flag skips.
     */
    c->sim[0].level_time += ticks;

    (void)q2_briefing_popup_tick(&c->popup, ticks, c->sim[0].level_time,
                                 cross, c->popup_cross_prev);
    c->popup_cross_prev = cross;
}

static void client_menu_frame(client *c)
{
    q2_menu_sound snd;
    u16 pad;

    /*
     * The CONTROLLER page's two greyed rows, given back — the port's decision,
     * kept in the port's own layer so menu.c stays the transcription it is.
     *
     * 0x8001CA28 takes SWAP Y AXIS and USE MOUSE out of the navigation unless
     * the connected controller is an ANALOGUE PAD, which is the right answer
     * for a console whose only analogue device is a stick. On a host that
     * always has a mouse it is two traps: SWAP Y AXIS is read by the mouse
     * styles and nothing else (pad.c's `invert`), so greying it under a mouse
     * greys the only row that does anything; and greying USE MOUSE would mean
     * the toggle that selects the device could never be reached to select it —
     * or, once off, ever be turned back on.
     */
    if (c->menu.page_id == Q2_PAGE_CONTROLLER) {
        c->menu.disabled[3] = 0;                       /* USE MOUSE   */
        if (c->settings.v[Q2_SET_PAD_CLASS] == 0)
            c->menu.disabled[2] = 0;                   /* SWAP Y AXIS */
    }

    /*
     * The pointer first, because what a click MEANS depends on the row it is
     * over and the cursor has to be there before the press is handed to the
     * engine.
     */
    pad  = client_menu_pointer(c, &c->menu);
    pad |= client_menu_pad(c);
    /* A key-down, not a held key, so it is one frame of TRIANGLE: the engine
     * sees the press on this advance and the release on the next. */
    if (c->menu_esc_back)
        pad |= Q2_PAD_TRIANGLE;
    c->menu_esc_back = false;

    /*
     * RIGHT-CLICK OUT OF THE PAUSE MENU.
     *
     * Deeper in, TRIANGLE is the back and the engine owns it. On the pause page
     * itself there is no parent and TRIANGLE does nothing (0x8001D824) — which
     * on the console is fine, because START closes the menu, and here Esc does.
     * A player driving the menu with the mouse alone would have no way out, so
     * the right button does there exactly what Esc does.
     *
     * The two PAUSE pages by name, not "any page with no parent": the death
     * screen and the front end's title also have none, and neither is a screen
     * anyone may dismiss — one is the end of a life and the other is the bottom
     * of the game.
     */
    if (c->mouse_right && !c->mouse_right_prev && c->menu.depth == 0 &&
        (c->menu.page_id == Q2_PAGE_PAUSE_SP ||
         c->menu.page_id == Q2_PAGE_PAUSE_MP)) {
        client_menu_close(c);
        client_play_menu_sound(c, Q2_MSND_BACK);
        return;
    }

    /* The death page's arm countdown is spent in level-clock units, so it needs
     * this frame's delta rather than a count of frames. See q2_menu.frame_dt. */
    c->menu.frame_dt = q2_build_tick_rate(&c->build) > 0
                           ? (s32)(Q2_DT_HZ / q2_build_tick_rate(&c->build))
                           : Q2_DT_NOMINAL;

    /* Which image the pages come from, refreshed here rather than at each of
     * the nine places `in_front_end` is assigned: this is the last point before
     * a button can change the page, so it cannot be stale when one does. */
    q2_menu_set_front_end(&c->menu, c->in_front_end);

    q2_menu_advance(&c->menu, pad);

    snd = q2_menu_take_sound(&c->menu);
    if (snd != Q2_MSND_NONE)
        client_play_menu_sound(c, snd);

    /*
     * The scene follows the page, because the module makes it follow the page:
     * every front-end builder opens with `module+0x3414`, and that call ends by
     * putting one of two thinks on the logo depending on which page is now up
     * (levelbin.h). Stepping off the title screen therefore shrinks it, and
     * stepping back grows it again.
     */
    if (c->in_front_end)
        q2_sim_scene_page(&c->sim[0],
                          c->menu.page_id == Q2_PAGE_FRONT_TITLE, true);

    client_menu_requests(c);
    q2_prompt_sync_menu(&c->prompts, &c->menu, c->in_front_end);
}

/* ------------------------------------------------------------------------- */
/*
 * One frame, in the order 0x800182C8 does it.
 *
 * The swap comes first, so a frame is always built into the buffer that is not
 * being shown. Then one full-screen background clear, then each live viewport
 * in turn — each one loading the GTE from its own view record and filling its
 * own 51-bucket slice of the single ordering table. Composition walks that one
 * table once; the draw-env packets sitting in it are what clip each viewport.
 *
 * There is one simulated player, so every viewport gets the same camera. That
 * is not a stand-in for split screen — it is what makes the reconstructed
 * layouts visible, since a second player would change nothing about the screen
 * work itself.
 */
/*
 * Which collision cell a viewport gathers its lights from.
 *
 * Both the lens flares and the per-entity three-light gather ask this, and they
 * must agree: the flare pass and the entity gather read the SAME SpaceLights
 * partition, so a viewport that lit its models out of one cell and its flares
 * out of another would be showing two different rooms at once. It is the
 * SecondaryCol node — see spacelights.h for why that hull and not PrimaryColl.
 *
 * In a split each viewport is its own player standing in its own cell, so the
 * node comes from that player's entity rather than from the sim's `current_node`
 * (which tracks whichever player last ticked).
 *
 * Zero rather than -1 in the front end, and the difference is not cosmetic: -1
 * selects the fallback record 0x8006B150 builds for a node-less entity, a grey
 * light of radius 0x7FFF sitting on the entity itself that outranks anything and
 * would take one of the three slots from the module's own rig every frame.
 */
static s32 client_light_node(const client *c, int p)
{
    if (c->in_front_end)
        return 0;

    if (c->mp_enabled && p >= 0 && p < Q2_MP_MAX_PLAYERS &&
        (p == 0 || c->sim_ready[p]))
        return c->sim[0].player[p].ent.node;

    return c->sim[0].current_node;
}

/*
 * Which SortData stream this viewport reads — the byte offset carried by the
 * camera's PrimaryColl cell.
 *
 * `current_node` cannot serve: it is the SECONDARY hull's cell, the movement
 * one, and the two hulls have different node counts in every zone (BASE1 zone 1
 * is 290 against 191). More importantly, the renderer itself indexes the
 * PrimaryColl node array and loads `lh +28` at 0x80066AFC. That halfword is the
 * exact byte offset into SortData; no reconstructed stream enumeration belongs
 * in the live path.
 *
 * Resolved from the eye rather than kept on the player, because the free-fly
 * camera has no player and still has to draw the world in some order. The
 * previous frame's answer is the search hint, which is what makes this a
 * portal-neighbour step rather than a sweep of the whole hull each frame.
 */
static s32 client_sort_cell(client *c, int p)
{
    s32 at[3];

    if (c->in_front_end || !c->sim[0].coll_primary_ready)
        return -1;

    /* 0x80038578 maps the owning entity's SecondaryCol cell into PrimaryColl,
     * then traces from entity+0x54 to view+0. The result is therefore the cell
     * containing the CAMERA, not the player's collision origin. At this point
     * client_draw_view has already installed this viewport's camera (including
     * the independent split-screen views), so query that position directly. */
    (void)p;
    at[0] = c->cam.pos[0];
    at[1] = c->cam.pos[1];
    at[2] = c->cam.pos[2];

    c->sort_cell = q2_coll_find_node(&c->sim[0].coll_primary, at,
                                     c->sort_cell, true);
    return c->sort_cell;
}

static bool client_sort_context(client *c, int p, u32 *out, u8 *area)
{
    q2_coll_node node;
    s32 cell = client_sort_cell(c, p);

    if (!out || !area || cell < 0 ||
        !q2_collision_get_node(&c->sim[0].coll_primary, (u32)cell, &node) ||
        node.sort_offset < 0)
        return false;

    *out = (u32)(u16)node.sort_offset;
    *area = node.contents;
    return true;
}

/*
 * The world draw, in the place 0x80066858 occupies: called from inside the
 * viewport's own draw, after its state is published and under its own gate.
 */
static void client_draw_view(void *user, q2_screen *s, int p,
                             psx_ot *ot, gte_state *gte)
{
    client *c = (client *)user;
    q2_world_stats stats;
    int owner = c->mp_enabled ? p : 0;

    /*
     * The viewport owns the field of view: SetGeomScreen(view+262) at
     * 0x80076B90, and a geometry offset at the viewport's own centre
     * (view+266/+268, SetGeomOffset at 0x80076B78).
     *
     * Both are taken from the view record rather than from the framebuffer,
     * because in a split they are not the same thing — the quad layout puts four
     * centres in one frame — and because this is the state q2_screen_view_begin
     * has already installed in the GTE for this viewport. Handing it back keeps
     * the world's own reload from quietly disagreeing with the screen's.
     */
    c->cam.projection = (u16)s->view[p].proj;
    /* The 2D extent as well as the offset — the flare rings scale by it, and
     * the two fields only differ on the two-horizontal split. */
    c->cam.ext_w      = s->view[p].vw;
    c->cam.ext_h      = s->view[p].vh;
    c->cam.ofs_x      = s->view[p].ofs_x;
    c->cam.ofs_y      = s->view[p].ofs_y;
    c->cam.far_z      = s->view[p].far_z;
    /* The sort range is the port's and does not come off the view record; see
     * q2_camera.sort_range for why the two must not be the same number. */
    c->cam.sort_range = c->ot_range > 0 ? c->ot_range : Q2_CAMERA_SORT_RANGE;
    /* The clip extent the linkers test every projected corner against —
     * 0x800B2C20's packed (clip_h << 16) | clip_w. */
    c->cam.clip_w     = s->ctx.clip_w;
    c->cam.clip_h     = s->ctx.clip_h;

    /* Extra views use their owner's eased eye and plain camera angles.
     * Recoil is added to that owner's weapon below, not to the world camera. */
    if (c->mp_enabled && p > 0 && p < Q2_MP_MAX_PLAYERS && c->sim_ready[p]) {
        q2_sim_player_eye(&c->sim[0], p, c->cam.pos);
        c->cam.pitch = c->mp_camera_angles[p][0];
        c->cam.yaw   = c->mp_camera_angles[p][1];
        c->cam.roll  = c->mp_camera_angles[p][2];
    }

    /* The viewport's far distance is also the subdivision threshold: the same
     * view+264 the original parks at 0x800B2CCC serves both. */
    c->render.subdiv_threshold = s->view[p].far_z;

    /*
     * The LENS FLARES, which had a complete implementation and no caller: the
     * zone's two flare fields were never assigned, so `q2_world_build_ot` took
     * the `if (z->lights)` branch on a null pointer every frame of every run and
     * the pass this project transcribed out of 0x800759F0 has never executed.
     * BASE3's zone 2 reports six flare-carrying lights in the cell the player
     * starts in and drew none of them.
     *
     * Both fields are per-VIEWPORT, which is why they are set here and not once
     * at load: the original's pass is driven off the same per-viewport loop this
     * function is, and in a split each player stands in a different cell. The
     * node is the SecondaryCol one, because that is what partitions SpaceLights
     * — see spacelights.h — and it is the same node the entity gather below
     * uses, for the same reason.
     */
    c->zone.lights     = c->lights_ready ? &c->light_world : NULL;
    c->zone.light_node = client_light_node(c, p);

    /*
     * THE WORLD'S DRAW ORDER, which is authored rather than computed, and which
     * this port has been deriving from depth because nothing ever assigned it.
     *
     * `q2_world_zone.sort` has been read by the renderer since SortData was
     * decoded and written by NOBODY, so `forced_bucket` was -1 on every quad of
     * every frame and the fallback ran always: bucket = depth * span / 6400,
     * against a 51-entry viewport slice. Measured, a scene's depths run to
     * 22,492 on SECURITY and 17,778 on BASE3 — so roughly everything past 6,400
     * units landed in the slice's last bucket together, drawn in reverse link
     * order with no relation to distance. That is the reported "far distance
     * culling": geometry does not vanish, it is painted in an arbitrary order
     * and the wrong surfaces win.
     *
     * WHICH STREAM a viewport uses is explicit. The camera cell at view+146 is
     * read at 0x80066AD4, indexes the PrimaryColl 36-byte node records, and the
     * following `lh +28` at 0x80066AFC supplies the SortData BYTE offset. The
     * old equal-count observation was real but insufficient: enumerating the
     * variable streams and assuming cell i meant stream i threw away the map's
     * actual lookup table.
     *
     * A zone with no chunk keeps the depth fallback; several zone 0s ship an
     * empty one.
     */
    {
        if (c->sort_ready && c->use_sort &&
            client_sort_context(c, p, &c->zone.sort_offset,
                                &c->zone.sort_area) &&
            c->zone.sort_offset < c->sortdata.size) {
            c->zone.sort = &c->sortdata;
        } else {
            c->zone.sort = NULL;
        }
    }

    q2_world_build_ot(&c->zone, &c->cam, s->view[p].w, s->view[p].h,
                      ot, gte, &c->render, &stats);
    /* How many Gouraud corners ProcessMobileLights actually repainted, summed
     * over the run — the number that says the world is being lit at all. The
     * timed table that feeds it is ticked once a frame around q2_screen_build,
     * not here: this function runs per VIEWPORT. */
    c->world_lit_points    += stats.mobile_corners;
    c->world_lit_unslotted += stats.mobile_unslotted;
    c->shot_stats = stats;
    c->cre_drawn  = 0;
    c->cre_faces  = 0;
    c->player_drawn = 0;
    c->player_faces = 0;

    /*
     * The map's items, into the table the world has just been built into — the
     * same reason the weapon and the effects go there, and the reason
     * entitydraw is a module rather than something done inline: an item sorts
     * against the crate it stands behind because both are in one list.
     *
     * It comes straight after the world and before everything else because that
     * is what it is: level content, not presentation.
     *
     * `player` is 0 because there is one, and it is what makes an item this
     * player has already collected invisible to this view — the per-player
     * block's whole purpose. The texture-page table is the world's, so an item
     * on a page the world has already promoted blends at the promoted mode.
     * `lights` is NULL: the client has no q2_light_world, so an item is drawn
     * at its own glow tint exactly as this module did before lighting existed.
     */
    if (c->sim[0].entities_ready) {
        q2_entity_draw_ctx ectx;
        q2_entity_draw_stats estats;

        memset(&ectx, 0, sizeof(ectx));
        ectx.bank          = c->model_bank_ready ? &c->model_bank : NULL;
        ectx.clut4_count_a = c->clut4_count_a;
        ectx.player        = (u32)owner;
        ectx.tpage         = &c->render.tpage;

        /*
         * The lights, and the cell to gather them from. `coll_node` was -1,
         * which is "no node" — so even had a light world been passed, every
         * entity would have taken the fallback. The sim tracks the player's
         * own cell every tick and that is the one the engine uses.
         */
        ectx.lights        = c->lights_ready ? &c->light_world : NULL;
        /*
         * The node the lights are gathered from is the PLAYER's, and the title
         * screen has no player — see client_light_node, which the flare pass
         * above shares so the two cannot pick different cells.
         *
         * The front end's rig is the evidence for the 0 that helper returns. A
         * front end that lit its logo by falling through to the node-less grey
         * would not spend five `0x80075C34` calls a frame placing lights around
         * it (levelbin.h). QFRONT's two nodes carry no static lights either, so
         * 0 contributes nothing of its own and the gather is exactly the five.
         */
        ectx.coll_node     = client_light_node(c, p);
        /* And the hull, so each entity resolves its OWN cell rather than
         * borrowing the player's. `coll_node` above stays as the fallback for
         * the title screen, which has no collision world at all. */
        ectx.coll          = c->sim[0].coll_ready ? &c->sim[0].coll : NULL;
        ectx.coll_primary  = c->sim[0].coll_primary_ready
                           ? &c->sim[0].coll_primary : NULL;

        q2_entity_build_ot(&c->sim[0].entities, &ectx, &c->cam, ot, gte, &estats);
        /* Model entities are drawn through the same walk items are, so the
         * only way to tell a spawned Explosion from a silently-dropped one
         * is to count what the walk actually emitted. */
        c->ent_drawn    += estats.drawn;
        c->ent_no_model += estats.no_model;
        c->ent_outside  += estats.outside_zone;
        c->ent_faces    += estats.faces_emitted;
        c->ent_shadows  += estats.shadows_emitted;
    }

    /*
     * The creatures, into the same table for the same reason: a Soldier behind
     * a crate sorts behind it because both are in one list.
     *
     * WHICH ANIMATION A CREATURE IS PLAYING, and it is not chosen by index.
     *
     * `0x8006B924` keeps the animation position in a halfword at `entity+0x100`
     * and the current clip at `model+0x34`, and while the position is past the
     * clip's length it advances the pointer by that clip's own `next` delta and
     * subtracts its `frames`. So a model's clips are ONE CONTINUOUS TIMELINE
     * and the position is an offset into it — there is no clip index to find,
     * which is what the port was previously trying to reconstruct by matching a
     * move's length against a clip's.
     *
     * A creature's AI frame is a position on that same timeline (its module's
     * moves are numbered 0..474 for the Soldier), at three ticks per frame, so
     * the walk lands in the right clip on its own.
     */
    if (c->creatures_ready && c->cre_model) {
        u32 i;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            q2_model_instance inst;
            q2_model_draw_stats st;
            q2_model_pose pose[64];
            q2_model_anim clip;
            q2_light_env  cre_env;
            s32 cell = -1;
            bool posed = false;
            /* entity+0x100: where on the model's whole timeline the pose is,
             * which block B's draw orders are keyed on (model.h). */
            s32 anim_pos = 0;

            if (!m->in_use || !c->cre_model_ok[i])
                continue;

            {
                const q2_model *mdl = &c->cre_model[i];
                s32 frame = m->frame;
                u32 pose_tick = 0;
                bool have_clip = false;

                if (frame < 0)
                    frame = 0;

                if (mdl->hdr.num_parts > Q2PSX_ARRAY_COUNT(pose)) {
                    have_clip = false;
                } else if (m->currentmove) {
                    /*
                     * The clip its CURRENT MOVE plays, and the position within
                     * that clip — not a position on one continuous timeline.
                     * The engine keeps a current clip at model+0x34 and only
                     * walks the chain when the position overruns it, so a move
                     * selects a clip and the frame indexes into it. Walking the
                     * whole chain instead drifts: the Soldier's death move at
                     * AI frame 308 lands in the wrong clip and the body stands
                     * up halfway through falling over.
                     */
                    const q2_mmove *mv = m->currentmove;
                    s32 len = mv->last_frame - mv->first_frame + 1;

                    if (len > 0) {
                        s32 into = frame - mv->first_frame;

                        if (into < 0)
                            into = 0;
                        if (into >= len)
                            into = len - 1;

                        /*
                         * BY NAME first, which is what the engine does:
                         * 0x8006D330 walks block D comparing 12-byte names,
                         * and 0x8007EA44 places the frame at
                         * `start * 5 + 30 * (f - first)`. That per-move base is
                         * exactly what a bare `frame * 10` lacks, and lacking
                         * it is what made the timeline walk drift.
                         *
                         * Falls back to matching a clip by LENGTH when the
                         * module does not name the move — a substitute for
                         * something the disc never does, kept only because a
                         * decoded move without a name has nothing else to go on.
                         */
                        /*
                         * THE KEY IS THE FRAME, NOT THE MOVE.
                         *
                         * The module's table indexes FRAME RANGES, and a move
                         * can span several of them — the Soldier's attack1
                         * (0-11) covers Fire 1 Ready/Aim/Shoot/Done and its
                         * walk1 (215-247) covers Walk 1 Loop and Look. Asking
                         * for "the name of this move" therefore came back empty
                         * for exactly the moves a creature lives in, and the
                         * draw fell through to a length match and then a raw
                         * timeline walk: attack1 posed from "Death1", attack2
                         * from "Pain3", walk1 from "Stand3" running into
                         * "Death2".
                         *
                         * That is the moonwalk and the missing firing
                         * animation — a patrolling Soldier slid down the
                         * corridor with its legs locked in a standing pose, and
                         * a shooting one played its own death.
                         */
                        s32 aiframe = mv->first_frame + (s32)into;
                        const q2_cre_frame_name *rec =
                            q2_creature_world_frame_name(&c->creatures, m,
                                                         aiframe);
                        const char *mname = rec ? rec->name
                                                : client_move_name(m, mv);
                        s32 base = rec ? rec->first : mv->first_frame;
                        u32 pos = 0;
                        bool pose_held = false;

                        /*
                         * `into` is already clamped to [0, len-1] above, which
                         * matters: a monster whose frame counter has not caught
                         * up to its move sits at frame 0 while the move starts
                         * at 146, and the raw frame would be rejected as before
                         * the move's start. Clamping is what the surrounding
                         * code has always done and what a position before a
                         * move's base means anyway — the move begins there.
                         */
                        have_clip = false;
                        if (mname && !q2_model_position_for_move(
                                mdl, mname, aiframe, base, &pos))
                            /* The NAME is not in this model's block D. */
                            c->pose_name_absent++;
                        else if (mname)
                            /*
                             * `pos` is in TENTHS of an animation frame, which
                             * is the engine's unit (0x8006B5D8 divides it by
                             * ten). Keep it in that unit through the cursor and
                             * the position-aware timeline walk: dividing here
                             * used to discard exactly the remainder retail
                             * supplies to its pose interpolator.
                             */
                        {
                            s32 sample = client_model_anim_sample(
                                &c->cre_anim[i], mv, 1u, (s32)pos,
                                c->screen.dt, c->frame_index);

                            if (sample < 0)
                                sample = 0;
                            anim_pos = sample;
                            if (mname)
                                have_clip = q2_model_anim_at_position_held(
                                    mdl, (u32)sample, &clip, &pose_tick,
                                    &pose_held);
                        }
                        if (have_clip && pose_held) c->pose_held++;
                        if (have_clip)      c->pose_by_name++;
                        else if (mname)     c->pose_name_no_pos++;
                        else                c->pose_no_name++;


                        /*
                         * WHICH half fails matters and the one counter could
                         * not say. `pose_name_absent` is the name missing from
                         * block D — a real pairing gap. Everything else in
                         * `pose_name_no_pos` is a name that RESOLVED and whose
                         * position then fell off the end of the clip chain,
                         * which is an arithmetic fault, not a missing pairing.
                         */

                        if (!have_clip) {
                            have_clip = q2_model_anim_by_length(
                                mdl, (u32)len * Q2_CRE_TICKS_PER_FRAME,
                                client_move_ordinal(m, mv), &clip);
                            if (have_clip) {
                                s32 sample = client_model_anim_sample(
                                    &c->cre_anim[i], mv, 2u,
                                    into * Q2_MODEL_POS_PER_MOVE_FRAME,
                                    c->screen.dt, c->frame_index);
                                u32 last = clip.frames
                                    ? ((u32)clip.frames - 1u) *
                                      Q2_MODEL_TICKS_PER_FRAME
                                    : 0u;

                                pose_tick = sample > 0 ? (u32)sample : 0u;
                                if (pose_tick > last)
                                    pose_tick = last;
                                anim_pos = (s32)(q2_model_anim_base(mdl, &clip)
                                                 + pose_tick);
                            }
                        }
                    }
                }

                /* No move installed, or no clip of that length: the timeline
                 * walk, which is what every creature used before this. */
                if (!have_clip && mdl->hdr.num_parts <= Q2PSX_ARRAY_COUNT(pose)) {
                    s32 sample = client_model_anim_sample(
                        &c->cre_anim[i], NULL, 3u,
                        frame * Q2_MODEL_POS_PER_MOVE_FRAME,
                        c->screen.dt, c->frame_index);

                    if (sample < 0)
                        sample = 0;
                    anim_pos = sample;
                    have_clip = q2_model_anim_at_position(
                        mdl, (u32)sample, &clip, &pose_tick);
                }

                /* `pose_tick` remains in retail's 1/10-frame unit all the way
                 * into q2_model_pose_at. At a 30 Hz render clock the targets
                 * are 0,30,60… while the samples are 0,10,20,30…; variable-rate
                 * translations and rotations therefore receive the same 0..9
                 * interpolation remainder the executable retains at
                 * 0x8006B5D8. */
                if (have_clip)
                    posed = (q2_model_pose_at(mdl, &clip,
                                              pose_tick,
                                              pose) == Q2_OK);
            }

            q2_model_instance_init(&inst);
            inst.model         = &c->cre_model[i];
            inst.pose          = posed ? pose : NULL;
            inst.anim_pos      = anim_pos;

            /* The link object's area and cell, which outlive a frame whose
             * lookup misses — see client_cre_area. A -1 here would take
             * the creature off the portal walk and paint it through walls. */
            inst.sort_area = client_cre_area(c, i, &cell);
            {
                int axis;
                for (axis = 0; axis < 3; axis++) {
                    inst.sort_bounds_min[axis] = m->pos[axis] + m->mins[axis];
                    inst.sort_bounds_max[axis] = m->pos[axis] + m->maxs[axis];
                }
                inst.sort_bounds_valid = true;
            }

            /*
             * The lights reaching this creature. The item draw gets these
             * through the entity context; this loop calls q2_model_build_ot
             * directly, so it has to gather its own — three lights per entity,
             * which is all the GTE's light matrix has rows for (FORMATS §17).
             */
            if (c->lights_ready) {
                q2_light_set  set;
                /*
                 * The ambient the entity carries, which used to be NULL — so a
                 * vertex none of the three gathered lights reached came out
                 * pure black rather than dim, and a creature in an unlit
                 * corridor was a silhouette.
                 *
                 * AND IT IS THE ENTITY'S OWN BYTES NOW, not a literal. This
                 * was a static 0x30 triplet, so a creature struck by an energy
                 * weapon never took the colour 0x800586E8 writes into
                 * entity+0x2AC and the fade 0x8005B894 runs on it had nothing
                 * to show. The actor keeps the triplet (combat.h,
                 * q2_actor.ambient) and the presentation pass moves it; an
                 * actor that cannot be resolved falls back to the literal,
                 * which is also what the seed is.
                 */
                static const u8 cre_glow[3] = { Q2_ACTOR_AMBIENT_DEFAULT,
                                                Q2_ACTOR_AMBIENT_DEFAULT,
                                                Q2_ACTOR_AMBIENT_DEFAULT };
                const q2_actor *ga = client_cre_actor(c, m);

                q2_light_gather(&set, &c->light_world, m->pos, cell, 0);
                q2_light_env_build(&cre_env, &set, Q2_LIGHT_ONE,
                                   Q2_LIGHT_ONE,
                                   ga ? ga->ambient : cre_glow);
                inst.light = &cre_env;

                /*
                 * The colour matrix a creature is actually lit by, because
                 * "the monsters are green" is a claim about nine numbers and
                 * none of them were ever printed. Column j is light j; rows are
                 * red, green and blue.
                 */
                if (c->zone_trace && (c->frame_index % 60) == 0)
                    Q2_INFO("[light] cre %u cell %d active %u"
                            "  L0 %d,%d,%d  L1 %d,%d,%d  L2 %d,%d,%d"
                            "  back %d,%d,%d", i, (int)cell, cre_env.active,
                            cre_env.colour.m[0][0], cre_env.colour.m[1][0],
                            cre_env.colour.m[2][0],
                            cre_env.colour.m[0][1], cre_env.colour.m[1][1],
                            cre_env.colour.m[2][1],
                            cre_env.colour.m[0][2], cre_env.colour.m[1][2],
                            cre_env.colour.m[2][2],
                            cre_env.back[0], cre_env.back[1], cre_env.back[2]);
            }
            inst.origin[0]     = m->pos[0];
            /*
             * `m->pos` is the entity ORIGIN; a model is placed on its FEET,
             * which is Q2_EYE_BASE below.
             *
             * AND THEN RAISED AGAIN BY THE MODEL'S OWN BIAS, which this path
             * did not do — so every creature on the disc was drawn sunk into
             * the floor by `ext2`: 251 units for a Soldier, 419 for a Berserk,
             * 507 for a Tank Commander. That is a Soldier cut off flat at the
             * shins, and it is the "monsters clip into the geometry" report.
             *
             * 0x8006D118 and 0x800588E4..0x80058904 (FORMATS §5398, §5422) put
             * it exactly this way: the draw origin is the position lowered by
             * 286 and raised again by the model's own bias, kept at entity+0xF8
             * out of `lh model[+0x1C]`. item.c:651 has always done it; this
             * loop never did.
             *
             * ext2 IS the posed sole height on every creature model the disc
             * ships — Soldier 251 against a posed maximum of 251, Arachner
             * 217/217, Gunner 380/376 — so subtracting it stands the model on
             * the floor the drop sweep found. Nothing else moves: the hit
             * sphere, the AI and the collision all key off `m->pos`.
             */
            /*
             * ...AND A CORPSE IS PLACED BY ITS OWN POSE, which is the
             * difference between a body that lies on the floor and one that
             * hangs over it.
             *
             * `ext2` is the model's SOLE HEIGHT and it is exact for the pose it
             * was measured on. Reading every one of the Soldier's thirty-one
             * clips, each standing, walking and firing pose has its lowest
             * vertex at exactly 251 — which IS ext2, so the formula above
             * stands those on the floor to the unit. The death clips do not
             * share it: clip 8 runs 164, 36, 61, 134, 216, 249, 312, 255 over
             * its thirty frames as the body is thrown up and comes down. Drawn
             * against a fixed 251 that same body floats by 190 at frame 9 and
             * sinks by 61 at frame 25, and whichever frame it stops on is the
             * one it keeps.
             *
             * A DEPARTURE, and stated as one: the console draws every pose
             * against the one constant (0x800588F0 subtracts the copy at
             * obj+0xF8, which 0x80058868 fills once from `lh model[+0x1C]` and
             * nothing ever updates), so a console body rests wherever its last
             * death frame happens to put it too. This port is asked for corpses
             * that lie on the ground, so it measures the pose.
             *
             * Narrow on purpose — only once the body has come to REST. While
             * the death animation is playing the console formula is used
             * unchanged, because the arc of a body being thrown backwards is
             * the animation and pinning its lowest vertex to the floor would
             * flatten it. `frame == last_frame` on a dead creature is exactly
             * "the death has played out".
             *
             * It also degenerates: on any pose whose lowest vertex is already
             * `ext2` this computes the same number the line below it would.
             */
            {
                s32 low = c->cre_model[i].hdr.ext2;

                if (m->dead && posed && m->currentmove
                    && m->frame == m->currentmove->last_frame) {
                    s32 pose_low;

                    if (q2_model_pose_low_y(&c->cre_model[i], pose,
                                            Q2PSX_ARRAY_COUNT(pose),
                                            &pose_low))
                        low = pose_low;
                }

                inst.origin[1] = m->pos[1] + Q2_EYE_BASE - low;
            }
            inst.origin[2]     = m->pos[2];
            inst.yaw           = m->angles[2];
            inst.clut4_count_a = c->clut4_count_a;
            /* The map's own page table, so a creature's faces reach the pages
             * they ask for — and share the world's ABR promotions. Without it
             * q2_tpage_model falls back to the canonical table, which is right
             * but does not carry this frame's promotions. */
            inst.tpage         = &c->render.tpage;

            /*
             * And keep it, for the damage effects (client_fx_pose): the next
             * combat tick's crackle and sparks sample THIS mesh through
             * 0x8006CC44. Copied rather than pointed at, because `pose` and the
             * light live in this loop's frame. Every viewport writes the same
             * pose — the cursor advances once per display frame — so a split
             * screen keeps the last one, which is the same one.
             */
            if (c->cre_fx) {
                client_fx_pose *fp = &c->cre_fx[i];
                u32 parts = c->cre_model[i].hdr.num_parts;

                fp->inst       = inst;
                fp->inst.light = NULL;
                fp->inst.tpage = NULL;
                fp->inst.pose  = NULL;
                if (posed && parts <= CLIENT_FX_POSE_MAX &&
                    parts <= Q2PSX_ARRAY_COUNT(pose)) {
                    memcpy(fp->pose, pose, parts * sizeof(pose[0]));
                    fp->inst.pose = fp->pose;
                }
                fp->valid = true;
            }

            q2_model_build_ot(&inst, &c->cam, ot, gte, &st);
            if (st.faces_emitted) {
                c->cre_drawn++;
                c->cre_faces += st.faces_emitted;
            }
        }
    }

    /*
     * The other players — actual articulated world bodies, not only combat
     * spheres. Retail's Male2 has ten named moves and the same model position
     * path creatures use, including the sub-frame remainder. Each viewport
     * omits its own first-person body and sees everybody else's.
     */
    if (c->mp_enabled && c->player_anim_base_ok) {
        int pi;

        for (pi = 0; pi < c->mp.player_count && pi < Q2_MP_MAX_PLAYERS; pi++) {
            const q2_player *pl = &c->sim[0].player[pi];
            const q2_player_death *death = &c->death[pi];
            const q2_model *body;
            q2_model_pose pose[64];
            q2_model_instance inst;
            q2_model_draw_stats st;
            q2_light_env env;
            q2_coll_node area_node;
            s32 at[3];
            s32 cell = -1;
            bool posed;

            if (pi == p || (pi > 0 && !c->sim_ready[pi]) ||
                death->stage == Q2_PDEATH_GIBBED ||
                death->stage == Q2_PDEATH_GONE)
                continue;

            body = c->player_model_ok[pi] ? &c->player_model[pi]
                                          : &c->player_model[0];
            if (body->hdr.num_parts > Q2PSX_ARRAY_COUNT(pose))
                continue;

            posed = client_player_pose(c, pi, pose);

            q2_model_instance_init(&inst);
            inst.model  = body;
            inst.pose   = posed ? pose : NULL;
            inst.origin[0] = pl->pos[0];
            inst.origin[1] = pl->pos[1] - body->hdr.ext2;
            inst.origin[2] = pl->pos[2];
            inst.yaw       = pl->yaw;
            /* body_fade spends retail entity+0xFC, which is a lighting
             * intensity rather than a geometric transform. */
            inst.scale     = Q2_ONE_12;
            inst.clut4_count_a = c->clut4_count_a;
            inst.tpage         = &c->render.tpage;

            at[0] = pl->pos[0];
            at[1] = q2_sim_origin_y(pl->pos[1]);
            at[2] = pl->pos[2];
            if (c->sim[0].coll_ready)
                cell = q2_coll_find_node(&c->sim[0].coll,
                                         at, -1, true);
            if (cell >= 0 &&
                q2_collision_get_node(&c->sim[0].coll,
                                      (u32)cell, &area_node))
                inst.sort_area = area_node.contents & 0x7F;
            {
                int axis;
                for (axis = 0; axis < 3; axis++) {
                    inst.sort_bounds_min[axis] =
                        at[axis] - Q2_SWEEP_HALF_EXTENT;
                    inst.sort_bounds_max[axis] =
                        at[axis] + Q2_SWEEP_HALF_EXTENT;
                }
                inst.sort_bounds_valid = true;
            }

            if (c->lights_ready) {
                q2_light_set set;
                /* The other body draw, and the console runs the same chain for
                 * a player: the player think calls 0x8005B880 at 0x8003B004,
                 * so this ambient moves exactly as a creature's does. The live
                 * player's actor is `combat.self`; a parked one's is its own
                 * `pcombat[pi].self` (combat.h, q2_actor.ambient). */
                static const u8 glow[3] = { Q2_ACTOR_AMBIENT_DEFAULT,
                                            Q2_ACTOR_AMBIENT_DEFAULT,
                                            Q2_ACTOR_AMBIENT_DEFAULT };
                const q2_actor *ga = (pi == c->sim[0].cur_player)
                                         ? &c->sim[0].combat.self
                                         : &c->sim[0].pcombat[pi].self;

                q2_light_gather(&set, &c->light_world, at, cell, 0);
                q2_light_env_build(&env, &set,
                                   death->stage == Q2_PDEATH_FADING
                                       ? death->scale : Q2_LIGHT_ONE,
                                   Q2_LIGHT_ONE, ga ? ga->ambient : glow);
                inst.light = &env;
            }

            q2_model_build_ot(&inst, &c->cam, ot, gte, &st);
            if (st.faces_emitted) {
                c->player_drawn++;
                c->player_faces += st.faces_emitted;
            }
        }
    }

    /*
     * Effects go into the SAME table as the world, which is the whole point of
     * an ordering table: a spark behind a crate sorts behind it because both
     * are in one list, not because anything tested them against each other.
     *
     * The beam pool is emptied after the last viewport rather than here, since
     * one queue feeds every view — 0x80064F10 draws and then resets, and doing
     * the reset per view would make split screen lose the beams in every
     * viewport but the first.
     */
    /* The creatures' swing trails, once a frame, off the meshes this frame
     * has just posed (client_cre_blurs). */
    if (p == 0)
        client_cre_blurs(c);
    c->fx_prims += q2_fx_build_ot(&c->sim[0].fx, &c->cam, (u32)p, ot, gte);

    /*
     * And the projectiles themselves, which nothing has ever drawn. Until now
     * the only thing a bolt, a rocket or a BFG ball put on screen was the
     * dynamic light it casts — the room brightened where the bolt was and the
     * bolt was not there. Same table as the world and the effects, for the
     * same reason: the sort has to be able to put a bolt behind a crate.
     *
     * See entitydraw.h for where the geometry comes from and which part of it
     * is inference rather than transcription.
     */
    /*
     * And the debris, for the same reason and from the same pool the physics
     * loop has been stepping all along — see entitydraw.h. The bank is the
     * map's own, because the pieces' model indices were registered out of it.
     */
    if (c->model_bank_ready)
        c->debris_faces +=
            q2_fx_debris_build_ot(&c->sim[0].fx, &c->model_bank,
                                  c->sim[0].coll_primary_ready
                                      ? &c->sim[0].coll_primary
                                      : (c->sim[0].coll_ready
                                          ? &c->sim[0].coll : NULL),
                                  c->lights_ready ? &c->light_world : NULL,
                                  &c->render.tpage, c->clut4_count_a,
                                  &c->cam, ot, gte);

    /*
     * A rocket and a launched grenade are drawn as their own models out of
     * the map's CastList, lit like any entity (entitydraw.h); the rest keep
     * the bolt's box. The hull is the primary one their cells are kept in.
     */
    {
        q2_entity_draw_ctx pctx;
        const q2_collision *phull = c->sim[0].coll_primary_ready
                                        ? &c->sim[0].coll_primary
                                        : (c->sim[0].coll_ready
                                            ? &c->sim[0].coll : NULL);

        memset(&pctx, 0, sizeof(pctx));
        pctx.bank          = c->model_bank_ready ? &c->model_bank : NULL;
        pctx.clut4_count_a = c->clut4_count_a;
        pctx.tpage         = &c->render.tpage;
        pctx.lights        = c->lights_ready ? &c->light_world : NULL;
        pctx.coll          = phull;
        pctx.coll_node     = -1;

        c->proj_prims += q2_projectiles_build_ot(
            &c->sim[0].combat.projectiles, phull, &c->cam, ot, gte, &pctx);
    }

    /*
     * The status bar, into this viewport's own slice — because the console
     * draws it from this very hook (`0x800337D0`), not from an overlay pass.
     * Its anchor is the viewport's `sbar_x`/`sbar_y`, which is what those two
     * halfwords turn out to be (statusbar.h).
     *
     * The data is the sim's, read here rather than pushed: health and armour
     * come straight off the inventory, and the ammo shown is the ammo the
     * CURRENT weapon uses, which is the same indirection the console makes
     * through its weapon-to-ammo map.
     */
    /* `icons_resident`, not `menu_font_ready`: the upload succeeds when any of
     * the three images lands, and the status bar needs THIS one.
     *
     * And it goes away on the same screens the overlay does. The suppression
     * rule below covered the crosshair and the HUD and not this, so the level
     * tally was drawn over a live status bar — health, armour and the ammo
     * counter sitting on top of a board that belongs to the overlay camera and
     * draws one thing at a time. The pause menu is deliberately NOT in the
     * list: the world, the bar and the gun stay visible behind it. */
    if (c->icons_ready && c->menu_font_ready && c->menu_font.icons_resident &&
        !c->mission_open && !c->endmis_open && !c->credits_open &&
        !c->mcard_open) {
        q2_statusbar *bar = &c->sbar[(p >= 0 && p < Q2_MP_MAX_PLAYERS) ? p : 0];
        q2_inventory *inv;
        q2_sbar_layout bar_layout;
        int weapon;
        int ammo;

        /* Multiplayer parks every non-current player's combat half in its own
         * slot. The old bar always read the live slot, so every viewport showed
         * player zero's health and weapon. Read the owner of this viewport. */
        if (p == c->sim[0].cur_player) {
            inv = &c->sim[0].combat.inv;
            weapon = c->sim[0].combat.weapon_id;
        } else {
            inv = &c->sim[0].pcombat[p].inv;
            weapon = c->sim[0].pcombat[p].weapon_id;
        }

        /*
         * 0x80035424..0x80035438: `ammo[ammoIdx[weapon]]` with the ONE-based
         * id in hand. This used to index the inventory's zero-based
         * q2_weapon_ammo with that one-based id — every gun's counter read its
         * neighbour's pool. q2_sbar_ammo_for_weapon reads the same table the
         * carousel gates on (statusbar.h).
         */
        ammo = q2_sbar_ammo_for_weapon(inv, weapon);

        switch (s->layout) {
        case Q2_SCREEN_LAYOUT_TWO_H: bar_layout = Q2_SBAR_LAYOUT_TWO_H; break;
        case Q2_SCREEN_LAYOUT_TWO_V: bar_layout = Q2_SBAR_LAYOUT_TWO_V; break;
        case Q2_SCREEN_LAYOUT_QUAD:  bar_layout = Q2_SBAR_LAYOUT_QUAD;  break;
        default:                     bar_layout = Q2_SBAR_LAYOUT_ONE;  break;
        }

        q2_statusbar_anchor(bar, s->view[p].sbar_x, s->view[p].sbar_y);
        q2_statusbar_layout(bar, bar_layout, p, s->disp.height);
        bar->players = s->view_count > 0 ? s->view_count : 1;
        bar->health  = inv->health;
        bar->armour  = inv->armour;
        bar->ammo    = (s16)ammo;
        bar->frags   = (c->mp_enabled && p < c->mp.player_count)
                           ? c->mp.frags[p] : 0;
        bar->weapon  = weapon;

        /*
         * The carousel's two slots, as 0x80037ECC writes them: +100 is
         * 0x80050758 walked forward from the SELECTED weapon (client+102) and
         * +96 walked back, with the owned bit and one shot's ammo as its gates
         * and 0 for a walk that comes back to the gun in hand.
         *
         * THE PAIR IS A LATCH, NOT A FORMULA. Six functions write it
         * (statusbar.h, `strip`) and nothing else does, so between their
         * events it holds what the last one saw: a pool that falls — a shot,
         * or the power armour spending cells (0x80057BC8) — writes nothing,
         * and the hyperblaster at 49 cells keeps the BFG in its slot until the
         * next event. So this is the latch's per-frame front, which writes
         * only on a mark of an event it cannot see (a pickup, inside the sim);
         * the select, the auto-select and the spawn call the writer where they
         * happen (client_sbar_write_slots). It stays after bar->weapon is set,
         * so field 12 and the two slots are read from the same id.
         */
        if (q2_statusbar_weapon_slots_track(bar, inv, weapon))
            c->slots_inferred++;

        /*
         * The health icon IS hard-coded — offset 170 into a five-byte record,
         * rect 34, with no branch above it (0x80035190).
         *
         * The armour icon is not, and used to be. `armour > 0 ? 30 : 0` put
         * rect 30 — the POWER SHIELD — on the bar for every player wearing any
         * vest, because statusbar.h recorded 0x8003565C's `lbu 150(a0)` as
         * unconditional when it is one of five arms of a select on the flag
         * word. q2_statusbar_armour_state runs that select, and the state
         * machine in front of it (cells gate, one-second alternation, pinned
         * power state) that chooses which of the two readouts the counter
         * shows.
         *
         * The clock it runs on is the LEVEL clock, which is what 0x800AEBAC
         * is — 300 ticks to the second. This used to be `c->frame_index`, a
         * rendered-frame count at 30 Hz, which made the low-value blink and
         * the armour alternation ten times too slow.
         */
        bar->health_icon = Q2_SBAR_ICON_HEALTH;
        bar->cells       = inv->ammo[Q2_AMMO_CELLS];
        bar->ticks       = (u32)c->sim[0].level_time;
        q2_statusbar_armour_state(bar, inv->flags);
        q2_statusbar_powerup_state(bar, inv);

        /*
         * WHAT WAS JUST PICKED UP, which nothing has ever drawn.
         *
         * The touch dispatch has written both halves of this since the item
         * path was transcribed — the effect at `inv->last_item` and a
         * 900-tick deadline at `inv->item_name_until`, exactly as 0x800372F0
         * does — and no reader existed, so a player collected a shotgun and
         * the screen said nothing. `0x800359C0` is the reader: the bar's
         * fourth sub-draw, which fills the upper-left icon field from the
         * effect and prints the name beside it.
         *
         * Both halves are set here and emitted below, in the console's own
         * order: the TEXT first, because within a bucket the ordering table
         * draws last-in first and the console emits the caption before the
         * field walk that draws every icon.
         *
         * `q2_item_pickup_caption` mutates — the expiry clears `last_item` in
         * place, which is where the console does it too. Only the one-player
         * hook calls this sub-draw; all three split hooks omit it.
         *
         * AND A DEAD PLAYER'S BAR SKIPS IT. 0x80033C68 branches the one-player
         * hook around 0x800359C0 once health is at or below zero, and that one
         * sub-draw both prints the caption (0x80035B20) and expires it
         * (0x80035A4C) — so a dead player sees no caption, and only the expiry
         * CHECK waits: the deadline is absolute against the level clock
         * (0x80035A34..0x80035A40), so one that passed during death is cleared
         * by the first live frame (statusbar.h, q2_statusbar_stripped). The
         * else below clears the icon and the HUD's line. The bar's layout and
         * health are already set above.
         */
        if (bar_layout == Q2_SBAR_LAYOUT_ONE && !q2_statusbar_stripped(bar)) {
            const char *pickup_name = NULL;
            u8          pickup_icon = 0;

            if (q2_item_pickup_caption(inv,
                                       c->sim[0].level_time,
                                       c->item_table_ready ? &c->item_table
                                                           : NULL,
                                       &pickup_icon, &pickup_name))
                q2_hud_pickup(&c->hud[0], pickup_name);
            else
                q2_hud_pickup(&c->hud[0], NULL);

            bar->pickup_icon = pickup_icon;

            if (c->hud_font_ready) {
                q2_hud_ctx pctx;

                q2_hud_ctx_centre_in(&pctx, c->width, c->height);
                /*
                 * A DEPTH OF ZERO, not `q2_screen_view_otz`, and the two are
                 * not interchangeable. The overlay's emitter links with
                 * `psx_ot_add`, which takes a depth and inverts it inside the
                 * window `q2_screen_view_begin` has installed for this
                 * viewport; the status bar links with `psx_ot_add_bucket`,
                 * which takes an absolute bucket. Handing the bucket to the
                 * depth door made 416 clamp to the far end of the slice and
                 * the caption was emitted BEHIND the world — six glyphs laid
                 * out, none of them on screen.
                 *
                 * Zero is the near end of this viewport's own slice, which is
                 * one bucket in front of the bar's — so the caption lands on
                 * top of the icon beside it, exactly as the console's order
                 * puts it (the text is printed before the field walk that
                 * draws every icon, and a bucket is drawn newest-last).
                 */
                q2_hud_pickup_build_ot(&c->hud[0], &c->hud_font, &pctx, ot, 0);
            }
        } else {
            bar->pickup_icon = 0;
            q2_hud_pickup(&c->hud[0], NULL);
        }

        q2_statusbar_build_ot(bar, c->menu_font.tpage_icons,
                              c->menu_font.clut_text, ot,
                              q2_screen_view_otz(s, p, 0), 0, 0);
    }

    /*
     * The weapon in the hands, into the SAME table as the world it stands in.
     * That is the whole reason it is a model and not an overlay: it sorts
     * against the wall the player has walked into rather than always winning,
     * which is exactly what the console does and exactly what a blit cannot.
     *
     * It is placed on the eye — `feet.y + 286 - view_height` is the camera's own
     * expression (FORMATS §9.12) — so it crouches when the view crouches without
     * anything here having to know that.
     */
    /*
     * Off on the overlay-camera screens, for the reason the status bar is —
     * the gun was being drawn over the level tally.
     *
     * AND OFF WHEN THERE IS NOBODY HOLDING IT. The view weapon is not a
     * drawing mode on this console, it is an ENTITY: 0x8004EE0C opens with
     * `s6 = self->[68]` and then `s7 = s6->[12]`, so `entity+0x44` is a
     * pointer to a whole second entity with a client block of its own. The
     * death handler FREES it — 0x800397F8 passes that pointer to 0x8006D280,
     * which detaches it, unlinks it and pushes it back onto the free stack at
     * 0x800B2BAC — and then writes -40 over the same word, which is where
     * `gib_health` comes from. One field, two lives, and the transition is the
     * gun disappearing.
     *
     * It is doubly gone, because 0x8004EE0C's ONE caller is 0x8003AD98 inside
     * the player think, and `player_die` has just uninstalled that too.
     * The port had neither, so a dead player kept a floating blaster.
     */
    c->vw_drawn[p] = 0;
    if (c->vw_model_ready[owner] && c->death[owner].linked_weapon &&
        !c->mission_open && !c->endmis_open &&
        !c->credits_open && !c->mcard_open) {
        q2_model_instance proto;
        q2_model_draw_stats mstats;
        q2_light_env vw_env;
        s16 aim[3], kick[3];

        q2_model_instance_init(&proto);
        proto.tpage         = &c->render.tpage;
        proto.clut4_count_a = c->clut4_count_a;
        /*
         * THE GUN IS ONE THING IN THE TABLE, and it is in front of the world.
         *
         * Sorted per face it spanned three buckets with wall polygons landing
         * between them, so geometry the player stood close to painted over the
         * muzzle — the "view weapon clips into world geometry" report. The
         * original links a model's faces at ONE point (modeldraw.h), and for
         * the weapon in hand that point is ahead of the geometry: it is drawn
         * over the view, not into it.
         *
         * AND ONE BUCKET BEHIND THE STATUS BAR, named through the bar's own
         * helper rather than derived separately.
         *
         * This used to ask for depth 0, "the frontmost bucket of whichever
         * window is installed", which happened to be the bar's bucket while a
         * viewport slice was 51 entries. Subdividing the table pulled the two
         * apart — the depth path reaches the top of the subdivided window while
         * the bar lands where its console index scales to — and the gun began
         * drawing over the HUD. Asking for layer 1 where the bar asks for layer
         * 0 states the relationship instead of relying on two arithmetics
         * landing on the same number.
         */
        proto.bucket_override = (s32)q2_screen_view_otz(s, p, 1);

        /*
         * THE WORLD LIGHT BRANCH, selected by a SIGNED test.
         *
         * 0x8004F750 writes 1 to the viewmodel entity's +0xF4, and 0x8006B040
         * tests it with `bgez`: non-negative takes the ordinary world dynamic
         * list and static lamps; negative alone reaches the alternate list.
         * Reading the halfword as a boolean inverted the branch and produced a
         * dark grey gun that the retail capture immediately falsified.
         *
         * That colour is 0x40 per component, copied into +0x2AC by the entity
         * allocator at 0x8006C1D8..0x8006C1FC.  The old gather mechanism was
         * right; its guessed 0x30 floor was not.
         */
        if (c->lights_ready) {
            q2_light_set set;
            s32 at[3];

            at[0] = c->sim[0].player[owner].pos[0];
            at[1] = q2_sim_origin_y(c->sim[0].player[owner].pos[1]);
            at[2] = c->sim[0].player[owner].pos[2];

            q2_light_gather(&set, &c->light_world, at,
                            client_light_node(c, p), c->vw[owner].light_selector);
            q2_light_env_build(&vw_env, &set, c->vw[owner].scale, c->vw[owner].fade,
                               c->vw[owner].glow);
            proto.light = &vw_env;
        }

        /*
         * THE KICK THE WEAPON GETS IS THE DECAYED ONE, and it was the raw
         * amplitude.
         *
         * `0x8004F40C` sums the player's aim with what `0x80038260` RETURNS,
         * and that function is three blocks each scaling a stored amplitude by
         * how much of its own period is left — firing over 30 ticks, damage
         * over 150, landing over 90. `q2_sim_view_angles` is that function
         * transcribed. `combat.kick` is the STORED amplitude those blocks scale,
         * not their result: feeding it here gave the weapon a kick that never
         * decayed and was at full strength the moment it was set.
         *
         * So the kick is taken as the difference between the summed angles and
         * the plain ones, which is exactly the three contributions and nothing
         * else.
         */
        {
            s32 summed[3];

            q2_sim_player_view_angles(&c->sim[0], owner, summed);

            aim[0]  = (s16)c->sim[0].player[owner].pitch;
            aim[1]  = (s16)c->sim[0].player[owner].yaw;
            aim[2]  = (s16)c->sim[0].player[owner].roll;
            kick[0] = (s16)(summed[0] - c->sim[0].player[owner].pitch);
            kick[1] = (s16)(summed[1] - c->sim[0].player[owner].yaw);
            kick[2] = (s16)(summed[2] - c->sim[0].player[owner].roll);
        }

        c->vw_drawn[p] = q2_vw_build_ot(&c->vw[owner], &proto,
                       c->sim[0].player[owner].pos, c->sim[0].player[owner].view_height,
                       aim, kick, &c->cam, ot, gte, &mstats);
    }

    /*
     * The glint, OFF by default (F6 shows it).
     *
     * Nothing the engine does raises the `0x04000000` flag it draws on — only
     * BIGGUN's level script does, and this port does not run relocated level
     * modules yet. Drawing one anyway would be putting an effect on screen that
     * the console never puts there, so the reconstruction sits behind a toggle
     * and the default frame has no glint in it.
     *
     * The phase runs 4..1, which is what the script writes and what the band
     * formula's `4 - phase` expects.
     */
    if (c->sim[0].glint.ready && (c->sim[0].glint.raised || c->show_glint)) {
        s32 at[3];

        q2_sim_eye(&c->sim[0], at);
        q2_fx_glint_draw(&c->sim[0].glint, at, c->cam.yaw, &c->cam, ot, gte);

        /*
         * And the LIGHT the renderer raises alongside the mesh — 0x800648B8,
         * unconditional in both of its callers, and the only site in the image
         * that asks for a flare style at runtime (effect.h).
         *
         * It will not produce one, here or on the console. The engine's frame
         * runs the flare stage, then the entity draw, then the light list's
         * reset; the glint is an entity draw, so the light is raised one stage
         * too late and cleared before the next pass. This port has the same
         * shape — the flare pass is inside q2_world_build_ot above, and
         * q2_light_world_begin_frame clears on the next tick — so the
         * unreachability is reproduced rather than worked around. Raising it
         * earlier would put an effect on screen the console never shows.
         */
        if (c->lights_ready)
            q2_light_add_dynamic(&c->light_world, at, c->sim[0].glint.tint,
                                 Q2_FX_GLINT_LIGHT_INNER,
                                 Q2_FX_GLINT_LIGHT_OUTER,
                                 Q2_FX_GLINT_LIGHT_STYLE,
                                 Q2_FX_GLINT_LIGHT_SHIFT);
    }

    /* The crosshair and messages use this viewport's dimensions and draw
     * environment. A single full-screen overlay put the crosshair in the
     * gutter and let one player's notifications cross into another view. */
    if (c->mp_enabled && c->hud_ready && c->hud_font_ready &&
        !c->in_front_end && !c->menu.open && !c->mission_open &&
        !c->mcard_open && !c->endmis_open && !c->credits_open) {
        q2_hud_ctx ctx;
        q2_hud *hud = &c->hud[owner];

        hud->crosshair = (c->settings.v[Q2_SET_CROSSHAIR] != 0);
        q2_hud_ctx_default(&ctx, s->view[p].w, s->view[p].h);
        q2_hud_build_ot(hud, &c->hud_font, &ctx, ot, 0);
    }
}

/*
 * Flip, capture and put the finished buffer in the window.
 *
 * Split out of `client_frame` because two things now compose a frame — the
 * ordinary one and the loading screen — and the flip, the screenshot and the
 * window blit have to be the same for both or a capture of the loading screen
 * would come out through a different path than every other capture.
 */
static void client_present(client *c)
{
    void *pixels;
    int pitch;
    const psx_framebuffer *front;

    /*
     * SCREEN POSITION into the display envs, as Config_InstallVideo
     * (0x8001C29C) puts it there — openquestions #40. The console calls it from
     * the SCREEN POSITION page every frame it is up, after the video reset and
     * a settings load, and from both functions that set the envs up; the
     * settings cannot change anywhere else, so doing it once a frame here,
     * before the env is put, shows exactly what those calls would. It sits in
     * client_present so the loading screen is placed the same way.
     */
    q2_screen_install_video(&c->screen, c->settings.v[Q2_SET_SCREEN_X],
                            c->settings.v[Q2_SET_SCREEN_Y]);
    q2_screen_present(&c->screen);
    front = q2_screen_front(&c->screen);

    /*
     * The capture comes off the finished front buffer, before anything SDL
     * touches it — so a headless run and a windowed one write byte-identical
     * frames, and neither depends on a driver's idea of what a 15-bit texture
     * looks like.
     */
    if ((c->shot_path || c->shot_display_path) && c->shot_every > 0 &&
        (c->frame_index % c->shot_every) == 0)
        client_write_shot(c, true);

    if (!c->texture || !c->renderer)
        return;

    if (SDL_LockTexture(c->texture, NULL, &pixels, &pitch)) {
        int y;
        for (y = 0; y < c->height; y++) {
            memcpy((u8 *)pixels + (size_t)y * pitch,
                   front->px + (size_t)y * c->width,
                   (size_t)c->width * sizeof(u16));
        }
        SDL_UnlockTexture(c->texture);
    }

    SDL_RenderClear(c->renderer);
    {
        /*
         * THE PICTURE'S SHAPE, which is not the buffer's.
         *
         * The GPU's five horizontal modes all span the same active line, so a
         * 512-wide frame is the same picture as a 320-wide one with pixels half
         * as wide; PAL fills the 4:3 raster with 256 lines. That makes a
         * framebuffer pixel exactly 2:3, and blitting the buffer to fill the
         * window — which is what this did — a 1.5x horizontal stretch.
         *
         * AND THE PICTURE'S EXTENT, which is not the buffer's either. What
         * reaches the television is what PutDispEnv programs from the display
         * env: 240 lines — all of a 240-line buffer, 240 of PAL's 248 — starting
         * where SCREEN POSITION, installed by client_frame as
         * Config_InstallVideo installs it, says. So this blits the part of the
         * buffer the ranges scan, where they put it, measured from the picture
         * at the reset routine's position; the fit rectangle is that picture.
         * See q2_screen_place.
         */
        SDL_FRect src, dst;
        int out_w = 0, out_h = 0;
        q2_screen_placement pl;

        SDL_GetCurrentRenderOutputSize(c->renderer, &out_w, &out_h);
        client_place(c, out_w, out_h, &pl);

        if (pl.src_w > 0 && pl.src_h > 0) {
            src.x = (float)pl.src_x;  src.y = (float)pl.src_y;
            src.w = (float)pl.src_w;  src.h = (float)pl.src_h;
            dst.x = pl.dst_x;         dst.y = pl.dst_y;
            dst.w = pl.dst_w;         dst.h = pl.dst_h;
            SDL_RenderTexture(c->renderer, c->texture, &src, &dst);
        }
    }
    SDL_RenderPresent(c->renderer);
}

static void client_frame(client *c)
{
    q2_screen_hooks hooks;

    q2_screen_frame_begin(&c->screen, &c->ot);

    /*
     * THE LOADING SCREEN OWNS THE WHOLE FRAME, and there is nothing behind it.
     *
     * Black, the logo turning in the corner, and the page — built into the
     * same ordering table and composed by the same rasteriser as everything
     * else, because both halves of it are ordinary primitives: the logo is a
     * model and the word is a menu page (loading.h).
     *
     * It composes against the SCREEN'S OWN VRAM IMAGE rather than the
     * session's. The level whose pages were resident has just been replaced,
     * and a zone gate inside one map does not re-upload them at all — so a
     * screen that borrowed the live image would either draw from a bank that
     * has gone or take the map's textures away to get one.
     *
     * `q2_screen_build` is not called: no viewport is installed, no world is
     * walked, and the buffer is cleared here rather than by the background env
     * a viewport pass would have armed.
     */
    if (c->loading.open) {
        psx_raster_opts lo = c->opts;

        lo.textures = true;
        q2_loading_build_ot(&c->loading, &c->ot, c->width, c->height);
        /*
         * And it clears: TestIt empties both ordering tables at 0x8006E188 and
         * 0x8006E194 before the vblank hook takes the screen, so nothing of the
         * level it is leaving survives under it.
         */
        psx_fb_clear(q2_screen_back(&c->screen), 0);
        q2_screen_compose(&c->screen, &c->ot, c->loading.vram, &lo);
        client_present(c);
        return;
    }

    /*
     * 0x800780C0 clears the whole screen once and turns the per-viewport clears
     * off, which is what paints the gutters between split viewports.
     *
     * THE COLOUR IS BLACK, and this used to write (16, 16, 32) with that
     * address as its authority. `0x800780C0` writes no colour at all: it zeroes
     * `view+260` on every viewport and calls the full-screen background env.
     * The colour lives at gp+1604, and there are exactly three references to it
     * in the whole image — `0x80076A00` reads it into the env's rgb, and
     * `0x8006E0B0` and `0x80070FA0` each `memset` it to zero. It is four zero
     * bytes on disc and nothing ever writes anything else, so `q2_screen_init`
     * already has it right and the frame should not overwrite it.
     *
     * It showed up as a navy field behind the title screen, which is where a
     * retail capture is unambiguous: the front end draws one model over black.
     */
    c->screen.disp.bg_enable = 1;
    c->screen.background_enable = true;

    /* Each viewport reads its owner's submerged bit (view+288). */
    {
        int p;

        for (p = 0; p < c->screen.view_count; p++) {
            int owner = c->mp_enabled ? p : 0;
            bool submerged =
                (c->sim[0].player[owner].ent.flags & Q2_ENT_UNDERWATER) != 0;
            q2_screen_water_set(&c->screen, p, true, submerged);
        }
    }

    memset(&hooks, 0, sizeof(hooks));
    hooks.view = client_draw_view;
    hooks.user = c;
    {
        q2_camera saved_cam = c->cam;
        /*
         * PROCESSTIMEDLIGHTS, AND EXACTLY AROUND THE WORLD PASS.
         *
         * ProcessLights (0x80075B94) is three calls — ProcessTimedLights
         * (0x800741AC), ProcessMobileLights (0x80074598), then the two list
         * pointers reset — and the frame calls it at 0x8003903C, which is AFTER
         * DrawAllModels at 0x80039020. So a timed light is appended to the
         * world list, consumed by the world repaint and thrown away inside one
         * call: it NEVER reaches the per-entity GTE gather. Ticking the table
         * beside `q2_light_world_begin_frame` at the top of the frame, which is
         * where it looks like it belongs, would light every model the console
         * leaves dark.
         *
         * This port draws the world before its entities and applies the repaint
         * inline as each quad's baked colour is copied, so the console's
         * visibility window is reproduced by bracketing the whole screen build:
         * append here, and cut the list back to its previous length the moment
         * the last viewport has been walked. The dynamic lights already on the
         * list — a rocket's glow, a muzzle flash — are untouched either side,
         * which is right: those are visible to both passes on both machines.
         *
         * ONCE A FRAME, not once a viewport. The console's ProcessLights runs
         * from the frame function and the split screen's four views are drawn
         * inside `q2_screen_build`, so a tick inside `client_draw_view` would
         * count a two-player match's timed lights down twice as fast.
         */
        u32 dyn_before = c->lights_ready ? c->light_world.dynamic_world_count
                                         : 0;

        if (c->lights_ready)
            c->timed_lights_emitted += q2_light_timed_tick(&c->light_world);

        q2_screen_build(&c->screen, &c->ot, &c->gte, &hooks);

        if (c->lights_ready)
            c->light_world.dynamic_world_count = dyn_before;

        /* A paused frame must start from player zero's camera again. */
        c->cam = saved_cam;
    }

    /* Every viewport has now drawn from the beam queue, so it can go. This is
     * the tail of 0x80064F10, moved out to where "the last viewport" is a
     * thing that can be said. */
    q2_fx_beams_reset(&c->sim[0].fx);

    /*
     * The menu is part of the frame, not something painted over it afterwards.
     * It links into the overlay slice (menudraw.h) BEFORE composition, so the
     * one walk of the ordering table produces the world and then the menu on
     * top of it — which is what the console does, and is why the frozen world
     * shows through where the menu draws nothing.
     */
    if (c->menu.open && c->menu_font_ready) {
        q2_menu_draw_opts mo;

        q2_menu_draw_opts_default(&mo, &c->menu_font);
        /* The layout is authored for 512x248; centre that block in whatever
         * this window is rather than scaling 4bpp texels. The pointer undoes
         * exactly this, which is why the origin is one function. */
        client_menu_origin(c, &mo.origin_x, &mo.origin_y);
        mo.view_x   = 0;
        mo.view_w   = c->width < Q2_MENU_SCREEN_W ? c->width
                                                  : Q2_MENU_SCREEN_W;
        q2_menu_build_ot(&c->menu, &c->ot, &mo);
        q2_prompt_build_ot(&c->prompts, &c->menu_font, &c->ot, mo.bucket);
    }

    /*
     * The card front end, through the same path � its screens ARE menu pages
     * in every respect but having a page id, so they draw with the same font,
     * the same bar and the same rules.
     */
    if (c->mcard_open && c->menu_font_ready && c->card_menu.page) {
        q2_menu_draw_opts mo;

        q2_menu_draw_opts_default(&mo, &c->menu_font);
        client_menu_origin(c, &mo.origin_x, &mo.origin_y);
        mo.view_x   = 0;
        mo.view_w   = c->width < Q2_MENU_SCREEN_W ? c->width
                                                  : Q2_MENU_SCREEN_W;

        q2_menu_build_ot(&c->card_menu, &c->ot, &mo);
    }

    /* And the SAVE? prompt, which is one of those same screens raised by the
     * transition rather than by the front end (0x800203C4). */
    if (c->save_prompt_open && c->menu_font_ready && c->save_prompt_menu.page) {
        q2_menu_draw_opts mo;

        q2_menu_draw_opts_default(&mo, &c->menu_font);
        client_menu_origin(c, &mo.origin_x, &mo.origin_y);
        mo.view_x   = 0;
        mo.view_w   = c->width < Q2_MENU_SCREEN_W ? c->width
                                                  : Q2_MENU_SCREEN_W;

        q2_menu_build_ot(&c->save_prompt_menu, &c->ot, &mo);
    }

    /*
     * The overlay, whenever neither the menu nor the mission screen is up —
     * which is the console's arrangement, since both of those are the overlay
     * camera's and it draws one thing at a time. The crosshair follows the
     * PLAYER page's setting, which is the one menu toggle the HUD reads
     * (0x80043A58).
     *
     * AND NEVER IN THE FRONT END, which is not the same test as "no menu is
     * up". QFRONT is a screen and not a level — it carries no icon sheet
     * because it draws no status bar — and it had been getting away with the
     * menu's own suppression until the half second between a difficulty and
     * the opening reel, which is a front end with the page closed. A crosshair
     * appeared over the blank title screen for exactly those fifteen frames.
     */
    if (!c->mp_enabled && c->hud_ready && c->hud_font_ready && !c->in_front_end &&
        !c->menu.open && !c->mission_open && !c->mcard_open &&
        !c->endmis_open && !c->credits_open) {
        q2_hud_ctx ctx;

        c->hud[0].crosshair = (c->settings.v[Q2_SET_CROSSHAIR] != 0);
        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_build_ot(&c->hud[0], &c->hud_font, &ctx, &c->ot, 0);
    }

    /*
     * THE VIEW'S OWN COORDINATES, along the top — P, or `--coords`.
     *
     * `c->cam` is the camera client_draw_view rewrote for the last viewport it
     * composed, so in a single-player frame it is the player's view: the eye
     * position q2_sim_eye publishes and the angles q2_sim_view_angles does, not
     * the feet and not the body's facing. In a split it reports the last of the
     * viewports rather than all of them, which is the honest thing a one-line
     * readout can say.
     *
     * Drawn OUTSIDE the overlay's own gate above. That gate is the console's
     * arrangement for the console's overlay; this is a reader's tool, and it is
     * wanted under a paused menu as much as in play. It stays out of the front
     * end, a film and the boot chain, where a world position means nothing.
     *
     * y = 4 puts it above Q2_HUD_MSG_TOP, so a pickup line stacking down the
     * top-left corner never has to share the row. The string carries no markup
     * escape — no @, ^, |, ~, # or & — so the interpreter at 0x80042328 lays it
     * out as plain glyphs.
     */
    if (c->show_coords && c->hud_font_ready && !c->in_front_end &&
        !c->film_open && !c->boot_open) {
        q2_hud_ctx ctx;
        q2_hud_pen pen;
        char line[72];

        /* The yaw is shown on the circle `--yaw` takes, 0..4095: the camera's
         * own accumulates and goes negative, and -1055 and 3041 are the same
         * heading. The pitch is left signed, because `--pitch` is. */
        snprintf(line, sizeof(line),
                 "X %d  Y %d  Z %d  YAW %d  PITCH %d",
                 (int)c->cam.pos[0], (int)c->cam.pos[1], (int)c->cam.pos[2],
                 (int)(c->cam.yaw & 4095), (int)c->cam.pitch);

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);
        /* q2_hud_measure is the original's measurer and counts CHARACTERS; the
         * glyph advance is a constant 8 (hud.h). */
        ctx.home_x = (s16)(ctx.width / 2 - q2_hud_measure(line) * 8 / 2);
        ctx.home_y = 4;
        q2_hud_print(&c->hud_font, &ctx, &pen, &c->ot, 0, line);
    }

    /*
     * The end-of-match scoreboard — what Q2_MP_REQ_RESULTS asks for.
     *
     * The console loads QMRESULT, a level directory of its own whose LevelBin
     * draws the screen; this shows the lines that module composes, in its
     * order, with the overlay's own text emitter. The words are the module's,
     * read out of its strings; the LAYOUT is not — where QMRESULT puts each
     * line goes through engine text calls whose offsets have not been read, so
     * these are stacked and centred rather than placed.
     */
    if (c->mp_scoreboard && c->hud_font_ready) {
        char lines[12][Q2_MP_SCORE_LINE];
        u32 n = q2_mp_scoreboard(&c->mp, NULL, lines, 12);
        u32 li;
        q2_hud_ctx ctx;
        q2_hud_pen pen;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);
        for (li = 0; li < (u32)c->mp.player_count; li++) {
            if (c->mp_results.ready & (1u << li)) {
                size_t len = strlen(lines[li + 1]);
                snprintf(lines[li + 1] + len, Q2_MP_SCORE_LINE - len, "  READY");
            }
        }

        /*
         * A line's position is the CONTEXT's home, not the pen's x and y —
         * the pen carries state across a string, the context says where the
         * string starts. Setting the pen instead put all five lines on one
         * row, each starting where the last one ended.
         */
        for (li = 0; li < n; li++) {
            /* `q2_hud_measure` is the original's measurer and returns
             * CHARACTERS, not pixels — the glyph advance is a constant 8. */
            ctx.home_x = (s16)(ctx.width / 2 -
                               q2_hud_measure(lines[li]) * 8 / 2);
            ctx.home_y = (s16)(ctx.height / 4 + (s32)li * 16);
            q2_hud_print(&c->hud_font, &ctx, &pen, &c->ot, 0, lines[li]);
        }

        /*
         * And the two rows the module holds STATICALLY, which is the whole of
         * what it holds — the per-player rows are built from the session, which
         * is why nothing static describes them (#101).
         *
         *     module+0x051F0  ALL PLAYERS PRESS   256, 180
         *     module+0x05208  FIRE TO CONTINUE    256, 200
         *
         * They used to come off the END of `q2_mp_scoreboard`'s line list and
         * stack 16 pixels under the last score, which put them wherever the
         * player count left them — four players pushed them two rows further
         * down than two did. They are furniture, not scores: fixed rows on the
         * module's own page, and the score list grows between the title and
         * them.
         *
         * x is 256 because the console's screen is 512 wide and every row in
         * this family is centred; the port centres in its own width for the
         * same reason. y is scaled by the same ratio rather than used raw, so
         * the pair keeps its 20-pixel gap at any height.
         */
        {
            static const struct { const char *text; int y; } k_prompt[] = {
                { "ALL PLAYERS PRESS", 180 },
                { "FIRE TO CONTINUE",  200 }
            };
            u32 k;

            for (k = 0; k < 2; k++) {
                ctx.home_x = (s16)(ctx.width / 2 -
                                   q2_hud_measure(k_prompt[k].text) * 8 / 2);
                ctx.home_y = (s16)((s32)ctx.height * k_prompt[k].y / 240);
                q2_hud_print(&c->hud_font, &ctx, &pen, &c->ot, 0,
                             k_prompt[k].text);
            }
        }
    }

    /*
     * THE END-OF-MATCH BANNER, which was composed and only ever logged.
     *
     * A match or a VERSUS round ends, the port kept rendering the arena for a
     * second and a half and then the scoreboard appeared. The player never saw
     * "TIME UP", "GAME OVER", "ROUND OVER" or "ROUND DRAWN", and never saw who
     * had won: q2_mp_banner, q2_mp_find_winner and q2_mp_winner_text were all
     * called, and all three only inside a Q2_INFO.
     *
     * What the console does, out of QMULTI.C's per-frame hook at 0x80100F24:
     *
     *   0x80100F54..0x80100F98  pick one of four one-item tables by end state —
     *                           module+0x1518 TIME UP, +0x1548 GAME OVER,
     *                           +0x1578 ROUND OVER, +0x15A8 ROUND DRAWN. Each
     *                           record is { text, x = 256, y = 124 }.
     *   0x80100FD8              engine+484, enter page 45
     *   0x80100FF4              engine+512 with module+0x14E8 and a1 = 16 —
     *                           the WINNER row, { "", x = 256, y = 20 }
     *   0x80101010              0x8010098C composes the winner line into it,
     *                           SKIPPED when the end state is 5 (ROUND DRAWN,
     *                           0x80101008's `beq`)
     *   0x801010BC              engine+512 with the banner table and a1 = 32
     *   0x80101114              engine+524, the menu tick, every frame until
     *                           module+0x15DC counts past zero
     *
     * So the winner line is ABOVE the banner, at the top of the screen, not
     * under it: y = 20 against y = 124. The port draws both at those two rows
     * with the overlay's own emitter rather than through a menu page, for the
     * same reason the scoreboard above does — what the engine's page 45 puts
     * around them is furniture whose offsets have not been read, and the world
     * must keep running and keep taking no input while they are up, which a
     * real menu page here would not do.
     */
    if (c->mp_enabled && c->hud_font_ready && !c->mp_scoreboard &&
        c->mp.end != Q2_MP_RUNNING && c->mp_last_request == Q2_MP_REQ_NONE) {
        const char *banner = q2_mp_banner(&c->mp);

        if (banner) {
            q2_hud_ctx ctx;
            q2_hud_pen pen;

            q2_hud_ctx_centre_in(&ctx, c->width, c->height);
            q2_hud_pen_default(&pen);

            if (c->mp.end != Q2_MP_END_ROUND_DRAWN) {
                char who[64];
                int  w = q2_mp_find_winner(&c->mp);

                q2_mp_winner_text(&c->mp, w, NULL, who, sizeof(who));
                ctx.home_x = (s16)(ctx.width / 2 -
                                   q2_hud_measure(who) * 8 / 2);
                ctx.home_y = (s16)((s32)ctx.height * 20 / 240);
                q2_hud_print(&c->hud_font, &ctx, &pen, &c->ot, 0, who);
            }

            ctx.home_x = (s16)(ctx.width / 2 -
                               q2_hud_measure(banner) * 8 / 2);
            ctx.home_y = (s16)((s32)ctx.height * 124 / 240);
            q2_hud_print(&c->hud_font, &ctx, &pen, &c->ot, 0, banner);
            c->mp_banner_frames++;
        }
    }

    /*
     * VIEW CREDITS: the module's roll, scrolling up the middle of the screen.
     *
     * The scroll rate and the spacing are this port's, not a reading — the
     * module's own arrangement is in code the port does not run, and #123 says
     * so. What IS the disc's is every word and their order.
     */
    if (c->credits_open && c->hud_font_ready) {
        q2_hud_ctx ctx;
        q2_hud_pen pen;
        u32 li;
        s32 top;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);

        /* One line every 14 pixels, the whole roll sliding up at a pixel every
         * other frame, and looping when the last line has left the top. */
        top = (s32)ctx.height - (c->credits_scroll / 2);
        if (top + (s32)c->credits_count * 14 < 0)
            c->credits_scroll = 0;

        for (li = 0; li < c->credits_count; li++) {
            s32 y = top + (s32)li * 14;

            if (y < -14 || y > (s32)ctx.height)
                continue;
            ctx.home_x = (s16)(ctx.width / 2 -
                               q2_hud_measure(c->credits[li]) * 8 / 2);
            ctx.home_y = (s16)y;
            q2_hud_print(&c->hud_font, &ctx, &pen, &c->ot, 0, c->credits[li]);
        }
        c->credits_scroll++;
    }

    /*
     * The mission screen, into the same overlay slice — which is where the
     * console's own overlay camera puts it (mission.h). It is mutually
     * exclusive with the menu because opening it closes the menu.
     */
    if (c->mission_open && c->hud_font_ready) {
        q2_hud_ctx ctx;
        q2_hud_pen pen;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);

        /*
         * THE PANEL FIRST, and the port had none.
         *
         * `DBInterMission`'s first act, before a single glyph, is
         * `jal 0x8003F0E4` at 0x80021B74 with the four `MissionRect` halfwords
         * packed into a0/a1 — `DrawDarkenedBox`, which is this port's
         * `q2_panel_build_ot` (two half-strength black TILEs and the eight-quad
         * rounded frame). Without it the tally was white text floating over the
         * corridor the player was standing in, while the end-of-mission placard
         * that appears seconds later had the full panel: the inconsistency was
         * visible inside one transition.
         *
         * The two bucket numbers are the briefing's and the placard's, so all
         * three screens sit on the same furniture in the same order.
         */
        if (c->menu_font_ready) {
            q2_panel_rect r;

            r.x = c->mission.box.x;
            r.y = c->mission.box.y;
            r.w = c->mission.box.w;
            r.h = c->mission.box.h;
            q2_panel_build_ot(&c->menu_font, &r, &c->ot,
                              psx_ot_depth_bucket(&c->ot, 2),
                              psx_ot_depth_bucket(&c->ot, 1), NULL);
        }

        /* The HUD layer takes a DEPTH rather than a bucket (gpu.h), and zero
         * is the front — which lands in the overlay slice, since no viewport
         * window is installed once q2_screen_build has returned. */
        q2_mission_build_ot(&c->mission, &c->hud_font, &ctx, &pen,
                            &c->ot, 0);
        /* And the press that leaves it, on the same bar the briefing and the
         * placard use. See where the board is raised. */
        q2_prompt_build_ot(&c->prompts, &c->menu_font, &c->ot,
                           psx_ot_depth_bucket(&c->ot, 0));
    }

    /*
     * The briefing screen — the panel, the text over it, and the BACK prompt
     * sliding up from the bottom. Mutually exclusive with the other two
     * overlay screens for the same reason they are with each other.
     */
    /*
     * The end-of-mission placard, on the same furniture and in the same slice.
     * Mutually exclusive with the briefing for the same reason the briefing is
     * with the menu.
     */
    if (c->endmis_open && c->hud_font_ready && c->menu_font_ready) {
        q2_hud_ctx ctx;
        q2_hud_pen pen;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);
        q2_endmission_build_ot(&c->endmis, &c->hud_font, &c->menu_font,
                               &ctx, &pen, &c->ot, 2, 1, 0);
        q2_prompt_build_ot(&c->prompts, &c->menu_font, &c->ot,
                           psx_ot_depth_bucket(&c->ot, 0));
    }

    if (c->briefing_open && !c->endmis_open &&
        c->hud_font_ready && c->menu_font_ready) {
        q2_hud_ctx ctx;
        q2_hud_pen pen;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);
        q2_briefing_build_ot(&c->briefing, &c->hud_font, &c->menu_font,
                             &ctx, &pen, &c->ot, 2, 1, 0);
        q2_prompt_build_ot(&c->prompts, &c->menu_font, &c->ot,
                           psx_ot_depth_bucket(&c->ot, 0));
    }

    /*
     * THE OBJECTIVES POP-UP, on the same furniture — it is the same screen.
     *
     * The composer reads `q2_briefing`, so the pop-up's two global strings are
     * copied in here rather than duplicated: the Location field stays the map's
     * own, which is what the console shows, and the orders and objective are
     * whatever the last HELPCOMPUTER left in the globals.
     *
     * Drawn last of the three so it is on top of an arrival briefing that has
     * not been dismissed, which is the order the console's frame gives it
     * (0x80038EDC runs the driver after the menu draw and suppresses that draw
     * while the pop-up is up).
     */
    if (c->popup.visible && c->hud_font_ready && c->menu_font_ready) {
        q2_briefing view = c->briefing;
        q2_hud_ctx  ctx;
        q2_hud_pen  pen;

        q2_briefing_set_orders(&view, c->popup.orders);
        q2_briefing_set_objective(&view, c->popup.objective);

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);
        q2_briefing_build_ot(&view, &c->hud_font, &c->menu_font,
                             &ctx, &pen, &c->ot, 2, 1, 0);
        q2_prompt_build_ot(&c->prompts, &c->menu_font, &c->ot,
                           psx_ot_depth_bucket(&c->ot, 0));
    }

    /* The prompts animate whether or not anything is showing them, which is
     * how they slide away when a screen closes (prompt.h). */
    q2_prompt_step(&c->prompts);

    q2_screen_compose(&c->screen, &c->ot, c->vram, &c->opts);

    /*
     * A film OWNS the screen.
     *
     * It is written straight into the finished buffer rather than linked into
     * the ordering table, because that is what the hardware does: MDEC output
     * is DMA'd to the frame buffer as a rectangle and never becomes a GPU
     * primitive. So the composed frame is cleared away underneath it — the
     * ordering table was still built and walked, which is work this frame did
     * not need, and it is left that way because a QENDMIS map has no world in
     * it to speak of and the alternative is a second frame path to maintain.
     */
    if (c->film_open) {
        psx_fb_clear(q2_screen_back(&c->screen), 0);
        client_film_blit(c);
    }

    /* And so does a boot screen, for the same reason and by the same route. */
    if (c->boot_open)
        client_boot_blit(c);

    client_present(c);
}


/* ------------------------------------------------------------------------- */
static void usage(void)
{
    printf("q2psx - native Quake II PSX\n\n");
    printf("usage: q2psx [--disc <path>] [--map NAME] [--zone N] [--scale N]\n"
           "             [--aspect MODE] [--saves DIR]\n\n");
    printf("  --disc   a .cue, .bin, .img or .iso, or a drive letter; without\n"
           "           it a launcher lists your discs and drives and starts\n"
           "           on the one played last\n");
    printf("  --map    level directory name (default BASE0)\n");
    printf("  --zone   zone index within the map (default: the level's own\n"
           "           start, its \"Default\" arrival point)\n");
    printf("  --no-loadout  start with the blaster alone; otherwise --map\n"
           "           hands the player what the campaign would have by\n"
           "           then (q2psx-inspect loadout prints the table)\n");
    printf("  --scale  buffer pixels across, i.e. horizontal zoom (default 3)\n");
    printf("  --aspect 4:3 (default) | tv | square | stretch\n");
    printf("           4:3    the drawn buffer as 4:3, as the game is played\n");
    printf("           tv     the strict pixel shape, 2:3 on PAL: 3%% taller\n");
    printf("           square one buffer pixel per window pixel: a 1.5x stretch\n");
    printf("           stretch fill the window, whatever shape it is\n");
    printf("  --saves  where save files live (default: the platform's own)\n");
    printf("\n  --version, -v  what this build is, and the commit it came from\n");
    printf("\n  running without a player:\n");
    printf("  --headless    no window, no audio; a fixed 1/30 s step\n");
    printf("  --demo        drive the pad from a fixed script rather than keys\n");
    printf("  --dm --dm-players N  start a split-screen arena match (1..4)\n");
    printf("  --dm-split horizontal|vertical  choose the two-player split\n");
    printf("  --dm-rounds N  rounds needed to win Versus (1..32767)\n");
    printf("  --gamepad-player-one  assign four pads starting with player one\n");
    printf("  --crosshair / --no-crosshair  override the crosshair setting\n");
    printf("  --vibration / --no-vibration  override the VIBRATION setting\n");
    printf("  --watch       frame and aim at the nearest live creature\n");
    printf("  --watch-hold N  ...and keep a killed creature framed for N frames\n");
    printf("  --movie NAME  play a film from Q2DATA/MOVIES and nothing else\n"
           "                (TAKE1BP.STX, OUTRO1P.STX, ROGUEINP.STX on PAL;\n"
           "                 TAKE1B.STX, OUTRO1.STX, ROGUEIN1.STX on NTSC)\n");
    printf("  --new-game    confirm a difficulty: the opening reel, then level 1\n");
    printf("  --boot        the logo screens and intro film before the menu\n");
    printf("  --no-boot     ...and skip them in a run that would show them\n");
    printf("  --frames N    stop after N frames\n");
    printf("  --shot P.ppm  write the console's own framebuffer to P.ppm\n");
    printf("  --shot-every N  ...and one every N frames, numbered\n");
    printf("  --shot-display P.ppm  ...and the WINDOW alongside it: the frame\n"
           "                placed as the television is shown it, 240 lines\n"
           "                of the buffer where SCREEN POSITION puts them\n");
    printf("  --screen-pos X Y  override SCREEN POSITION (-20..20, -32..64)\n");
    printf("  --powerup KIND hold quad|invuln|enviro|breather for HUD capture\n");
    printf("  --fire-event NAME [F]  queue ONE named Events entry point at\n"
           "                frame F, instead of every trigger volume on the\n"
           "                map. Level authors name the interesting ones\n"
           "                (BIGGUN: PLATFORM, DestroyGlass, GravTeleport)\n");
    printf("  --at X,Y,Z    stand here instead of at the zone's spawn point\n");
    printf("  --yaw N       ...facing this way (the engine's 0..4095)\n");
    printf("  --pitch N     ...and looking this far up or down\n");
    printf("  --coords      show the view's world position and angles along\n"
           "                the top of the frame; the P key toggles the same\n"
           "                readout while you play\n");
    printf("  --zone-trace  log every zone gate, teleport and unexplained\n"
           "                jump in the player's position while you play\n");
    printf("  --zone-probe  ...and, without playing, where each of this map's\n"
           "                zone gates leads and whether it lands anywhere\n");
    printf("  --report      print the run's own counters at exit, one\n"
           "                'report.<group>.<name> <integer>' per line\n");
    printf("  --ot-range N  how far the depth sort reaches, in world units\n"
           "                (default %d)\n", Q2_CAMERA_SORT_RANGE);
    printf("  --sort-data   use the zone's authored SortData (the default)\n");
    printf("  --depth-sort  diagnostic fallback: derive world order from depth\n");
}

/* ------------------------------------------------------------------------- */
/* Zone instrumentation                                                       */
/* ------------------------------------------------------------------------- */
/*
 * The watchdog behind `--zone-trace`.
 *
 * A player walks; a player does not jump 2,000 units in a thirtieth of a
 * second. Anything that does is a relocation, and the only question worth
 * asking about it is whether something MEANT to do it. Every deliberate path —
 * the zone gate, the TELEPORT primitive, a spawn, a map change, the number-key
 * zone hotkeys — leaves its name in `move_reason` on the way past, so a jump
 * that arrives with the field empty was nobody's decision and is the fault.
 *
 * The threshold is the console's own scale: Q2_VIEW_STAND is 576, so 2,000 is
 * a bit over three standing heights and no run, fall or lift covers it in one
 * frame. The cell the player lands in is reported with it, because a relocation
 * INTO a valid cell and one into no cell at all are different bugs.
 */
#define ZONE_TRACE_JUMP 2000

static void client_zone_watch(client *c)
{
    const q2_player *p;
    s32    now[3];
    s64    dx, dy, dz;
    double dist;

    if (!c->zone_trace)
        return;

    c->trace_frame++;
    p = &c->sim[0].player[c->sim[0].cur_player];
    now[0] = p->pos[0];
    now[1] = p->pos[1];
    now[2] = p->pos[2];

    if (!c->last_pos_valid) {
        c->last_pos[0]    = now[0];
        c->last_pos[1]    = now[1];
        c->last_pos[2]    = now[2];
        c->last_pos_valid = true;
        Q2_INFO("[zone] f%-6u start  zone %d  pos (%d,%d,%d)  cell %d",
                c->trace_frame, c->zone_index, now[0], now[1], now[2],
                (int)c->sim[0].current_node);
        return;
    }

    dx   = (s64)now[0] - c->last_pos[0];
    dy   = (s64)now[1] - c->last_pos[1];
    dz   = (s64)now[2] - c->last_pos[2];
    dist = sqrt((double)(dx * dx + dy * dy + dz * dz));

    if (dist >= ZONE_TRACE_JUMP) {
        c->jumps_seen++;
        Q2_WARN("[zone] f%-6u JUMP %.0f units  (%d,%d,%d) -> (%d,%d,%d)"
                "  zone %d  cell %d  reason: %s",
                c->trace_frame, dist,
                c->last_pos[0], c->last_pos[1], c->last_pos[2],
                now[0], now[1], now[2],
                c->zone_index, (int)c->sim[0].current_node,
                c->move_reason ? c->move_reason
                               : "*** NONE - NOTHING ASKED FOR THIS ***");
    }

    c->move_reason = NULL;
    c->last_pos[0] = now[0];
    c->last_pos[1] = now[1];
    c->last_pos[2] = now[2];
}

/*
 * `--zone-probe` — where does a zone gate actually LEAVE you?
 *
 * The reported fault is "suddenly teleporting you to a different part of the
 * map when you reach the end of a zone", and the first answer to it assumed a
 * gate must move the player, on the strength of one measurement: BASE1's zone-0
 * SPAWN point resolves to no cell in zone 1. That measurement proves nothing.
 * The spawn point is the START of zone 0; a gate fires at its END, tens of
 * thousands of units away. The question was never whether the zone-0 spawn is
 * inside zone 1 — of course it is not — but whether the GATE's own doorway is.
 *
 * So this asks that. For every trigger volume whose script reaches a ZONEGATE,
 * it takes the volume's centre and asks the DESTINATION zone's movement hull
 * which cell holds it. A gate that lands in a real cell of the zone it names is
 * a doorway between two adjacent regions of one coordinate space, and the
 * console has nothing to do on arrival but keep walking. A gate that lands
 * nowhere needs an arrival point, and that arrival point has to be found.
 *
 * The destination's StartPos points are resolved in the same hull, so the two
 * answers can be read against each other rather than argued.
 */
enum { PROBE_MAX_GATES = 128, PROBE_MAX_ZONES = 12 };

typedef struct probe_gate {
    u32  trig;
    s32  centre[3];
    s32  min[3], max[3];
    char dest_name[16];
    int  dest;
    s32  node_in_dest;
    u32  zones_holding;      /* bitmask of zones whose hull holds the centre */
} probe_gate;

static void client_zone_probe(client *c, const char *map)
{
    static probe_gate gates[PROBE_MAX_GATES];
    u32 gate_count = 0;
    int zone_count = 0;
    int z, g;

    printf("=== zone probe: %s ===\n", map);

    /*
     * Zone 0 first, because the trigger and event chunks live in the map's
     * COMMON file and one load is enough to read every gate on the map.
     */
    if (!client_load_zone(c, map, 0)) {
        printf("  %s: no zone 0\n", map);
        return;
    }

    if (c->sim[0].triggers_ready && c->sim[0].events_ready) {
        const q2_events *ev = &c->sim[0].event_rt.events;
        u32 i;

        for (i = 0; i < c->sim[0].triggers.count &&
                    gate_count < PROBE_MAX_GATES; i++) {
            q2_trigger t;
            u32        visit[24];
            u32        n_visit = 0, vi;

            if (!q2_trigger_get(&c->sim[0].triggers, i, &t))
                continue;
            if (t.event_offset == Q2_TRIGGER_NO_EVENT)
                continue;

            visit[n_visit++] = t.event_offset;

            /*
             * A trigger's own record may only TRIGGER others, so the gate can
             * sit one or two lists down. Followed breadth-first with a visited
             * set, because event lists are allowed to name each other.
             */
            for (vi = 0; vi < n_visit && gate_count < PROBE_MAX_GATES; vi++) {
                q2_event_record rec;
                u32             item_i;

                if (!q2_events_record_at(ev, visit[vi], &rec))
                    continue;

                for (item_i = 0; item_i < rec.n_items; item_i++) {
                    q2_event_item it;

                    if (!q2_events_get_item(ev, &rec, item_i, &it))
                        break;

                    if (it.opcode == Q2_EVOP_TRIGGER) {
                        u32       n = 0, k;
                        const u8 *offs = NULL;

                        if (q2_events_get_list(&it, &n, &offs)) {
                            for (k = 0; k < n && n_visit < 24; k++) {
                                u32 off = q2_events_list_entry(offs, k);
                                u32 s;
                                for (s = 0; s < n_visit; s++)
                                    if (visit[s] == off) break;
                                if (s == n_visit)
                                    visit[n_visit++] = off;
                            }
                        }
                        continue;
                    }

                    if (it.opcode != Q2_EVOP_ZONEGATE || !it.payload)
                        continue;

                    {
                        probe_gate *pg = &gates[gate_count++];
                        u32 k;
                        int val = 0, digits = 0;

                        memset(pg, 0, sizeof(*pg));
                        pg->trig = i;
                        for (k = 0; k < 3; k++) {
                            pg->min[k]    = t.min[k];
                            pg->max[k]    = t.max[k];
                            pg->centre[k] = (t.min[k] + t.max[k]) / 2;
                        }
                        for (k = 0; k < 12 && it.payload[k]; k++)
                            pg->dest_name[k] = (char)it.payload[k];

                        for (k = 0; pg->dest_name[k]; k++) {
                            char ch = pg->dest_name[k];
                            if (ch >= '0' && ch <= '9') {
                                val = val * 10 + (ch - '0');
                                digits++;
                            } else if (digits) {
                                digits = 0; val = 0;
                            }
                        }
                        pg->dest         = digits ? val : -1;
                        pg->node_in_dest = -1;
                    }
                }
            }
        }
    }

    printf("  gates found: %u\n", gate_count);

    /*
     * Now walk every zone with the hull loaded and ask it about each gate's
     * doorway — every zone's answer, not just the destination's, because a
     * doorway that resolves in BOTH the zone it leaves and the zone it names is
     * a shared threshold and settles the question outright.
     */
    for (z = 0; z < PROBE_MAX_ZONES; z++) {
        q2_start_pos_list spawns;
        u32 i;

        if (z != 0 && !client_load_zone(c, map, z))
            break;
        zone_count = z + 1;

        for (g = 0; g < (int)gate_count; g++) {
            s32 n = q2_coll_find_node(&c->sim[0].coll, gates[g].centre, -1,
                                      true);

            if (n >= 0)
                gates[g].zones_holding |= 1u << z;
            if (gates[g].dest == z)
                gates[g].node_in_dest = n;
        }

        /*
         * SortData census beside both hulls. This no longer infers a mapping:
         * the renderer loads the PrimaryColl cell selected by view+146 and
         * reads that on-disc record's exact byte offset at +28. Tiled stream
         * count remains useful here only as a format diagnostic.
         */
        {
            q2_sortdata sd;
            u32         streams = 0;
            q2_result   sr;

            memset(&sd, 0, sizeof(sd));
            sr = q2_sortdata_parse(&sd, &c->zone.zone);
            if (sr == Q2_OK)
                streams = q2_sortdata_enumerate(&sd, NULL, 0);

            printf("  --- zone %d: %u sort streams (%s, %u bytes) |"
                   " coll(sec) %u cells |"
                   " coll(pri) %u cells | %u scene nodes ---\n",
                   z, streams, q2_result_str(sr), sd.size,
                   c->sim[0].coll_ready ? c->sim[0].coll.node_count : 0,
                   c->sim[0].coll_primary_ready
                       ? c->sim[0].coll_primary.node_count : 0,
                   c->zone.scene.node_count);
        }
        if (q2_start_pos_parse(&spawns, &c->common) == Q2_OK) {
            for (i = 0; i < spawns.count; i++) {
                q2_start_pos sp;
                s32 at[3], n;

                if (!q2_start_pos_get(&spawns, i, &sp) || sp.zone != z)
                    continue;
                at[0] = sp.x;
                at[1] = q2_sim_origin_y(sp.y);
                at[2] = sp.z;
                n = q2_coll_find_node(&c->sim[0].coll, at, -1, true);
                printf("      StartPos '%-14s' (%7d,%6d,%7d) yaw %5d  cell %d\n",
                       sp.name, sp.x, sp.y, sp.z, sp.angle, (int)n);
            }
        }
    }

    printf("  --- gates (%d zones on this map) ---\n", zone_count);
    for (g = 0; g < (int)gate_count; g++) {
        char held[64];
        int  k, o = 0;

        held[0] = '\0';
        for (k = 0; k < zone_count && o < 50; k++)
            if (gates[g].zones_holding & (1u << k))
                o += snprintf(held + o, sizeof(held) - (size_t)o, "%d ", k);

        printf("      trig %3u -> '%s' (zone %d)  box (%d..%d, %d..%d, %d..%d)\n",
               gates[g].trig, gates[g].dest_name, gates[g].dest,
               gates[g].min[0], gates[g].max[0],
               gates[g].min[1], gates[g].max[1],
               gates[g].min[2], gates[g].max[2]);
        printf("               centre (%d,%d,%d) resolves in zones [%s]"
               " ; cell in DEST %d = %d%s\n",
               gates[g].centre[0], gates[g].centre[1], gates[g].centre[2],
               held[0] ? held : "none",
               gates[g].dest, (int)gates[g].node_in_dest,
               gates[g].node_in_dest >= 0
                   ? "   <-- DOORWAY IS INSIDE THE DESTINATION"
                   : "");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * `--report`: the run's own numbers, in one block, in one shape.
 *
 * Everything here already existed. What did not was a place to read it from:
 * the big per-shot census in `client_write_shot` is prose, is only printed when
 * a picture is written, and reports the moment the shutter fell rather than the
 * run. So a scripted sweep over the whole disc could tell that the client had
 * not crashed and nothing else — not whether a creature ever woke, whether a
 * door ever moved, whether the level had warned its way through its load.
 *
 * One `key value` per line under a fixed prefix, because the consumer is a
 * script: `report.<group>.<name> <integer>`. Integers only, so a comparison is
 * a comparison and not a diff of English.
 *
 * Emitted at exit, after the frame loop and before anything is freed.
 */
static void client_report(const client *c)
{
    const q2_sim *s = &c->sim[0];
    u32 cre_live = 0, cre_hunting = 0, cre_dead = 0, cre_moved = 0;
    u32 cre_patrol = 0;

    if (c->creatures_ready) {
        u32 i;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];

            if (m->dead)
                cre_dead++;
            if (!m->in_use || m->dead)
                continue;
            cre_live++;
            if (m->enemy)
                cre_hunting++;
            /*
             * Walking a route rather than standing: monster_start_go
             * (0x80061BA4) only leaves `movetarget` set when G_PickTarget
             * resolved the record's target to a path corner.
             */
            if (m->movetarget)
                cre_patrol++;
            /*
             * MOVED, not "walked": a creature the AI never reached and one
             * whose every step was refused are both stationary, and the whole
             * point of this figure is to tell either of them from one that is
             * chasing. `cre_home` is the spawn position the client keeps.
             */
            if (c->cre_home &&
                (m->pos[0] != c->cre_home[i * 3 + 0] ||
                 m->pos[2] != c->cre_home[i * 3 + 2]))
                cre_moved++;
        }
    }

#define REPORT(name, value) Q2_INFO("report.%s %ld", name, (long)(value))
    REPORT("run.frames",            c->frame_index);
    REPORT("run.ticks",             s->tick_count);
    REPORT("run.level_time",        s->level_time);
    REPORT("run.errors",            q2_log_count(Q2_LOG_ERROR));
    REPORT("run.warnings",          q2_log_count(Q2_LOG_WARN));

    REPORT("level.loads",           c->loads_done);
    REPORT("level.loads_failed",    c->loads_failed);
    REPORT("level.loading_screens", c->loading_raises);
    REPORT("level.save_prompts",    c->save_prompts);
    /* client+0x46 for player one, the term the weapon model's matrix carries
     * and the camera's does not, and the pitch RotMatrixYXZ is actually handed
     * once the clip rotation is added to it. Both read zero until something
     * writes client+0x44 — see the seek beside q2_vw_advance. */
    REPORT("view.aim_assist",       c->vw[0].aim_assist);
    /* The camera's roll as a signed angle on the 4096-step circle, which is
     * where the death cam's ease toward -384 can be read (0x800380C8). */
    REPORT("view.cam_roll",         ((c->cam.roll + 2048) & 0xFFF) - 2048);
    REPORT("view.clip_pitch",       q2_vw_clip_pitch(&c->vw[0]));
    REPORT("level.zone",            c->zone_index);
    REPORT("level.nodes",           c->zone.scene.node_count);
    REPORT("level.vertices",        c->zone.points.count);
    REPORT("level.jumps_seen",      c->jumps_seen);
    REPORT("level.secrets_found",   c->secrets_found);
    REPORT("level.secrets_total",   c->secrets_total);
    /* The mission row's Kills pair, which is the LEVEL's and not the resident
     * zone's — `creatures.dead` below is the resident zone's live set and the
     * two differ the moment a gate has been crossed. */
    {
        u32 kdead, kplaced;

        client_level_tally(c, &kdead, &kplaced);
        REPORT("level.kills_found",  kdead);
        REPORT("level.kills_total",  kplaced);
    }
    REPORT("level.secret_sounds",   c->secret_sounds);

    /* Where they ended up. Three integers rather than a vector, because the
     * consumer is a script and a script wants numbers it can subtract. */
    REPORT("player.x",              s->player[0].pos[0]);
    REPORT("player.y",              s->player[0].pos[1]);
    REPORT("player.z",              s->player[0].pos[2]);
    REPORT("player.on_ground",      s->player[0].on_ground ? 1 : 0);
    REPORT("player.health",         s->combat.inv.health);
    REPORT("player.armour",         s->combat.inv.armour);
    REPORT("player.weapon",         s->combat.weapon_id);
    REPORT("player.attacks",        c->player_attacks);
    REPORT("match.banner_frames", c->mp_banner_frames);
    REPORT("audio.quad_raises",     c->quad_raises);
    /* One per FIRE CALL, and — for the chaingun alone — one more per bullet
     * out of the clone at 23/63. See client_advance_view_weapon. */
    REPORT("player.water_splashes", s->water_splashes);
    REPORT("audio.weapon_sounds",   c->weapon_sounds);
    /* SoundByCreature's bang at 0x8005AA54, one per Explosion model that
     * bound, and PSXEXPLODE's own report at 0x8002695C beside it. A brush
     * in a map with the model adds one to each. */
    REPORT("audio.explosion_sounds", c->explosion_sounds);
    REPORT("audio.explosion_sounds_missed", c->explosion_sounds_missed);
    REPORT("audio.explosive_reports", c->explosive_sounds);
    REPORT("audio.chaingun_bangs",  c->chaingun_bangs);
    REPORT("audio.shotgun_cocks",   c->cocks_played);
    REPORT("audio.quad_gated",      c->quad_gated);
    /*
     * 0x800B2E74 and 0x800B2E50 — the live CD volume and the one it is walking
     * toward at four a tick (CdCheckMusic, 0x800718F8). A run that ends with
     * the two equal has finished its ramp; one that ends with the live value at
     * zero under a pause page is the duck. `music_tracks` counts playlist
     * STARTS, which is how a zone gate shows up here at all: 0x800794F8 zeroes
     * the countdown and the next entry begins.
     */
    REPORT("audio.music_level",     c->music_level);
    REPORT("audio.music_target",    c->music_target);
    REPORT("audio.music_tracks",    c->music_tracks);
    REPORT("player.shots",          c->shots_fired);
    REPORT("player.shots_dry",      c->shots_dry);
    REPORT("player.weapon_lines",   c->weapon_lines);
    /* The player tick's three screen-flash raisers: 0x80039A80 and its warning
     * beep, and the health/armour compare at 0x8003AE10. */
    REPORT("player.damage_flashes", c->hud_flashes[0]);
    REPORT("player.powerup_tints",  c->powerup_tints[0]);
    REPORT("player.powerup_warns",  c->powerup_warns[0]);
    REPORT("player.armour_flashes", c->armour_flashes[0]);
    REPORT("player.power_flashes",  c->power_flashes[0]);

    /* 0x80045D54's verdicts. `gate_rewinds` counts the frames retail threw
     * away because they ended inside a live entity box; `gate_unplaced` the
     * ones that ended in no collision cell. Both climbing steadily on a map
     * with no movers means the gate is misfiring, not that the map is bad. */
    REPORT("player.gate_relocated", (s32)q2_move_step_scan.relocated);
    REPORT("player.gate_rewinds",   (s32)q2_move_step_scan.rewound_overlap);
    REPORT("player.gate_unplaced",  (s32)q2_move_step_scan.rewound_unplaced);

    REPORT("creatures.placed",      c->creatures_ready ? c->creatures.set.count : 0);
    REPORT("creatures.live",        cre_live);
    REPORT("creatures.hunting",     cre_hunting);
    REPORT("creatures.dead",        cre_dead);
    REPORT("creatures.moved",       cre_moved);
    /* The patrol routes: how many corners this map's PathCorner group spawned
     * (0x8007F390) and how many live creatures are walking one. */
    REPORT("creatures.corners",     c->creatures_ready ? c->creatures.corner_count : 0);
    REPORT("creatures.patrolling",  cre_patrol);
    REPORT("creatures.thoughts",    c->ai_thoughts);
    REPORT("creatures.swings",      c->cre_swings);
    /* Swings `fire_hit`'s own reach test threw away (0x80061198, crebind.c).
     * A run where these climb is one where the player broke contact during
     * the wind-up — exactly the swing that used to land from any distance. */
    REPORT("creatures.swings_short", q2_cre_actions.melee_short);
    REPORT("creatures.shots",       c->cre_shots);
    REPORT("creatures.dying_shots", c->cre_dying_shots);
    REPORT("creatures.rounds",      c->cre_rounds);
    REPORT("creatures.rounds_player",   c->cre_rounds_player);
    REPORT("creatures.rounds_creature", c->cre_rounds_creature);
    REPORT("creatures.rounds_world",    c->cre_rounds_world);
    REPORT("creatures.launched",    c->cre_launched);
    REPORT("creatures.flashes",     c->cre_flashes);
    REPORT("creatures.fire_sounds", c->cre_fire_sounds);
    REPORT("creatures.sounds",      c->cre_sounds);
    REPORT("creatures.drops",       c->cre_drops);
    /* The three ways a creature's step can be cut short, so a run can tell
     * "nothing blocked it" from "nothing tried to move". */
    REPORT("creatures.traces",      c->ai_world.stats.traces);
    REPORT("creatures.blocked_door", c->ai_world.stats.trace_blocked_ent);
    REPORT("creatures.blocked_body", c->ai_world.stats.trace_blocked_body);
    /* The lost-you pursuit: how often a creature reached its waypoint and how
     * often the breadcrumb ring gave it the next one. */
    REPORT("creatures.pursue_arrived", q2_ai_stats.pursue_arrived);
    REPORT("creatures.pursue_marker",  q2_ai_stats.pursue_marker);

    /* Not "how many bursts were raised" — how many primitives reached the
     * ordering table. The two differ by exactly the area cull. */
    REPORT("fx.prims",              c->fx_prims);
    REPORT("fx.flashes",            c->sim[0].fx.stats.flashes_drawn);
    REPORT("fx.blur_samples",       c->sim[0].fx.stats.blur_samples);
    REPORT("fx.blur_quads",         c->sim[0].fx.stats.blur_quads);
    REPORT("fx.bursts",             c->ent_bursts);
    REPORT("fx.debris_models",      c->debris_models);
    REPORT("fx.debris_faces",       c->debris_faces);
    /* DrawTelePorts: pads registered by PSXTELEPORT_T, columns queued on the
     * last frame, and every dynamic light the walk placed over the run. */
    REPORT("fx.teleport_pads",      c->teleports.count);
    REPORT("fx.teleport_columns",   c->teleport_drawn);
    REPORT("fx.teleport_lights",    c->teleport_lights);

    /*
     * There is no lights group in the original's own reporting because there is
     * no reporting; these exist so a headless run can see a timed light at all.
     * `timed_active` is how many of the sixteen slots are still counting down
     * at the end of the run, `timed_emitted` is every append ProcessTimedLights
     * made over the whole run, and `world_lit` is how many world vertices
     * ProcessMobileLights repainted.
     */
    REPORT("lights.timed_active",   q2_light_timed_active(&c->light_world));
    REPORT("lights.timed_emitted",  c->timed_lights_emitted);
    REPORT("lights.world_lit",      c->world_lit_points);
    REPORT("lights.world_unslotted", c->world_lit_unslotted);

    REPORT("world.movers",          c->movers_ready ? c->movers.count : 0);
    REPORT("world.mover_boxes",     s->mover_count);
    REPORT("world.rot_steps",       c->rot_steps);
    REPORT("world.rot_moved",       c->rot_moved);
    REPORT("world.rot_loop_starts", c->rot_loop_starts);
    REPORT("world.rot_loop_stops",  c->rot_loop_stops);
    REPORT("world.rot_clicks",      c->rot_clicks);
    {
        u32 not_shut = 0;
        u32 hatches  = client_hatches(c, &not_shut);

        REPORT("world.hatches",     hatches);
        REPORT("world.hatches_open", not_shut);
    }
    {
        u32 running = 0, strokes = 0;
        s32 offset  = 0;
        u32 pistons = client_pistons(c, &running, &strokes, &offset);

        REPORT("world.pistons",         pistons);
        REPORT("world.pistons_running", running);
        REPORT("world.piston_strokes",  strokes);   /* half-strokes MADE */
        REPORT("world.piston_offset",   offset);    /* signed, summed     */
    }
    REPORT("world.key_prompts",     c->key_prompts);
    REPORT("world.centre_lines",    c->centre_lines);
    REPORT("world.breakable_hits",  s->breakable_hits);
    /* Drained, not queued: `sim->glass_count` is zero again by the time this
     * runs, so the client's own tally is the only durable one. */
    REPORT("world.glass_smashes",   c->glass_sounds + c->glass_sounds_missed);
    REPORT("world.explosive_blasts", s->explosive_destroyed);
    REPORT("world.triggers",        s->triggers.count);

    REPORT("script.calls",          s->event_rt.call_count);
    REPORT("script.strings",        c->script_strings);
    REPORT("script.sounds",         c->script_sounds);
    REPORT("script.summoned",       c->script_summoned);
    REPORT("script.teleports",      c->script_teleports);
    REPORT("script.units",          c->script_units);
#undef REPORT
}

int main(int argc, char **argv)
{
    client c;
    const char *disc_path = NULL;
    /* The launcher's choice, and its window for the game to take over. */
    static char launch_path[1024];
    SDL_Window *launch_window = NULL;
    const char *map = "BASE0";
    bool map_given = false;
    bool zone_probe = false;
    int zone_index = 0;
    bool zone_given = false;
    int scale = 3;                   /* mirrored into c.win_scale */
    int i;
    u64 last;
    /*
     * WHAT THE PROCESS TELLS ITS CALLER.
     *
     * `main` used to return 0 whatever happened, including from the three
     * `goto done` arms below — so `--map NOSUCHMAP` and `--movie TYPO` were
     * indistinguishable, to a script, from a level that loaded and played.
     * Every scripted sweep in this project is a subprocess whose first check is
     * the exit code, which made that check worthless.
     */
    int exit_code = 0;

    /* Answered before any setup, and before --disc is required: someone
     * asking a binary what it is should not need a disc to find out. The
     * same check, in the same place, as q2psx-inspect. */
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 ||
                      strcmp(argv[1], "-v") == 0)) {
        q2_version_print();
        return 0;
    }

    memset(&c, 0, sizeof(c));
    /* PrimaryColl cell +28 carries the exact SortData byte offset used by the
     * retail renderer. Keeping the authored order on is therefore the parity
     * path; the old depth mapping remains an explicit diagnostic. */
    c.use_sort = true;
    /* Deathmatch settings, applied after the map loads. -1 keeps the
     * shipped default the session initialiser installs. */
    s16 mp_rounds = q2_mp_round_options[Q2_MP_ROUND_OPTION_DEFAULT];
    q2_mp_mode mp_mode    = Q2_MP_DEATHMATCH;

    c.trace_cre = -1;
    c.give_powerup = -1;
    /* Alive, and holding a gun, before anything has loaded: `memset` leaves
     * `linked_weapon` false, and the view weapon draw now asks for it. */
    {
        int pi;

        for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++)
            q2_player_death_init(&c.death[pi]);
    }
    /* The same switch the inspect tool honours, and for the same reason:
     * several load-time decisions -- which movers a zone drops, which object
     * slots resolve -- are only ever reported at Q2_LOG_DEBUG, and the client
     * had no way to ask for them. */
    if (getenv("Q2PSX_VERBOSE"))
        q2_log_set_level(Q2_LOG_DEBUG);
    int        mp_players = 2;
    s16        mp_horizontal_split = -1;
    s16        force_crosshair = -1;
    s16        force_vibration = -1;
    s16        mp_frags   = -2;
    s16        mp_minutes = -2;


    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc)       disc_path = argv[++i];
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) { map = argv[++i]; map_given = true; }
        else if (!strcmp(argv[i], "--zone") && i + 1 < argc)  { zone_index = atoi(argv[++i]); zone_given = true; }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = c.win_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless"))              c.headless = true;
        else if (!strcmp(argv[i], "--demo"))                  c.demo = true;
        else if (!strcmp(argv[i], "--crosshair"))             force_crosshair = 1;
        else if (!strcmp(argv[i], "--no-crosshair"))          force_crosshair = 0;
        else if (!strcmp(argv[i], "--vibration"))             force_vibration = 1;
        else if (!strcmp(argv[i], "--no-vibration"))          force_vibration = 0;
        else if (!strcmp(argv[i], "--watch"))                 c.watch = true;
        else if (!strcmp(argv[i], "--watch-hold") && i + 1 < argc) {
            c.watch      = true;
            c.watch_hold = (u32)atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--zone-probe"))            zone_probe = true;
        else if (!strcmp(argv[i], "--sort-data"))             c.use_sort = true;
        else if (!strcmp(argv[i], "--depth-sort"))            c.use_sort = false;
        else if (!strcmp(argv[i], "--no-autoswitch"))         c.no_autoswitch = true;
        else if (!strcmp(argv[i], "--god"))                   c.god = true;
        else if (!strcmp(argv[i], "--no-loadout"))            c.no_loadout = true;
        else if (!strcmp(argv[i], "--continues") && i + 1 < argc)
            c.continues = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ot-range") && i + 1 < argc) c.ot_range = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zone-trace"))            c.zone_trace = true;
        else if (!strcmp(argv[i], "--report"))                c.report = true;
        else if (!strcmp(argv[i], "--fire-triggers")) {
            /* An optional frame to fire ON, so a test can let the player take
             * damage or collect something first and then walk through the
             * door. Without it, the first simulated frame. */
            c.fire_triggers = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                c.fire_at_frame = strtol(argv[++i], NULL, 10);
            c.fire_interval = c.fire_at_frame > 0 ? c.fire_at_frame : 60;
        }
        else if (!strcmp(argv[i], "--fire-event") && i + 1 < argc) {
            c.fire_event = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                c.fire_at_frame = strtol(argv[++i], NULL, 10);
        }
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            int x = 0, y = 0, z = 0;
            if (sscanf(argv[++i], "%d,%d,%d", &x, &y, &z) == 3) {
                c.at[0] = x; c.at[1] = y; c.at[2] = z;
                c.at_given = true;
            } else {
                fprintf(stderr, "--at wants X,Y,Z\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--no-lasers"))             c.no_lasers = true;
        /*
         * `--glint`: what F6 toggles, from the command line. The glint was only
         * reachable by keypress, which put it out of reach of a headless
         * capture — and it is the one effect in the engine that raises a style-1
         * dynamic light, so "does the glint's flare come out" was a question no
         * scripted run could ask.
         */
        else if (!strcmp(argv[i], "--glint"))                 c.show_glint = true;
        else if (!strcmp(argv[i], "--shoot"))                 c.shoot = true;
        else if (!strcmp(argv[i], "--shoot-to-kill"))         c.shoot_to_kill = true;
        /* The same readout P toggles, for a run with nobody at the keyboard. */
        else if (!strcmp(argv[i], "--coords"))                c.show_coords = true;
        /*
         * `--save-load N`: quick-save at frame N and quick-load on the next
         * frame, reporting the world state either side. A round-trip test at
         * the data level cannot say whether the CLIENT hands over everything
         * it owns — the movers, the panes and the creatures all live outside
         * the sim and each had to be wired separately.
         */
        else if (!strcmp(argv[i], "--save-load") && i + 1 < argc)
            c.save_load_at = strtol(argv[++i], NULL, 10);
        /* `--credits`: open the roll straight away. A headless run cannot walk
         * the title screen to OPTIONS and press X. */
        else if (!strcmp(argv[i], "--credits"))               c.show_credits = true;
        /*
         * `--new-game`: confirm a difficulty for the player.
         *
         * Same reason as `--credits`, and it is the only way to reach either
         * OPENING FILM from a script: neither is a map you can ask for, both are
         * what beginning a game does, and the demo pad deliberately steps in and
         * out of the title page rather than committing to it. It costs what a
         * player's start costs — the reel and then the intro, 2,456 frames and
         * 1,280 — so a capture that wants a level should ask for the map.
         */
        else if (!strcmp(argv[i], "--new-game"))              c.start_new_game = true;
        /*
         * `--boot` / `--no-boot`: whether this run walks the logo screens and
         * the intro film in front of the menu. A windowed run does by default
         * and a headless one does not; both flags exist so a capture can have
         * it either way. See the boot note beside `in_front_end`'s assignment.
         */
        else if (!strcmp(argv[i], "--boot"))                  c.boot_chain = true;
        else if (!strcmp(argv[i], "--no-boot"))               c.no_boot = true;
        /* `--keys`: hand the player every key. A scripted run cannot go and
         * find one, and the records behind `ONKEYDO` are otherwise unreachable
         * in a sweep — which is the gate working, and also why what is behind
         * it goes unmeasured. */
        else if (!strcmp(argv[i], "--keys"))                  c.all_keys = true;
        /*
         * `--objectives N`: raise the objectives pop-up at frame N. The screen
         * is normally reached from a trigger volume's HELPCOMPUTER or from the
         * pause menu's MISSION row, and a headless run can walk into neither.
         */
        else if (!strcmp(argv[i], "--objectives") && i + 1 < argc)
            c.popup_at_frame = (s32)strtol(argv[++i], NULL, 10);
        /* `--weapon N`: hold weapon slot N, fed. See the field's note. */
        else if (!strcmp(argv[i], "--weapon") && i + 1 < argc)
            c.give_weapon = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--powerup") && i + 1 < argc) {
            const char *w = argv[++i];

            if      (!strcmp(w, "quad"))     c.give_powerup = Q2_POWERUP_QUAD;
            else if (!strcmp(w, "invuln"))   c.give_powerup = Q2_POWERUP_INVULN;
            else if (!strcmp(w, "enviro"))   c.give_powerup = Q2_POWERUP_ENVIRO;
            else if (!strcmp(w, "breather")) c.give_powerup = Q2_POWERUP_BREATHER;
            else {
                fprintf(stderr, "--powerup wants quad|invuln|enviro|breather\n");
                return 2;
            }
        }
        /* `--armour body|combat|jacket|shield|shard`: see the field's note. */
        else if (!strcmp(argv[i], "--armour") && i + 1 < argc) {
            const char *w = argv[++i];

            c.give_armour_points = 100;
            c.give_armour_cells  = 0;
            if      (!strcmp(w, "body"))   c.give_armour_flag = Q2_INV_ARMOUR_BODY;
            else if (!strcmp(w, "combat")) c.give_armour_flag = Q2_INV_ARMOUR_COMBAT;
            else if (!strcmp(w, "jacket") ||
                     !strcmp(w, "shard"))  c.give_armour_flag = Q2_INV_ARMOUR_JACKET;
            else if (!strcmp(w, "shield")) {
                c.give_armour_flag   = Q2_INV_POWER_SHIELD;
                c.give_armour_points = 0;
                c.give_armour_cells  = 100;
            }
            else if (!strcmp(w, "both")) {
                /* A vest AND a shield — the one state that alternates. */
                c.give_armour_flag   = Q2_INV_ARMOUR_COMBAT | Q2_INV_POWER_SHIELD;
                c.give_armour_cells  = 100;
            }
            else {
                fprintf(stderr, "--armour wants body|combat|jacket|shield|both\n");
                return 2;
            }
        }
        /* Play a film and nothing else — the campaign reaches OUTRO1P by
         * finishing, and that is a long way to go to look at a decoder. */
        else if (!strcmp(argv[i], "--movie") && i + 1 < argc)
            c.film_arg = argv[++i];
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) {
            c.at_yaw = (s16)atoi(argv[++i]);
            c.yaw_given = true;
        }
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) {
            c.at_pitch = (s16)atoi(argv[++i]);
            c.pitch_given = true;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            c.frames_total = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            c.shot_path = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc)
            c.shot_every = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--shot-display") && i + 1 < argc)
            c.shot_display_path = argv[++i];
        else if (!strcmp(argv[i], "--screen-pos") && i + 2 < argc) {
            c.screen_pos_set = true;
            c.screen_pos_x = (int)strtol(argv[++i], NULL, 10);
            c.screen_pos_y = (int)strtol(argv[++i], NULL, 10);
        }
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) q2_save_set_dir(argv[++i]);
        else if (!strcmp(argv[i], "--dm")) {
            c.mp_enabled = true;
            if (!map_given) { map = "MATRIX1"; map_given = true; }
        }
        else if (!strcmp(argv[i], "--dm-mode") && i + 1 < argc)
            mp_mode = (q2_mp_mode)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-players") && i + 1 < argc)
            mp_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-split") && i + 1 < argc) {
            const char *split = argv[++i];
            if (!strcmp(split, "horizontal"))      mp_horizontal_split = 1;
            else if (!strcmp(split, "vertical"))   mp_horizontal_split = 0;
            else {
                fprintf(stderr, "--dm-split wants horizontal|vertical\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--gamepad-player-one"))
            c.gamepad_player_one = true;
        else if (!strcmp(argv[i], "--dm-stage"))
            c.mp_stage = true;
        else if (!strcmp(argv[i], "--trace-cre") && i + 1 < argc)
            c.trace_cre = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-frags") && i + 1 < argc)
            mp_frags = (s16)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-rounds") && i + 1 < argc) {
            char *end;
            long value = strtol(argv[++i], &end, 10);
            if (*end || value < 1 || value > 32767) {
                fprintf(stderr, "--dm-rounds wants an integer from 1 to 32767\n");
                return 2;
            }
            mp_rounds = (s16)value;
        }
        else if (!strcmp(argv[i], "--dm-minutes") && i + 1 < argc)
            mp_minutes = (s16)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) {
            const char *a = argv[++i];
            if      (!strcmp(a, "4:3"))     c.fit = Q2_SCREEN_FIT_FULL_4_3;
            else if (!strcmp(a, "tv"))      c.fit = Q2_SCREEN_FIT_TELEVISION;
            else if (!strcmp(a, "square"))  c.fit = Q2_SCREEN_FIT_SQUARE;
            else if (!strcmp(a, "stretch")) c.fit = Q2_SCREEN_FIT_STRETCH;
            else { usage(); return 1; }
        }
        else { usage(); return 1; }
    }

    /*
     * NO DISC NAMED, so ask — unless nobody is there to answer. A headless
     * run is a script, and a script that forgot --disc wants the usage and a
     * failing exit code, not a window it cannot see.
     */
    if (!disc_path) {
        if (c.headless) {
            usage();
            return 1;
        }
        if (!q2_launcher_run(launch_path, sizeof(launch_path),
                             &launch_window)) {
            SDL_Quit();
            return 0;
        }
        disc_path = launch_path;
    }

    if (disc_open(&c.disc, disc_path) != Q2_OK) {
        fprintf(stderr, "cannot open disc '%s'\n", disc_path);
        return 1;
    }

    if (q2_identify(c.disc, &c.build) != Q2_OK) {
        fprintf(stderr, "this does not look like a Quake II PSX disc\n");
        disc_close(c.disc);
        return 1;
    }

    printf("%s (%s, %s)\n",
           c.build.desc ? c.build.desc->name : "uncatalogued build",
           c.build.serial, q2_video_std_str(c.build.video));

    /* The North American build's string lookup asks for `<key>US` before
     * `<key>`, and the shared level data answers it (leveltext.h). */
    q2_leveltext_set_variant(q2_region_string_suffix(c.build.region));

    /*
     * The console's own framebuffer, brought up the way 0x800764DC brings it
     * up: 512 x 248 on PAL, 512 x 240 on NTSC, read out of each executable.
     * Everything is rendered here and upscaled; the dither and the vertex
     * snapping are defined in these pixels, so rendering at a higher resolution
     * would change the look.
     *
     * This is also what the menu's coordinates were always in — its tables put
     * the title at x = 256 of 512 — so the screen and the menu now agree
     * instead of the menu being mapped onto a smaller buffer.
     */
    if (q2_screen_init(&c.screen, c.build.video) != Q2_OK) {
        fprintf(stderr, "cannot bring the screen up\n");
        disc_close(c.disc);
        return 1;
    }
    Q2_INFO("screen: %ux%u, %u Hz fields", c.screen.disp.width,
            c.screen.disp.height, c.screen.disp.field_hz);

    c.width  = c.screen.disp.width;
    c.height = c.screen.disp.height;

    /* Texture pages start at ABR 0 and are promoted as opaque geometry is
     * drawn, exactly as the engine's own table is. */
    q2_world_render_init(&c.render);

    /*
     * A headless run brings SDL up at all only to keep the shutdown path
     * uniform; there is no video, no audio device and no window. Everything the
     * frame needs — the ordering table, the rasteriser, the screen — is the
     * port's own code and does not know SDL exists.
     */
    if (c.headless) {
        Q2_INFO("headless: %ld frame%s at 1/30 s%s", c.frames_total,
                c.frames_total == 1 ? "" : "s", c.demo ? ", demo pad" : "");
        if (c.frames_total <= 0)
            c.frames_total = 1;
        goto no_window;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        disc_close(c.disc);
        return 1;
    }

    q2_gamepads_open(&c.gamepads, c.gamepad_player_one ? 0 : 1);

    /* Declare the stream at the console's own 37800 Hz and let SDL resample to
     * whatever the device wants. */
    {
        SDL_AudioSpec spec;
        spec.format   = SDL_AUDIO_S16LE;
        spec.channels = XA_CHANNELS;
        spec.freq     = XA_SAMPLE_RATE;

        c.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, NULL, NULL);
        if (c.audio)
            SDL_ResumeAudioStreamDevice(c.audio);
        else
            Q2_WARN("no audio device: %s", SDL_GetError());
    }

    /*
     * The window opens at `scale` buffer pixels across and however tall the
     * console's pixel shape makes that — 1536 x 1080 at the default scale of 3,
     * not the 1536 x 744 the buffer's own dimensions suggest. Stretching
     * vertically rather than squeezing horizontally keeps every one of the 512
     * columns the 512-wide mode was chosen for.
     *
     * It is clamped to the display it opens on so a large scale on a small
     * screen does not put the title bar off the top, and the fit is recomputed
     * from the real window size every frame anyway, so a clamped window is
     * simply a smaller correct picture.
     */
    {
        int ww = 0, wh = 0;
        SDL_Rect usable;

        q2_screen_window_size(&c.screen, c.fit, scale, &ww, &wh);

        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
            usable.w > 0 && usable.h > 0) {
            int max_w = usable.w * 9 / 10;
            int max_h = usable.h * 9 / 10;

            if (ww > max_w) { wh = (int)((s64)wh * max_w / ww); ww = max_w; }
            if (wh > max_h) { ww = (int)((s64)ww * max_h / wh); wh = max_h; }
        }

        if (ww < 64) ww = 64;
        if (wh < 64) wh = 64;

        /* The launcher's window, when there was one, becomes the game's: no
         * close and reopen between choosing the disc and the first frame. */
        if (launch_window) {
            c.window = launch_window;
            SDL_SetWindowSize(c.window, ww, wh);
            SDL_SetWindowResizable(c.window, true);
            SDL_SetWindowPosition(c.window, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED);
        } else {
            c.window = SDL_CreateWindow("Q2PSX-PC", ww, wh,
                                        SDL_WINDOW_RESIZABLE);
        }
    }
    if (!c.window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        disc_close(c.disc);
        return 1;
    }

    c.renderer = SDL_CreateRenderer(c.window, NULL);

    /*
     * VSYNC, so the front end does not free-run.
     *
     * QFRONT is two nodes and eight vertices and drew at around 560 fps
     * uncapped. Nothing here is wrong at that rate any more — the accumulator
     * carries its remainder now — but the console presents at the field rate
     * and a title screen spinning a logo as fast as the GPU will go is not
     * what it did. Failure is ignored: a driver that will not vsync still
     * gets a correct, faster picture.
     */
    if (c.renderer)
        SDL_SetRenderVSync(c.renderer, 1);
    /*
     * XBGR1555, NOT XRGB1555 — RED LIVES IN THE LOW BITS.
     *
     * The framebuffer is uploaded by a straight memcpy of the console's own
     * halfwords, so the texture format has to name the console's own bit
     * layout. The PSX GPU packs a 15-bit pixel as
     *
     *     bit 15    mask/STP        bits 14..10  B
     *     bits 9..5 G               bits 4..0    R
     *
     * which is red-in-the-low-bits: `psx_rgb555()` in gpu.h builds exactly
     * that, and `unpack555()` in raster.c reads it back the same way. SDL
     * names formats most-significant-channel-first, so that layout is
     * XBGR1555; XRGB1555 is its mirror and reads B where R is.
     *
     * That mirror is what a wrong value here looks like: the picture is
     * otherwise perfect, and red and blue are exchanged in every pixel of it.
     * Quake II's rust-brown rock renders slate blue and its violet sky renders
     * crimson, which reads as a palette or a PAL-vs-NTSC fault rather than as
     * one enum, and sends the search a long way from the actual line.
     *
     * Nothing upstream of here is affected, and that is the tell: `--shot`
     * writes its PPM off the same front buffer through `unpack555()` before
     * SDL is handed anything, so the captures come out right while the window
     * does not. A defect visible only in the window and never in a capture is
     * a presentation-format defect by construction.
     */
    c.texture  = SDL_CreateTexture(c.renderer, SDL_PIXELFORMAT_XBGR1555,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   c.width, c.height);

    /* Nearest-neighbour: the whole point is to show the original's pixels. */
    SDL_SetTextureScaleMode(c.texture, SDL_SCALEMODE_NEAREST);

    /*
     * And prove the format above, rather than trusting the name.
     *
     * The whole defect this guards against is silent: a wrong enum produces a
     * complete, sharp, correctly-lit picture with two channels exchanged, and
     * nothing in the pipeline objects. Worse, `--shot` keeps writing correct
     * PPMs the whole time, so the project's own comparison workflow reports
     * everything is fine while the window says otherwise.
     *
     * So ask SDL what it would pack pure red into and check it against what the
     * GPU model packs pure red into. They must be the same halfword. This costs
     * one call at startup and turns "the colours look odd" into a line of
     * output naming the two values.
     */
    {
        const SDL_PixelFormatDetails *fd =
            SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_XBGR1555);

        if (fd) {
            u32 sdl_red = SDL_MapRGB(fd, NULL, 255, 0, 0);
            u16 psx_red = psx_rgb555(255, 0, 0);

            if ((u16)sdl_red != psx_red)
                Q2_WARN("framebuffer format mismatch: SDL packs red as %04X, "
                        "the GPU model as %04X — red and blue will be "
                        "exchanged on screen",
                        (unsigned)sdl_red, (unsigned)psx_red);
        }
    }

no_window:
    /* 217 buckets, `ClearOTag(db + 10984, 217)` at 0x80018398 — the table is
     * carved into per-viewport slices, so its size is not a tuning knob. */
    psx_ot_init(&c.ot, Q2_SCREEN_OT_ENTRIES, 300000);
    psx_raster_opts_default(&c.opts);
    c.vram = (psx_vram *)calloc(1, sizeof(psx_vram));

    q2_camera_default(&c.cam, c.width, c.height);

    /*
     * The menu's layout is in the console's own 512x248 framebuffer, which is
     * where every coordinate in its tables was authored; the renderer maps that
     * onto whatever this window is. Passing the NTSC height would be a guess —
     * only PAL's 248 has been read out of a build (openquestions #30).
     */
    /*
     * A session starts by installing a layout (0x8003F8D8's jump table on the
     * session mode). The boot state the screen came up in is the front end's;
     * this is single player in a level.
     */
    q2_screen_set_layout(&c.screen, Q2_SCREEN_LAYOUT_ONE, 1);

    q2_menu_settings_defaults(&c.settings);
    q2_menu_reset_video_for(&c.settings, c.screen.disp.height);

    /*
     * And the CD volume target with them: FrontEndResetSoundDefaults writes the
     * slider at 0x8001FA60 and then `sh v0, 0x800B2E50` at 0x8001FA74 with v0
     * still holding 96, which is that slider doubled. The LIVE value is left at
     * its BSS zero, which is what makes the first track of a session swell in
     * rather than start at level.
     */
    c.music_target = (s16)(c.settings.v[Q2_SET_MUSIC] * 2);

    /*
     * USE MOUSE, on — and only for a run with a window and a player at it.
     *
     * `q2_menu_reset_player` writes 0 because it is 0x8001BDA8 transcribed, and
     * the console's default controller is a pad. This is the same question
     * 0x8001C8A8 asks — WHICH controller is connected — with the answer a PC
     * gives, so it is set here rather than by editing the reset routine, which
     * has to keep saying what the executable says. RESET TO DEFAULTS on the
     * PLAYER page turns it off again, which is the player's call to make.
     *
     * A scripted run keeps the console's answer: its pad script is written in
     * STANDARD A's buttons and there is no mouse to grab.
     */
    if (!c.headless && !c.demo)
        c.settings.v[Q2_SET_USE_MOUSE] = 1;

    /* Nothing is being clicked yet. Zero is a valid item index, so the idle
     * value has to be -1. */
    c.menu_click_index = -1;
    c.menu_click_part  = Q2_MENU_HIT_NONE;

    q2_menu_init(&c.menu, &c.settings, Q2_MENU_SCREEN_H);
    q2_menu_set_fb_height(&c.menu, c.screen.disp.height);
    q2_menu_set_multiplayer(&c.menu, false);
    q2_menu_set_us_english(&c.menu, c.build.region == Q2_REGION_NTSC_U);
    /* The pads are already open, so ask each one whether it can shock before
     * anything can reach OPTIONS -> CONTROLLER. */
    client_menu_bind_pads(&c);

    /* The level-completion screen. Its counters are the sim's to fill; until
     * kills and secrets are tallied it honestly reads zero. */
    q2_mission_init(&c.mission);
    c.mission_row = -1;
    /* gp+362's .data seed, 0x800AE76A = 0x04B0. Set ONCE, because
     * `PadEndInterMission` is its only writer and `SetInterMission` does not
     * re-arm it: the first board is gated for the same four seconds as every
     * board after it. */
    c.mission_gate = Q2_MISSION_GATE_TICKS;
    /* Two frames back, so the very first tick of a session is a resume: a key
     * held while the window opens must not be a press. */
    c.pad_frame   = -2;
    q2_briefing_init(&c.briefing);
    q2_prompt_init(&c.prompts);
    /* ONCE per session, not per level: the two strings are global on the
     * console and are never reset, so the game shows "Awaiting Orders." until
     * its first HELPCOMPUTER and each one after that advances the pair. */
    q2_briefing_popup_init(&c.popup);

    /*
     * The memory-card front end, with the port's file-backed save system behind
     * its three function pointers. The signatures match exactly, so this is a
     * plain assignment rather than three thunks.
     */
    q2_save_ui_init(&c.save_ui);
    c.mcard_host.poll    = q2_save_ui_poll;
    c.mcard_host.request = q2_save_ui_request;
    c.mcard_host.choose  = q2_save_ui_choose;
    c.mcard_host.user    = &c.save_ui;
    q2_mcard_init(&c.mcard, &c.mcard_host);

    /* The shadow menu the card screens navigate and draw through. It shares the
     * settings block so a screen with a widget on it would work, though none
     * of the nine has one. */
    q2_menu_init(&c.card_menu, &c.settings, Q2_MENU_SCREEN_H);
    q2_menu_set_fb_height(&c.card_menu, c.screen.disp.height);
    q2_menu_set_us_english(&c.card_menu, c.build.region == Q2_REGION_NTSC_U);

    /* And a third, for the one card screen the TRANSITION raises rather than
     * the front end: "DO YOU WANT TO SAVE?" (0x800203C4). */
    q2_menu_init(&c.save_prompt_menu, &c.settings, Q2_MENU_SCREEN_H);
    q2_menu_set_fb_height(&c.save_prompt_menu, c.screen.disp.height);
    q2_menu_set_us_english(&c.save_prompt_menu,
                           c.build.region == Q2_REGION_NTSC_U);

    Q2_INFO("saves: %s", q2_save_dir());

    /*
     * The UI's tables come out of the boot executable, not off the disc's data
     * files: the glyph coordinates for the 8-pixel face and — the part that
     * matters here — the built-in palette bank every UI primitive samples
     * through (hudtables.h §"Palettes"). Without it the menu's letterforms are
     * in VRAM with no colours to read them by, so this is a hard requirement
     * for the menu rather than an optional extra, and it is loaded once
     * because a build's tables do not change per level.
     */
    c.hud_tables_ready = (q2_hud_tables_load(&c.hud_tables, c.disc,
                                             &c.build) == Q2_OK);

    /* The status bar's tables, from the same executable. */
    c.icons_ready = (q2_icon_tables_load(&c.icons, c.disc, &c.build) == Q2_OK);
    if (c.icons_ready)
        for (i = 0; i < Q2_MP_MAX_PLAYERS; i++)
            q2_statusbar_init(&c.sbar[i], &c.icons, 1);
    /*
     * The palette bank, so the bar draws each sprite in its own colours. The
     * bank is already in VRAM by this point — the menu font uploads it — so
     * this hands over the clut ids, not the pixels.
     */
    if (c.icons_ready && c.hud_tables_ready)
        for (i = 0; i < Q2_MP_MAX_PLAYERS; i++)
            q2_statusbar_set_palettes(&c.sbar[i], &c.hud_tables);
    else
        Q2_WARN("no status-bar tables for this build");
    if (!c.hud_tables_ready)
        Q2_WARN("no UI tables for this build — the menu will not draw");

    /*
     * THE LOADING SCREEN'S OWN ASSETS, opened once and held for the run.
     *
     * After the UI tables, because the palette bank is what the LOADING line is
     * coloured by; before anything loads a level, because `client_load_zone`
     * raises the screen and a screen that is not open yet is simply never
     * raised. A disc without QDUMMY says so once and runs without it.
     */
    if (q2_loading_open(&c.loading, c.disc, &c.hud_tables, &c.settings) ==
        Q2_OK)
        Q2_INFO("loading screen: the logo strip and the word, out of %s's "
                "frontend.lbm", Q2_LOADING_MAP);
    else
        Q2_INFO("loading screen: this disc has no %s — transitions cut "
                "straight through", Q2_LOADING_MAP);

    /*
     * The overlay, AFTER the tables it reads. This block used to sit above the
     * load, testing a flag that `memset(&c, 0, ...)` had just cleared, so
     * `q2_hud_init` never ran and `hud_ready` never became true — the client
     * drew no notifications, no centre line and no crosshair, on a build whose
     * tables load perfectly well.
     */
    if (c.hud_tables_ready) {
        /* One player, so four notification lines — the table at 0x8009D648
         * indexed by player count (hudtables.h). */
        q2_hud_init(&c.hud[0], &c.hud_tables, 1);
        c.hud[0].crosshair = (c.settings.v[Q2_SET_CROSSHAIR] != 0);
        c.hud_ready = true;
    }

    /*
     * The view weapon's animation bank, out of the same executable. It is per
     * disc rather than per map because the clips are code-segment data — only
     * the model the clips drive comes off a map.
     */
    c.vm_ready = (q2_vm_tables_load(&c.vm_tables, c.disc, &c.build) == Q2_OK);
    if (c.vm_ready)
        Q2_INFO("view weapon: %u animation keys", c.vm_tables.key_count);
    else
        Q2_WARN("no view-model bank for this build — no weapon in hand");

    /*
     * The item table, from the same executable. A build with no catalogued
     * addresses falls back to the transcribed PAL table rather than to no items:
     * unlike the effect ramps, the transcription is checked against the disc on
     * every run of `q2psx-inspect items`, so it is a known-good copy of exactly
     * this data rather than a guess.
     */
    c.item_table_ready = (q2_item_table_load(&c.item_table, c.disc,
                                             &c.build) == Q2_OK);
    if (c.item_table_ready)
        Q2_INFO("item table: %u records", c.item_table.count);
    else
        Q2_WARN("no item table for this build — using the built-in one");

    /*
     * The effect tables, from the same executable and for the same reason: a
     * ramp is nineteen gradients at a fixed address, and a build we have no
     * addresses for gets no effects rather than nineteen gradients read out of
     * somebody else's data.
     */
    c.fx_tables_ready = (q2_fx_tables_load_disc(&c.fx_tables, c.disc,
                                                &c.build) == Q2_OK);
    if (!c.fx_tables_ready)
        Q2_WARN("no effect tables for this build — nothing will spark");

    /*
     * Boot into the FRONT END, which is what the console arrives at: the level
     * it draws over is record 0 of the level table, `QFront` -> `LEVELS/QFRONT/`,
     * and `q2_menu_open` special-cases page 46 (0x8001A40C).
     *
     * It is not what the console STARTS at. Ahead of the front end sit two logo
     * screens and the intro film (`boot_open`), and `boot_chain` is whether this
     * run walks them. A windowed run does, because that is what a player gets. A
     * headless one does not unless it asks: ten seconds of logos and fifty-one
     * of film in front of every scripted front-end capture would be 1,850 frames
     * of something the capture did not ask for, which is the same argument the
     * old attract reel's headless guard made and the same answer.
     *
     * `--map` overrides all of it, because going straight to a level is what
     * every capture and every check in this project wants; without one, the game
     * starts where a player starts it.
     */
    snprintf(c.first_map, sizeof(c.first_map), "%s", map);
    if (!map_given) {
        c.in_front_end = true;
        map = "QFRONT";
        zone_index = 0;
        if (!c.headless && !c.no_boot)
            c.boot_chain = true;
    }

    /*
     * Start the match BEFORE the map loads, because placing the local player is
     * part of loading it and the spawn selector needs the session to exist.
     *
     * This is what the port never did: multiplayer.[ch] reconstructs the whole
     * of QMULTI.C — the scoring, the frag and time limits, the VERSUS round
     * rules, the banner countdown and the two game-state requests — and not one
     * of those entry points had a caller anywhere in the game. The rules ran in
     * the test suite and nowhere else.
     */
    if (force_crosshair >= 0)
        c.settings.v[Q2_SET_CROSSHAIR] = force_crosshair;
    /* VIBRATION is off until the CONTROLLER page turns it on, as on the
     * console (0x8001BDA8 resets it to 0); this is the shortcut. */
    if (force_vibration >= 0)
        c.settings.v[Q2_SET_VIBRATION] = force_vibration;
    /* SCREEN POSITION, clamped to the page's own range so the override cannot
     * reach a value the console could not (Q2_MENU_SCREEN_*). */
    if (c.screen_pos_set) {
        int px = c.screen_pos_x, py = c.screen_pos_y;

        if (px < Q2_MENU_SCREEN_X_MIN) px = Q2_MENU_SCREEN_X_MIN;
        if (px > Q2_MENU_SCREEN_X_MAX) px = Q2_MENU_SCREEN_X_MAX;
        if (py < Q2_MENU_SCREEN_Y_MIN) py = Q2_MENU_SCREEN_Y_MIN;
        if (py > Q2_MENU_SCREEN_Y_MAX) py = Q2_MENU_SCREEN_Y_MAX;
        c.settings.v[Q2_SET_SCREEN_X] = (s16)px;
        c.settings.v[Q2_SET_SCREEN_Y] = (s16)py;
    }

    /* The library's SetShockData callers reach the pads through here. */
    q2_shock_attach(client_shock_sink, &c);

    if (c.mp_enabled) {
        s16 frag = (mp_frags != -2)
                       ? mp_frags
                       : q2_mp_frag_options[Q2_MP_FRAG_OPTION_DEFAULT];
        s16 time = (mp_minutes != -2)
                       ? mp_minutes
                       : (mp_mode == Q2_MP_VERSUS
                              ? Q2_MP_NO_LIMIT
                              : q2_mp_time_options[Q2_MP_TIME_OPTION_DEFAULT]);

        if (mp_horizontal_split >= 0)
            c.settings.v[Q2_SET_HORIZONTAL_SPLIT] = mp_horizontal_split;
        client_mp_configure(&c, mp_mode, mp_players, frag, time, mp_rounds);
    }

    /*
     * Level music — this map's own playlist, not "track A because the mapping
     * is not decoded yet".
     *
     * The level record carries seven track ids and a jump-back byte, and an id
     * names a file and a channel through the table at 0x800A1DD8 (musictable.h).
     * A build whose tables are not catalogued falls silent rather than picking
     * a track that would be somebody else's.
     *
     * LOADED BEFORE THE FIRST ZONE, because the zone load is what selects the
     * music now (client_music_for_level). Loading them after it, as this used
     * to, meant the boot map ran its selection with both tables still marked
     * unready and started silent.
     */
    c.music_table_ready = (q2_music_table_load(&c.music_table, c.disc,
                                               &c.build) == Q2_OK);
    c.level_table_ready = (q2_level_table_load(&c.level_table, c.disc,
                                               &c.build) == Q2_OK);
    if (!c.music_table_ready)
        Q2_WARN("no music table for this build: the game will be silent");

    /*
     * `--map` takes a directory OR a level table display name.
     *
     * Not a convenience. The two cinematic screens are only reachable by
     * display name: `Intro FMV` and `Extro FMV` are records 10 and 11 of the
     * level table and BOTH are the directory `QFMV`, so a directory name cannot
     * distinguish them — the name IS the selector, which is exactly how the
     * module chooses between TAKE1BP and OUTRO1P. It is also how the engine
     * itself names levels: MISCOMPLETE looks its destination up this way.
     */
    if (map_given && c.level_table_ready) {
        const q2_level_entry *e = q2_level_find_display(&c.level_table, map);

        if (e && !e->is_placeholder && e->directory[0] &&
            !client_name_eq(map, e->directory)) {
            snprintf(c.film_screen, sizeof(c.film_screen), "%s", map);
            Q2_INFO("--map '%s' is the level table's %s", map, e->directory);
            map = e->directory;
            snprintf(c.first_map, sizeof(c.first_map), "%s", map);
        }
    }

    /*
     * THE MUSIC IS HELD FOR EXACTLY THE RUNS THAT WALK THE CHAIN.
     *
     * The same condition the chain itself is started under, further down, and
     * it is here rather than beside `boot_chain` because that flag is set in
     * two places: `--boot` sets it while the arguments are being read, long
     * before any of this. Arming the hold next to one of them left the other
     * playing the menu track over the legal screen, which is the bug this was
     * meant to fix. See `music_held`.
     */
    c.music_held = (c.boot_chain && c.in_front_end);

    /* A static question about the map, asked and answered without playing it. */
    if (zone_probe) {
        client_zone_probe(&c, map);
        goto done;
    }

    /* A level started from the command line is a fresh start, entered at its
     * "Default" arrival point and in that point's zone unless `--zone` says
     * otherwise. An arena has no Default: its players take MultiSpawns. */
    if (!c.in_front_end && !c.mp_enabled && !zone_given)
        zone_index = client_default_zone(&c, map);

    /* ...and carrying what the campaign would have handed them by then. */
    if (!c.in_front_end && !c.mp_enabled && !c.no_loadout &&
        c.item_table_ready &&
        q2_loadout_for_level(&c.launch_loadout, c.disc, &c.item_table, map,
                             q2_cre_skill()) > 0) {
        c.launch_loadout_valid = true;
        snprintf(c.launch_loadout_map, sizeof(c.launch_loadout_map), "%s", map);
    }

    if (!client_load_zone(&c, map, zone_index)) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        exit_code = 1;
        goto done;
    }

    /*
     * A session starts IN the game. The free-fly camera is a debug view for
     * looking at geometry with no physics in the way, and F4 is how you get to
     * it; booting into it meant a fresh launch ran none of the player's frame —
     * no movement model, no view kicks, no weapon in the hands, no status bar —
     * until a key was pressed that nothing tells the player about. A loaded save
     * already forced this on for exactly the same reason.
     */
    c.sim_enabled = true;

    /*
     * The title screen — or, if this run walks the boot chain, the first logo
     * screen, with the title screen four steps away at the end of it.
     * `client_boot_advance` finishes by calling exactly the function below.
     */
    if (c.boot_chain && c.in_front_end)
        client_boot_start(&c);
    else if (c.in_front_end)
        client_enter_front_end(&c);

    /* `--movie NAME`: play a film over whatever was loaded, and close the
     * front end so nothing is drawn on top of it. */
    if (c.film_arg) {
        if (client_film_start(&c, c.film_arg)) {
            client_menu_close(&c);
            c.in_front_end = false;
        } else {
            fprintf(stderr, "no such movie: %s\n", c.film_arg);
            exit_code = 1;
            goto done;
        }
    }

    /*
     * `--new-game`: the request the SINGLE PLAYER row raises, raised here.
     *
     * Through the menu's own one-shot rather than by calling the load directly,
     * so a scripted start goes down exactly the path a player's start goes
     * down — the skill, the carry flags, the intro film and the hand-off to
     * the first map, in that order.
     */
    if (c.start_new_game && !c.film_arg && !c.boot_open) {
        c.menu.request = Q2_MREQ_NEW_GAME;
        client_menu_requests(&c);
    }

    if (c.show_credits) {
        client_menu_requests(&c);   /* nothing pending; this just settles it */
        {
            const dat_chunk *lb = c.common.chunk[Q2_COMMON_LEVEL_BIN];

            if (lb && lb->data && lb->size)
                c.credits_count = q2_levelbin_credits(lb->data, lb->size,
                                                      c.credits,
                                                      Q2_LB_CREDITS_MAX);
            c.credits_open = c.credits_count > 0;
            if (c.credits_open)
                client_menu_close(&c);
            Q2_INFO("credits: %u lines", c.credits_count);
        }
    }

    c.running = true;
    last = c.headless ? 0 : SDL_GetTicks();

    while (c.running) {
        SDL_Event ev;
        u64   now;
        float dt;

        /*
         * A scripted run advances on a FIXED step rather than on the wall
         * clock, so its output is a function of the frame number alone. The
         * step is the console's own 1/30 s — everything the port times is in
         * 1/300 s units and the screen clamps a frame at 30 of them.
         */
        if (c.headless) {
            dt = 1.0f / 30.0f;
        } else {
            now = SDL_GetTicks();
            dt  = (float)(now - last) / 1000.0f;
            last = now;

            if (dt > 0.1f)
                dt = 0.1f;
        }

        while (!c.headless && SDL_PollEvent(&ev)) {
            int gp = q2_gamepads_event(&c.gamepads, &ev);
            if (gp >= 0 && (ev.type == SDL_EVENT_GAMEPAD_ADDED ||
                            ev.type == SDL_EVENT_GAMEPAD_REMOVED)) {
                Q2_INFO("controller: player %d %s", gp + 1,
                        c.gamepads.id[gp] ? "connected" : "disconnected");
                /* The pad set changed, so the VIBRATION gate has to be asked
                 * again — a DualShock plugged in mid-session is shockable and
                 * the pad that just left is not. */
                client_menu_bind_pads(&c);
            }
            if (gp >= 0 && ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                ev.gbutton.button == SDL_GAMEPAD_BUTTON_START &&
                !c.in_front_end && !c.mp_scoreboard && !c.film_open &&
                !c.boot_open && !c.mcard_open && !c.mission_open &&
                !c.briefing_open && !c.endmis_open && !c.credits_open) {
                if (c.menu.open) {
                    client_menu_close(&c);
                } else {
                    /* Same as the Escape path below: page 26's install is what
                     * composes the KILLS/SECRETS row, so the numbers have to be
                     * in the menu before it opens. */
                    client_menu_fill_stats(&c);
                    /*
                     * WHO PRESSED START. 0x800AE8B8 is `Pauser`, and it decides
                     * two things: which 34-byte configuration record
                     * FrontEndAddPausedTitle colours the pause banner from
                     * (0x800B32B8 + p*34 in TEAM DEATHMATCH, 0x800B32BA + p*34
                     * otherwise), and which entry of the shockable array the
                     * VIBRATION gate reads (0x8001C9B4). It has to be set
                     * before the open, because page 26's install hook is what
                     * composes the banner.
                     */
                    q2_menu_set_pauser(&c.menu, gp);
                    /*
                     * AND THE PAUSE PAGE IS A SILENT SCREEN.
                     *
                     * InGame_GOTO_PAUSED (0x8001FD88) is two calls and nothing
                     * else: `jal 0x80073C68` — killallsounds, which walks all
                     * twenty-four voice slots at 0x80073CA8..0x80073CC0
                     * (`slti v0, a0, 24`) clearing each one's key-on record and
                     * resetting the driver's channel word — and then
                     * `jal 0x8001D5A8`, which builds the page. GameCheckPaused
                     * reaches it at 0x8003E894 on the START press and calls
                     * killallsounds itself at 0x8003E860 on the pad-violation
                     * arm, so every effect still sounding is cut on the frame
                     * the page appears: a hyperblaster tail, a rocket's flight
                     * loop, a door, a creature's pain cry. Here they ran to
                     * their ends underneath the menu.
                     *
                     * The music bed is a separate global and is NOT touched by
                     * this — it ducks on its own ramp; see client_music_target.
                     */
                    client_voices_stop(&c);
                    q2_menu_open(&c.menu);
                }
            }
            if (ev.type == SDL_EVENT_QUIT) {
                c.running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                /*
                 * A film takes any key and stops. The console lets START or
                 * X out of one, and a port with a keyboard has no reason to
                 * be fussier than that — a movie you cannot skip is the
                 * complaint every FMV of this era earned.
                 */
                if (c.film_open) {
                    client_film_stop(&c);
                    continue;
                }
                /* A boot screen goes the same way, and see `client_boot_skip`
                 * for why that is the port's decision and not the disc's. */
                if (c.boot_open) {
                    client_boot_skip(&c);
                    continue;
                }
                /*
                 * THE BEAT BEFORE IT TAKES NOTHING. 0x80101CD0 never reads the
                 * pad, so the half second between the difficulty and the film
                 * cannot be shortened, skipped or interrupted — and swallowing
                 * the press is not cosmetic here: Escape would otherwise open
                 * the pause menu over the blank front end, and the beat would
                 * stall for good behind it, because it only counts down on the
                 * frames no page owns.
                 */
                if (c.start_beat > 0.0)
                    continue;
                /* Any key leaves the credits, back to the title. */
                if (c.credits_open) {
                    c.credits_open = false;
                    q2_menu_open(&c.menu);
                    q2_menu_goto(&c.menu, Q2_PAGE_FRONT_TITLE);
                    continue;
                }
                if (c.mp_scoreboard) continue;
                switch (ev.key.key) {
                case SDLK_ESCAPE:
                    /* START on the console: it opens the pause menu, and
                     * closes it again from the root page. Deeper in, the
                     * menu's own TRIANGLE handling owns going back — so Esc
                     * hands it one TRIANGLE press rather than doing nothing,
                     * which left Backspace and the right button as the only
                     * way up a level.
                     *
                     * THE TALLY BOARD EATS IT AND DOES NOTHING. It used to
                     * dismiss the board, which the console never does: the
                     * tally spin at 0x80018ED8 calls `PadEndInterMission` and
                     * nothing else, and that reads the FIRE binding
                     * (`MapPadInputToFire(0)`, 0x8001FC50) — not a menu
                     * confirm. So START neither leaves the board nor opens the
                     * pause menu behind it, and the `else if` below must not
                     * see the press either. */
                    if (c.save_prompt_open) {
                        /* Deliberately nothing: 0x800203F8 raises 0x800B348A
                         * and GameCheckPaused's START arm refuses to unpause
                         * while it is up (0x8003E6A0). YES or NO, or nothing. */
                    }
                    else if (c.mcard_open)
                        client_card_close(&c);
                    else if (c.mission_open) {
                        /* Deliberately nothing. */
                    }
                    else if (!c.menu.open) {
                        /*
                         * The pause page's KILLS/SECRETS row. The numbers are
                         * the ones the level tally already keeps; the line
                         * itself is composed by page 26's own entry hook, the
                         * way 0x8001D638 composes it as part of installing the
                         * page, so filling them in before opening is correct.
                         */
                        client_menu_fill_stats(&c);
                        /* Escape is player one's START, so player one paused. */
                        q2_menu_set_pauser(&c.menu, 0);
                        /* And killallsounds with it — InGame_GOTO_PAUSED
                         * (0x8001FD88), exactly as the gamepad twin above. */
                        client_voices_stop(&c);
                        q2_menu_open(&c.menu);
                    }
                    else if (c.menu.depth == 0)
                        client_menu_close(&c);
                    else
                        c.menu_esc_back = true;
                    break;
                case SDLK_F12:
                    /*
                     * The briefing. On the console it is shown between levels
                     * by the outer state machine; what triggers it is not
                     * established, so it gets a key rather than an invented
                     * trigger — the same call the memory-card screens got.
                     */
                    c.briefing_open = !c.briefing_open;
                    if (c.briefing_open) {
                        client_menu_close(&c);
                        c.mission_open = false;
                        q2_prompt_show(&c.prompts, Q2_PROMPT_BACK, 216);
                    } else {
                        q2_prompt_hide_all(&c.prompts);
                    }
                    break;
                case SDLK_F7:
                    /* The card front end, saving. On the console it is reached
                     * from SAVE?'s YES (page 39); what SHOWS that prompt is not
                     * established, so the port gives it a key rather than
                     * inventing a trigger. */
                    if (c.mcard_open)
                        client_card_close(&c);
                    else
                        client_card_open(&c, Q2_SAVE_UI_SAVE);
                    break;
                case SDLK_F8:
                    /* The same front end, loading. Same screens, same rules,
                     * the other direction. */
                    if (c.mcard_open)
                        client_card_close(&c);
                    else
                        client_card_open(&c, Q2_SAVE_UI_LOAD);
                    break;
                /* Not while a screen is up: quick load reloads the zone, and
                 * doing that under an open front end would pull the world out
                 * from under it. */
                case SDLK_F9:
                    if (!c.mcard_open && !c.menu.open)
                        client_quick_save(&c);
                    break;
                case SDLK_F10:
                    if (!c.mcard_open && !c.menu.open)
                        client_quick_load(&c);
                    break;
                case SDLK_F11:
                    /* The framebuffer, not the window — see above. */
                    client_screenshot(&c);
                    break;
                case SDLK_V: {
                    /*
                     * How the picture is shaped on the way out. The default is
                     * 4:3; `square` is the raw buffer, which is what every
                     * framebuffer dump of this game looks like and is a 1.5x
                     * horizontal stretch of what a television showed. Having
                     * both a key away is the point — the two are easy to argue
                     * about and trivial to compare.
                     */
                    int next = (int)c.fit + 1;
                    int pn = 1, pd = 1;

                    if (next >= Q2_SCREEN_FIT_COUNT)
                        next = 0;
                    c.fit = (q2_screen_fit)next;
                    q2_screen_pixel_aspect(&c.screen, &pn, &pd);
                    Q2_INFO("aspect: %s (console pixel %d:%d)",
                            q2_screen_fit_name(c.fit), pn, pd);
                    break;
                }
                case SDLK_P:
                    /*
                     * The view's own coordinates along the top of the frame.
                     * P because every other letter on the keyboard already
                     * means something to the pad bindings, and because this is
                     * the port's tool rather than the console's: nothing on
                     * this disc puts a position on screen.
                     */
                    c.show_coords = !c.show_coords;
                    Q2_INFO("coordinates: %s", c.show_coords ? "on" : "off");
                    break;
                case SDLK_F1: c.opts.dither    = !c.opts.dither;    break;
                case SDLK_F2: c.opts.affine_uv = !c.opts.affine_uv; break;
                case SDLK_F3:
                    /*
                     * Submerge, as a testing override. Authored trigger volumes
                     * resolve UNDERWATER themselves; this is still useful for
                     * inspecting the same downstream console behaviour away
                     * from a pool: swimming physics, water life support, and
                     * the screen effect that ramps up over about fourteen
                     * frames.
                     */
                    c.force_underwater = !c.force_underwater;
                    Q2_INFO("underwater: %s", c.force_underwater ? "on" : "off");
                    break;
                case SDLK_F4:
                    c.sim_enabled = !c.sim_enabled;
                    Q2_INFO("movement: %s", c.sim_enabled ? "simulated" : "free-fly");
                    break;
                case SDLK_F5: {
                    /* Every layout the session code can install. There is one
                     * simulated player, so the extra viewports show the same
                     * camera — what is on show here is the screen work, not a
                     * second player. */
                    int next = (int)c.screen.layout + 1;
                    if (next >= Q2_SCREEN_LAYOUT_COUNT)
                        next = 0;
                    q2_screen_set_layout(&c.screen, (q2_screen_layout)next,
                                         next == Q2_SCREEN_LAYOUT_QUAD ? 4 : 2);
                    Q2_INFO("layout: %s, %d viewport%s",
                            q2_screen_layout_name(c.screen.layout),
                            c.screen.view_count,
                            c.screen.view_count == 1 ? "" : "s");
                    break;
                }
                case SDLK_F6:
                    /* The glint. Off by default because only BIGGUN's level
                     * script raises the flag that draws it, and this port does
                     * not run relocated level modules — so showing one is a
                     * deliberate look at a reconstruction, not gameplay. */
                    c.show_glint = !c.show_glint;
                    Q2_INFO("glint: %s%s", c.show_glint ? "on" : "off",
                            c.sim[0].glint.ready ? "" : " (this map has no mesh)");
                    break;
                default:
                    if (!c.menu.open &&
                        ev.key.key >= SDLK_0 && ev.key.key <= SDLK_9) {
                        int z = (int)(ev.key.key - SDLK_0);
                        /*
                         * A DEBUG HOTKEY THAT LOOKS EXACTLY LIKE THE REPORTED
                         * FAULT. 0-9 loads that zone and respawns you in it,
                         * and nothing about the binding says so — so a stray
                         * number key during play is indistinguishable, from the
                         * player's chair, from a gate misfiring. Named in the
                         * log for that reason.
                         */
                        Q2_INFO("hotkey %d: load zone %d", z, z);
                        c.move_reason = "number-key zone hotkey";
                        client_load_zone(&c, c.map, z);
                    }
                    break;
                }
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                /*
                 * Grabbed, the mouse is the look axis and its POSITION means
                 * nothing — the pointer is pinned and only the deltas are real.
                 * Free, it is a pointer and the deltas mean nothing.
                 *
                 * The two signs are the port's and were settled against the
                 * picture rather than argued: a frame rendered at pitch 500
                 * looks at the ceiling, so up is POSITIVE pitch, and moving the
                 * mouse forward is a negative yrel. Yaw needs no correction.
                 */
                if (c.mouse_grabbed) {
                    c.look_acc_x += (double)ev.motion.xrel;
                    c.look_acc_y -= (double)ev.motion.yrel;
                } else {
                    c.pointer_x     = ev.motion.x;
                    c.pointer_y     = ev.motion.y;
                    c.pointer_valid = true;
                }
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                bool down = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);

                if (!c.mouse_grabbed) {
                    c.pointer_x     = ev.button.x;
                    c.pointer_y     = ev.button.y;
                    c.pointer_valid = true;
                }

                if (ev.button.button == SDL_BUTTON_LEFT)
                    c.mouse_left = down;
                else if (ev.button.button == SDL_BUTTON_RIGHT)
                    c.mouse_right = down;

                /*
                 * The two screens that take ANY key also take any click: a
                 * film and the credit roll. Answering them with the keyboard
                 * only would be the one place the mouse stopped working.
                 *
                 * THE TALLY BOARD IS NOT ONE OF THEM ANY MORE. A right-click
                 * used to dismiss it as "the back gesture"; the console's board
                 * has one way off it and it is the FIRE edge after the 1200-tick
                 * gate (`PadEndInterMission`, mission.h). TRIANGLE — which is
                 * what the right button is here — does nothing to it, and a
                 * gesture that skipped the gate defeated the whole lock. Return,
                 * Enter and Space are CROSS, so the keyboard still has a way
                 * out; it just has to wait four seconds like a pad does.
                 */
                if (down && c.film_open) {
                    client_film_stop(&c);
                } else if (down && c.boot_open) {
                    client_boot_skip(&c);
                } else if (down && c.credits_open) {
                    c.credits_open = false;
                    q2_menu_open(&c.menu);
                    q2_menu_goto(&c.menu, Q2_PAGE_FRONT_TITLE);
                }
            } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                /*
                 * Queued rather than applied — see client_wheel_notch. Bounded
                 * so a violent spin cannot take a second to drain: past a few
                 * notches the player has stopped meaning individual weapons.
                 */
                c.wheel_queue += (int)ev.wheel.y;
                if (c.wheel_queue >  8) c.wheel_queue =  8;
                if (c.wheel_queue < -8) c.wheel_queue = -8;
            }
        }

        /*
         * Who owns the pointer this frame, and what the CONTROLLER page has
         * been set to. Both before the input dispatch below, so a menu closed
         * on the previous frame has the mouse back on this one.
         */
        client_apply_input(&c);
        client_update_grab(&c);

        if (q2_loading_step(&c.loading, (double)dt)) {
            /*
             * THE LOADING SCREEN IS IN FRONT OF EVEN THAT.
             *
             * Nothing under it runs while it is up, and that is not only
             * tidiness: the level it is standing over has just been replaced,
             * so a tick here would be the first tick of a level the player has
             * not seen yet, and a press would be taken by a page they cannot
             * see. The console has the same property for a blunter reason —
             * its screen is up during a synchronous read, and nothing runs
             * during one of those at all.
             *
             * `q2_loading_step` turns the logo and spends the hold; it goes
             * false on the frame the half second is up, and that frame is the
             * first one the world gets back.
             */
        } else if (c.boot_open) {
            /*
             * A BOOT SCREEN IS IN FRONT OF EVERYTHING AND HAS NOTHING BEHIND
             * IT. No map is loaded while one is up — the console loads a
             * directory because that is the only way its engine gets to a
             * screen, and what the screen then shows is two rectangles — so
             * there is no world to tick and no page to take the pad.
             */
            client_boot_tick(&c, dt);
        } else if (c.mcard_open) {
            /*
             * The card front end sits in front of everything: it is a separate
             * engine with its own state and its own release rule, not a page,
             * so it takes the pad first and nothing ticks underneath it.
             */
            client_card_frame(&c);
        } else if (c.save_prompt_open) {
            /*
             * THE SAVE? PROMPT, and nothing under it moves. `Paused` is up
             * (0x8003E754) and MainGameLoop zeroes the frame delta while it is
             * (0x800183C4), so the new level's first frame is held exactly
             * where the load left it until YES or NO is chosen.
             */
            client_save_prompt_frame(&c);
        } else if (c.menu.open) {
            /* The world is frozen while the menu is up: no input, no tick.
             *
             * Except on the TITLE SCREEN, which is not a paused world — it is
             * QFRONT running with page 46 over it, and its module keeps a
             * per-frame hook (levelbin.h). Freezing it froze the logo, which is
             * why the scene appeared as a still the moment it was spawned.
             * Only the entity set steps; see q2_sim_scene_advance for why that
             * is the whole of this level rather than a shortcut through it. */
            client_menu_frame(&c);
            if (c.in_front_end) {
                q2_sim_scene_advance(&c.sim[0], (double)dt);
                client_scene_lights(&c);
            } else if (c.menu.page_id == Q2_PAGE_DEATH) {
                /*
                 * AND NEITHER IS THE DEATH PAGE A PAUSED WORLD.
                 *
                 * Page 41 is drawn over a level that is still running: the body
                 * falls, the camera rolls into the death cam, and the creatures
                 * that killed you carry on. Freezing it made death a still
                 * frame the instant the page opened — the roll never started,
                 * because nothing advanced to roll it.
                 *
                 * The pad still belongs to the menu, so the input the sim gets
                 * is the neutral one; the world half of the tick is what has to
                 * keep running.
                 */
                client_input_simulated(&c, dt);
            }
        } else if (c.start_beat > 0.0) {
            /*
             * THE BEAT BETWEEN A DIFFICULTY AND THE REEL, and it is not a
             * frozen frame.
             *
             * The console never leaves the level to run it: 0x80101E4C hides
             * the five title objects and swaps the page hook for the countdown,
             * and QFRONT goes on running underneath. So the scene still steps
             * and the lights still follow it — the same half of the tick the
             * title screen gets, minus the menu that has just closed.
             *
             * The pad is nobody's for these fifteen frames, because 0x80101CD0
             * does not read it.
             */
            q2_sim_scene_advance(&c.sim[0], (double)dt);
            client_scene_lights(&c);
            client_start_beat(&c, dt);
        } else if (c.mission_open || c.briefing_open || c.endmis_open) {
            /*
             * AND THE INTERMISSION BOARDS FREEZE IT TOO. This was the bug that
             * killed the player at every level exit.
             *
             * The level-end tally is not a screen drawn over a running world:
             * 0x80018ED8 SPIN-LOOPS on it —
             *
             *     80018F08  jal 0x80018868      one iteration of the tally
             *     80018F10  beq v0, zero, 0x80018F08
             *
             * — and the in-game logic at 0x800190AC is AFTER that loop, not
             * inside it. 0x80018868 draws and swaps and nothing else, and at
             * 0x80018928 it ZEROES the frame-delta accumulator the in-game
             * frame reads, so no time passes at all while the board is up.
             *
             * The port left the sim running behind it. The creature that was
             * shooting you when you reached the exit kept shooting, the death
             * check kept running, and the HUD was suppressed so the health
             * draining was not even visible. Measured windowed on Base2: the
             * board comes up at 100 hp and the player is dead by frame 4600,
             * still standing at the door, with the level change never made.
             */
            client_intermission_frame(&c, dt);
        } else if (c.popup.visible) {
            /*
             * THE OBJECTIVES POP-UP HOLDS THE WORLD, the same way the menu
             * does and for the same reason: 0x800213B0 raises the engine's
             * "a menu owns the frame" flag at 0x800AE8B4, and 0x800190C8
             * branches the whole in-game logic block around it.
             *
             * Only the pop-up's own tick runs, so its deadline still counts
             * down and CROSS can still dismiss it.
             */
            client_popup_frame(&c, dt);
        } else if (c.mp_scoreboard) {
            if (client_mp_results_input(&c, dt)) {
                Q2_INFO("multiplayer: all players ready; returning to setup");
                client_enter_front_end(&c);
                q2_menu_goto(&c.menu, Q2_PAGE_FRONT_DMSETUP);
            }
        } else if (c.sim_enabled) {
            client_input_simulated(&c, dt);
        } else {
            client_input(&c, dt);
        }

        /* The button edges the menu's click handling is built on, taken after
         * everything that reads them. */
        /* A frame the world did not run is a Paused frame to the pads. */
        if (!c.shock_live)
            client_shock_hold(&c);
        c.shock_live = false;

        c.mouse_left_prev  = c.mouse_left;
        c.mouse_right_prev = c.mouse_right;
        c.menu_esc_back    = false;   /* unspent if no menu frame ran */
        {
            int pi;
            for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++)
                c.gamepads.pressed[pi] = 0;
        }

        /*
         * Sampled here — after the tick that moves the player and before the
         * transitions that relocate them — so one frame's walking and one
         * frame's teleporting are never averaged into the same displacement.
         */
        client_zone_watch(&c);

        /* The death screen was never answered — 0x800B2A10 ran out and the
         * console asked for game state 8, which loads QFRONT. Here rather than
         * in the frame for the same reason the zone gate is. */
        if (c.death_abandoned) {
            c.death_abandoned = false;
            c.death_abandon   = 0;
            client_menu_close(&c);
            client_enter_front_end(&c);
        }

        if (!client_mp_restart_round(&c)) {
            Q2_ERROR("multiplayer: failed to reload the next round");
            c.running = false;
        }

        /* A zone gate fired somewhere in the script: another zone of the same
         * map. Deferred to here because the gate fires inside the tick, and
         * the load frees the triggers the runtime is standing in. */
        {
            u32 target;
            if (q2_sim_take_zone_change(&c.sim[0], &target)) {
                /*
                 * A GATE TO THE ZONE WE ARE ALREADY IN IS NOT A GATE — and
                 * this is no longer where that is decided. 0x80079178 refuses
                 * a same-zone gate BEFORE it stores the request (0x800791E0 /
                 * 0x800791E8), and events_rt.c now makes that test at that
                 * point, against `event_rt.current_zone`, which
                 * client_load_zone sets after every attach. So a same-zone gate
                 * never reaches here: it does not abort its record either, and
                 * the door behind it opens.
                 *
                 * Kept as a belt-and-braces guard, because the one thing it
                 * prevents is a volume in the middle of zone 0 that names
                 * "Zone0" reloading the zone every time the player walks
                 * through it — which reads as the level restarting under you —
                 * and that must stay impossible even if the runtime is ever
                 * attached without the resident zone being set.
                 */
                if ((int)target == c.zone_index) {
                    Q2_DEBUG("zone gate names the zone we are in (%u)", target);
                    if (c.zone_trace)
                        Q2_INFO("[zone] f%-6u gate to the resident zone %u"
                                " ('%s') IGNORED", c.trace_frame, target,
                                c.sim[0].event_rt.pending_zone_name);
                } else {
                    Q2_INFO("zone gate -> zone %u ('%s')", target,
                            c.sim[0].event_rt.pending_zone_name);
                    if (c.zone_trace) {
                        const q2_player *pl =
                            &c.sim[0].player[c.sim[0].cur_player];
                        Q2_WARN("[zone] f%-6u GATE FIRED  zone %d -> %u"
                                " ('%s')  standing at (%d,%d,%d) cell %d",
                                c.trace_frame, c.zone_index, target,
                                c.sim[0].event_rt.pending_zone_name,
                                pl->pos[0], pl->pos[1], pl->pos[2],
                                (int)c.sim[0].current_node);
                    }
                    c.move_reason    = "zone gate";
                    c.carry_player   = true;
                    c.carry_same_map = true;
                    /* Carried across the load: the gate's name is what names
                     * the entry point on the far side. */
                    snprintf(c.gate_name, sizeof(c.gate_name), "%s",
                             c.sim[0].event_rt.pending_zone_name);
                    client_load_zone(&c, c.map, (int)target);
                }
            }
        }

        if (!client_apply_teleports(&c)) {
            Q2_ERROR("failed to load the teleport destination");
            c.running = false;
        }

        /* The mission row is the LEVEL's and is written as its two counters
         * move, not when it ends — see `client_mission_update`. */
        client_mission_update(&c);

        /*
         * And a LOADMAP: the same deferral, one level up.
         *
         * **NO TALLY BOARD HERE, and that is the correction.** This port used
         * to raise the MISSION screen at every level boundary on the reading
         * that it is "the one the game shows when a level ends". It is not,
         * and the call graph has exactly one edge at each step: the board's
         * draw at `0x80021ADC` is reached only from `0x80018944`, which is
         * only in the tally frame `0x80018868`, which is only spun by
         * `0x80018ED8`, which is only reached by game state 7, which is only
         * written by `0x8002DCB4` — MISCOMPLETE's exec. A LOADMAP writes 2 at
         * `0x8002DD80` and the outer state machine just loads the map.
         *
         * So a level change is the load and nothing else, and the six-row
         * board with "Mission %d - Complete" over it is what a UNIT ends on.
         */
        if (c.map_change_pending && !c.unit_over) {
            c.map_change_pending = false;
            client_change_map_and_brief(&c);
        }

        /*
         * A MISCOMPLETE, which is the one that does hold a screen. The tally
         * board goes up with the destination waiting behind it, exactly as
         * `0x80018ED8` shows the board before it writes `EndMission N`.
         */
        if (c.map_change_pending && c.unit_over) {
            c.map_change_pending = false;

            client_mission_update(&c);
            /*
             * SetInterMission, 0x800224B8, in the order it does it: the camera
             * hook, then `GetMissionName`, then the board's HEIGHT — its last
             * act is `sh v0, 358(gp)` at 0x800224E8, from
             * `PreProcessInterMission`. The rectangle behind the text hugs the
             * table, growing ten pixels for every level the unit listed, so it
             * has to be computed after the rows and the wrapped objective are
             * final and not left at the .data seed.
             */
            c.mission.box.h = q2_mission_box_height(&c.mission);
            c.mission_open      = true;
            c.mission_after_map = true;
            c.mission_frames    = 0;
            /* The arrival briefing belongs to the level that is ending, and
             * its own release calls `q2_prompt_hide_all` — which would take
             * the board's prompt down with it if the player reached the exit
             * while it was still up. */
            c.briefing_open     = false;
            client_menu_close(&c);
            {
                int rs = 0, rst = 0, rk = 0, rkt = 0, rw, rows = 0;

                q2_mission_totals(&c.mission, &rs, &rst, &rk, &rkt);
                for (rw = 0; rw < Q2_MISSION_ROWS; rw++)
                    if (c.mission.row[rw].name[0])
                        rows++;
                /* The rectangle is logged with the totals because it is DERIVED
                 * from them — `PreProcessInterMission` counts the same rows to
                 * size the panel, so a board whose height does not match its
                 * row count is the first sign the two have drifted. */
                Q2_INFO("tally: mission %d complete — %d level%s, "
                        "secrets %d/%d, kills %d/%d, box %d,%d %dx%d",
                        c.mission.unit, rows, rows == 1 ? "" : "s",
                        rs, rst, rk, rkt,
                        (int)c.mission.box.x, (int)c.mission.box.y,
                        (int)c.mission.box.w, (int)c.mission.box.h);
            }
            /*
             * AND SAY THAT IT CAN BE DISMISSED. The console's tally spins on a
             * pad press with no timeout at all (0x80018214 latches the edge
             * inside the loop) and draws a prompt to say so. This port had the
             * press wired and no prompt, so the only way out of the board was
             * to guess — which is the whole reason a 10-second timeout was
             * invented for it. With the prompt up the timeout is headless-only,
             * where there is nobody to press anything.
             */
            q2_prompt_show(&c.prompts, Q2_PROMPT_SELECT, 216);
        }

        /*
         * A transition puts up two screens and both wait for the press their
         * prompt asks for, as the console's do. Headless has nobody to press
         * anything, so it holds each for a fixed count and goes on — otherwise
         * every scripted run would stop at the first boundary, which is
         * precisely where the interesting part starts.
         *
         * `briefing_open` is NOT one of them any more: it is the debug key's
         * screen now, and the panel a level actually shows is the pop-up the
         * script raises, on its own fifteen-second deadline.
         */
        /*
         * The end-of-mission placard.
         *
         * NOT WHILE THE LOADING SCREEN IS OVER IT. A unit end raises the
         * placard as part of the QENDMIS load, so for the half second the
         * screen is up the placard exists and is not on the display — and a
         * headless run's release is a frame count, so without this it spends a
         * third of its wait behind something the player cannot see. Nothing
         * under the loading screen runs; this is one of the two counters that
         * were not under it.
         */
        if (c.endmis_open && !c.loading.open) {
            c.endmis_frames++;
            if (c.headless && c.endmis_frames >= Q2_INTERMISSION_HEADLESS) {
                c.endmis_open = false;
                q2_prompt_hide_all(&c.prompts);
            }
        } else {
            c.endmis_frames = 0;
        }

        /*
         * The placard dismissed — and on to the level the unit's last LOADMAP
         * named. See `unit_next_map` for why the port has to carry it across
         * and the console does not.
         */
        if (c.endmis_await && !c.endmis_open) {
            c.endmis_await = false;
            if (c.unit_next_map[0]) {
                char next[Q2_UF_NAME_LEN + 1], start[Q2_UF_NAME_LEN + 1];

                snprintf(next, sizeof(next), "%s", c.unit_next_map);
                snprintf(start, sizeof(start), "%s", c.unit_next_start);
                c.unit_next_map[0]   = '\0';
                c.unit_next_start[0] = '\0';
                c.endmission         = false;
                snprintf(c.pending_map, sizeof(c.pending_map), "%s", next);
                snprintf(c.pending_start, sizeof(c.pending_start), "%s", start);
                Q2_INFO("end of mission over -> %s '%s'", next, start);
                client_change_map_and_brief(&c);
            }
        }

        if (c.mission_after_map && !c.loading.open) {
            c.mission_frames++;
            /*
             * HEADLESS ONLY, now that the board says how to leave it.
             *
             * The console's tally has NO timeout: 0x80018214 spins until a pad
             * edge arrives, and it draws a prompt so the player knows to give
             * it one. The port had the press wired and drew no prompt, so a
             * windowed player faced a board with no visible way out — which is
             * why a 10-second release was invented for it. The prompt is raised
             * with the board now, so the invented release goes and the press is
             * the only way out, exactly as on the console.
             *
             * A headless run still needs one: there is nobody to press
             * anything, and without it every scripted run would stop at the
             * first level boundary, which is precisely where the interesting
             * part starts.
             */
            /*
             * AND IT STILL CANNOT LEAVE EARLIER THAN THE CONSOLE DOES. The gate
             * at gp+362 refuses the pad for 1200 ticks and there is no second
             * way off the board, so a headless stand-in that pressed at frame
             * 45 would be dismissing a screen the console would not let anyone
             * dismiss. It waits for both now: 1200 ticks at the headless dt of
             * 1/30 s is 120 frames, so the count is the one that binds.
             */
            if (c.headless && c.mission_frames >= Q2_INTERMISSION_HEADLESS &&
                c.mission_gate <= 0) {
                c.mission_open  = false;
                c.mission_gate  = Q2_MISSION_GATE_TICKS;   /* 0x80022538 */
                q2_prompt_hide_all(&c.prompts);
            }

            if (!c.mission_open) {
                c.mission_after_map = false;
                c.unit_over         = false;
                Q2_INFO("tally: dismissed after %u frames", c.mission_frames);
                /*
                 * THE UNIT'S SIX ROWS GO, and the port used to keep them.
                 *
                 * `ClearLevelIntermission` (0x80021A0C) is `GameInterMission`'s
                 * last act — `jal` at 0x8001902C, after the spin and after the
                 * destination has been written, which is exactly this point. It
                 * opens the "PlayerSave0" block and memsets 150 bytes at
                 * block+228, and that is where `GamePlayerLoadSave` keeps the
                 * six 25-byte records (0x8003DAB4 writes them, 0x8003DCC8 reads
                 * them back). The live array survives the call and does not
                 * need to: `InitialiseQuakePlayer` reloads it from the block at
                 * every level entry (0x8003B4B0), so the next unit starts on an
                 * empty table.
                 *
                 * Without this the six slots stayed full from unit 2 onward,
                 * `q2_mission_register` returned -1 for every level after the
                 * sixth, `mission_row` stayed -1 and `client_mission_update`
                 * returned early — so Secrets and Kills stopped being recorded
                 * for the rest of the campaign. mission.h used to document that
                 * as the console's behaviour; it is not, and the note is
                 * corrected there too.
                 *
                 * NOT the unit number: 0x800B2E20 is outside the 150 bytes.
                 */
                q2_mission_clear_rows(&c.mission);
                c.mission_row = -1;
                /* The block is what the console clears, so the snapshot's copy
                 * goes with it — a save written from a stale capture would
                 * otherwise restore the unit that has just ended. */
                q2_save_capture_mission(&c.snapshot, &c.mission);
                /*
                 * The destination the board was holding: `EndMission N`, or
                 * `Extro FMV` on the last unit. `0x80018ED8` writes it after
                 * the spin, which is exactly here.
                 */
                client_change_map_and_brief(&c);
            }
        }

        /*
         * `--fire-triggers`: queue every trigger volume on this map once. The
         * runtime runs the queue on its own next update, so this goes through
         * exactly the path a player standing in the volume goes through — the
         * only difference is who asked.
         */
        /*
         * `sim_ready[]` is the MULTIPLAYER spawn's array and player 0 never
         * enters it, so this used to test `c.sim_ready` — an array, always
         * true — and worked by accident. Testing `[0]` was a correct-looking
         * fix that silently disabled the whole flag. What this actually needs
         * is the level's triggers to be loaded, which is the thing the loop
         * below walks.
         */
        /* `--fire-event NAME`: one named record, once, and say whether the
         * map has it — a name that resolves nowhere is a typo and should not
         * look like a feature that did nothing. */
        if (c.fire_event && !c.fire_event_done && c.sim[0].event_rt.record_count &&
            (long)c.frame_index >= c.fire_at_frame) {
            c.fire_event_done = true;
            if (q2_event_rt_trigger_named(&c.sim[0].event_rt, c.fire_event))
                Q2_INFO("--fire-event: '%s' queued", c.fire_event);
            else
                Q2_WARN("--fire-event: this map names no '%s'", c.fire_event);
        }

        if (c.fire_triggers && c.sim[0].triggers.count &&
            (long)c.frame_index >= c.fire_at_frame) {
            u32 trigger_index, fired = 0;

            c.fire_triggers = false;
            for (trigger_index = 0;
                 trigger_index < c.sim[0].triggers.count;
                 trigger_index++) {
                q2_trigger tr;

                if (!q2_trigger_get(&c.sim[0].triggers, trigger_index, &tr))
                    continue;
                if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                    continue;
                if (q2_event_rt_trigger(&c.sim[0].event_rt, tr.event_offset))
                    fired++;
            }
            Q2_INFO("--fire-triggers: queued %u of %u trigger volumes",
                    fired, c.sim[0].triggers.count);
        }
        /* Every frame, not just menu frames: a zone load rebuilds the sim and
         * would otherwise drop back to the compiled-in constants. */
        client_apply_settings(&c);
        /* `--keys`: re-asserted every frame, because a zone load rebuilds the
         * inventory the same way it rebuilds the settings. */
        if (c.all_keys)
            c.sim[0].combat.inv.flags |= 0x0FFFu;
        /* `--objectives N`: the raise the pause menu's MISSION row performs. */
        if (c.popup_at_frame > 0 && (s32)c.frame_index == c.popup_at_frame) {
            q2_briefing_popup_raise(&c.popup, Q2_BRIEFING_MENU_DELAY,
                                    Q2_BRIEFING_SECONDS,
                                    c.sim[0].level_time, c.sim[0].cur_dt);
            c.popup_raises++;
            Q2_INFO("--objectives: raised at frame %d", (int)c.frame_index);
        }
        /* `--armour`: likewise re-asserted, and likewise only for observability.
         * The points are re-topped rather than latched so a run that takes
         * damage still exercises the armour arm to the end. */
        if (c.give_armour_flag) {
            q2_inventory *inv = &c.sim[0].combat.inv;

            inv->flags |= c.give_armour_flag;
            if (inv->armour < c.give_armour_points)
                inv->armour = c.give_armour_points;
            if (inv->ammo[Q2_AMMO_CELLS] < c.give_armour_cells)
                inv->ammo[Q2_AMMO_CELLS] = c.give_armour_cells;
        }
        /* `--powerup`: retained solely as a reproducible renderer probe. The
         * live pickup handlers still own the normal extend-from-later rule;
         * this reassertion merely keeps one HUD timer present across map loads
         * and long headless runs. */
        if (c.give_powerup >= 0 && c.give_powerup < Q2_POWERUP_SLOT_COUNT) {
            q2_inventory *inv = &c.sim[0].combat.inv;
            s32 until = c.sim[0].level_time + Q2_ITEM_POWERUP_TICKS;

            switch (c.give_powerup) {
            case Q2_POWERUP_QUAD:     inv->quad_until = until; break;
            case Q2_POWERUP_INVULN:   inv->invuln_until = until; break;
            case Q2_POWERUP_ENVIRO:   inv->enviro_until = until; break;
            case Q2_POWERUP_BREATHER: inv->breather_until = until; break;
            default: break;
            }
        }
        /* `--weapon N`: same treatment. The ammo is topped up every frame
         * rather than given once, so a long run does not quietly turn into a
         * test of the dry-trigger path halfway through. */
        if (c.give_weapon > 0 && c.give_weapon <= Q2_WID_COUNT) {
            q2_inventory *inv = &c.sim[0].combat.inv;
            const q2_weapon_tables *wt = q2_weapon_tables_builtin();
            int a;

            for (a = 1; a <= Q2_WID_COUNT; a++)
                inv->weapons |= wt->owned_bit[a];
            for (a = 0; a < Q2_AMMO_COUNT; a++)
                inv->ammo[a] = q2_inventory_ammo_max(inv, (q2_ammo)a);

            if (c.sim[0].combat.weapon_id != c.give_weapon) {
                c.sim[0].combat.weapon_id = c.give_weapon;
                if (c.vm_ready) {
                    q2_vw_select(&c.vw[0], c.give_weapon);
                    client_bind_view_model(&c, 0);
                }
            }
        }
        /*
         * CdCheckMusic's volume ramp, and where it sits is half the point.
         *
         * MainGameLoop calls it at 0x80018374 — the FIRST thing in the
         * iteration, before the GameIsPaused test at 0x800183C4 — so the CD
         * volume keeps moving on the frames the pause page owns and the world
         * does not. This block sits at frame-loop level, OUTSIDE the
         * menu / boot / film chain, for exactly that reason: a step inside that
         * chain would stop the moment a page took the frame, and a duck that
         * only runs while the game is not paused is no duck at all.
         *
         * Per console TICK rather than per rendered frame, because one
         * MainGameLoop iteration is one video field. A 144 Hz host would
         * otherwise duck the music three times faster than a console does.
         */
        c.music_ramp_clock += dt * (double)q2_build_tick_rate(&c.build);
        while (c.music_ramp_clock >= 1.0) {
            c.music_ramp_clock -= 1.0;
            c.music_target = client_music_target(&c);
            client_music_ramp(&c);
        }

        /*
         * The music countdown, on the console's own field rate — 50 Hz on PAL,
         * 60 on NTSC. At zero the engine moves to the next playlist entry
         * (0x80071A58) rather than waiting for the stream to run out, which is
         * what makes a duration a restart point and not a length.
         */
        if (c.music_open && c.music_total > 0) {
            c.music_clock += dt * (double)q2_build_tick_rate(&c.build);
            while (c.music_clock >= 1.0) {
                c.music_clock -= 1.0;
                if (c.music_left > 0)
                    c.music_left--;
            }
            if (c.music_left <= 0)
                client_music_advance(&c);
        }

        /* `--save-load N`, and the report is the point: what the client owned
         * before the save and what it owns after the load. */
        if (c.save_load_at > 0 && (long)c.frame_index == c.save_load_at) {
            u32 dead = 0, open_doors = 0, broken = 0, i2;

            for (i2 = 0; c.creatures_ready && i2 < c.creatures.set.count; i2++)
                if (c.creatures.set.monsters[i2].dead) dead++;
            for (i2 = 0; c.movers_ready && i2 < c.movers.count; i2++)
                if (c.movers.movers[i2].offset != 0) open_doors++;
            for (i2 = 0; i2 < c.sim[0].breakable_count; i2++)
                if (c.sim[0].breakable[i2].broken) broken++;
            Q2_INFO("save-load: BEFORE %u dead, %u doors moved, %u panes broken",
                    dead, open_doors, broken);
            client_quick_save(&c);
        }
        if (c.save_load_at > 0 && (long)c.frame_index == c.save_load_at + 1) {
            u32 dead = 0, open_doors = 0, broken = 0, i2;

            client_quick_load(&c);
            for (i2 = 0; c.creatures_ready && i2 < c.creatures.set.count; i2++)
                if (c.creatures.set.monsters[i2].dead) dead++;
            for (i2 = 0; c.movers_ready && i2 < c.movers.count; i2++)
                if (c.movers.movers[i2].offset != 0) open_doors++;
            for (i2 = 0; i2 < c.sim[0].breakable_count; i2++)
                if (c.sim[0].breakable[i2].broken) broken++;
            Q2_INFO("save-load: AFTER  %u dead, %u doors moved, %u panes broken",
                    dead, open_doors, broken);
        }

        /*
         * The film, on its own 25 fps clock rather than the game's 30 Hz tick.
         * That rate is forced by the container (movie.h) and driving it off the
         * tick would drop or double every fifth frame.
         *
         * When it ends, the placard the port would otherwise have shown takes
         * over, so the campaign finishes on something rather than on black.
         */
        if (c.film_open)
            client_film_tick(&c, dt);

        /*
         * A film that has ended hands over to whatever it was in front of.
         *
         * Three cases and they are not the same thing. THE OPENING REEL hands
         * the game over, which is a load of the intro FMV and then of the first
         * map — it does not go back to the title, because on the console the
         * title screen is already gone by the time it starts. A cinematic on the
         * WAY somewhere — the intro itself, and the extro if a later disc ever
         * carries one — resumes the journey. And an end-of-mission film with
         * nowhere to go leaves the placard up, which is what the campaign's last
         * frame has been.
         *
         * OUTSIDE the tick, on `film_done`, because a film does not only end by
         * running out: a press stops it, and that press is taken in the event
         * loop where there is no tick to notice. Handling this inside the tick
         * meant a skipped film left the front end with no title screen and no
         * menu — a black QFRONT that took a restart to leave.
         *
         * The reel is tested FIRST. It is the one case that sets no
         * `film_next_map` — the destination is not a map name, it is the whole
         * of `client_start_game` — and testing it first is what keeps a stale
         * name from ever answering for it.
         */
        if (!c.film_open && c.film_done) {
            c.film_done = false;

            if (c.film_to_front) {
                /*
                 * THE INTRO, WHICH IS A PRE-MENU CINEMATIC. QLOGOS asked for it
                 * by writing state 12 before the front end had ever been
                 * loaded, so what follows it is the front end's first
                 * appearance — the dispatcher's fall-through at `0x80018B54`,
                 * which loads `QFront` because no request flag is left
                 * standing.
                 */
                Q2_INFO("movie: intro over — the front end");
                client_enter_front_end(&c);
            } else if (c.film_is_start) {
                /*
                 * The reel is what the front end plays before it lets go, so
                 * what follows it is the game starting and not the title screen
                 * coming back — which is the whole difference between this and
                 * the attract loop it used to be mistaken for.
                 */
                Q2_INFO("movie: opening reel over — starting the game");
                client_start_game(&c);
            } else if (c.film_next_map[0]) {
                char next[64];

                snprintf(next, sizeof(next), "%s", c.film_next_map);
                c.film_next_map[0] = '\0';
                c.endmission       = false;
                c.endmis_open      = false;
                Q2_INFO("movie: over — on to %s", next);
                if (!client_load_zone(&c, next, 0))
                    Q2_WARN("movie: cannot load %s after the film", next);
            } else if (c.endmission && !c.endmis_open) {
                char line[Q2_BRIEFING_FIELD_MAX];

                snprintf(line, sizeof(line), "MISSION %d COMPLETE",
                         c.endmis_unit);
                q2_endmission_set(&c.endmis, line,
                                  "The campaign is over.");
                c.endmis_open = true;
                q2_prompt_show(&c.prompts, Q2_PROMPT_BACK, 216);
            } else {
                /*
                 * A FILM THAT ENDS WITH NOWHERE TO GO GOES TO THE FRONT END,
                 * because the alternative is a black screen with no way out.
                 *
                 * The Extro is the case that reaches here: the MISCOMPLETE arm
                 * sets `film_screen` to "Extro FMV" and never sets
                 * `film_next_map`, so when OUTRO1P runs out none of the three
                 * branches above applies and the session simply stops on the
                 * last frame. A player who finishes the campaign is left
                 * looking at nothing.
                 *
                 * The console does not stop either: QFMV's extro handler
                 * 0x80103DF8 waits four ticks and then writes 17 into the outer
                 * state word (0x800B2E28), and the dispatcher at 0x800187F8
                 * turns state 17 into a one-line setter and then state 6, which
                 * RE-ENTERS the main loop. The intro's 0x80103DBC does the same
                 * with state 6 directly. Handing the screen back is the shape of
                 * both; which page it lands on is this port's choice and the
                 * title is the only one that is always there.
                 */
                Q2_INFO("movie: over with no destination — back to the front end");
                client_enter_front_end(&c);
            }
        }

        /*
         * The one place audio reaches the device. After the film tick, because
         * the film is one of the two beds it can draw from and a film that ends
         * this frame should not have a sector of it pulled afterwards.
         */
        /* Where the listener is now, before the mix that uses it. */
        client_voices_update(&c);
        client_audio_pump(&c);

        /*
         * The screen's own clock, in the 1/300 s units everything the console
         * times is expressed in, clamped at 30 the way 0x800184B8 clamps it.
         *
         * It matters now that something reads it: the water effect ramps by
         * 24 per unit, so without this a 144 Hz host would fade the effect in
         * three times faster than the console does. Driving it from the real
         * elapsed time is what keeps a frame-rate-independent port timing the
         * effect the way the hardware timed it.
         */
        q2_screen_tick_dt(&c.screen, (double)dt);
        client_frame(&c);
        /* This level has now been seen, so a script call from here on is the
         * player having walked somewhere rather than the level arriving. */
        c.level_frames_drawn = true;

        c.frame_index++;
        if (c.frames_total > 0 && c.frame_index >= c.frames_total)
            c.running = false;
    }

    /*
     * The last frame, always — a run with no `--shot-every` asks for one
     * picture and gets exactly that, at the name it gave, with no number in it.
     */
    if ((c.shot_path || c.shot_display_path) &&
        (c.shot_every <= 0 || c.shots_written == 0)) {
        c.frame_index--;
        client_write_shot(&c, false);
    }

    if (c.report)
        client_report(&c);

done:
    client_boot_free(&c);
    q2_loading_close(&c.loading);
    if (c.hud_tables_ready)
        q2_hud_tables_free(&c.hud_tables);
    if (c.sfx_ready)
        q2_sound_bank_free(&c.sfx);
    if (c.icons_ready)
        q2_icon_tables_free(&c.icons);
    if (c.level_table_ready)
        q2_level_table_free(&c.level_table);
    q2_save_ui_free(&c.save_ui);
    q2_save_free(&c.snapshot);
    q2_event_store_free(&c.event_store);  /* the EVE_ blocks, one per map */
    q2_gib_attach(NULL);            /* before the set it names goes */
    q2_shock_attach(NULL, NULL);
    q2_sim_free(&c.sim[0]);
    q2_common_close(&c.common);
    q2_world_free_zone(&c.zone);
    free(c.vram);
    q2_vm_tables_free(&c.vm_tables);
    q2_screen_free(&c.screen);
    psx_ot_free(&c.ot);
    if (c.audio)    SDL_DestroyAudioStream(c.audio);
    if (c.texture)  SDL_DestroyTexture(c.texture);
    if (c.renderer) SDL_DestroyRenderer(c.renderer);
    if (c.window)   SDL_DestroyWindow(c.window);
    q2_gamepads_close(&c.gamepads);
    SDL_Quit();
    disc_close(c.disc);
    return exit_code;
}
