#include "cmd_effects.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "effect.h"
#include "raster.h"
#include "fxtables.h"
#include "hudtables.h"
#include "ident.h"
#include "level.h"

static void report(void *user, const char *what, s64 got, s64 want)
{
    (void)user;
    if (want < 0)
        printf("  PROBLEM   %-44s got %lld\n", what, (long long)got);
    else
        printf("  PROBLEM   %-44s got %lld, want %lld\n", what,
               (long long)got, (long long)want);
}

static const char *abr_name(u16 abr)
{
    switch (abr & Q2_FX_ABR_MASK) {
    case Q2_FX_ABR_HALF:  return "B/2+F/2";
    case Q2_FX_ABR_ADD:   return "B+F";
    case Q2_FX_ABR_SUB:   return "B-F";
    default:              return "B+F/4";
    }
}

/* The GPU command byte, spelled out. */
static const char *code_name(u8 code)
{
    static char buf[24];
    const char *base;

    switch (code & 0x3Cu) {
    case 0x20: base = "F3";  break;
    case 0x24: base = "FT3"; break;
    case 0x28: base = "F4";  break;
    case 0x2C: base = "FT4"; break;
    case 0x30: base = "G3";  break;
    case 0x34: base = "GT3"; break;
    case 0x38: base = "G4";  break;
    case 0x3C: base = "GT4"; break;
    default:   base = "?";   break;
    }

    snprintf(buf, sizeof(buf), "%s%s%s", base,
             (code & 0x02u) ? "+abe" : "",
             (code & 0x01u) ? "+raw" : "");
    return buf;
}

static const char *preset_name(q2_fx_preset_id id)
{
    switch (id) {
    case Q2_FX_EXPLOSION: return "explosion";
    case Q2_FX_BLOOD:     return "blood";
    case Q2_FX_BFG_BURST: return "bfg burst";
    /* 0x800596B0 is the ITEM materialise burst, not a gib — the row this
     * printed as "gib" until the preset was renamed (effect.h). */
    case Q2_FX_ITEM_MATERIALISE: return "item materialise";
    /* 0x80028DC8 is DrawTelePorts' (EVENT.C), the per-frame column a
     * registered TELEPORT volume pumps out of itself — not "scripted". */
    case Q2_FX_TELEPORT:  return "teleport column";
    /* 0x8003E0C0 is GenerateWaterSplash's, not a spark: the only caller of
     * 0x8003E014 is the wade arm of the player's state think (effect.h). */
    case Q2_FX_WATER_SPLASH: return "water splash";
    case Q2_FX_LASER_END: return "laser end";
    default:              return "?";
    }
}

/* ------------------------------------------------------------------------- */
static void print_ramps(const q2_fx_tables *t)
{
    u32 i;

    printf("Colour ramps  (0x%08X, %u x %u bytes)\n\n",
           Q2_FXT_ADDR_RAMPS, Q2_FX_RAMP_COUNT, (unsigned)Q2_FX_RAMP_STRIDE);
    printf("  idx  addr        blend    prim      first        mid"
           "          last\n");

    for (i = 0; i < Q2_FX_RAMP_COUNT; i++) {
        const q2_fx_ramp *r = &t->ramp[i];
        u32 a = r->colour[0], m = r->colour[16], z = r->colour[31];

        printf("  %3u  0x%08X  %-7s  %-9s %3u,%3u,%3u  %3u,%3u,%3u"
               "  %3u,%3u,%3u\n",
               i, t->ramp_addr[i], abr_name(r->abr),
               code_name(q2_fx_colour_code(a)),
               q2_fx_colour_r(a), q2_fx_colour_g(a), q2_fx_colour_b(a),
               q2_fx_colour_r(m), q2_fx_colour_g(m), q2_fx_colour_b(m),
               q2_fx_colour_r(z), q2_fx_colour_g(z), q2_fx_colour_b(z));
    }

    printf("\n  id -> record (0x%08X): ", Q2_FXT_ADDR_RAMP_INDEX);
    for (i = 0; i < Q2_FX_RAMP_COUNT; i++)
        printf("%u%s", t->ramp_id_to_index[i],
               i + 1 == Q2_FX_RAMP_COUNT ? "" : " ");
    printf("\n  permutation of 0..%u : %s\n",
           Q2_FX_RAMP_COUNT - 1,
           t->ramp_index_is_permutation ? "yes" : "NO");

    /*
     * The ramp the effect system reads is indexed by AGE, so the entries a
     * given lifetime can reach are a window at the tail. Printing the window
     * makes the consequence visible: a 15-tick burst never sees the bright end.
     */
    printf("\n  reachable window by lifetime:  life 10 -> entries %u..%u,"
           "  life 15 -> %u..%u,  life 25 -> %u..%u\n",
           q2_fx_ramp_index_for_life(10), q2_fx_ramp_index_for_life(1),
           q2_fx_ramp_index_for_life(15), q2_fx_ramp_index_for_life(1),
           q2_fx_ramp_index_for_life(25), q2_fx_ramp_index_for_life(1));
}

