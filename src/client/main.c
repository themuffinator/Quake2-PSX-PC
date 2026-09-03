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
#include "item.h"
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
#include "sim.h"
#include "statusbar.h"
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
 * A sound's own level before the slider, and the distance at which it reaches
 * zero. `level = base * (reach - dist) / 4096`, so a source at the listener is
 * three times its base and one at the reach is silent.
 */
#define CLIENT_SFX_BASE   63
#define CLIENT_SFX_REACH  12288
/* What a listener-local sound gets instead — the menu, the player's own weapon
 * and footsteps, the HUD. Never attenuated, never panned. */
#define CLIENT_SFX_LOCAL  254

typedef struct client_voice {
    bool         active;
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
} client_player_anim;

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
     * The shot this client has already been told about, against the sim's own
     * counter — see the reads in `client_input_simulated`.
     *
     * TWO cursors against ONE counter, because each read CONSUMES: the first
     * one to notice a new serial moves the cursor past it, and a shared cursor
     * would therefore let whichever ran first swallow the shot and leave the
     * other with nothing. The view model runs first, so sharing would give a
     * firing weapon a fire clip and no sound.
     */
    u32              shot_serial_heard;   /* the sound has been played  */
    u32              shot_serial_shown;   /* the view model has been told */

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
    q2_hud           hud;
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
    q2_viewweapon    vw;
    q2_model_bank    model_bank;
    bool             model_bank_ready;
    q2_model         vw_model;
    bool             vw_model_ready;
    int              vw_last_weapon;

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
    q2_actor        **cre_target;
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

    /* The music countdown, in 50 Hz ticks — 0x800B2710 and 0x800B2708. */
    s32               music_total;
    s32               music_left;
    double            music_clock;
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
     */
    u32               secrets_total;
    u32               secrets_found;
    u32               secret_seen[64];   /* item offsets already counted */
    u32               secret_seen_count;
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
    q2_start_pos      pending_teleport;
    bool              pending_teleport_have;

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

    q2_mover_set      movers;
    bool              movers_ready;

    /*
     * The train's two extra voices, drained rather than played.
     *
     * PLATFORM asks for id 13 while it moves and id 14 when it arrives, both
     * through 0x80040800's numeric SPU-parameter table rather than through the
     * bank (mover.h). There is nothing here that can play one, so the request
     * is TAKEN and counted: a latch nobody clears would sit at 14 for the rest
     * of the level and read as "the train is arriving" forever.
     */
    u32               train_move_calls;
    u32               train_stop_calls;
    u32               mover_triggers;   /* items the script reached */
    u32               mover_moved;      /* ticks on which one moved */
    u32               conveyor_steps;   /* BASE0 DOCRATES object writes */
    u32               mover_sounds;     /* transitions that made a noise */
    u32               rot_sounds;       /* ...and the hatches' own       */
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
    u32               ent_faces;
    u32               ent_shadows;    /* retail posed-footprint FT4s emitted */
    u32               explosive_sounds;
    u32               explosive_sounds_missed;
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

    bool              map_change_pending;
    char              pending_map[Q2_UF_NAME_LEN + 1];
    char              pending_start[Q2_UF_NAME_LEN + 1];

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
     * console builds a whole new player entity (0x8003B250) and clears the
     * client record (0x8003B040), and this is the port's side of that. */
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
    q2_world_stats   shot_stats;         /* what the last viewport drew     */
    long             frame_index;
    long             frames_total;       /* 0 = run until the window closes */
    long             shot_every;         /* 0 = only the last frame         */
    const char      *shot_path;          /* NULL = do not capture           */
    long             shots_written;
} client;

static void client_bind_view_model(client *c);
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

    q2_fx_group_spawn(&c->sim[0].fx, pos, vel, count,
                      q2_fx_ramp_at(&c->fx_tables, 10),
                      q2_fx_ramp_at(&c->fx_tables, 0), 32, 6144, 0);
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
static bool client_play_sound_at(client *c, const char *want, const s32 at[3]);
static bool client_find_sound(client *c, const char *want, q2_vag *out);

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
    c->cre_home     = NULL;
    c->cre_model    = NULL;
    c->cre_model_ok = NULL;
    c->cre_anim     = NULL;
    c->cre_actor    = NULL;
    c->cre_target   = NULL;

    if (c->creatures_ready) {
        q2_sim_set_targets(&c->sim[0], NULL, 0);
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
    }

    if (!c->cre_model || !c->cre_model_ok || !c->cre_anim ||
        !c->cre_actor || !c->cre_target) {
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
    /* Everything the map placed, until the zone test says otherwise. */
    c->cre_in_zone = c->creatures.set.count;

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
         */
        c->cre_in_zone = c->creatures.set.count - elsewhere;
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

        if (c->model_bank_ready) {
            idx = q2_model_bank_find(&c->model_bank, name);
            if (idx >= 0 &&
                q2_model_get(&c->model_bank, (u32)idx,
                             &c->cre_model[i]) == Q2_OK) {
                c->cre_model_ok[i] = true;
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
                resolved++;
            }
        }
    }

    q2_sim_set_targets(&c->sim[0], c->cre_target, c->creatures.set.count);

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
     * The breadcrumb trail the AI hunts along — `0x800D517C`, the sixteen-slot
     * ring at gp+17892. `q2_trail_add` had no caller anywhere in the tree, so
     * the trail was always empty and the three-stage pursuit a creature runs
     * when it loses you could never reach its second stage: it had nowhere to
     * follow you to.
     */
    q2_trail_init();

    q2_cre_set_melee_hook(client_cre_melee, c);
    q2_cre_set_sound_hook(client_cre_sound, c);
    q2_cre_set_fire_hook(client_cre_fire, c);
    q2_cre_set_shot_hook(client_cre_shot, c);

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
        q2_creature_world_wake(&c->creatures, player_origin);
    }
    c->ai_accum = 0.0;

    {
        u32 live = 0;
        for (i = 0; i < c->creatures.set.count; i++)
            if (c->creatures.set.monsters[i].in_use)
                live++;

        Q2_INFO("creatures: %u live in this zone, %u of %u spawn records "
                "placed, %u with a model%s",
                live, c->creatures.stats.placed, c->creatures.stats.records,
                resolved,
                c->creatures.stats.no_module
                    ? ", some classes have no module" : "");
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
    {
        q2_actor *attacker = NULL;
        size_t    idx      = (size_t)(m - c->creatures.set.monsters);

        if (c->cre_actor && m >= c->creatures.set.monsters &&
            idx < c->creatures.set.count)
            attacker = &c->cre_actor[idx];

        q2_sim_hurt_player(&c->sim[0], attacker, (s16)damage, Q2_MOD_MELEE,
                           c->creatures.sight.pos);
    }
}

/*
 * A creature's shot, with the figures read out of its own module.
 *
 * `soldier_fire` hands over `table * 8 + flash`, and the table is chosen by
 * skin: 0 blaster, 1 shotgun, 2 machinegun. Each arm's call is now read, and
 * every figure in it is id's own — which is the check that says the read is
 * right rather than merely self-consistent:
 *
 *     table 0  import +0x80  0x80062000  blaster   dmg 5, speed 600
 *     table 1  import +0x88  0x80061ED0  shotgun   dmg 2, kick 1,
 *                                                  spread 1000/500, 12 pellets
 *     table 2  import +0x84  0x80061DFC  bullet    dmg 2, kick 4,
 *                                                  spread 300/500
 *
 * Only the Soldier's are known, so only the Soldier shoots: another creature's
 * fire reaches this with a table this does not recognise and is dropped rather
 * than given a Soldier's gun.
 */
static void client_cre_fire(q2_monster *m, int flash, void *user)
{
    client *c = (client *)user;
    int table = flash >> 3;
    s16 damage;
    int shots;

    if (!c || !c->creatures_ready)
        return;
    if (m->enemy != &c->creatures.sight)
        return;

    /*
     * TWO CALLERS, TWO ENCODINGS, AND ONE OF THEM WAS BEING THROWN AWAY.
     *
     * A TRANSCRIBED creature hands over `weapon_table * 8 + flash_number`,
     * which for the Soldier's three tables is 0..23. A DECODED one has no such
     * table and hands over the IMPORT SLOT its think function called, which is
     * 0x80..0x9C — the eight projectile spawners (creature.h).
     *
     * The ranges do not overlap, so the two are told apart on sight. They were
     * not: this began `table = flash >> 3` and switched on 0, 1 and 2, so every
     * decoded creature's shot arrived as table 16..19 and hit `default: return`.
     * The action layer counted it as `fire_sent` and it went nowhere — six of
     * the disc's seven creatures firing into a `return` while the counter said
     * the shot had been delivered.
     *
     * It still does not fire, and now it says so. What a decoded creature's
     * shot DOES is not the port's to guess: `monster_fire_rocket` (0x8006210C)
     * scales the aim by 3/2 and passes its caller's `a1` and `a3` straight
     * through to 0x8004AF28 without touching them, so the damage and the speed
     * are the MODULE's arguments, not the engine's constants. Reading them is
     * per-creature transcription. Until a creature's table is read, its shot is
     * counted here rather than invented — the same rule the sound hook follows.
     */
    if (flash >= Q2_IMP_FIRE_BLASTER && flash <= Q2_IMP_FIRE_LASER) {
        c->cre_fire_no_figures++;
        return;
    }

    switch (table) {
    case 0:  damage = 5; shots = 1;  break;   /* blaster    */
    case 1:  damage = 2; shots = 12; break;   /* shotgun    */
    case 2:  damage = 2; shots = 1;  break;   /* machinegun */
    default: return;
    }

    /*
     * The spread is not modelled here, so a shotgun's twelve pellets would all
     * hit and make a guard four times deadlier than the console's. Halving the
     * count is not a figure from anywhere, so instead the trace is run once per
     * shot through the sim's own line of sight and only the shots that reach
     * land — which for a single-pellet weapon is exact and for the shotgun is
     * the honest approximation, stated rather than hidden.
     */
    c->cre_shots++;

    /*
     * THE MUZZLE FLASH AND THE REPORT, neither of which a creature had.
     *
     * `soldier_fire` hands over a (table, flash) pair and this took the damage
     * out of it and dropped everything else, so a guard shooting at you was
     * silent and unlit — the only clue you were under fire was your own health
     * falling. The player's shot raises both (simcombat.c); a creature's shot
     * is the same event seen from the other end and raises the same two.
     *
     * The light is the ENGINE's muzzle flash, not one invented here:
     * Q2_MUZZLE_LIGHT_* and the radii `q2_weapon_muzzle_light` rolls are what
     * the player's own gun uses. It goes at the ENTITY ORIGIN — the console
     * passes entity+0x54 for the player's, and `m->pos` already is that for a
     * monster, which is the one place a creature does not need the feet-to-
     * origin correction.
     *
     * The report is the module's own handle: index 9 `wep_shotgf1b` for a
     * shotgun guard, 8 `wep_machgf1b` for a machinegun guard. The blaster
     * guard gets neither, and that is not an omission — its module registers no
     * fire sound at all, because the bolt it throws carries its own.
     */
    if (c->lights_ready) {
        static const u8 muzzle_colour[3] = {
            Q2_MUZZLE_LIGHT_R, Q2_MUZZLE_LIGHT_G, Q2_MUZZLE_LIGHT_B
        };
        s32 inner, outer;

        q2_weapon_muzzle_light(q2_rng_next(&c->sim[0].combat.rng),
                               &inner, &outer);
        q2_light_add_dynamic(&c->light_world, m->pos, muzzle_colour,
                             inner, outer, 0, 0);
    }

    {
        const char *report = (table == 1) ? q2_cre_soldier_sound_name(9)
                           : (table == 2) ? q2_cre_soldier_sound_name(8)
                           : NULL;
        q2_vag vag;

        /*
         * Asked for by name and CHECKED, because "it plays a sound" is the
         * easiest claim in this file to make falsely: client_play_sound_at
         * returns false with no audio device, which is every headless run, so
         * a bare call proves nothing. A name the bank does not carry is
         * counted rather than swallowed.
         */
        if (report) {
            if (client_find_sound(c, report, &vag))
                client_play_sound_at(c, report, m->pos);
            else
                c->cre_sound_missing++;
            c->cre_fire_sounds++;
        }
    }

    while (shots-- > 0) {
        if (!q2_visible(m, &c->creatures.sight))
            break;
        q2_sim_hurt_player(&c->sim[0], NULL, damage,
                           table == 0 ? Q2_MOD_ENERGY_BOLT : Q2_MOD_BULLET,
                           c->creatures.sight.pos);
    }
}

/*
 * A CREATURE'S SHOT, WITH ITS OWN FIGURES — the hook that replaces the one
 * above for everything that has been transcribed.
 *
 * `client_cre_fire` takes an int and can therefore only serve the Soldier,
 * whose three weapons the client hardcodes. Every other creature reached it
 * naming an import slot and was declined; six of the seven modules could hunt
 * you down and never hurt you. `q2_cre_shot` carries what the module passes to
 * the engine's spawner, and all of it is read (crebind.h).
 *
 * What this does NOT reproduce, stated rather than hidden: the projectile
 * spawners really do spawn an entity that flies, and this resolves the shot
 * immediately along the line of sight instead. A rocket's travel time and its
 * splash are the difference, and both belong to the projectile system rather
 * than here. Damage, kick, spread and pellet count are the module's.
 */
static void client_cre_shot(q2_monster *m, const q2_cre_shot *shot, void *user)
{
    client *c = (client *)user;
    s32 shots;
    int mod;

    if (!c || !c->creatures_ready || !m || !shot)
        return;
    if (m->enemy != &c->creatures.sight)
        return;

    /*
     * A shot with no damage read is declined rather than guessed — the same
     * rule the sound hook follows for a name the module does not carry.
     */
    if (shot->damage <= 0) {
        c->cre_fire_no_figures++;
        return;
    }

    c->cre_shots++;

    /* The muzzle flash is the engine's own, exactly as the Soldier's is. */
    if (c->lights_ready) {
        static const u8 flash[3] = { Q2_MUZZLE_LIGHT_R, Q2_MUZZLE_LIGHT_G,
                                     Q2_MUZZLE_LIGHT_B };
        s32 inner, outer;

        q2_weapon_muzzle_light(q2_rng_next(&c->sim[0].combat.rng),
                               &inner, &outer);
        q2_light_add_dynamic(&c->light_world, m->pos, flash, inner, outer, 0, 0);
    }

    switch (shot->slot) {
    case Q2_IMP_FIRE_BLASTER: mod = Q2_MOD_ENERGY_BOLT; break;
    case Q2_IMP_FIRE_RAILGUN: mod = Q2_MOD_RAIL;        break;
    case Q2_IMP_FIRE_ROCKET:  mod = Q2_MOD_ROCKET;      break;
    case Q2_IMP_FIRE_GRENADE: mod = Q2_MOD_GRENADE;     break;
    default:                  mod = Q2_MOD_BULLET;      break;
    }

    shots = shot->count > 0 ? shot->count : 1;

    while (shots-- > 0) {
        if (!q2_visible(m, &c->creatures.sight))
            break;
        q2_sim_hurt_player(&c->sim[0], NULL, (s16)shot->damage, mod,
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
         * A name the bank does not carry is SILENCE, and on this disc that is
         * usually correct rather than a gap: `ara_idle1`, `ara_srch1`,
         * `ber_idle1`, `ber_srch1` and `tnk_idle1` are registered by their
         * modules and appear in NO map's bank anywhere on the disc. The
         * console's loader returns a null handle for those and playing one does
         * nothing. See openquestions #61.
         */
        if (!client_find_sound(c, name, &vag))
            c->cre_sound_missing++;
        else
            client_play_sound_at(c, name, m->pos);
    } else {
        c->cre_sound_unnamed++;
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
        c->ai_thoughts += q2_creature_world_tick(&c->creatures, eye);

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

    if (d->stage != Q2_PDEATH_ALIVE)
        return d->move != Q2_PMOVE_NONE ? d->move : Q2_PMOVE_DEATH1;

    if (serial != a->shot_serial) {
        a->shot_serial    = serial;
        a->attack_latched = true;
    }
    if (a->attack_latched)
        return Q2_PMOVE_ATTAK;
    if (!p->on_ground)
        return Q2_PMOVE_JUMP;
    if (abs(p->vel[0]) + abs(p->vel[2]) > 8)
        return Q2_PMOVE_RUN;
    return Q2_PMOVE_STAND;
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
static void client_mp_tick(client *c, float dt)
{
    s32 ticks = (s32)((double)dt * 300.0 + 0.5);
    q2_mp_request req;

    if (!c->mp_enabled || c->mp_last_request != Q2_MP_REQ_NONE)
        return;                     /* the match is over and asked for a screen */

    if (ticks < 1)
        ticks = 1;
    c->mp_level_time += ticks;

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

    /*
     * The banner has run out and the runtime wants a game state: 11 loads the
     * scoreboard, 19 restarts the round. Both are the engine's own ids, and
     * this port has neither screen, so the request is recorded and reported
     * rather than acted on — which is the honest half of the pair.
     *
     * Taking it also stops the session: on the console the request CHANGES THE
     * GAME STATE, so the level hook stops running. Leaving it ticking here made
     * the runtime re-ask on every frame, which is what the first run of this
     * code did — sixty-odd identical requests for one match that ended once.
     */
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
static u32 client_targets_for(client *c, int who)
{
    u32 n = 0, i;

    if (c->creatures_ready && c->cre_target)
        for (i = 0; i < c->creatures.set.count && n < Q2_CLIENT_MAX_TARGETS; i++)
            c->mp_target[n++] = c->cre_target[i];

    if (c->mp_enabled)
        for (i = 0; i < Q2_MP_MAX_PLAYERS && n < Q2_CLIENT_MAX_TARGETS; i++) {
            if ((int)i == who || !c->sim_ready[i])
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
            c->mp_world_target[w++] = (k == (u32)c->sim[0].cur_player)
                                          ? &c->sim[0].combat.self
                                          : &c->sim[0].pcombat[k].self;
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
 * A parked player takes damage on their ACTOR; their inventory is a separate
 * field and only the live player's pair is synchronised. Copy it back so a hit
 * landed while they were parked is still there when their frame runs.
 */
/*
 * Any player whose health has crossed zero scores a frag for whoever did it.
 *
 * This is the engine's own hook at 0x800396AC — `(*module)->[4](killer,
 * victim)` — with the killer taken from the actor's `last_attacker`, which is
 * the byte the original keeps at entity+222. `q2_mp_attribute_kill` decides
 * whether it counts: a world kill and the level's own hazards are nobody's
 * frag, however the victim came to be standing in them.
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
        if (a->health > 0) {
            c->mp_dead[i] = false;
            continue;
        }
        if (c->mp_dead[i])
            continue;                /* already counted this death */

        c->mp_dead[i] = true;
        {
            int killer = q2_mp_attribute_kill(a->last_attacker, a->last_mod);

            q2_mp_player_killed(&c->mp, killer, i);
            c->mp_deaths++;
            Q2_INFO("multiplayer: player %d killed by %d — frags %d %d %d %d",
                    i, killer, c->mp.frags[0], c->mp.frags[1],
                    c->mp.frags[2], c->mp.frags[3]);
        }
    }
}

static void client_sync_parked_health(client *c)
{
    int i;

    if (!c->mp_enabled)
        return;

    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++) {
        if (i == c->sim[0].cur_player || !c->sim_ready[i])
            continue;
        if (c->sim[0].pcombat[i].self.health != c->sim[0].pcombat[i].inv.health)
            c->sim[0].pcombat[i].inv.health =
                c->sim[0].pcombat[i].self.health;
    }
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
 * at it (0x8003B250: health 100, entity+222 at the "not a player" sentinel),
 * clears the client record and installs the Stand move. The pad and menu gates
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

    /*
     * The loadout goes back BEFORE the spawn, because `q2_sim_spawn` seeds the
     * pain diff from the inventory it finds: spawn first and the new life
     * starts with `prev_health` at whatever the corpse had, so the first tick
     * reads a hundred-point rise and the one after reads a drop as damage.
     */
    {
        int saved = c->sim[0].cur_player;

        c->sim[0].cur_player = pi;
        if (pi == 0) {
            c->sim[0].combat.inv       = c->mp_start_inv;
            c->sim[0].combat.weapon_id = c->mp_start_weapon;
            c->sim[0].combat.next_fire = 0;
        }
        q2_sim_spawn(&c->sim[0], feet, c->mp_view_yaw[pi]);
        c->sim[0].player[pi].ground_y = feet[1];
        c->sim[0].cur_player = saved;
    }

    if (pi > 0) {
        q2_sim_player_reset_combat(&c->sim[0], pi);
        c->sim[0].pcombat[pi].self.owner = (s8)pi;
    } else {
        q2_actor_from_player(&c->sim[0].combat.self, &c->sim[0].combat.inv,
                             c->sim[0].player[0].pos);
        c->sim[0].combat.self.owner = 0;
        c->cam.roll = 0;      /* and the death cam's tilt goes with the body */
    }

    q2_player_death_init(&c->death[pi]);
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

/* Selects the loaded level's music. Defined beside the rest of the music code;
 * declared here because client_load_zone ends by calling it. */
static void client_music_for_level(client *c, bool force);

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

/*
 * The pause page's status row. The same two pairs the level tally shows, so a
 * player can ask mid-level how they are doing — which is what the row is for.
 */
static void client_menu_fill_stats(client *c)
{
    u32 i, dead = 0;

    if (!c)
        return;

    if (c->creatures_ready)
        for (i = 0; i < c->creatures.set.count; i++)
            if (c->creatures.set.monsters[i].dead)
                dead++;

    q2_menu_set_stats(&c->menu, (int)dead, (int)c->cre_in_zone,
                      (int)c->secrets_found, (int)c->secrets_total);
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
 */
static void client_level_tally(const client *c, u32 *dead, u32 *placed)
{
    u32 i, d = 0;

    if (c->creatures_ready)
        for (i = 0; i < c->creatures.set.count; i++)
            if (c->creatures.set.monsters[i].dead)
                d++;

    *dead   = d;
    *placed = c->creatures_ready ? c->cre_in_zone : 0;
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
 * **NOTHING CLEARS IT BETWEEN UNITS, and this used to.** The table is a
 * CAMPAIGN's six rows, not a unit's, and the clear the port invented for the
 * unit boundary was the last guess left in this screen. Where the only clear
 * in the executable actually is:
 *
 *     0x8003D62C(player, 0)  looks up a block by the key "PlayerSave"
 *                            (0x8007FBEC) and, when it finds one, copies six
 *                            25-byte records out of it at +0xD4 into
 *                            0x8009B550 and hands the loaded level's counters
 *                            back with 0x80022210.
 *     0x8003DDB8             the `else` of that: memset(0x8009B550, 0, 150).
 *
 * So the six rows are cleared when there is no player block to restore — a new
 * game — and at no other time. The one place a per-unit clear belongs,
 * `0x80022498` in the MISCOMPLETE arm between the board's setup and its spin,
 * is a six-iteration loop with no body; and no engine export hands a level
 * module the array's address, so a module cannot clear it either.
 *
 * The consequence is the console's and the port now reproduces it: six rows,
 * first six distinct levels, and a seventh registers nothing. A player reaches
 * unit 2's board having visited exactly six levels, so that board is full, and
 * unit 3's shows the same six. That is a defect in the original rather than a
 * design, and it is transcribed here rather than tidied because a board that
 * lists a unit's own levels is a screen the console does not draw.
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
        Q2_WARN("mission: no row for %s — the campaign's six are taken, which "
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

    if (start && start[0]) {
        char   path[256];
        q2_buf buf;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(c->disc, path, &buf) == Q2_OK) {
            q2_common_file probe;

            if (q2_common_open(&probe, &buf) == Q2_OK) {
                q2_start_pos_list spawns;

                if (q2_start_pos_parse(&spawns, &probe) == Q2_OK &&
                    q2_start_pos_find(&spawns, start, &arrival)) {
                    zone         = arrival.zone;
                    have_arrival = true;
                }
                q2_common_close(&probe);
            } else {
                q2_buf_free(&buf);
            }
        }
    }

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
        q2_hud_init(&c->hud, &c->hud_tables, 1);

    c->map_changes++;
    Q2_INFO("LOADMAP -> %s zone %d%s%s", map, zone,
            have_arrival ? " at " : "", have_arrival ? arrival.name : "");
    return true;
}

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

    if (q2_sim_explosive_trigger_item(&c->sim[0], item->offset))
        c->explosive_scripted++;
}

static void client_event_call(void *user, const q2_event_item *item,
                              u8 call_index)
{
    client *c = (client *)user;

    if (!c || !c->sim[0].userfuncs_ready)
        return;

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
     * spawns. The console "switches zone first if the target is in another
     * one, then sets entity position"; the zone switch is not done here and is
     * stated rather than hidden — a target in the resident zone moves the
     * player, one elsewhere is refused and logged, so the two cases cannot be
     * confused with each other.
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
                    c->pending_teleport      = sp;
                    c->pending_teleport_have = true;
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
                for (k = 0; k < 2; k++) {
                    if (!q2_uf_operand_name(&call, (u32)k, key) || !key[0])
                        continue;
                    if (c->leveltext_ready)
                        text[k] = q2_leveltext_find(&c->leveltext, key);
                    if (text[k])
                        c->script_strings++;
                }

                q2_briefing_popup_set(&c->popup, text[0], text[1]);

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
    {
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
     * TIMER — the rest of the record, later.
     *
     * `ticks = (base + ((range * rand()) >> 15)) * 30`, and the 30 is not the
     * 300 every other time on this clock uses — `userfuncs.c` calls that out
     * and it is the sort of thing that is silently four-fifths wrong if
     * assumed. The RNG is the sim's rather than the BIOS's, which is a stated
     * divergence: the console's `rand()` stream is not reproduced here, so a
     * timer's jitter is the right shape and not the same sequence.
     */
    {
        q2_uf_call call;

        if (q2_uf_decode_call(&call, &c->sim[0].userfuncs, item) == Q2_OK &&
            call.prim == Q2_UF_TIMER) {
            u32 base = 0, range = 0;

            q2_uf_operand_u32(&call, 0, 0, &base);
            q2_uf_operand_u32(&call, 1, 0, &range);

            {
                s32 r  = (s32)(q2_rng_next(&c->sim[0].fx_rng) & 0x7FFFu);
                s32 t  = (s32)base + (s32)(((s64)range * r) >> 15);

                c->sim[0].event_rt.defer_ticks = t * 30;
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
                    q2_hud_message(&c->hud, text);
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

                /* The map's own words, on the overlay — which is what makes
                 * "counter++" a thing the player can see happen. */
                if (c->secret_message[0])
                    q2_hud_message(&c->hud, c->secret_message);

                Q2_INFO("secret found: %u of %u — \"%s\"", c->secrets_found,
                        c->secrets_total,
                        c->secret_message[0] ? c->secret_message : "(no text)");
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
     * TIMEDLIGHT — a script-placed dynamic light, and the last piece of the
     * fifteen `0x80075C34` call sites that this port could reach without
     * tracing a runtime value.
     *
     * The operand table (userfuncs.c) has carried its layout for a while with
     * nothing behind it: origin at +4 as three s32, `radius` at +18 "tripled
     * before the call", and a packed colour at +24. The triple is the engine's,
     * not a guess. The colour's own consumer is 0x80075D14; the low three bytes
     * are taken as r, g, b here, which is what every other packed colour on this
     * path does.
     *
     * FLKLIGHT is deliberately NOT handled: its on/off times are randomised as
     * ((rand()*500)>>15)+400, so it needs the engine's RNG stream to look right
     * rather than merely to appear.
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
        s32 radius;

        at[0]  = (s32)q2_rd_u32(p + 4);
        at[1]  = (s32)q2_rd_u32(p + 8);
        at[2]  = (s32)q2_rd_u32(p + 12);
        radius = (s32)q2_rd_u16(p + 18) * 3;
        packed = q2_rd_u32(p + 24);
        rgb[0] = (u8)(packed & 0xFF);
        rgb[1] = (u8)((packed >> 8) & 0xFF);
        rgb[2] = (u8)((packed >> 16) & 0xFF);

        if (radius > 0)
            q2_ent_light_at(&c->sim[0].ent_world.events, at, rgb, 0, radius);
        c->script_lights++;
    }
}


/* ------------------------------------------------------------------------- */
static bool client_load_zone(client *c, const char *map, int index)
{
    q2_world_zone loaded;
    s32 wmin[3], wmax[3];
    bool placed = false;
    bool same_map_transition = c->carry_player && c->carry_same_map &&
                               c->map[0] && client_name_eq(c->map, map);

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

    /* q2_sim_init memsets the struct, so the previous zone's trigger bitmap and
     * event runtime have to be released first or they leak on every zone
     * change -- and zone changes are exactly what the gates now cause. */
    q2_sim_free(&c->sim[0]);
    q2_sim_init(&c->sim[0], &c->zone, q2_build_tick_rate(&c->build));
    /* The client owns a view-weapon machine, so the machine decides when a
     * shot happens — the tick must not also fire from the raw trigger. */
    c->sim[0].fire_from_input = false;
    /* Re-armed after every load, because the memset above clears it. */
    c->sim[0].trace_zone      = c->zone_trace;
    c->sim[0].autoswitch      = !c->no_autoswitch;
    c->sim[0].invulnerable    = c->god;
    {
        s32 feet[3];
        feet[0] = c->cam.pos[0];
        feet[1] = c->cam.pos[1];
        feet[2] = c->cam.pos[2];
        q2_sim_attach_gameplay(&c->sim[0], &c->common);

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
            q2_result ir = q2_sim_attach_items(
                &c->sim[0], &c->common, index,
                c->item_table_ready ? &c->item_table : NULL,
                c->model_bank_ready ? &c->model_bank : NULL);

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
                Q2_INFO("items: %u placed", c->sim[0].entities.count);

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
             * takes the init arm. Save restore applies its selected weapon and
             * initialises once more after the save body is read, below.
             */
            if (!c->carry_player) {
                q2_vw_init(&c->vw, &c->vm_tables,
                           c->sim[0].combat.weapon_id);
                c->vw_last_weapon = c->sim[0].combat.weapon_id;
            }
            client_bind_view_model(c);
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
        }
        /*
         * The zone's lights: COMMON.DAT's `Lights` array and the zone's own
         * per-node index lists, which is exactly the pair `q2_light_gather`
         * wants. SpaceLights is partitioned by the SECONDARY collision node, so
         * it is opened against the hull the sim already has.
         */
        c->lights_ready = false;
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
        c->sim[0].event_rt.on_mover      = client_event_mover;
        c->sim[0].event_rt.on_mover_user = c;
        c->sim[0].event_rt.on_explosive      = client_event_explosive;
        c->sim[0].event_rt.on_explosive_user = c;

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
        }

        /*
         * ONE-SHOT, like `carry_player`, and for the same reason now that it
         * decides POSITION as well as the clock. Left standing it would make
         * the next load that does not set it explicitly keep a position from
         * the wrong level. `client_change_map` already clears it on the way in,
         * so this only closes the paths that do not.
         */
        c->carry_same_map = false;

        /*
         * The other players. Each gets its own sim, standing at its own
         * MultiSpawn, and from here on each moves under its own pad — so a
         * split-screen viewport shows a player walking rather than a fixed
         * camera parked at a spawn point.
         *
         * Their world halves run and are ignored: each instance spawns its own
         * copy of the map's items and runs its own script, and nothing reads or
         * draws any of it. That is the cost of the player living inside q2_sim,
         * and it is a cost rather than a bug — the duplicate worlds are
         * invisible and self-consistent. Question 53 is the fix.
         */
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

                    c->sim[0].cur_player = pi;
                    q2_sim_spawn(&c->sim[0], pfeet, c->mp_view_yaw[pi]);
                    c->sim[0].player[pi].ground_y = pfeet[1];
                    c->sim[0].cur_player = saved;
                }
                q2_sim_player_reset_combat(&c->sim[0], pi);
                c->sim[0].pcombat[pi].self.owner = (s8)pi;
                c->sim[0].player_count = pi + 1;
                c->sim_ready[pi] = true;
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
    }

    Q2_INFO("%s: %u nodes, %u vertices",
            c->zone.name, c->zone.scene.node_count, c->zone.points.count);

    /*
     * And this level's music. Here rather than in `main()` because a session
     * loads many zones and only the first one used to be asked what it sounded
     * like. A gate between zones of the same map resolves to the same level
     * record and is left alone — see client_music_for_level.
     */
    client_music_for_level(c, false);

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
     */
    c->music_id     = id;
    c->music_total  = (e->tenths ? e->tenths : Q2_MUSIC_FALLBACK_TENTHS) * 5;
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
 * A ZONE GATE IS NOT A LEVEL CHANGE. Both resolve to the same level record, and
 * restarting the playlist on every gate would restart the music every time the
 * player crossed a zone boundary inside one map — so the record is compared and
 * an unchanged one is left playing. `force` is for the one caller that needs
 * the restart anyway: coming back off a film, where the drive was taken away.
 */
static bool client_music_play_id(client *c, int id);

static void client_music_for_level(client *c, bool force)
{
    const q2_level_entry *lv;

    if (!c->level_table_ready || !c->music_table_ready)
        return;

    lv = q2_level_find(&c->level_table, c->map);
    if (lv == c->level && !force)
        return;                 /* same level: a zone gate, or nothing moved */

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
 * The music volume for the sector about to be handed out — the slider, and the
 * fade the two duration globals exist for.
 *
 * SOUND OPTIONS -> MUSIC. The original scales by the slider doubled
 * (0x800205F4 stores music*2 into the volume global), so the top of the slider
 * is full scale and the bottom is silence.
 *
 * `0x80071954` then scales by `remaining / 64` while the countdown is under 64
 * ticks and `0x80071980` by `elapsed / 64` while the elapsed count is — so a
 * track fades in over its first 64 ticks and out over its last 64. At 50 Hz
 * that is 1.28 seconds each way, and it is the reason the durations are restart
 * points rather than lengths.
 */
static s32 client_music_volume(const client *c)
{
    s32 vol = c->settings.v[Q2_SET_MUSIC] * 2;

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

        if (vol < 255) {
            u32 i;
            for (i = 0; i < n; i++)
                c->bed[i] = (s16)((c->bed[i] * vol) / 255);
        }
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

static void client_input_simulated(client *c, float dt)
{
    q2_input in;
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

    raw = client_pad_mask(c, &bind);

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
    pad->lx = pad->ly = pad->rx = pad->ry = 0;

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
            const bool *keys = c->demo ? NULL : SDL_GetKeyboardState(NULL);
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
    if (c->shoot) {
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
        const q2_monster *best = NULL;
        s64 best_d = 0;
        s32 eye0[3];
        u32 i;

        q2_sim_eye(&c->sim[0], eye0);

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            s64 dx, dy, dz, d;

            if (!m->in_use || m->dead || !c->cre_model_ok[i])
                continue;

            dx = m->pos[0] - eye0[0];
            dy = m->pos[1] - eye0[1];
            dz = m->pos[2] - eye0[2];
            d  = dx * dx + dy * dy + dz * dz;
            if (!best || d < best_d) { best = m; best_d = d; }
        }

        if (best) {
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
    if (step > 0) {
        if (in.buttons & Q2_BTN_WEAP_NEXT)      q2_sim_cycle_weapon(&c->sim[0], +1);
        else if (in.buttons & Q2_BTN_WEAP_PREV) q2_sim_cycle_weapon(&c->sim[0], -1);
    }

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
        for (i = 0; i < c->creatures.set.count; i++)
            q2_actor_from_monster(&c->cre_actor[i],
                                  &c->creatures.set.monsters[i]);
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

    ticked = (q2_sim_advance(&c->sim[0], &in, (double)dt) != 0);
    q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;
    client_sync_parked_health(c);
    client_score_deaths(c);

    /*
     * The other players, each on its own pad. In a headless demo run there is
     * one script, so each is given a rotated slice of it — otherwise four
     * players would walk in lockstep and a split screen would show one man
     * reflected four times, which proves nothing about four sims running.
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

        for (pi = 1; ticked && pi < Q2_MP_MAX_PLAYERS; pi++) {
            q2_input pin;

            if (!c->sim_ready[pi])
                continue;

            pin = in;
            if (c->demo) {
                /* Each player reads the same script at a different phase, so
                 * four sims produce four walks rather than one reflected. */
                q2_pad_config pcfg;

                u32 dpad = client_demo_pad((long)c->frame_index +
                                           (long)pi * 37);

                /* The same gap and the same rule: a scripted pad that was not
                 * rolled while a screen was up would raise an edge for every
                 * button its word happens to hold on the frame play resumes. */
                if (resumed)
                    q2_pad_roll_resume(&c->mp_pad[pi], dpad, dpad);
                else
                    q2_pad_roll(&c->mp_pad[pi], dpad);

                q2_pad_config_default(&pcfg);
                /* Player `pi` lives in sim[0] now; `sim[pi]` has been an
                 * uninitialised struct since they moved there. */
                pcfg.style = c->sim[0].player[pi].look_scheme;
                q2_pad_read(&c->mp_pad[pi], &pcfg, &pin);
            }

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

                /*
                 * Did that player's frame actually take a shot? `last_shot` is
                 * part of the swapped half, so after the tick it is parked in
                 * that player's slot. Counting it is what tells "the shot
                 * missed" apart from "no shot was ever fired", and those want
                 * very different fixes.
                 */
                if (c->sim[0].pcombat[pi].last_shot.fired)
                    c->mp_shots[pi]++;
                else if (c->sim[0].pcombat[pi].last_shot.dry)
                    c->mp_dry[pi]++;
                client_sync_parked_health(c);
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
    if (ticked)
        client_entity_events(c);

    /*
     * The weapon in the hands, advanced on the same clock. The selection comes
     * from the simulation, but the SWAP does not happen when the selection
     * changes — it happens when the lower clip has run and the 70-tick countdown
     * has expired, which is the machine's job, not this caller's.
     */
    if (c->vm_ready) {
        s32 ticks = (s32)((double)dt * 300.0 + 0.5);
        bool swapped;

        if (ticks < 1) ticks = 1;
        if (ticks > Q2_SCREEN_DT_MAX) ticks = Q2_SCREEN_DT_MAX;

        if (c->sim[0].combat.weapon_id != c->vw_last_weapon) {
            q2_vw_select(&c->vw, c->sim[0].combat.weapon_id);
            c->vw_last_weapon = c->sim[0].combat.weapon_id;

            /*
             * Name the weapon, which is what that line of the overlay is FOR.
             *
             * The string is the weapon's GLYPH, not its name: `weapon_glyph[]`
             * at 0x8009DC8C holds "&B", "&S", "&U" and the markup layer expands
             * an escape into a pre-rendered word out of chars.lbm's icon table
             * (hudtables.h). So "Shotgun" on screen is one sprite, not seven
             * characters, which is why hunting for the string never found it.
             *
             * This line used to carry a hardcoded "Quake II" posted once at
             * startup — a placeholder from before the overlay had anything real
             * to say, which then sat there for the whole session because
             * nothing ever replaced it.
             */
            if (c->hud_ready && c->hud_tables_ready) {
                int w = c->sim[0].combat.weapon_id;

                if (w > 0 && w < Q2_HUD_WEAPON_SLOTS)
                    q2_hud_message(&c->hud, c->hud_tables.weapon_glyph[w]);
            }
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
         * `in.attack` is still passed to the machine, because that argument is
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
            if (ticks > 0 && in.attack && q2_vw_wants_fire(&c->vw))
                q2_sim_fire(&c->sim[0]);

            if (c->sim[0].combat.shot_serial != c->shot_serial_shown) {
                c->shot_serial_shown = c->sim[0].combat.shot_serial;

                if (c->sim[0].combat.last_shot.dry) {
                    report = Q2_VW_FIRE_DENIED;
                    c->shots_dry++;
                } else if (c->sim[0].combat.last_shot.fired) {
                    report = Q2_VW_FIRED;
                    c->shots_fired++;
                    /*
                     * AND THE NOISE IT MADE. PlayerNoise (0x80062C74) is what
                     * puts a gunshot on the level's `sound_entity`, and
                     * FindTarget's second arm looks for exactly that. Without
                     * it the player could empty a magazine in a corridor and
                     * wake nobody — measured at 126 shots, 0 hunting.
                     */
                    if (c->creatures_ready)
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
            c->vw.quad_active =
                (c->sim[0].level_time <
                 c->sim[0].combat.inv.quad_until);

            swapped = q2_vw_advance(&c->vw, ticks, in.attack, report);
        }
        if (swapped)
            client_bind_view_model(c);

        /* Grenade3 is a hidden entity attached to the view weapon until the
         * model timeline crosses 411. Retail copies viewmodel+0xA4 every think
         * (0x8004A414), charges only while position 380 is pinned, and lets an
         * elapsed fuse force fire frame 2. Feed that entity after advancing the
         * model so both the attachment and its crossing are from this frame. */
        if (c->vw.weapon == Q2_WID_HAND_GRENADE ||
            q2_projectile_hand_held_index(&c->sim[0].combat.projectiles, 0) >= 0) {
            s16 aim[3], kick[3];
            s32 summed[3], attached[3];
            s32 cook_ticks = q2_vw_take_hand_grenade_cook(&c->vw);
            bool release = q2_vw_take_hand_grenade_release(&c->vw);
            q2_hand_grenade_update state;

            q2_sim_view_angles(&c->sim[0], summed);
            aim[0]  = (s16)c->sim[0].player[0].pitch;
            aim[1]  = (s16)c->sim[0].player[0].yaw;
            aim[2]  = (s16)c->sim[0].player[0].roll;
            kick[0] = (s16)(summed[0] - c->sim[0].player[0].pitch);
            kick[1] = (s16)(summed[1] - c->sim[0].player[0].yaw);
            kick[2] = (s16)(summed[2] - c->sim[0].player[0].roll);

            q2_vw_place(&c->vw, c->sim[0].player[0].pos,
                        c->sim[0].player[0].view_height,
                        aim, kick, attached, NULL);
            state = q2_sim_hand_grenade_update(&c->sim[0], attached,
                                                cook_ticks, release);
            if (state == Q2_HAND_GRENADE_EXPIRED)
                q2_vw_hand_grenade_expired(&c->vw);
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
            u32 n = q2_vw_take_frame_fires(&c->vw);
            s16 snd;

            while (n--) {
                q2_sim_fire(&c->sim[0]);
                c->player_attacks++;
            }

            /* And the clip's own sound at a band boundary — the chaingun's
             * spin-up, loop and spin-down, three of the eleven weapon sounds
             * the table names and nothing had ever played. */
            snd = q2_vw_take_frame_sound(&c->vw);
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
             * by name rather than by index.
             */
            if (q2_vw_take_quad_sound(&c->vw))
                client_play_sound(c, "itm_damage3");
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
        if (q2_vw_take_refire(&c->vw))
            q2_sim_autoselect_weapon(&c->sim[0]);

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
            if (q2_vw_take_event(&c->vw, &ev)) {
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
        if (c->sim[0].combat.shot_serial != c->shot_serial_heard) {
            const q2_fire_result_v2 *shot = &c->sim[0].combat.last_shot;

            c->shot_serial_heard = c->sim[0].combat.shot_serial;

            if ((shot->fired || shot->dry) && shot->sound >= 0) {
                const q2_weapon_tables *wt = q2_weapon_tables_builtin();

                if ((u32)shot->sound < Q2_WT_SOUND_COUNT &&
                    wt->sound[shot->sound][0])
                    client_play_sound(c, wt->sound[shot->sound]);
            }
        }
    }

    /* The overlay ages on logic ticks, not on drawn frames — one notification
     * retires every 60 (hud.h). The flash is the other way round and is
     * decremented inside q2_hud_build_ot. */
    if (c->hud_ready) {
        q2_hud_tick(&c->hud, 1);

        /*
         * The damage flash, fed the player's real condition. `q2_hud_track`
         * raises it when either figure FALLS, with armour taking precedence
         * exactly as the original's branch order does — grey for a hit the
         * armour took, red for one that reached flesh, and the asymmetric
         * strength arithmetic that gives an armour graze a fainter flash than
         * a solid hit (hud.h).
         *
         * This is the last thing the overlay was missing: it was built, it was
         * drawn, and nothing had ever told it how the player was doing.
         *
         * And raising it is only half of it. The overlay owns the arithmetic;
         * the TILE is the screen's, sized to the viewport and linked into that
         * viewport's own slice (screen.h), because on the console the two are
         * one record — the raise at 0x8003AE10 writes `ctx+0x2A0` and the draw
         * at 0x80076764 reads `view+672`, which are the same halfwords. Here
         * they are two structs, so the frame the flash is raised is the frame
         * it has to be handed over; after that the screen owns the countdown
         * and the overlay must not touch it.
         *
         * Viewport 0 because there is one player. A split-screen session would
         * hand each player's flash to its own viewport, which is exactly what
         * the shared record does for free on the console.
         */
        if (q2_hud_track(&c->hud, c->sim[0].combat.inv.health,
                         c->sim[0].combat.inv.armour))
            q2_screen_flash_set(&c->screen, 0, c->hud.flash.rgb,
                                c->hud.flash.strength, c->hud.flash.mode);
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
     * Drop a breadcrumb. The original writes one as the player moves, and the
     * AI's lost-you pursuit walks them backwards; ten frames apart is close
     * enough that a sixteen-slot ring covers the few seconds the pursuit
     * looks over.
     */
    if (c->creatures_ready && (c->frame_index % 10) == 0)
        q2_trail_add(eye, (s16)c->sim[0].player[0].yaw);

    /* The multiplayer session's own frame, on the same clock. */
    client_mp_tick(c, dt);

    /*
     * The level's own beams, re-queued because the transient pool empties every
     * frame — which is exactly what the console's walk at 0x8002EE38 does with
     * its own list, every frame, for ever. Not on the rotators' clock and not
     * gated on them: a zone with lasers and no rotating brush is ordinary.
     */
    c->laser_drawn = c->no_lasers
                     ? 0
                     : q2_laserbeams_draw(&c->lasers, &c->sim[0].fx,
                                          &c->sim[0].fx_rng);

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

        if (c->movers_ready) {

            /*
             * AND WHAT THE TRANSITIONS SOUND LIKE. The mover set asks; this is
             * the only place that knows where a mover's box is, and the
             * console positions every one of these calls at the CENTRE of that
             * box (with the live displacement added on the closing arm), so
             * the sound follows the door.
             */
            {
                u32 mi;

                for (mi = 0; mi < c->movers.count; mi++) {
                    s8 which = q2_mover_take_sound(&c->movers, mi);
                    s32 at[3];
                    u32 t;
                    bool have = false;

                    if (which < 0 || which >= Q2_MVSND_COUNT)
                        continue;

                    for (t = 0; t < c->sim[0].mover_count; t++) {
                        const q2_move_target *mt = &c->sim[0].volumes[t];
                        int k;

                        if (mt->id != (s32)mi)
                            continue;
                        for (k = 0; k < 3; k++)
                            at[k] = (mt->min[k] + mt->max[k]) / 2;
                        have = true;
                        break;
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
             * AND THE TRAIN'S TWO, which are not bank keys and cannot go
             * through the call above. Taken here so the field cannot latch,
             * and counted so a future mixer has a number to check itself
             * against.
             */
            {
                u32 mi;

                for (mi = 0; mi < c->movers.count; mi++) {
                    u8 id = q2_mover_take_travel_sound(&c->movers, mi);

                    if (id == Q2_MOVER_TRAVEL_MOVE_ID)
                        c->train_move_calls++;
                    else if (id == Q2_MOVER_TRAVEL_STOP_ID)
                        c->train_stop_calls++;
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
                    s8  which = q2_rotator_take_sound(&c->rotators, ri);
                    s32 at[3];

                    if (which < 0 || which >= Q2_ROTSND_COUNT)
                        continue;

                    /* A rotator turns a Scene node, and that node is where the
                     * sound is. An unbound one stays silent rather than falling
                     * back to a listener-local play, which is the mistake the
                     * movers above just had corrected. */
                    if (!client_node_centre(c, c->rotators.rotators[ri].node,
                                            at))
                        continue;

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
    }

    /*
     * WHAT THE FRAME'S SHOTS DID — drained here because this is below every
     * site that can damage a creature: the sim tick (projectiles, splash and
     * the creatures' own attacks), the view weapon's trigger, and the fire
     * driver's per-animation-frame shots. See the note at the old site above.
     */
    if (c->creatures_ready && c->cre_actor) {
        u32 i;
        for (i = 0; i < c->creatures.set.count; i++) {
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
             * the port has no inflictor entity to hand over.
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
    }

    /*
     * The AI, on its own clock and looking at where the player is now — as an
     * entity ORIGIN, not an eye. `q2_visible` adds the sight client's view
     * height itself, so handing it the eye added the view height twice and put
     * the player end of every sight line 400 units into the ceiling.
     */
    {
        s32 player_origin[3];

        player_origin[0] = c->sim[0].player[0].pos[0];
        player_origin[1] = q2_sim_origin_y(c->sim[0].player[0].pos[1]);
        player_origin[2] = c->sim[0].player[0].pos[2];
        client_creatures_tick(c, dt, player_origin);
    }

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

                q2_player_death_tick(d, health, tick, c->mp_enabled,
                                     client_mp_rng(c));

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
                /*
                 * The local player answers with the button. The parked ones
                 * have nobody to press it, so they wait for their own body —
                 * the 1500 the corpse think installs — and come back when it
                 * has dissolved. STATED as the port's choice: on the console
                 * the module decides, and the module is driven by a pad.
                 */
                bool ask = (pi == 0)
                               ? (pad->buttons & ~pad->prev & (u16)bind.fire) != 0
                               : (d->stage == Q2_PDEATH_FADING ||
                                  d->stage == Q2_PDEATH_GONE ||
                                  d->stage == Q2_PDEATH_GIBBED);

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
     */
    if (c->sim[0].player[0].ent2_flags & Q2_ENT2_DEAD) {
        s32 cur  = c->cam.roll & 0xFFF;
        s32 want = (-384) & 0xFFF;
        s32 d    = (want - cur) & 0xFFF;
        s32 frac = 100;                 /* 1/2048 units per frame of ease */

        if (!(d & 0x800))
            cur = cur + ((d * frac + 2047) >> 11);
        else
            cur = cur - (((4096 - d) * frac + 2047) >> 11);

        c->cam.roll = (s16)(cur & 0xFFF);
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
        const q2_monster *best = NULL;
        s64 best_d = 0;
        u32 i;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            s64 dx, dy, dz, d;

            if (!m->in_use || m->dead || !c->cre_model_ok[i])
                continue;

            dx = m->pos[0] - eye[0];
            dy = m->pos[1] - eye[1];
            dz = m->pos[2] - eye[2];
            d  = dx * dx + dy * dy + dz * dz;
            if (!best || d < best_d) { best = m; best_d = d; }
        }

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

    k = SDL_GetKeyboardState(NULL);
    if (!k)
        return 0;

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
 */
static void client_menu_origin(const client *c, int *ox, int *oy)
{
    if (ox) *ox = (c->width  - Q2_MENU_SCREEN_W) / 2;
    if (oy) *oy = (c->height - Q2_MENU_SCREEN_H) / 2;
}

/*
 * The pointer in menu space, undoing the two transforms the frame applied on
 * the way out: the window fit (q2_screen_fit_rect plus SCREEN POSITION, exactly
 * as client_frame composes them) and then the menu block's origin.
 *
 * False when there is no pointer to speak of — headless, or grabbed for
 * mouselook, in which case its motion is look input and its position is
 * meaningless.
 */
static bool client_menu_pointer_pos(const client *c, int *mx, int *my)
{
    int out_w = 0, out_h = 0;
    int px = 0, py = 0, pw = 0, ph = 0;
    int ox = 0, oy = 0;
    double sx, sy, fx, fy;

    if (c->headless || !c->renderer || !c->pointer_valid || c->mouse_grabbed)
        return false;

    SDL_GetCurrentRenderOutputSize(c->renderer, &out_w, &out_h);
    q2_screen_fit_rect(&c->screen, c->fit, out_w, out_h, &px, &py, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return false;

    sx = (double)c->settings.v[Q2_SET_SCREEN_X];
    sy = (double)(c->settings.v[Q2_SET_SCREEN_Y] - 24);

    fx = ((double)c->pointer_x -
          ((double)px + sx * (double)pw / (double)Q2_SCREEN_PAL_WIDTH)) *
         (double)c->width / (double)pw;
    fy = ((double)c->pointer_y -
          ((double)py + sy * (double)ph / (double)Q2_SCREEN_PAL_HEIGHT)) *
         (double)c->height / (double)ph;

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
    q2_menu_hit hit;
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
 * RIGHT MOUSE / RIGHT MOUSE 2 / HUNTER MOUSE instead of three schemes there is
 * no stick to drive. The analogue class is never selected because this port has
 * no gamepad path at all: styles 3..5 are unreachable rather than broken.
 *
 * Cheap enough to run every frame, which is what makes the toggle take effect
 * the moment it is flipped rather than at the next page change.
 */
static void client_apply_input(client *c)
{
    bool want_mouse = c->settings.v[Q2_SET_USE_MOUSE] != 0;
    int  style;
    int  i;

    if (want_mouse) {
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
        c->sim[0].player[i].look_scheme = style;

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
           c->boot_open || c->popup.visible;
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
    v->active = true;
    c->voice_started++;
    c->voice_last = v;

    return true;
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
            v->level = CLIENT_SFX_LOCAL;
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

        level = (CLIENT_SFX_BASE * (CLIENT_SFX_REACH - dist)) / 4096;
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

            if (!c->lights_ready ||
                !q2_light_add_dynamic(&c->light_world, ev->e[i].pos,
                                      ev->e[i].glow, inner,
                                      ev->e[i].radius, 0, 0))
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

    q2_save_capture_mission(&c->snapshot, &c->mission);
    /* The doors, which the client owns rather than the sim. Without them a
     * reload shuts every one the player opened AND cannot reopen it: the
     * script flags are carried, so the record that opened it has run. */
    if (c->movers_ready)
        q2_save_capture_movers(&c->snapshot, &c->movers);
    /* And who is dead: without this a save reloads into a room the player has
     * already cleared, full again. */
    if (c->creatures_ready)
        q2_save_capture_creatures(&c->snapshot, &c->creatures.set);
    q2_save_set_settings(&c->snapshot, c->settings.v, Q2_SET_COUNT);
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
        return false;
    }

    rc = q2_save_apply(s, &c->sim[0], NULL, c->build.serial, c->map);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot apply the save: %s", q2_result_str(rc));
        return false;
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
    if (c->movers_ready)
        q2_save_apply_movers(s, &c->movers);
    if (c->creatures_ready)
        q2_save_apply_creatures(s, &c->creatures.set);

    /* The weapon in the hands follows the restored selection. Without this the
     * player holds whatever the fresh spawn gave them while the simulation
     * thinks they are holding the railgun. */
    if (c->vm_ready) {
        q2_vw_init(&c->vw, &c->vm_tables, c->sim[0].combat.weapon_id);
        c->vw_last_weapon = c->sim[0].combat.weapon_id;
        client_bind_view_model(c);
    }

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
        q2_hud_message(&c->hud, text);
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

/*
 * The capture a scripted run writes.
 *
 * `--shot out.ppm` alone writes the last frame to that name; with `--shot-every`
 * it becomes a stem — `out.ppm` -> `out_0000.ppm`, `out_0030.ppm` — so a run
 * produces a strip that can be flipped through. The framebuffer is written, not
 * the window: these are the console's own 512x248 pixels, which is what every
 * comparison in this project is made in.
 */
static void client_write_shot(client *c, bool numbered)
{
    char path[512];
    q2_result rc;

    if (!c->shot_path)
        return;

    if (numbered) {
        const char *dot = strrchr(c->shot_path, '.');
        size_t stem = dot ? (size_t)(dot - c->shot_path) : strlen(c->shot_path);

        if (stem > sizeof(path) - 32)
            stem = sizeof(path) - 32;
        memcpy(path, c->shot_path, stem);
        snprintf(path + stem, sizeof(path) - stem, "_%04ld%s",
                 c->frame_index, dot ? dot : ".ppm");
    } else {
        snprintf(path, sizeof(path), "%s", c->shot_path);
    }

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
                c->vw.fires_started, c->shots_fired, c->shots_dry,
                c->vw.keys_played);

    /* How many times a screen owned the frame and the pad pair had to be
     * reseeded instead of rolled. Zero would mean the detection never fired. */
    Q2_INFO("  pad: %u resume%s", c->pad_resumes,
            c->pad_resumes == 1 ? "" : "s");

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
        /* The mixer, because "sounds are broken" needs a number to argue with.
         * `dropped` is voices that found all 24 busy — a steady stream of those
         * means something is raising more than the SPU could ever have played. */
        Q2_INFO("  audio     %u voices started, %u dropped on a full 24, "
                "%d bytes queued",
                c->voice_started, c->voice_dropped,
                c->audio ? SDL_GetAudioStreamQueued(c->audio) : 0);
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
        Q2_INFO("  entities  %u drawn (%u faces, %u shadows),"
                " %u could not resolve a model",
                c->ent_drawn, c->ent_faces, c->ent_shadows,
                c->ent_no_model);
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
                        "at %d/%d -> (%d,%d,%d), %u running voices, %u stops",
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

        Q2_INFO("  decoded   %u thinks (%u unbound), %u calls (%u unclassified), "
                "%u fire calls: %u sent, %u no enemy, %u dead enemy",
                q2_cre_actions.thinks_run, q2_cre_actions.thinks_unbound,
                q2_cre_actions.calls_seen, q2_cre_actions.calls_unclassified,
                q2_cre_actions.fire_calls, q2_cre_actions.fire_sent,
                q2_cre_actions.fire_no_enemy, q2_cre_actions.fire_dead_enemy);

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
static void client_bind_view_model(client *c)
{
    const char *name;
    s32 index;

    c->vw_model_ready = false;
    q2_vw_set_model(&c->vw, NULL);

    if (!c->vm_ready || !c->model_bank_ready)
        return;

    name = q2_vw_model_name(&c->vw);
    if (!name || !name[0])
        return;

    index = q2_model_bank_find(&c->model_bank, name);
    if (index < 0) {
        Q2_DEBUG("no view model '%s' in %s", name, c->map);
        return;
    }

    if (q2_model_get(&c->model_bank, (u32)index, &c->vw_model) != Q2_OK)
        return;

    c->vw_model_ready = true;
    q2_vw_set_model(&c->vw, &c->vw_model);
    Q2_INFO("view weapon: %s", name);
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
    client_music_for_level(c, true);

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

/*
 * The film the front end opens a new game with, named as a bare literal at
 * QFRONT's module+0xDC4 because no movie-table record could hold the name.
 */
#define Q2_START_REEL "ROGUEINP.STX"

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

#define Q2_BOOT_SCREENS  (sizeof(k_boot_screens) / sizeof(k_boot_screens[0]))

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
    q2_menu_set_multiplayer(&c->menu, false);
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
        const q2_boot_screen *s = &k_boot_screens[c->boot_index++];

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
 * remainder, at the BUILD's rate — 50 on this PAL disc, which puts the two
 * screens at 7.1 s and 3.5 s.
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

    s = &k_boot_screens[c->boot_index - 1];
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

    s = &k_boot_screens[c->boot_index - 1];
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

    /*
     * A NEW GAME is the one thing that empties the mission table, and it is
     * the only thing that does on the console either: `0x8003D62C`'s restore
     * finds no "PlayerSave" block and takes the `memset(0x8009B550, 0, 150)`
     * at `0x8003DDB8`. See `client_mission_enter` for why there is no clear at
     * a unit boundary.
     */
    q2_mission_init(&c->mission);
    c->mission_row = -1;

    client_load_zone(c, c->first_map, 0);
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

    if (!client_film_start(c, Q2_START_REEL)) {
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
static void client_intermission_frame(client *c, float dt)
{
    u16 pad = client_menu_pad(c);
    bool cross = (pad & (Q2_PAD_CROSS | Q2_PAD_START)) != 0;

    (void)dt;

    /* An edge, so the press that opened the board cannot also close it. */
    if (cross && !c->popup_cross_prev) {
        if (c->endmis_open)       c->endmis_open   = false;
        else if (c->mission_open) c->mission_open  = false;
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

    if (c->mp_enabled && p > 0 && p < Q2_MP_MAX_PLAYERS && c->sim_ready[p])
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

    /*
     * In a split, each viewport is a different PLAYER, and until now every one
     * of them showed the same camera — the screen work was right and there was
     * only ever one thing to look at.
     *
     * Viewport 0 is player 0 and keeps the camera the frame built; the others
     * follow their OWN sim's eye and view angles. Each of players 1..3 has a
     * q2_sim of its own, spawned at its own MultiSpawn and advanced on its own
     * pad every frame, so a split shows four people walking about rather than
     * one camera reflected.
     *
     * What is still shared and should not be is the WORLD: each instance owns a
     * copy of the map's items and its own script runtime, and only player 0's
     * is read or drawn. See openquestions #53 — the fix is pulling the player
     * out of q2_sim, which is a change to sim.c rather than to this caller.
     */
    if (c->mp_enabled && p > 0 && p < Q2_MP_MAX_PLAYERS && c->sim_ready[p]) {
        const q2_player *pl = &c->sim[0].player[p];

        c->cam.pos[0] = pl->pos[0];
        c->cam.pos[1] = pl->pos[1] - Q2_EYE_BASE;
        c->cam.pos[2] = pl->pos[2];
        c->cam.yaw    = pl->yaw;
        c->cam.pitch  = pl->pitch;
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
        ectx.player        = 0;
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

        q2_entity_build_ot(&c->sim[0].entities, &ectx, &c->cam, ot, gte, &estats);
        /* Model entities are drawn through the same walk items are, so the
         * only way to tell a spawned Explosion from a silently-dropped one
         * is to count what the walk actually emitted. */
        c->ent_drawn    += estats.drawn;
        c->ent_no_model += estats.no_model;
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
            q2_coll_node  area_node;
            s32 cell = -1;
            bool posed = false;

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

            if (c->sim[0].coll_ready)
                cell = q2_coll_find_node(&c->sim[0].coll,
                                         m->pos, -1, true);
            if (cell >= 0 &&
                q2_collision_get_node(&c->sim[0].coll,
                                      (u32)cell, &area_node))
                inst.sort_area = area_node.contents & 0x7F;
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
                 * corridor was a silhouette. 0x80058944 stores "000" as the
                 * spawn default, i.e. 0x30 per component.
                 */
                static const u8 cre_glow[3] = { 0x30, 0x30, 0x30 };

                q2_light_gather(&set, &c->light_world, m->pos, cell, 0);
                q2_light_env_build(&cre_env, &set, Q2_LIGHT_ONE,
                                   Q2_LIGHT_ONE, cre_glow);
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
                static const u8 glow[3] = { 0x30, 0x30, 0x30 };

                q2_light_gather(&set, &c->light_world, at, cell, 0);
                q2_light_env_build(&env, &set,
                                   death->stage == Q2_PDEATH_FADING
                                       ? death->scale : Q2_LIGHT_ONE,
                                   Q2_LIGHT_ONE, glow);
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
    q2_fx_build_ot(&c->sim[0].fx, &c->cam, (u32)p, ot, gte);

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
    c->proj_prims += q2_projectiles_build_ot(&c->sim[0].combat.projectiles,
                                             c->sim[0].coll_primary_ready
                                                 ? &c->sim[0].coll_primary
                                                 : (c->sim[0].coll_ready
                                                     ? &c->sim[0].coll : NULL),
                                             &c->cam, ot, gte);

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
        int ammo = 0;

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

        if (weapon > 0 && weapon < Q2_WEAPON_COUNT) {
            s8 kind = q2_weapon_ammo[weapon];
            if (kind >= 0 && kind < Q2_AMMO_COUNT)
                ammo = inv->ammo[kind];
        }

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
         * The weapon strip's two slots. The console reads them out of a record
         * whose writer has not been traced, so what goes in is derived here:
         * the weapon before and the weapon after the one in hand, walking the
         * owned bitmask in slot order and wrapping. With only the blaster both
         * come out the same and the bar's own guard collapses them to one
         * icon, which is what capture shows.
         */
        {
            int cur = weapon, prev = weapon, next = weapon, k;

            for (k = 1; k < Q2_HUD_WEAPON_SLOTS; k++) {
                int lo = cur - k, hi = cur + k;

                while (lo < 1) lo += Q2_HUD_WEAPON_SLOTS - 1;
                while (hi >= Q2_HUD_WEAPON_SLOTS) hi -= Q2_HUD_WEAPON_SLOTS - 1;

                if (prev == cur && (inv->weapons & (1u << lo)))
                    prev = lo;
                if (next == cur && (inv->weapons & (1u << hi)))
                    next = hi;
            }
            bar->strip[0] = (u8)(prev > 0 ? prev : 0);
            bar->strip[1] = (u8)(next > 0 ? next : 0);
        }

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
         */
        if (bar_layout == Q2_SBAR_LAYOUT_ONE) {
            const char *pickup_name = NULL;
            u8          pickup_icon = 0;

            if (q2_item_pickup_caption(inv,
                                       c->sim[0].level_time,
                                       c->item_table_ready ? &c->item_table
                                                           : NULL,
                                       &pickup_icon, &pickup_name))
                q2_hud_pickup(&c->hud, pickup_name);
            else
                q2_hud_pickup(&c->hud, NULL);

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
                q2_hud_pickup_build_ot(&c->hud, &c->hud_font, &pctx, ot, 0);
            }
        } else {
            bar->pickup_icon = 0;
            q2_hud_pickup(&c->hud, NULL);
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
    if (c->vw_model_ready && c->death[0].linked_weapon &&
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

            at[0] = c->sim[0].player[0].pos[0];
            at[1] = q2_sim_origin_y(c->sim[0].player[0].pos[1]);
            at[2] = c->sim[0].player[0].pos[2];

            q2_light_gather(&set, &c->light_world, at,
                            client_light_node(c, p), c->vw.light_selector);
            q2_light_env_build(&vw_env, &set, c->vw.scale, c->vw.fade,
                               c->vw.glow);
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

            q2_sim_view_angles(&c->sim[0], summed);

            aim[0]  = (s16)c->sim[0].player[0].pitch;
            aim[1]  = (s16)c->sim[0].player[0].yaw;
            aim[2]  = (s16)c->sim[0].player[0].roll;
            kick[0] = (s16)(summed[0] - c->sim[0].player[0].pitch);
            kick[1] = (s16)(summed[1] - c->sim[0].player[0].yaw);
            kick[2] = (s16)(summed[2] - c->sim[0].player[0].roll);
        }

        q2_vw_build_ot(&c->vw, &proto,
                       c->sim[0].player[0].pos, c->sim[0].player[0].view_height,
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
}

static void client_frame(client *c)
{
    void *pixels;
    int pitch;
    const psx_framebuffer *front;
    q2_screen_hooks hooks;

    q2_screen_frame_begin(&c->screen, &c->ot);

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

    /*
     * What the water effect reads off view+288, published before the build
     * because the effect writes the shake and the build's draw envs read it.
     *
     * On the console this is not a publication at all: the viewport holds a
     * pointer to its player's entity and reads bit 0x100 of the flag word
     * itself. The port has no such pointer to hand the screen, so the one bit
     * it wants is handed over instead. Every viewport gets the same answer for
     * the same reason they all get the same camera — there is one player.
     */
    {
        bool submerged =
            (c->sim[0].player[0].ent.flags & Q2_ENT_UNDERWATER) != 0;
        int p;

        for (p = 0; p < c->screen.view_count; p++)
            q2_screen_water_set(&c->screen, p, true, submerged);
    }

    memset(&hooks, 0, sizeof(hooks));
    hooks.view = client_draw_view;
    hooks.user = c;
    q2_screen_build(&c->screen, &c->ot, &c->gte, &hooks);

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
    if (c->hud_ready && c->hud_font_ready && !c->in_front_end &&
        !c->menu.open && !c->mission_open && !c->mcard_open &&
        !c->endmis_open && !c->credits_open) {
        q2_hud_ctx ctx;

        c->hud.crosshair = (c->settings.v[Q2_SET_CROSSHAIR] != 0);
        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_build_ot(&c->hud, &c->hud_font, &ctx, &c->ot, 0);
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

    q2_screen_present(&c->screen);
    front = q2_screen_front(&c->screen);

    /*
     * The capture comes off the finished front buffer, before anything SDL
     * touches it — so a headless run and a windowed one write byte-identical
     * frames, and neither depends on a driver's idea of what a 15-bit texture
     * looks like.
     */
    if (c->shot_path && c->shot_every > 0 &&
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
         * SCREEN POSITION, honoured — openquestions #40.
         *
         * The page writes `0x800B3368` / `0x800B336A` (defaults 0 and 24) and
         * an exhaustive sweep finds **no reader anywhere in the executable**:
         * the obvious consumer would be the display env's screen rectangle,
         * which `SetDefDispEnv` explicitly zeroes. So on this build the page is
         * inert, and the port must not pretend otherwise about the CONSOLE.
         *
         * It can still do the honest thing for the player: a control that
         * exists and does nothing is a bug from the outside. The offset is
         * applied here, at presentation, where it shifts the finished image the
         * way a television's own position control would — and nowhere near the
         * ordering table, so it cannot perturb clipping or the viewport
         * rectangles that the reconstruction does depend on.
         *
         * The default y of 24 is treated as the neutral point, because that is
         * what the reset routine writes and a fresh install must not be
         * off-centre.
         */
        /*
         * THE PICTURE'S SHAPE, which is not the buffer's.
         *
         * The GPU's five horizontal modes all span the same active line, so a
         * 512-wide frame is the same picture as a 320-wide one with pixels half
         * as wide; PAL fills the 4:3 raster with 256 lines. That makes a
         * framebuffer pixel exactly 2:3, and blitting the buffer to fill the
         * window — which is what this did — a 1.5x horizontal stretch.
         *
         * q2_screen_fit_rect does the whole of it: the largest rectangle of the
         * right shape that fits, centred, with the rest of the window left as
         * border. It takes any window aspect, so a 16:9 monitor pillarboxes and
         * a tall window letterboxes without this having to know which.
         */
        SDL_FRect dst;
        int out_w = 0, out_h = 0;
        int px = 0, py = 0, pw = 0, ph = 0;
        float sx = (float)c->settings.v[Q2_SET_SCREEN_X];
        float sy = (float)(c->settings.v[Q2_SET_SCREEN_Y] - 24);

        SDL_GetCurrentRenderOutputSize(c->renderer, &out_w, &out_h);
        q2_screen_fit_rect(&c->screen, c->fit, out_w, out_h,
                           &px, &py, &pw, &ph);

        /*
         * SCREEN POSITION moves the picture, so its units are buffer pixels
         * scaled by the PICTURE's size and not by the window's — otherwise the
         * same setting would shift by a different amount depending on how much
         * of the window is border.
         */
        dst.x = (float)px + sx * (float)pw / (float)Q2_SCREEN_PAL_WIDTH;
        dst.y = (float)py + sy * (float)ph / (float)Q2_SCREEN_PAL_HEIGHT;
        dst.w = (float)pw;
        dst.h = (float)ph;

        SDL_RenderTexture(c->renderer, c->texture, NULL, &dst);
    }
    SDL_RenderPresent(c->renderer);
}

/* ------------------------------------------------------------------------- */
static void usage(void)
{
    printf("q2psx - native Quake II PSX\n\n");
    printf("usage: q2psx --disc <path> [--map NAME] [--zone N] [--scale N]\n"
           "             [--aspect MODE] [--saves DIR]\n\n");
    printf("  --disc   a .cue, .bin, .img or .iso, or a drive letter\n");
    printf("  --map    level directory name (default BASE0)\n");
    printf("  --zone   zone index within the map (default 0)\n");
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
    printf("  --movie NAME  play a film from Q2DATA/MOVIES and nothing else\n"
           "                (TAKE1BP.STX, OUTRO1P.STX, ROGUEINP.STX)\n");
    printf("  --new-game    confirm a difficulty: the opening reel, then level 1\n");
    printf("  --boot        the logo screens and intro film before the menu\n");
    printf("  --no-boot     ...and skip them in a run that would show them\n");
    printf("  --frames N    stop after N frames\n");
    printf("  --shot P.ppm  write the console's own framebuffer to P.ppm\n");
    printf("  --shot-every N  ...and one every N frames, numbered\n");
    printf("  --powerup KIND hold quad|invuln|enviro|breather for HUD capture\n");
    printf("  --fire-event NAME [F]  queue ONE named Events entry point at\n"
           "                frame F, instead of every trigger volume on the\n"
           "                map. Level authors name the interesting ones\n"
           "                (BIGGUN: PLATFORM, DestroyGlass, GravTeleport)\n");
    printf("  --at X,Y,Z    stand here instead of at the zone's spawn point\n");
    printf("  --yaw N       ...facing this way (the engine's 0..4095)\n");
    printf("  --pitch N     ...and looking this far up or down\n");
    printf("  --zone-trace  log every zone gate, teleport and unexplained\n"
           "                jump in the player's position while you play\n");
    printf("  --zone-probe  ...and, without playing, where each of this map's\n"
           "                zone gates leads and whether it lands anywhere\n");
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

int main(int argc, char **argv)
{
    client c;
    const char *disc_path = NULL;
    const char *map = "BASE0";
    bool map_given = false;
    bool zone_probe = false;
    int zone_index = 0;
    int scale = 3;
    int i;
    u64 last;

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
    s16        mp_frags   = -2;
    s16        mp_minutes = -2;


    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc)       disc_path = argv[++i];
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) { map = argv[++i]; map_given = true; }
        else if (!strcmp(argv[i], "--zone") && i + 1 < argc)  zone_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless"))              c.headless = true;
        else if (!strcmp(argv[i], "--demo"))                  c.demo = true;
        else if (!strcmp(argv[i], "--watch"))                 c.watch = true;
        else if (!strcmp(argv[i], "--zone-probe"))            zone_probe = true;
        else if (!strcmp(argv[i], "--sort-data"))             c.use_sort = true;
        else if (!strcmp(argv[i], "--depth-sort"))            c.use_sort = false;
        else if (!strcmp(argv[i], "--no-autoswitch"))         c.no_autoswitch = true;
        else if (!strcmp(argv[i], "--god"))                   c.god = true;
        else if (!strcmp(argv[i], "--continues") && i + 1 < argc)
            c.continues = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ot-range") && i + 1 < argc) c.ot_range = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zone-trace"))            c.zone_trace = true;
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
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) q2_save_set_dir(argv[++i]);
        else if (!strcmp(argv[i], "--dm")) {
            c.mp_enabled = true;
            if (!map_given) { map = "MATRIX1"; map_given = true; }
        }
        else if (!strcmp(argv[i], "--dm-mode") && i + 1 < argc)
            mp_mode = (q2_mp_mode)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-players") && i + 1 < argc)
            mp_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-stage"))
            c.mp_stage = true;
        else if (!strcmp(argv[i], "--trace-cre") && i + 1 < argc)
            c.trace_cre = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-frags") && i + 1 < argc)
            mp_frags = (s16)atoi(argv[++i]);
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

    if (!disc_path) {
        usage();
        return 1;
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

    /*
     * The console's own framebuffer, brought up the way 0x800764DC brings it
     * up: 512 x 248 on PAL, read out of the executable rather than assumed.
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
    if (c.screen.disp.height_is_inferred)
        Q2_WARN("no NTSC framebuffer has been read out of an NTSC build; "
                "using PAL's 512x248");

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

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        disc_close(c.disc);
        return 1;
    }

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
     * console's pixel shape makes that — 1536 x 1116 at the default scale of 3,
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

        c.window = SDL_CreateWindow("Q2PSX-PC", ww, wh, SDL_WINDOW_RESIZABLE);
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
    q2_menu_set_multiplayer(&c.menu, false);

    /* The level-completion screen. Its counters are the sim's to fill; until
     * kills and secrets are tallied it honestly reads zero. */
    q2_mission_init(&c.mission);
    c.mission_row = -1;
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
     * The overlay, AFTER the tables it reads. This block used to sit above the
     * load, testing a flag that `memset(&c, 0, ...)` had just cleared, so
     * `q2_hud_init` never ran and `hud_ready` never became true — the client
     * drew no notifications, no centre line and no crosshair, on a build whose
     * tables load perfectly well.
     */
    if (c.hud_tables_ready) {
        /* One player, so four notification lines — the table at 0x8009D648
         * indexed by player count (hudtables.h). */
        q2_hud_init(&c.hud, &c.hud_tables, 1);
        c.hud.crosshair = (c.settings.v[Q2_SET_CROSSHAIR] != 0);
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
    if (c.mp_enabled) {
        s16 frag = (mp_frags != -2)
                       ? mp_frags
                       : q2_mp_frag_options[Q2_MP_FRAG_OPTION_DEFAULT];
        s16 time = (mp_minutes != -2)
                       ? mp_minutes
                       : (mp_mode == Q2_MP_VERSUS
                              ? Q2_MP_NO_LIMIT
                              : q2_mp_time_options[Q2_MP_TIME_OPTION_DEFAULT]);

        client_mp_configure(&c, mp_mode, mp_players, frag, time,
                            q2_mp_round_options[Q2_MP_ROUND_OPTION_DEFAULT]);
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

    /* A static question about the map, asked and answered without playing it. */
    if (zone_probe) {
        client_zone_probe(&c, map);
        goto done;
    }

    if (!client_load_zone(&c, map, zone_index)) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
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
                switch (ev.key.key) {
                case SDLK_ESCAPE:
                    /* START on the console: it opens the pause menu, and
                     * closes it again from the root page. Deeper in, the
                     * menu's own TRIANGLE handling owns going back. The
                     * mission screen sits in front of all of that and takes
                     * the press first. */
                    if (c.mcard_open)
                        client_card_close(&c);
                    else if (c.mission_open)
                        c.mission_open = false;
                    else if (!c.menu.open) {
                        /*
                         * The pause page's KILLS/SECRETS row, which was dead
                         * code: `q2_menu_set_stats` implements the disc's own
                         * format string at 0x800AB30C and had no caller
                         * anywhere in src/, so `m->status` was always empty and
                         * menudraw's `if (m->status[0])` never fired. The
                         * numbers are the ones the level tally already keeps.
                         */
                        client_menu_fill_stats(&c);
                        q2_menu_open(&c.menu);
                    }
                    else if (c.menu.depth == 0)
                        client_menu_close(&c);
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
                 */
                if (down && c.film_open) {
                    client_film_stop(&c);
                } else if (down && c.boot_open) {
                    client_boot_skip(&c);
                } else if (down && c.credits_open) {
                    c.credits_open = false;
                    q2_menu_open(&c.menu);
                    q2_menu_goto(&c.menu, Q2_PAGE_FRONT_TITLE);
                } else if (down && ev.button.button == SDL_BUTTON_RIGHT &&
                           c.mission_open) {
                    /* The back gesture, on the one screen in front of the menu
                     * that Esc also dismisses. */
                    c.mission_open = false;
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

        if (c.boot_open) {
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
        } else if (c.sim_enabled) {
            client_input_simulated(&c, dt);
        } else {
            client_input(&c, dt);
        }

        /* The button edges the menu's click handling is built on, taken after
         * everything that reads them. */
        c.mouse_left_prev  = c.mouse_left;
        c.mouse_right_prev = c.mouse_right;

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

        /* A zone gate fired somewhere in the script: another zone of the same
         * map. Deferred to here because the gate fires inside the tick, and
         * the load frees the triggers the runtime is standing in. */
        {
            u32 target;
            if (q2_sim_take_zone_change(&c.sim[0], &target)) {
                /*
                 * A GATE TO THE ZONE WE ARE ALREADY IN IS NOT A GATE.
                 * 0x80079178 makes the same test before it stores the request,
                 * and without it a volume in the middle of zone 0 that names
                 * "Zone0" reloads the zone every time the player walks through
                 * it — which reads as the level restarting under you.
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

        /*
         * A queued TELEPORT: switch zone first when the target is in another
         * one, then place — which is the order userfuncs.c records.
         */
        if (c.pending_teleport_have) {
            q2_start_pos sp = c.pending_teleport;
            s32 to[3];

            c.pending_teleport_have = false;
            c.move_reason           = "TELEPORT primitive";

            if (c.zone_trace) {
                const q2_player *pl = &c.sim[0].player[c.sim[0].cur_player];
                Q2_WARN("[zone] f%-6u TELEPORT  '%s' zone %d -> (%d,%d,%d)"
                        "  from (%d,%d,%d) in zone %d",
                        c.trace_frame, sp.name, (int)sp.zone, sp.x, sp.y, sp.z,
                        pl->pos[0], pl->pos[1], pl->pos[2], c.zone_index);
            }

            if (sp.zone != c.zone_index) {
                c.carry_player   = true;
                c.carry_same_map = true;
                client_load_zone(&c, c.map, sp.zone);
            }

            to[0] = sp.x; to[1] = sp.y; to[2] = sp.z;
            q2_sim_spawn(&c.sim[0], to, sp.angle);
            c.cam.pos[0] = sp.x;
            c.cam.pos[1] = sp.y;
            c.cam.pos[2] = sp.z;
            c.cam.yaw    = sp.angle;
            Q2_INFO("teleported to '%s' in zone %d", sp.name, (int)sp.zone);
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
                Q2_INFO("tally: mission %d complete — %d level%s, "
                        "secrets %d/%d, kills %d/%d",
                        c.mission.unit, rows, rows == 1 ? "" : "s",
                        rs, rst, rk, rkt);
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
        /* The end-of-mission placard. */
        if (c.endmis_open) {
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

        if (c.mission_after_map) {
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
            if (c.headless && c.mission_frames >= Q2_INTERMISSION_HEADLESS)
                c.mission_open = false;

            if (!c.mission_open) {
                c.mission_after_map = false;
                c.unit_over         = false;
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
        if (c.give_weapon > 0 && c.give_weapon < Q2_WEAPON_COUNT) {
            q2_inventory *inv = &c.sim[0].combat.inv;
            const q2_weapon_tables *wt = q2_weapon_tables_builtin();
            int a;

            for (a = 1; a < Q2_WEAPON_COUNT; a++)
                inv->weapons |= wt->owned_bit[a];
            for (a = 0; a < Q2_AMMO_COUNT; a++)
                inv->ammo[a] = q2_inventory_ammo_max(inv, (q2_ammo)a);

            if (c.sim[0].combat.weapon_id != c.give_weapon) {
                c.sim[0].combat.weapon_id = c.give_weapon;
                if (c.vm_ready) {
                    q2_vw_select(&c.vw, c.give_weapon);
                    client_bind_view_model(&c);
                }
            }
        }
        /*
         * The music countdown, on the console's own 50 Hz. At zero the engine
         * moves to the next playlist entry (0x80071A58) rather than waiting for
         * the stream to run out, which is what makes a duration a restart point
         * and not a length.
         */
        if (c.music_open && c.music_total > 0) {
            c.music_clock += dt * 50.0;
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
    if (c.shot_path && (c.shot_every <= 0 || c.shots_written == 0)) {
        c.frame_index--;
        client_write_shot(&c, false);
    }

done:
    client_boot_free(&c);
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
    SDL_Quit();
    disc_close(c.disc);
    return 0;
}