static void print_beams(const q2_fx_tables *t)
{
    u32 i, f;

    printf("\nBeam styles  (0x%08X, %u x %u bytes)\n\n",
           Q2_FXT_ADDR_BEAM_STYLES, Q2_FX_BEAM_STYLE_COUNT,
           (unsigned)Q2_FX_BEAM_STYLE_STRIDE);
    printf("  idx  addr        prim       colour       reached by\n");

    for (i = 0; i < Q2_FX_BEAM_STYLE_COUNT; i++) {
        u32 c = t->beam[i].tube[0].colour[0];
        char who[32] = "";
        u32 k, n = 0;

        for (k = 0; k < Q2_FX_LASER_KIND_COUNT; k++) {
            if (t->laser[k].style == i) {
                char one[8];
                snprintf(one, sizeof(one), "%s%u", n++ ? "," : "", k);
                strncat(who, one, sizeof(who) - strlen(who) - 1);
            }
        }
        /* Styles 3 and 4 are not reached from the laser dispatcher — they are
         * reached from the TIMED beam list, style 3 by the BFG. Printing that
         * stops "(nothing)" from reading as "unused", which is what an earlier
         * pass concluded from this very table. */
        if (!n) {
            if (i == Q2_FX_TIMED_BEAM_STYLE)
                snprintf(who, sizeof(who), "BFG timed beam");
            else
                snprintf(who, sizeof(who), "timed beam (0x8004E9F4)");
        }

        printf("  %3u  0x%08X  %-9s  %3u,%3u,%3u  %s%s\n",
               i, Q2_FXT_ADDR_BEAM_STYLES + i * Q2_FX_BEAM_STYLE_STRIDE,
               code_name(q2_fx_colour_code(c)),
               q2_fx_colour_r(c), q2_fx_colour_g(c), q2_fx_colour_b(c),
               n ? "laser " : "", who);
    }

    printf("\n  hull faces (identical in every style):\n    tube    ");
    for (f = 0; f < Q2_FX_BEAM_TUBE_FACES; f++)
        printf("(%u,%u,%u,%u) ", t->beam[0].tube[f].v[0],
               t->beam[0].tube[f].v[1], t->beam[0].tube[f].v[2],
               t->beam[0].tube[f].v[3]);
    printf("\n    cap near ");
    for (f = 0; f < Q2_FX_BEAM_CAP_FACES; f++)
        printf("(%u,%u,%u,%u) ", t->beam[0].cap_near[f].v[0],
               t->beam[0].cap_near[f].v[1], t->beam[0].cap_near[f].v[2],
               t->beam[0].cap_near[f].v[3]);
    printf("\n    cap far  ");
    for (f = 0; f < Q2_FX_BEAM_CAP_FACES; f++)
        printf("(%u,%u,%u,%u) ", t->beam[0].cap_far[f].v[0],
               t->beam[0].cap_far[f].v[1], t->beam[0].cap_far[f].v[2],
               t->beam[0].cap_far[f].v[3]);
    printf("\n");
}

static void print_lasers(const q2_fx_tables *t)
{
    u32 i;

    printf("\nLaser kinds  (dispatch 0x%08X)\n\n", Q2_FXT_ADDR_LASER_JUMP);
    printf("  kind  arm         radius  style  ramp  damage  mod\n");

    for (i = 0; i < Q2_FX_LASER_KIND_COUNT; i++) {
        const q2_fx_laser_kind *k = &t->laser[i];
        printf("  %4u  0x%08X  %6d  %5u  %4u  %6d  %3d\n",
               i, t->laser_jump[i], k->radius, k->style, k->ramp,
               k->damage, k->mod);
    }
}

/* ------------------------------------------------------------------------- */
/*
 * The runtime, with no screen.
 *
 * Every claim here is one an implementation error would break: the pool fills
 * and then refuses; a group's quads separate over time; the ramp entry the
 * group reads advances with its age; a laser queues one beam and eight bursts;
 * the beam hull closes; the debris burst biases upward.
 */
static u32 exercise(const q2_fx_tables *t)
{
    q2_fx_world w;
    q2_rng rng;
    u32 bad = 0, i;

    q2_fx_world_init(&w, t);
    q2_rng_seed(&rng, 0x51A5E5u);

    printf("\nRuntime\n\n");
    printf("  pool           : %u groups, %u beams, %u debris\n",
           w.group_count, (unsigned)Q2_FX_BEAMS_MAX,
           (unsigned)Q2_FX_DEBRIS_MAX);
    printf("  quad budget    : %u for one viewport, %u for two, %u for four\n",
           q2_fx_budget(w.group_count, 1), q2_fx_budget(w.group_count, 2),
           q2_fx_budget(w.group_count, 4));

    /* The name column is 16 wide so "item materialise" fits it. */
    printf("\n  preset            count  life   size  shift  ramps   spawned"
           "  spread after 8 ticks\n");

    for (i = 0; i < Q2_FX_PRESET_COUNT; i++) {
        const q2_fx_preset *p = q2_fx_preset_at((q2_fx_preset_id)i);
        s32 at[3] = { 1000, 2000, 3000 };
        s32 slot;
        s32 lo[3], hi[3];
        u32 q;
        int k, tick;

        q2_fx_world_clear(&w);
        slot = q2_fx_spawn(&w, &rng, (q2_fx_preset_id)i, at, 0);

        if (slot < 0) {
            printf("  %-16s  FAILED TO SPAWN\n", preset_name((q2_fx_preset_id)i));
            bad++;
            continue;
        }

        for (tick = 0; tick < 8; tick++)
            q2_fx_tick(&w);

        lo[0] = lo[1] = lo[2] =  0x7FFFFFFF;
        hi[0] = hi[1] = hi[2] = -0x7FFFFFFF;

        for (q = 0; q < w.group[slot].count; q++) {
            s32 pt[3];
            q2_fx_group_point(&w.group[slot], q, pt);
            for (k = 0; k < 3; k++) {
                if (pt[k] < lo[k]) lo[k] = pt[k];
                if (pt[k] > hi[k]) hi[k] = pt[k];
            }
        }

        printf("  %-16s  %5u  %4u  %5d  %5u  %2u,%-2u  %7d  %d x %d x %d\n",
               preset_name((q2_fx_preset_id)i), p->count,
               w.group[slot].life + 8, w.group[slot].size, p->spread_shift,
               p->ramp0, p->ramp1, slot,
               hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);

        /* A burst that has not separated after eight ticks is one whose
         * relative velocities were not stored, which is the single easiest
         * thing to get wrong in the spawner. */
        if (hi[0] - lo[0] == 0 && hi[1] - lo[1] == 0 && hi[2] - lo[2] == 0) {
            printf("  PROBLEM   %-44s\n", "burst did not separate");
            bad++;
        }
    }

    /* --- the pool refuses when full ------------------------------------- */
    {
        s32 at[3] = { 0, 0, 0 };
        u32 made = 0, tries, want;
        const q2_fx_preset *sp = q2_fx_preset_at(Q2_FX_WATER_SPLASH);
        u32 rep = (sp && sp->repeat) ? sp->repeat : 1u;

        /*
         * ONE q2_fx_spawn IS NOT ONE GROUP. Six of the eight spawn sites sit
         * inside an outer loop and the preset's `repeat` column carries the
         * bound; the splash's is four (`slti v0, s2, 4` at 0x8003E0DC). This
         * check used to assume one and read a correct pool as broken the
         * moment the loop was implemented.
         */
        tries = w.group_count / rep + 8u;
        want  = w.group_count / rep;

        q2_fx_world_clear(&w);
        for (i = 0; i < tries; i++) {
            if (q2_fx_spawn(&w, &rng, Q2_FX_WATER_SPLASH, at, 0) >= 0)
                made++;
        }
        printf("\n  pool refuses when full : %u of %u calls accepted"
               " (%u groups each, pool %u)%s\n",
               made, tries, rep, w.group_count,
               made == want ? "" : "   <-- WRONG");
        if (made != want)
            bad++;
    }

    /* --- the ramp entry advances with age -------------------------------- */
    {
        s32 at[3] = { 0, 0, 0 };
        s32 slot;
        u32 first, later;

        q2_fx_world_clear(&w);
        slot = q2_fx_spawn(&w, &rng, Q2_FX_EXPLOSION, at, 0);
        first = q2_fx_group_colour(&w.group[slot], 0);
        for (i = 0; i < 5; i++)
            q2_fx_tick(&w);
        later = q2_fx_group_colour(&w.group[slot], 0);

        printf("  ramp advances with age : %3u,%3u,%3u -> %3u,%3u,%3u%s\n",
               q2_fx_colour_r(first), q2_fx_colour_g(first),
               q2_fx_colour_b(first), q2_fx_colour_r(later),
               q2_fx_colour_g(later), q2_fx_colour_b(later),
               first == later ? "   <-- WRONG" : "");
        if (first == later)
            bad++;
    }

    /* --- a laser queues one beam and eight bursts ------------------------ */
    {
        s32 from[3] = { 0, 0, 0 }, to[3] = { 4000, 0, 0 };
        q2_fx_laser_result res;
        u32 kind;

        printf("\n  laser  beam  bursts  damage  mod\n");
        for (kind = 0; kind < Q2_FX_LASER_KIND_COUNT; kind++) {
            q2_fx_world_clear(&w);
            if (!q2_fx_laser(&w, &rng, kind, from, to, 0,
                             Q2_FX_LASER_END_FROM | Q2_FX_LASER_END_TO,
                             &res)) {
                printf("  %5u  UNKNOWN KIND\n", kind);
                bad++;
                continue;
            }
            printf("  %5u  %4s  %6u  %6d  %3d%s\n", kind,
                   res.queued ? "yes" : "NO", res.groups, res.damage, res.mod,
                   (res.queued && res.groups == 2 * Q2_FX_LASER_END_GROUPS)
                       ? "" : "   <-- WRONG");
            if (!res.queued || res.groups != 2 * Q2_FX_LASER_END_GROUPS)
                bad++;
        }
    }

    /* --- the beam hull closes -------------------------------------------- */
    {
        q2_fx_beam b;
        s32 hull[Q2_FX_BEAM_VERTS][3];
        s32 max_r = 0;
        u32 v;

        memset(&b, 0, sizeof(b));
        b.from[0] = 0;    b.from[1] = 0; b.from[2] = 0;
        b.to[0]   = 8192; b.to[1]   = 0; b.to[2]   = 0;
        b.radius  = 64;

        if (!q2_fx_beam_hull(&b, hull)) {
            printf("\n  beam hull              : FAILED TO BUILD\n");
            bad++;
        } else {
            for (v = 0; v < Q2_FX_BEAM_VERTS; v++) {
                const s32 *anchor = (v < 6) ? b.from : b.to;
                s32 dy = hull[v][1] - anchor[1];
                s32 dz = hull[v][2] - anchor[2];
                s32 r2 = dy * dy + dz * dz;
                if (r2 > max_r)
                    max_r = r2;
                /* Every hull point must sit on its own ring's plane: the
                 * hexagon is perpendicular to the beam, so no point may drift
                 * along the beam's own axis. */
                if (hull[v][0] != anchor[0]) {
                    printf("  PROBLEM   hull vertex %u left the ring plane\n", v);
                    bad++;
                }
            }
            printf("\n  beam hull              : 12 points, max radius %d"
                   " (asked %d)\n", (s32)((max_r > 0) ? max_r : 0), 64 * 64);
        }

        /* A degenerate beam builds no hull, exactly as 0x8006350C bails. */
        b.to[0] = b.from[0];
        if (q2_fx_beam_hull(&b, hull)) {
            printf("  PROBLEM   %-44s\n", "degenerate beam built a hull");
            bad++;
        }
    }

    /* --- debris leaps ----------------------------------------------------- */
    {
        s32 bmin[3] = { -100, -100, -100 }, bmax[3] = { 100, 100, 100 };
        u32 made, up = 0;

        q2_fx_world_clear(&w);
        q2_fx_debris_register(&w, 7);
        made = q2_fx_debris_burst(&w, &rng, bmin, bmax, NULL, 24, 0);

        for (i = 0; i < Q2_FX_DEBRIS_MAX; i++) {
            if (w.debris[i].in_use && w.debris[i].vel[1] < 0)
                up++;
        }
        printf("  debris burst           : %u pieces, %u moving upward%s\n",
               made, up, (made && up == made) ? "" : "   <-- WRONG");
        if (!made || up != made)
            bad++;
    }

    /* --- the trail's band ------------------------------------------------- */
    {
        s16 z[9];
        u8  rgb[9][3];
        u8  tint[3] = { 255, 255, 255 };
        u32 peak = 0;
        s32 best = -1;

        for (i = 0; i < 9; i++)
            z[i] = (s16)(i * 2048);

        q2_fx_glint_shade(z, 9, tint, 8192, 3, rgb);
        for (i = 0; i < 9; i++) {
            if ((s32)rgb[i][0] > best) {
                best = rgb[i][0];
                peak = i;
            }
        }
        printf("  trail band             : peak at vertex %u of 9,"
               " brightness %d%s\n", peak, best, best > 0 ? "" : "   <-- WRONG");
        if (best <= 0)
            bad++;
    }

    return bad;
}

/* ------------------------------------------------------------------------- */
/*
 * One frame, drawn for real.
 *
 * Seven bursts in a row and three beams behind them, through the GTE, the
 * ordering table and the rasteriser the game uses. The backdrop is a gradient
 * rather than black on purpose: every ramp on this disc is drawn with ABE set,
 * so against black an additive quad and an opaque one look identical and a
 * blend-mode error would be invisible.
 */
static int draw_frame(const q2_fx_tables *t, const disc *d, const char *out_ppm)
{
    const int W = 512, H = 248;
    q2_fx_world w;
    q2_rng rng;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram;
    u32 emitted = 0, i;
    int y;

    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram)
        return 1;

    if (psx_ot_init(&ot, 256, 8192) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        free(vram);
        return 1;
    }

    gte_init(&gte);
    gte_set_projection(&gte, 256, W / 2, H / 2);

    memset(&cam, 0, sizeof(cam));
    cam.projection = 256;
    cam.far_z      = Q2_CAMERA_FAR_DEFAULT;

    q2_fx_world_init(&w, t);
    q2_rng_seed(&rng, 0xB1A5Eu);

    /* One of each preset, spread across the view and aged a little so the
     * ramp lookup is doing something. */
    for (i = 0; i < Q2_FX_PRESET_COUNT; i++) {
        s32 at[3];
        at[0] = -1400 + (s32)i * 460;
        at[1] = -260;
        at[2] = 1600;
        q2_fx_spawn(&w, &rng, (q2_fx_preset_id)i, at, 0);
    }
    for (i = 0; i < 4; i++)
        q2_fx_tick(&w);

    /* Three beams, one per reachable style, behind the bursts. */
    for (i = 0; i < 3; i++) {
        s32 a[3], b[3];
        a[0] = -3000;  a[1] = 300 + (s32)i * 260;  a[2] = 3200;
        b[0] =  3000;  b[1] = a[1];                b[2] = 3200;
        q2_fx_beam_add_style(&w, a, b, 40, 0, i);
    }

    for (y = 0; y < H; y++) {
        int x;
        for (x = 0; x < W; x++)
            fb.px[y * W + x] = psx_rgb555((u8)(24 + x / 12),
                                          (u8)(24 + y / 8), 48);
    }

    emitted = q2_fx_build_ot(&w, &cam, 0, &ot, &gte);

    /*
     * BIGGUN's real trail mesh, drawn from the disc rather than from a
     * hand-made one. This is the check the numbers cannot be: a mesh whose
     * split or stride was misread still decodes to plausible counts, and only
     * looks wrong.
     */
    {
        q2_buf buf;
        q2_common_file common;
        u32 trail = 0;

        if (disc_read_file(d, "Q2DATA/LEVELS/BIGGUN/COMMON.DAT", &buf)
            == Q2_OK) {
            if (q2_common_open(&common, &buf) == Q2_OK) {
                const dat_chunk *chunk = common.chunk[Q2_COMMON_GLINT_MOD];
                q2_fx_glint_mesh mesh;

                if (chunk && chunk->data &&
                    q2_fx_glint_mesh_decode(&mesh, chunk->data, chunk->size)) {
                    s32 at[3] = { -1500, -900, 2600 };
                    q2_fx_glint gl;
                    const dat_chunk *lb = common.chunk[Q2_COMMON_LEVEL_BIN];
                    q2_fx_glint_script script;
                    s32 angles[3] = { 0, 700, 0 };

                    /*
                     * The MULTI-BAND path, with the six bands BIGGUN's script
                     * writes. The single-band path is the same call with a zero
                     * count; this one is worth showing because it is the half
                     * that has two widths and a phase per band.
                     */
                    memset(&gl, 0, sizeof(gl));
                    gl.mesh       = mesh;
                    gl.ready      = true;
                    if (lb && q2_fx_glint_scan(&script, lb->data, lb->size) &&
                        script.bands_found) {
                        gl.scripted = true;
                        gl.band_count = script.band_count;
                        memcpy(gl.base_angle, script.angle, sizeof(gl.base_angle));
                        q2_rng_seed(&gl.script_rng, 0);
                        q2_fx_glint_refresh(&gl, angles);
                    }

                    trail = q2_fx_glint_draw(&gl, at, 700, &cam, &ot, &gte);
                }
                q2_common_close(&common);
            } else {
                q2_buf_free(&buf);
            }
        }
        emitted += trail;
        printf("\n  glint: %u faces of BIGGUN's GlintMod\n", trail);
    }

    psx_raster_opts_default(&opts);
    psx_raster_ot(&fb, &ot, vram, &opts);

    printf("  frame: %u primitives (%u quads, %u beam faces) -> %s\n",
           emitted, w.stats.quads_emitted, w.stats.beam_faces_emitted,
           out_ppm);

    if (psx_fb_write_ppm(&fb, out_ppm) != Q2_OK)
        fprintf(stderr, "cannot write %s\n", out_ppm);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    return emitted ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/*
 * The glint mesh, off the disc.
 *
 * `GlintMod` is optional and only one map of the forty-nine carries it, so this
 * sweeps them all: a decode that got the 864-byte split or the 8-byte vertex
 * stride wrong would either fail outright or produce indices past the end of
 * the vertex array, and both are reported rather than assumed away.
 */
static u32 check_glint_meshes(const disc *d)
{
    static const char *const k_maps[] = {
        "BASE0", "BASE1", "BASE2", "BASE3", "BIGGUN", "BOSS1", "BOSS2",
        "BUNK1", "CITY1", "CITY2", "CITY3", "COMPLEX", "CORE", "FACT1",
        "FACT2", "FACT3", "HANGAR1", "HANGAR2", "JAIL1", "JAIL2", "JAIL3",
        "LAB", "MINE1", "MINE2", "MINE3", "MINE4", "MINTRO", "POWER1",
        "POWER2", "SEWER1", "SPACE", "STRIKE", "TRAIN", "WARE1", "WARE2",
        "WASTE1", "WASTE2", "WASTE3"
    };
    u32 bad = 0, found = 0, i;

    printf("\nGlint mesh (`GlintMod`)\n\n");

    for (i = 0; i < sizeof(k_maps) / sizeof(k_maps[0]); i++) {
        char path[128];
        q2_buf buf;
        q2_common_file common;
        const dat_chunk *chunk;
        q2_fx_glint_mesh mesh;
        u32 f, max_index = 0;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", k_maps[i]);
        if (disc_read_file(d, path, &buf) != Q2_OK)
            continue;

        if (q2_common_open(&common, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        chunk = common.chunk[Q2_COMMON_GLINT_MOD];
        if (!chunk || !chunk->data) {
            q2_common_close(&common);
            continue;
        }

        found++;

        if (!q2_fx_glint_mesh_decode(&mesh, chunk->data, chunk->size)) {
            printf("  %-8s %6u bytes  DECODE FAILED\n", k_maps[i], chunk->size);
            bad++;
            q2_common_close(&common);
            continue;
        }

        for (f = 0; f < mesh.face_count * 4u; f++) {
            if (mesh.index[f] > max_index)
                max_index = mesh.index[f];
        }

        printf("  %-8s %6u bytes  %u faces, %u vertices, highest index %u"
               "  band %d..%d\n",
               k_maps[i], chunk->size, mesh.face_count, mesh.vert_count,
               max_index, mesh.vert[0][3],
               mesh.vert[mesh.vert_count - 1][3]);

        /*
         * The check that says the split was right: every index must land inside
         * the vertex array. Read at any other offset they would not.
         */
        if (max_index >= mesh.vert_count) {
            printf("  PROBLEM   %-44s got %u, want < %u\n",
                   "an index escapes the vertex array", max_index,
                   mesh.vert_count);
            bad++;
        }

        /* The band starts at 1024, which is the constant the renderer offsets
         * its centre by (0x800648EC) — the check that the fourth halfword is
         * the band and not padding. */
        if (mesh.vert[0][3] != 1024) {
            printf("  PROBLEM   %-44s got %d, want 1024\n",
                   "the first vertex's band coordinate", mesh.vert[0][3]);
            bad++;
        }

        q2_common_close(&common);
    }

    if (!found)
        printf("  no map on this disc carries one\n");
    else
        printf("\n  %u of %u maps carry a glint mesh\n", found,
               (u32)(sizeof(k_maps) / sizeof(k_maps[0])));

    /*
     * What turns a glint on. Nothing in the executable does — see effect.h —
     * so the flag must come from a level script, and BIGGUN's `LevelBin` is a
     * relocatable MIPS module rather than bytecode. Both halves are found here
     * by scanning it for the instruction pair, which is the check that the
     * claim in effect.h is about this disc rather than about a memory of it.
     */
    {
        q2_buf buf;
        q2_common_file common;
        u32 raises = 0, tests = 0;

        printf("\n  what raises the 0x04000000 glint flag:\n");

        if (disc_read_file(d, "Q2DATA/LEVELS/BIGGUN/COMMON.DAT", &buf)
            == Q2_OK) {
            if (q2_common_open(&common, &buf) == Q2_OK) {
                const dat_chunk *lb = common.chunk[Q2_COMMON_LEVEL_BIN];
                u32 off;

                for (off = 0; lb && lb->data && off + 12 <= lb->size;
                     off += 4) {
                    /* lw rX, 268(rY) ; lui rZ, 0x0400 ; or/and */
                    u32 a = q2_rd_u32(lb->data + off);
                    u32 b = q2_rd_u32(lb->data + off + 4);
                    u32 c = q2_rd_u32(lb->data + off + 8);

                    /* lw rX, 268(rY) — opcode 0x23, displacement 0x10C. */
                    if ((a >> 26) != 0x23u || (a & 0xFFFFu) != 268u)
                        continue;
                    /* lui rZ, 0x0400 — opcode 0x0F. The register lives in bits
                     * 16..20, so the opcode has to be masked off the top six
                     * bits rather than compared against the whole halfword. */
                    if ((b >> 26) != 0x0Fu || (b & 0xFFFFu) != 0x0400u)
                        continue;

                    if ((c & 0x3Fu) == 0x25u) {          /* or  = raise  */
                        raises++;
                        printf("    LevelBin +0x%05X  raise\n", off);
                    } else if ((c & 0x3Fu) == 0x24u) {   /* and = test   */
                        tests++;
                        printf("    LevelBin +0x%05X  test\n", off);
                    }
                }
                q2_common_close(&common);
            } else {
                q2_buf_free(&buf);
            }
        }

        printf("    BIGGUN's level script: %u raise, %u test\n", raises, tests);

        /*
         * And what the port reads back out of it. This is the whole mechanism
         * that makes the glint fire without executing the module: the script is
         * asked, in the same terms it was written in.
         */
        if (disc_read_file(d, "Q2DATA/LEVELS/BIGGUN/COMMON.DAT", &buf)
            == Q2_OK) {
            if (q2_common_open(&common, &buf) == Q2_OK) {
                const dat_chunk *lb = common.chunk[Q2_COMMON_LEVEL_BIN];
                q2_fx_glint_script s;

                if (lb && lb->data &&
                    q2_fx_glint_scan(&s, lb->data, lb->size)) {
                    printf("    q2_fx_glint_scan: raises at +0x%05X,"
                           " %u bands, phase %u\n",
                           s.raise_offset, s.band_count, s.phase);
                    if (s.band_count != Q2_FX_GLINT_BANDS ||
                        s.phase != Q2_FX_GLINT_PHASE_START || !s.bands_found ||
                        !s.lightning_found) {
                        printf("  PROBLEM   %-44s\n",
                               "the scan did not recover the bands and lightning");
                        bad++;
                    }
                    if (s.bands_found) {
                        u32 band;
                        for (band = 0; band < s.band_count; band++)
                            printf("    band %u: %d, %d, %d\n", band,
                                   s.angle[band][0], s.angle[band][1],
                                   s.angle[band][2]);
                    }
                } else {
                    printf("  PROBLEM   %-44s\n",
                           "q2_fx_glint_scan found nothing");
                    bad++;
                }
                q2_common_close(&common);
            } else {
                q2_buf_free(&buf);
            }
        }

        printf("    target rule: entity +0xD2 == %u and +0xDA == %u,"
               " over %u records of %u bytes\n",
               Q2_FX_GLINT_TARGET_KIND, Q2_FX_GLINT_TARGET_ID,
               Q2_FX_GLINT_ENT_MAX, Q2_FX_GLINT_ENT_STRIDE);
        if (!raises) {
            printf("  PROBLEM   %-44s\n",
                   "nothing raises the glint flag on this disc");
            bad++;
        }
    }

    return bad;
}

/* ------------------------------------------------------------------------- */
int cmd_effects(const disc *d, const char *out_ppm)
{
    q2_build_id id;
    q2_exe exe;
    q2_fx_tables t;
    q2_result r;
    u32 bad = 0;

    if (!d)
        return 1;

    r = q2_identify(d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot identify the disc\n");
        return 1;
    }

    r = q2_exe_load(&exe, d, id.exe_name[0] ? id.exe_name : NULL);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load the executable: %s\n", q2_result_str(r));
        return 1;
    }

    r = q2_fx_tables_load(&t, &exe);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the effect tables: %s\n",
                q2_result_str(r));
        q2_exe_free(&exe);
        return 1;
    }

    printf("Effect tables, read from %s\n\n", id.exe_name);

    print_ramps(&t);
    print_beams(&t);
    print_lasers(&t);

    /*
     * The particle image. The page is `chars.lbm`'s and the palette is
     * built-in id 75 — `[0x800E3F2C + 150]`, where 0x800E3F2C is the CLUT-id
     * table the boot palette loop fills. Both are resolved out of the same HUD
     * tables the overlay uses, so this checks the record exists rather than
     * asserting a number.
     */
    {
        q2_hud_tables ht;

        printf("\nParticle image\n\n");

        if (q2_hud_tables_load(&ht, d, &id) == Q2_OK) {
            const q2_hud_palette *pal =
                q2_hud_palette_get(&ht, Q2_FX_CLUT_PALETTE_ID);

            printf("  page     : chars.lbm (the 8-pixel face, [0x800DDD5A])\n");
            printf("  uv patch : (%u,%u)..(%u,%u), 16x16\n",
                   Q2_FX_QUAD_U0, Q2_FX_QUAD_V0, Q2_FX_QUAD_U1, Q2_FX_QUAD_V1);
            printf("  palette  : built-in id %u ([0x800E3F2C + 150])%s\n",
                   Q2_FX_CLUT_PALETTE_ID, pal ? "" : "   <-- NOT IN THE BANK");
            if (!pal)
                bad++;
            else
                printf("  clut id  : 0x%04X\n", pal->clut_id);
        } else {
            printf("  the UI tables did not load for this build\n");
        }
    }

    printf("\nStructural checks\n\n");
    /*
     * Accumulated, not assigned. The particle-image check above counts a
     * missing palette into `bad`, and the assignment this used to be threw
     * that away — a disc with no CLUT record reported OK. The "all clear"
     * line still speaks only for the structural checks.
     */
    {
        u32 structural = q2_fx_tables_check(&t, &exe, report, NULL);

        if (!structural)
            printf("  all clear\n");
        bad += structural;
    }

    bad += exercise(&t);
    bad += check_glint_meshes(d);

    if (out_ppm)
        bad += (u32)draw_frame(&t, d, out_ppm);

    printf("\n%s\n", bad ? "FAILED" : "OK");

    q2_exe_free(&exe);
    return bad ? 1 : 0;
}
