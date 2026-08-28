#include "screen.h"

#include "trig.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */
/*
 * The layout functions all halve a dimension with the compiler's signed
 * shift-and-correct idiom (`sra 16; srl 31; addu; sra 1`), which is C's
 * division truncating toward zero. Reproduced literally so that an odd width —
 * the 255 of the vertical split — halves the way the console halves it.
 */
static s16 half(s16 v)
{
    return (s16)(v / 2);
}

/*
 * `(w << 9) / divisor`, the field at view+282. The divisor is the framebuffer
 * HEIGHT for the two full-screen layouts and the framebuffer WIDTH for the
 * three split ones — that asymmetry is in the code, not a transcription slip
 * (0x8007761C and 0x80077E0C read 0x800B2DA2; 0x80077824, 0x80077A30 and
 * 0x80077C2C read 0x800B2DA0).
 */
static s16 aspect_of(s16 w, u16 divisor)
{
    if (divisor == 0)
        return 0;
    return (s16)(((s32)w << 9) / (s32)divisor);
}

/* ------------------------------------------------------------------------- */
/* Tables the bring-up fills                                                  */
/* ------------------------------------------------------------------------- */
/*
 * 0x80078378, called from 0x800765A0. The loop writes `(i-1) << 5` into slots
 * 1..4 and then stores 32 into slot 0, so slot 0 and slot 2 agree — which is
 * why an unset selector blends additively rather than at half strength.
 */
const u16 q2_screen_blend_word[Q2_SCREEN_BLEND_SLOTS] = { 32, 0, 32, 64, 96 };

u16 q2_screen_blend_bits(unsigned sel)
{
    return q2_screen_blend_word[sel < Q2_SCREEN_BLEND_SLOTS ? sel : 0];
}

psx_blend q2_screen_blend_mode(unsigned sel)
{
    return (psx_blend)((q2_screen_blend_bits(sel) >> 5) & 3);
}

bool q2_screen_blend_defined(unsigned sel)
{
    return sel < Q2_SCREEN_BLEND_SLOTS;
}

/*
 * The meter's nine bars: column, colour, and which accumulators the build
 * clears. Columns are the immediates at 0x80076EC8, 0x80076F1C, 0x80076F7C,
 * 0x80076FD8, 0x80077030, 0x80077088, 0x800770EC, 0x80077144 and 0x8007719C;
 * colours are the words at 0x800AECA0 onward, which SetTile leaves in the
 * packet's code byte and the port reads as r,g,b.
 */
const s16 q2_screen_meter_x[Q2_SCREEN_METER_BARS] = {
    20, 23, 28, 31, 34, 37, 40, 43, 46
};

const u8 q2_screen_meter_rgb[Q2_SCREEN_METER_BARS][3] = {
    {   0, 255, 255 },   /* 0x800AECA0 */
    { 255, 255,   0 },   /* 0x800AECA4 */
    {   0,   0, 255 },   /* 0x800AECA8 */
    {   0, 255,   0 },   /* 0x800AECAC */
    { 255,   0,   0 },   /* 0x800AECB0 */
    { 255,   0, 255 },   /* 0x800AECB4 */
    { 255, 255, 255 },   /* 0x800AECB8 */
    { 255,   0, 255 },   /* 0x800AECB4 again, at 0x80077174 */
    { 255,   0, 255 }    /* 0x800AECBC, one past the block the port can read */
};

/*
 * Bars 0 and 1 hang from a negative origin — `-(gp+1612) - 3`, so -23 with the
 * default 20 — while the rest sit at `22 - (gp+1612)`. That is not a
 * transcription slip: the first two are differences between a start and an end
 * stamp and only become visible once they exceed 23 halved units.
 */
static s16 meter_bar_y(const q2_screen_meter *m, int bar)
{
    return (bar < 2) ? (s16)(-m->base_y - 3) : (s16)(22 - m->base_y);
}

/* Bar 6 is skipped entirely when its accumulator is zero (0x800770E4). */
static bool meter_bar_live(const q2_screen_meter *m, int bar)
{
    return bar != 6 || m->value[6] != 0;
}

const char *q2_screen_layout_name(q2_screen_layout l)
{
    switch (l) {
    case Q2_SCREEN_LAYOUT_ONE:         return "one";
    case Q2_SCREEN_LAYOUT_TWO_H:       return "two-horizontal";
    case Q2_SCREEN_LAYOUT_TWO_V:       return "two-vertical";
    case Q2_SCREEN_LAYOUT_QUAD:        return "quad";
    case Q2_SCREEN_LAYOUT_FULL_SINGLE: return "full-single";
    default:                           return "?";
    }
}

/* ------------------------------------------------------------------------- */
/* Layouts                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Every one of these is a transcription of one setup function. The shared tail
 * — geometry offset at half the viewport, the aspect divide, the clear colour
 * copied out of gp+1604 and the isbg out of gp+18732 — is factored out here
 * because all five do it identically; everything that differs is spelled out at
 * the call site so it can be read against the disassembly.
 */
static void view_common(q2_screen *s, q2_screen_view *v, u16 aspect_divisor)
{
    v->ofs_x = half(v->w);
    v->ofs_y = half(v->h);
    v->aspect = aspect_of(v->w, aspect_divisor);

    v->bg_rgb[0] = s->disp.bg_rgb[0];
    v->bg_rgb[1] = s->disp.bg_rgb[1];
    v->bg_rgb[2] = s->disp.bg_rgb[2];
    v->bg_enable = s->disp.bg_enable;

    v->shake_x = 0;
    v->shake_y = 0;

    /*
     * The layouts do NOT set view+144; a view gets `|= 7` when its player's
     * camera is installed (0x800380AC, 0x8003848C), and until then bit 0 is
     * clear and the world draw is skipped. The port has no player object at
     * this layer, so it stands in for that here and says so — a caller that
     * models the player can clear the bit back and the viewport will draw its
     * env, its clear and its flash with no world in it, exactly as the console
     * does between a layout being installed and a camera existing.
     */
    v->flags = Q2_SCREEN_VIEW_LIVE;
}

/*
 * 0x80077454's second half — the display envs, cross paired so that the env
 * `PutDispEnv` is handed (the one belonging to the buffer being DRAWN) shows
 * the other buffer's rectangle. It is not part of any layout: the bring-up
 * calls it once (0x80076598) and a session start calls it again (0x8007CACC)
 * alongside installing the layout, which is what puts the pairing back after
 * the boot layout has flattened both envs to (0,0).
 */
static void display_envs_cross(q2_screen *s)
{
    s->disp.env[0].x = (s16)s->disp.width;   /* SetDefDispEnv(db0+10940, 512, 0, ..) */
    s->disp.env[0].y = 0;
    s->disp.env[1].x = 0;                    /* SetDefDispEnv(db1+10940,   0, 0, ..) */
    s->disp.env[1].y = 0;
}

/* 0x80077D0C — one player, the whole framebuffer, double buffered. */
static void layout_one(q2_screen *s)
{
    q2_screen_view *v = &s->view[0];

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;               s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    v->x = 0;  v->y = 0;
    v->w = (s16)s->disp.width;
    v->h = (s16)s->disp.height;
    v->vw = v->w; v->vh = v->h;
    v->kick_scale = 8192;              /* 0x80077D9C */
    v->proj   = 160;                    /* 0x80077E50 */
    v->far_z  = 6400;                   /* 0x80077E58 */
    v->sbar_x  = 93;                     /* 0x80077E60 */
    v->sbar_y  = 201;                    /* 0x80077E6C */
    view_common(s, v, s->disp.height);

    s->view_count = 1;
}

/*
 * 0x80077540 — the boot-time full-screen view. Two things make it its own
 * layout rather than a duplicate of ONE: the projection distance is 320 rather
 * than 160, and it leaves BOTH buffer origins at (0,0) and writes both display
 * envs to (0,0) as well, i.e. it is single buffered. That is the state the
 * front end runs in.
 */
static void layout_full_single(q2_screen *s)
{
    q2_screen_view *v = &s->view[0];

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0; s->disp.buf[0].y = 0;
    s->disp.buf[1].x = 0; s->disp.buf[1].y = 0;

    /*
     * 0x800776D4 / 0x800776F8: this is the only layout that calls
     * SetDefDispEnv, and it hands each buffer's env that buffer's OWN origin —
     * both (0,0) here. The cross pairing the boot function set up is undone,
     * which is precisely what makes this layout single buffered.
     */
    s->disp.env[0] = s->disp.buf[0];
    s->disp.env[1] = s->disp.buf[1];

    v->x = 0;  v->y = 0;
    v->w = (s16)s->disp.width;
    v->h = (s16)s->disp.height;
    v->vw = v->w; v->vh = v->h;
    v->kick_scale = 8192;              /* 0x800775C4 */
    v->proj   = 320;                    /* 0x8007766C */
    v->far_z  = 6400;                   /* 0x80077674 */
    v->sbar_x  = 93;                     /* 0x8007767C */
    v->sbar_y  = 201;                    /* 0x80077688 */
    view_common(s, v, s->disp.height);

    s->view_count = 1;
}

/*
 * 0x80077900 — two players stacked, which is what HORIZONTAL SPLIT selects.
 * Full framebuffer width; the height is `(fb_h + 1) / 2 - 1`, and the second
 * viewport starts at 121 rather than at 124, so the two overlap by three lines.
 * That overlap is in the constants (0x800779A0 and 0x80077ACC) and is kept.
 */
static void layout_two_h(q2_screen *s)
{
    int i;
    s16 h = (s16)(((s32)s->disp.height + 1) / 2 - 1);

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;                  s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    for (i = 0; i < 2; i++) {
        q2_screen_view *v = &s->view[i];
        v->w = (s16)s->disp.width;
        v->h = h;
        v->vw = 320;                    /* 0x800779BC — NOT the viewport size */
        v->vh = 160;                    /* 0x800779C4 */
        v->kick_scale = 6144;          /* 0x80077A6C */
        v->proj  = 160;                 /* 0x80077A24 */
        v->far_z = 6400;                /* 0x80077A28 */
        v->sbar_x = 0;
        v->sbar_y = 95;                  /* 0x80077A98 */
        view_common(s, v, s->disp.width);
    }

    s->view[0].x = 0;  s->view[0].y = 1;    /* 0x80077AC4 / 0x80077AC8 */
    s->view[1].x = 0;  s->view[1].y = 121;  /* 0x80077AD0 / 0x80077AD4 */

    s->view_count = 2;
}

/* 0x80077AEC — two players side by side. Width is `fb_w / 2 - 1`. */
static void layout_two_v(q2_screen *s)
{
    int i;
    s16 w = (s16)(half((s16)s->disp.width) - 1);

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;                  s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    for (i = 0; i < 2; i++) {
        q2_screen_view *v = &s->view[i];
        v->w = w;
        v->h = (s16)s->disp.height;
        v->vw = v->w; v->vh = v->h;
        v->kick_scale = 6144;          /* 0x80077C68 */
        v->proj  = 175;                 /* 0x80077C18 */
        v->far_z = 6400;                /* 0x80077C20 */
        v->sbar_x = 0;
        v->sbar_y = (s16)(s->disp.height - 32);  /* 0x80077C9C */
        view_common(s, v, s->disp.width);
    }

    s->view[0].x = 0;   s->view[0].y = 0;   /* memset at 0x80077CE4 */
    s->view[1].x = 256; s->view[1].y = 0;   /* 0x80077CF0 / 0x80077CF4 */

    s->view_count = 2;
}

/*
 * 0x8007771C — the 2x2 split, used for both three and four players. Note the
 * one-pixel inset: the quadrants start at 1 and 257 / 1 and 124, not at 0.
 */
static void layout_quad(q2_screen *s)
{
    int i;
    static const s16 qx[4] = { 1, 257, 1, 257 };
    static const s16 qy[4] = { 1, 1, 124, 124 };

    memset(s->view, 0, sizeof(s->view));

    s->disp.buf[0].x = 0;                  s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width; s->disp.buf[1].y = 0;

    for (i = 0; i < 4; i++) {
        q2_screen_view *v = &s->view[i];
        v->w = 256;                     /* 0x80077794 */
        v->h = 123;                     /* 0x8007779C */
        v->vw = v->w; v->vh = v->h;
        v->kick_scale = 6144;          /* 0x8007786C */
        v->proj  = 160;                 /* 0x80077810 */
        v->far_z = 4000;                /* 0x80077818 */
        v->sbar_x = 0;
        v->sbar_y = 0;
        view_common(s, v, s->disp.width);
        v->x = qx[i];
        v->y = qy[i];
    }

    s->view_count = 4;
}

/* 0x80077EAC — the overlay camera at 0x800D6870, run by every layout. */
static void layout_overlay(q2_screen *s)
{
    q2_screen_view *v = &s->overlay;

    memset(v, 0, sizeof(*v));

    v->x = 0; v->y = 0;
    v->w = (s16)s->disp.width;
    v->h = (s16)s->disp.height;
    v->vw = v->w; v->vh = v->h;
    v->kick_scale = 8192;              /* 0x80077EF8 */
    v->proj = 320;                      /* 0x80077F90 */
    view_common(s, v, s->disp.height);
    v->bg_enable = 0;                   /* 0x80077FB4 — the overlay never clears */
}

void q2_screen_set_layout(q2_screen *s, q2_screen_layout layout, int players)
{
    if (!s)
        return;

    /* A session start restores the cross pairing (0x8007CACC -> 0x80077454)
     * before it installs its layout; the boot layout is the one that undoes it
     * again, which it does itself. */
    if (layout != Q2_SCREEN_LAYOUT_FULL_SINGLE)
        display_envs_cross(s);

    layout_overlay(s);

    switch (layout) {
    case Q2_SCREEN_LAYOUT_ONE:         layout_one(s);         break;
    case Q2_SCREEN_LAYOUT_TWO_H:       layout_two_h(s);       break;
    case Q2_SCREEN_LAYOUT_TWO_V:       layout_two_v(s);       break;
    case Q2_SCREEN_LAYOUT_QUAD:        layout_quad(s);        break;
    case Q2_SCREEN_LAYOUT_FULL_SINGLE: layout_full_single(s); break;
    default:                           layout_one(s); layout = Q2_SCREEN_LAYOUT_ONE; break;
    }

    /*
     * Three players use the four-quadrant layout with the count overridden
     * afterwards (0x8003FAE4 calls 0x8007771C, which writes 4, then stores 3).
     * The fourth quadrant is simply never drawn into and shows the background.
     */
    if (layout == Q2_SCREEN_LAYOUT_QUAD && players == 3)
        s->view_count = 3;

    s->layout = layout;
}

q2_screen_layout q2_screen_layout_for(int players, bool horizontal_split)
{
    if (players <= 1)
        return Q2_SCREEN_LAYOUT_ONE;
    if (players == 2)
        return horizontal_split ? Q2_SCREEN_LAYOUT_TWO_H : Q2_SCREEN_LAYOUT_TWO_V;
    return Q2_SCREEN_LAYOUT_QUAD;
}

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                   */
/* ------------------------------------------------------------------------- */
q2_result q2_screen_init(q2_screen *s, q2_video_std video)
{
    q2_result r;

    if (!s)
        return Q2_ERR_INVALID_ARG;

    memset(s, 0, sizeof(*s));

    /*
     * 0x800764DC. SetVideoMode(1) then 512 and 248 into 0x800B2DA0/DA2. An NTSC
     * build's equivalents have not been read, and PAL's 248 already refuted the
     * commonly repeated 256 — so rather than substitute one folklore number for
     * another, an NTSC request gets the PAL geometry and says so.
     */
    s->disp.width         = Q2_SCREEN_PAL_WIDTH;
    s->disp.height        = Q2_SCREEN_PAL_HEIGHT;
    s->disp.video_mode    = 1;
    s->disp.field_hz      = 50;
    s->disp.vsync_divisor = 2;
    s->disp.height_is_inferred = (video != Q2_VIDEO_PAL);

    /* 0x8007657C..0x80076590: the draw/display indices and the three-slot
     * rotation the swap advances alongside them. */
    s->disp.draw_buffer = 0;
    s->disp.disp_buffer = 1;
    s->disp.rotate[0] = 0;
    s->disp.rotate[1] = 1;
    s->disp.rotate[2] = 2;

    /* 0x80077454 sets the display envs cross-paired, buffer 0 showing x = 512.
     * The background colour and its enable both start cleared: gp+1604 is four
     * zero bytes on disc and gp+18732 is in the zeroed segment. */
    s->disp.buf[0].x = 0;
    s->disp.buf[0].y = 0;
    s->disp.buf[1].x = (s16)s->disp.width;
    s->disp.buf[1].y = 0;
    display_envs_cross(s);
    s->disp.bg_rgb[0] = s->disp.bg_rgb[1] = s->disp.bg_rgb[2] = 0;
    s->disp.bg_enable = 0;

    /*
     * 0x800765A0 fills the blend table, and the boot GTE state is
     * SetGeomOffset(width/2, height/2) with SetGeomScreen(gp+1608), whose
     * halfword on disc is 160 — the same projection distance the one-player
     * layout uses, not the 320 the boot layout will overwrite it with.
     */
    s->meter.enable = false;
    s->meter.base_y = Q2_SCREEN_METER_BASE_Y;

    /* Neither clear flag is set at boot: 0x800B2D94 and gp+18712 are both in
     * the zeroed segment, so until something arms them each viewport clears its
     * own rectangle — and its isbg comes from gp+18732, which is zero too. */
    s->suppress_clear    = false;
    s->background_enable = false;

    r = psx_fb_init(&s->buf[0], s->disp.width, s->disp.height);
    if (r != Q2_OK)
        return r;
    r = psx_fb_init(&s->buf[1], s->disp.width, s->disp.height);
    if (r != Q2_OK) {
        psx_fb_free(&s->buf[0]);
        return r;
    }

    /* The state the front end boots into (0x8006DFB8 -> 0x80077540). */
    q2_screen_set_layout(s, Q2_SCREEN_LAYOUT_FULL_SINGLE, 1);

    s->exit_code = Q2_SCREEN_EXIT_NONE;
    s->dt        = Q2_SCREEN_DT_NOMINAL;

    /* The single-player size of the "Water Moves" pool. A multiplayer session
     * doubles it (0x80062CF8) and is the only thing that changes it. */
    s->water_moves      = Q2_SCREEN_WATER_MOVES;
    s->water_moves_used = 0;
    return Q2_OK;
}

void q2_screen_free(q2_screen *s)
{
    if (!s)
        return;
    psx_fb_free(&s->buf[0]);
    psx_fb_free(&s->buf[1]);
}

void q2_screen_buffer_origin(const q2_screen *s, int index, int *x, int *y)
{
    if (!s || index < 0 || index > 1) {
        if (x) *x = 0;
        if (y) *y = 0;
        return;
    }
    if (x) *x = s->disp.buf[index].x;
    if (y) *y = s->disp.buf[index].y;
}

/* ------------------------------------------------------------------------- */
/* The shape of a pixel                                                       */
/* ------------------------------------------------------------------------- */
/*
 * See the header for why this is not read out of the executable. The ratio is
 * `4 * lines : 3 * width` reduced, which for PAL's 512-wide buffer against a
 * 256-line picture is exactly 2:3.
 */
static int gcd_int(int a, int b)
{
    while (b) { int t = a % b; a = b; b = t; }
    return a ? a : 1;
}

void q2_screen_pixel_aspect(const q2_screen *s, int *num, int *den)
{
    int lines, n, d, g;

    if (!s || s->disp.width == 0) {
        if (num) *num = 1;
        if (den) *den = 1;
        return;
    }

    /*
     * An NTSC request gets PAL's geometry with `height_is_inferred` set, and the
     * same caveat applies here: nothing has been read out of an NTSC build, so
     * the 240 is the standard's, not this game's. It is used anyway because the
     * alternative — pretending a PAL pixel shape is right for NTSC — is worse,
     * and the flag is what tells a caller not to trust it.
     */
    lines = (s->disp.video_mode == 1) ? Q2_SCREEN_LINES_4_3_PAL
                                      : Q2_SCREEN_LINES_4_3_NTSC;

    n = 4 * lines;
    d = 3 * (int)s->disp.width;
    g = gcd_int(n, d);

    if (num) *num = n / g;
    if (den) *den = d / g;
}

const char *q2_screen_fit_name(q2_screen_fit fit)
{
    switch (fit) {
    case Q2_SCREEN_FIT_TELEVISION: return "television";
    case Q2_SCREEN_FIT_SQUARE:     return "square";
    case Q2_SCREEN_FIT_FULL_4_3:   return "4:3";
    case Q2_SCREEN_FIT_STRETCH:    return "stretch";
    default:                       return "?";
    }
}

/*
 * The picture's aspect as a rational `wn : wd`, so the fit below can be done in
 * integers and cannot drift on a window size that ought to land exactly.
 */
static void picture_aspect(const q2_screen *s, q2_screen_fit fit,
                           int *wn, int *wd)
{
    int pn = 1, pd = 1;

    switch (fit) {
    case Q2_SCREEN_FIT_SQUARE:
        break;                    /* pixels stay 1:1 */
    case Q2_SCREEN_FIT_TELEVISION:
        q2_screen_pixel_aspect(s, &pn, &pd);
        break;
    case Q2_SCREEN_FIT_FULL_4_3:
    default:
        *wn = 4; *wd = 3;
        return;
    }

    *wn = (int)s->disp.width  * pn;
    *wd = (int)s->disp.height * pd;
}

void q2_screen_fit_rect(const q2_screen *s, q2_screen_fit fit,
                        int win_w, int win_h,
                        int *x, int *y, int *w, int *h)
{
    int wn, wd, ow, oh;

    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;

    if (!s || s->disp.width == 0 || s->disp.height == 0 ||
        fit == Q2_SCREEN_FIT_STRETCH) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = win_w;
        if (h) *h = win_h;
        return;
    }

    picture_aspect(s, fit, &wn, &wd);

    /*
     * Fit by whichever axis runs out first. Both products are done in 64 bits
     * because a 4K window against a 512-wide picture is only 2^31 away from
     * overflowing the obvious int form, and a caller is entitled to pass a
     * window size this module did not choose.
     */
    if ((s64)win_w * wd <= (s64)win_h * wn) {
        ow = win_w;                                  /* pillar-free, letterbox */
        oh = (int)(((s64)win_w * wd) / wn);
    } else {
        oh = win_h;                                  /* letterbox-free, pillar */
        ow = (int)(((s64)win_h * wn) / wd);
    }

    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;

    if (x) *x = (win_w - ow) / 2;
    if (y) *y = (win_h - oh) / 2;
    if (w) *w = ow;
    if (h) *h = oh;
}

void q2_screen_window_size(const q2_screen *s, q2_screen_fit fit, int scale,
                           int *w, int *h)
{
    int wn, wd, ww, wh;

    if (scale < 1)
        scale = 1;

    if (!s || s->disp.width == 0 || s->disp.height == 0) {
        if (w) *w = 512 * scale;
        if (h) *h = 248 * scale;
        return;
    }

    /*
     * `scale` counts buffer pixels across, so the width is exact and the height
     * is whatever makes the pixels the right shape. Stretching vertically rather
     * than squeezing horizontally is deliberate: the horizontal detail is what
     * the 512-wide mode was chosen for, and throwing a third of it away to get
     * the aspect right would be a strange trade.
     */
    ww = (int)s->disp.width * scale;

    if (fit == Q2_SCREEN_FIT_STRETCH) {
        if (w) *w = ww;
        if (h) *h = (int)s->disp.height * scale;
        return;
    }

    picture_aspect(s, fit, &wn, &wd);
    wh = (int)(((s64)ww * wd + wn / 2) / wn);
    if (wh < 1) wh = 1;

    if (w) *w = ww;
    if (h) *h = wh;
}

void q2_screen_view_fov(const q2_screen *s, const q2_screen_view *v,
                        q2_screen_fov *out)
{
    int pn = 1, pd = 1;
    double th, tv;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    if (!v || v->proj <= 0 || v->w <= 0 || v->h <= 0)
        return;

    /*
     * Half the viewport over the projection distance. The halving is the
     * geometry offset's, which every layout puts at the viewport's own centre,
     * so the frustum is symmetric and one tangent describes both sides.
     */
    th = (double)(v->w / 2) / (double)v->proj;
    tv = (double)(v->h / 2) / (double)v->proj;

    out->horizontal = 2.0 * atan(th) * (180.0 / 3.14159265358979323846);
    out->vertical   = 2.0 * atan(tv) * (180.0 / 3.14159265358979323846);

    q2_screen_pixel_aspect(s, &pn, &pd);
    out->display_aspect = ((double)v->w * pn) / ((double)v->h * pd);

    /* 1.0 would be an undistorted picture. This game is 1.5 everywhere. */
    if (out->display_aspect > 0.0 && tv > 0.0)
        out->squeeze = (th / tv) / out->display_aspect;
}

/* ------------------------------------------------------------------------- */
/* The frame                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * 0x80018214. Two things happen: the draw/display indices flip, and a separate
 * three-slot ring rotates by one. The ring is initialised to {0,1,2} at
 * 0x8007657C and is not the buffer index — it survives here because it is part
 * of the swap's observable state and dropping it would quietly change whatever
 * consumes it.
 */
static void screen_swap(q2_screen *s)
{
    u8 a = s->disp.rotate[0];
    u8 b = s->disp.rotate[1];
    u8 c = s->disp.rotate[2];
    u8 previous;

    s->disp.rotate[0] = b;
    s->disp.rotate[1] = c;
    s->disp.rotate[2] = a;

    previous            = s->disp.draw_buffer;
    s->disp.draw_buffer = (u8)(1 - previous);
    s->disp.disp_buffer = previous;
}

void q2_screen_frame_begin(q2_screen *s, psx_ot *ot)
{
    if (!s)
        return;

    screen_swap(s);
    s->frame++;

    s->background_armed = false;
    s->overlay_armed    = false;
    memset(s->env_linked, 0, sizeof(s->env_linked));

    /* 0x80063480 re-points the cursor at the start of this buffer's pool, which
     * is the same thing as handing the frame a fresh allowance. */
    s->water_moves_used = 0;

    if (ot) {
        psx_ot_clear(ot);
        psx_ot_set_window(ot, 0, 0);
    }
}

void q2_screen_background(q2_screen *s)
{
    int i;

    if (!s)
        return;

    s->background_armed    = true;
    s->background_enable   = true;

    /*
     * 0x800780C0 clears every viewport's isbg immediately after arming the
     * full-screen env, so the frame is cleared once rather than once per
     * viewport. Without this the per-viewport clears would run *after* the
     * full-screen one and erase nothing visible, but the split-screen gutters
     * would then be the only thing the full-screen clear owns — which is
     * precisely what it is for.
     *
     * The loop bound is the LIVE viewport count (0x80078178 loads 0x800B2C2C),
     * not the four records — so the fourth quadrant of a three-player split
     * keeps whatever isbg it was left with, and never gets to use it because
     * nothing draws it.
     */
    for (i = 0; i < s->view_count && i < Q2_SCREEN_MAX_VIEWS; i++)
        s->view[i].bg_enable = 0;
}

/* ------------------------------------------------------------------------- */
/* What a full-screen tile blends with                                        */
/* ------------------------------------------------------------------------- */
/*
 * The damage flash and the water tint are both untextured `TILE` packets with
 * the semi-transparency bit set and NO texture page of their own — `SetTile`
 * writes a code byte and nothing else, and neither site adds a `DR_TPAGE`. On
 * hardware the blend mode for such a primitive comes from the texture-page
 * register, i.e. from whatever the last polygon drawn before it happened to
 * set, so the value is not statically determined and cannot simply be read out
 * of the executable.
 *
 * It can still be settled, by two arguments that agree:
 *
 *   - BOTH TILES FADE TO ZERO, and a fade has to be continuous at its ends. The
 *     flash's mode 2 scales its colour by `strength/initial`, so it finishes at
 *     (0,0,0) the frame before it stops being drawn; the water's tint starts at
 *     (0,0,0) the frame the amplitude leaves zero. Under `B/2 + F/2` a zero
 *     colour still halves the screen, so both effects would snap — a damage
 *     flash would end by dimming the picture and then popping back. Under
 *     `B + F` a zero colour is a no-op, and both ramps are smooth. The same
 *     holds for `B - F` and `B + F/4`, which is what makes this an argument
 *     against HALF rather than an argument for ADD on its own.
 *
 *   - WHAT IS ACTUALLY IN THE REGISTER at that point in the table is the last
 *     world polygon's blend, and the world's selector table makes an unset
 *     selector ADD rather than the half blend an unset field would suggest —
 *     see q2_screen_blend_word. So the ordinary case really is `B + F`.
 *
 * Between the three, only ADD gives the water a blue cast; `B - F` would take
 * blue *out* and leave the screen yellow, and `B + F/4` at (15,0,55) is too
 * faint to be worth ramping to over fourteen frames.
 *
 * This is a REASONED choice and not a transcribed one, which is why it is
 * written down here rather than left as a constant in two places.
 */
#define SCREEN_TILE_BLEND PSX_BLEND_ADD

/* ------------------------------------------------------------------------- */
/* The damage flash — 0x80076764                                              */
/* ------------------------------------------------------------------------- */
/*
 * One full-viewport semi-transparent tile at the front of the viewport's own
 * slice. The colour rule is a four-way switch on view+680 and the countdown is
 * one step per DRAWN frame, taken after the packet is linked (0x80076988) — so
 * a flash of strength 1 is drawn once at full strength and then stops.
 */
static void flash_emit(q2_screen_view *v, int p, psx_ot *ot)
{
    q2_screen_flash *f = &v->flash;
    psx_prim *prim;
    s32 frac;
    int i;

    if (!ot || f->strength == 0)
        return;

    /* `(strength << 16) / initial`, a 16.16 fraction. The original divides
     * without checking, and a zero divisor is a hardware trap rather than a
     * behaviour to reproduce, so the port declines the frame instead. */
    if (f->initial == 0)
        return;
    frac = ((s32)f->strength << 16) / (s32)f->initial;

    switch (f->mode) {
    case Q2_SCREEN_FLASH_SHIFT:
        /* 0x80076830: every channel is `b - (channel * frac >> 16)`. Using the
         * blue component as the base for all three is what the code does. */
        for (i = 0; i < 3; i++)
            f->out[i] = (u8)((s32)f->rgb[2] - (((s32)f->rgb[i] * frac) >> 16));
        break;

    case Q2_SCREEN_FLASH_FADE:
        for (i = 0; i < 3; i++)
            f->out[i] = (u8)(((s32)f->rgb[i] * frac) >> 16);
        break;

    case Q2_SCREEN_FLASH_SOLID:
        for (i = 0; i < 3; i++)
            f->out[i] = f->rgb[i];
        break;

    default:
        break;      /* mode 0 leaves the tile holding last frame's colour */
    }

    prim = psx_ot_add_bucket(ot, ((u32)Q2_SCREEN_OT_VIEW_BASE
                                  + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                                  + Q2_SCREEN_OT_VIEW_FLASH) * PSX_OT_SUBDIV);
    if (prim) {
        /* 0x80076950: the tile is viewport-local (0,0) and takes the viewport's
         * UNSHAKEN size — the shake is already in the draw env's offset. */
        prim->kind = PSX_PRIM_TILE;
        prim->xy[0].x = 0;
        prim->xy[0].y = 0;
        prim->xy[2].x = v->w;
        prim->xy[2].y = v->h;
        prim->rgb[0].r = f->out[0];
        prim->rgb[0].g = f->out[1];
        prim->rgb[0].b = f->out[2];
        prim->semi_transparent = true;   /* `ori 2` into the code at 0x800767A8 */
        prim->tpage = psx_make_tpage(0, 0, SCREEN_TILE_BLEND, PSX_TEX_4BIT);
    }

    f->strength--;
}

void q2_screen_flash_set(q2_screen *s, int p, const u8 rgb[3],
                         s16 strength, s16 mode)
{
    q2_screen_flash *f;

    if (!s || p < 0 || p >= Q2_SCREEN_MAX_VIEWS)
        return;

    f = &s->view[p].flash;
    if (rgb) {
        f->rgb[0] = rgb[0];
        f->rgb[1] = rgb[1];
        f->rgb[2] = rgb[2];
    }
    f->strength = strength;
    f->initial  = strength;
    f->mode     = mode;
}

/* ------------------------------------------------------------------------- */
/* The water effect — 0x80062DF0                                              */
/* ------------------------------------------------------------------------- */
/*
 * Everything here is that one function, in the order it runs it: ramp the
 * amplitude, derive the shake, advance the two phases, emit the column strips,
 * emit the row strips, and lay the tint over the lot.
 */

/*
 * The two amplitudes. `amp * 4095` is `(amp << 12) - amp` in the original, and
 * both divides are the compiler's truncate-toward-zero form — which for a
 * non-negative amplitude is just a shift, but is written out because the field
 * is signed and a caller may hand this a negative one.
 */
static s32 water_div_trunc(s32 v, s32 shift)
{
    /* `bgez; addiu (1<<shift)-1; sra shift` — C's `/` on a power of two. */
    if (v < 0)
        v += (1 << shift) - 1;
    return v >> shift;
}

void q2_screen_water_amplitudes(s16 amp, s32 *horizontal, s32 *vertical)
{
    s32 a = ((s32)amp << 12) - (s32)amp;      /* amp * 4095 */

    if (horizontal) *horizontal = water_div_trunc(a * 4, 12);
    if (vertical)   *vertical   = water_div_trunc(a * 2, 12);
}

/*
 * One strip offset: `(cos(phase) * amplitude) >> 24`, truncating toward zero,
 * plus the shake. The shift is 24 rather than 12 because the cosine is 1.3.12
 * and the amplitude has a second factor of 4096 folded into it.
 */
static s32 water_offset(u16 phase, s32 amplitude, s32 shake)
{
    s32 prod = (s32)((s64)q2_cos12((s32)(phase & 0xFFF)) * (s64)amplitude);

    if (prod < 0)
        prod += 0x00FFFFFF;
    return (prod >> 24) + shake;
}

/*
 * Where this viewport's rectangle is in VRAM, as the water effect computes it.
 *
 * 0x80062F94 tests the draw-buffer index and adds the framebuffer WIDTH when it
 * is set, rather than reading the buffer-origin table — the same shortcut the
 * overlay camera takes, and wrong in the same place. Under every session layout
 * the two agree; under the boot layout, where both origins are (0,0), the odd
 * frame's copies address the half of VRAM that layout never displays. The port
 * carries the difference rather than the constant, exactly as env_from_overlay
 * does, so the discrepancy survives and a copy that lands outside the buffer
 * does nothing.
 */
static s16 water_origin_x(const q2_screen *s, const q2_screen_view *v)
{
    s16 assumed = (s->disp.draw_buffer != 0) ? (s16)s->disp.width : 0;
    s16 actual  = s->disp.buf[s->disp.draw_buffer].x;

    return (s16)(v->x + (assumed - actual));
}

/*
 * One strip copy. Returns false when the pool is empty, which stops BOTH passes
 * rather than skipping one strip: 0x800630EC and 0x80063210 both branch past the
 * remainder of the loops straight to the tint.
 */
static bool water_move(q2_screen *s, psx_ot *ot, u32 bucket,
                       s16 sx, s16 sy, s16 dx, s16 dy, s16 w, s16 h)
{
    psx_prim *prim;

    if (s->water_moves_used >= s->water_moves)
        return false;
    s->water_moves_used++;

    prim = psx_ot_add_bucket(ot, bucket);
    if (!prim)
        return false;

    prim->kind = PSX_PRIM_MOVE;
    prim->xy[PSX_MOVE_SRC].x  = sx;
    prim->xy[PSX_MOVE_SRC].y  = sy;
    prim->xy[PSX_MOVE_DST].x  = dx;
    prim->xy[PSX_MOVE_DST].y  = dy;
    prim->xy[PSX_MOVE_SIZE].x = w;
    prim->xy[PSX_MOVE_SIZE].y = h;
    return true;
}

/*
 * The column pass — 0x80063014.
 *
 * `i` runs to `w` INCLUSIVE, and the last iteration is forced to emit whatever
 * run is open even if its offset has not changed; that is what closes the strip
 * against the viewport's right edge. `0x7FFFFFFF` is the "nothing open yet"
 * sentinel, and it works because the first offset can never be that value.
 */
static bool water_columns(q2_screen *s, const q2_screen_view *v, psx_ot *ot,
                          u32 bucket, u16 phase, s32 amp_v, s32 shake_y)
{
    s16 vx = water_origin_x(s, v);
    s16 vy = v->y;
    s32 prev_i = 0x7FFFFFFF;
    s32 prev_d = 0x7FFFFFFF;
    s32 i;

    for (i = 0; i <= (s32)v->w; i++, phase = (u16)((phase + Q2_SCREEN_WATER_COL_STEP) & 0xFFF)) {
        s32 d = water_offset(phase, amp_v, shake_y);

        if (d == prev_d && i != (s32)v->w)
            continue;

        if (i != 0) {
            s16 w = (s16)(i - prev_i);
            s16 h = (s16)(v->h - prev_d);

            /* Both zero tests are on the TRUNCATED halfword (`sll 16; beq`),
             * so a run 65536 columns long would be dropped. It cannot happen at
             * these viewport sizes; it is transcribed rather than tidied. */
            if (w != 0 && h != 0 &&
                !water_move(s, ot, bucket,
                            (s16)(vx + prev_i), (s16)(vy + prev_d),
                            (s16)(vx + prev_i), vy, w, h))
                return false;
        }

        prev_i = i;
        prev_d = d;
    }

    return true;
}

/* The row pass — 0x8006313C. The same shape with the axes exchanged, a phase
 * step of 13 rather than 20, and the larger of the two amplitudes. */
static bool water_rows(q2_screen *s, const q2_screen_view *v, psx_ot *ot,
                       u32 bucket, u16 phase, s32 amp_h, s32 shake_x)
{
    s16 vx = water_origin_x(s, v);
    s16 vy = v->y;
    s32 prev_i = 0x7FFFFFFF;
    s32 prev_d = 0x7FFFFFFF;
    s32 i;

    for (i = 0; i <= (s32)v->h; i++, phase = (u16)((phase + Q2_SCREEN_WATER_ROW_STEP) & 0xFFF)) {
        s32 d = water_offset(phase, amp_h, shake_x);

        if (d == prev_d && i != (s32)v->h)
            continue;

        if (i != 0) {
            s16 w = (s16)(v->w - prev_d);
            s16 h = (s16)(i - prev_i);

            if (w != 0 && h != 0 &&
                !water_move(s, ot, bucket,
                            (s16)(vx + prev_d), (s16)(vy + prev_i),
                            vx, (s16)(vy + prev_i), w, h))
                return false;
        }

        prev_i = i;
        prev_d = d;
    }

    return true;
}

/*
 * The tint — 0x80063270. A full-viewport semi-transparent TILE whose three
 * channels are the amplitude scaled by 15, 0 and 55 over 4096. It is added
 * LAST, which in a prepend list means it draws FIRST: the strips then displace
 * a picture that has already been tinted, rather than the tint sitting flat on
 * top of a wobbling one.
 */
static void water_tint(const q2_screen_view *v, psx_ot *ot, u32 bucket, s16 amp)
{
    psx_prim *prim = psx_ot_add_bucket(ot, bucket);
    s32 a = (s32)amp;

    if (!prim)
        return;

    prim->kind = PSX_PRIM_TILE;
    prim->xy[0].x = 0;
    prim->xy[0].y = 0;
    prim->xy[2].x = v->w;
    prim->xy[2].y = v->h;

    /* `(amp << 4) - amp` and `(((amp << 3) - amp) << 3) - amp`, both >> 12. */
    prim->rgb[0].r = (u8)water_div_trunc(a * Q2_SCREEN_WATER_TINT_R, 12);
    prim->rgb[0].g = (u8)Q2_SCREEN_WATER_TINT_G;
    prim->rgb[0].b = (u8)water_div_trunc(a * Q2_SCREEN_WATER_TINT_B, 12);

    prim->semi_transparent = true;      /* `ori 2` into the code at 0x80063280 */
    prim->tpage = psx_make_tpage(0, 0, SCREEN_TILE_BLEND, PSX_TEX_4BIT);
}

static void water_update(q2_screen *s, q2_screen_view *v, int p, psx_ot *ot)
{
    q2_screen_water *w = &v->water;
    s32 amp, amp_h, amp_v, shake_x, shake_y;
    u32 bucket;

    /* view+288 == 0: the whole function returns before the ramp, so a viewport
     * with no owner keeps its amplitude, its phases and its shake. */
    if (!w->owned)
        return;

    /* The ramp, in the halfword the original keeps it in. Both directions store
     * the unclamped sum first and then correct it, so the clamp is on the value
     * that has already wrapped. */
    amp = (s32)(s16)((u16)w->amp +
                     (u16)(w->submerged ? (s32)(Q2_SCREEN_WATER_AMP_RATE * s->dt)
                                        : -(s32)(Q2_SCREEN_WATER_AMP_RATE * s->dt)));
    if (w->submerged) {
        if (amp >= Q2_SCREEN_WATER_AMP_MAX + 1)
            amp = Q2_SCREEN_WATER_AMP_MAX;
    } else {
        if (amp < 0)
            amp = 0;
    }
    w->amp = (s16)amp;

    if (amp == 0) {
        /* 0x80062F0C: `memset(view+780, 0, 4)` with the two phase halfwords
         * zeroed in the delay slots. Leaving the water therefore also resets
         * the wave, so going back in starts it at the same place. */
        v->shake_x = 0;
        v->shake_y = 0;
        w->phase_row = 0;
        w->phase_col = 0;
        return;
    }

    q2_screen_water_amplitudes((s16)amp, &amp_h, &amp_v);
    shake_x = water_div_trunc(amp_h, 12);
    shake_y = water_div_trunc(amp_v, 12);

    v->shake_x = (s16)shake_x;
    v->shake_y = (s16)shake_y;

    /* The phases advance whether or not anything is emitted, and they advance
     * at different rates — 5 and 7 — so the two passes never lock together. */
    w->phase_row = (u16)(w->phase_row + Q2_SCREEN_WATER_ROW_RATE * s->dt);
    w->phase_col = (u16)(w->phase_col + Q2_SCREEN_WATER_COL_RATE * s->dt);

    if (!ot)
        return;

    bucket = ((u32)Q2_SCREEN_OT_VIEW_BASE
              + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
              + Q2_SCREEN_OT_VIEW_WATER) * PSX_OT_SUBDIV;

    /*
     * Columns, then rows, then the tint — and the tint is drawn even when the
     * pool ran dry partway through, because the overrun branch jumps past both
     * loops and lands on it.
     */
    if (water_columns(s, v, ot, bucket, w->phase_col, amp_v, shake_y))
        (void)water_rows(s, v, ot, bucket, w->phase_row, amp_h, shake_x);

    water_tint(v, ot, bucket, (s16)amp);
}

void q2_screen_water_set(q2_screen *s, int p, bool owned, bool submerged)
{
    if (!s || p < 0 || p >= Q2_SCREEN_MAX_VIEWS)
        return;

    s->view[p].water.owned     = owned;
    s->view[p].water.submerged = submerged;
}

/* ------------------------------------------------------------------------- */
/* The performance meter — 0x80076E88                                         */
/* ------------------------------------------------------------------------- */
static void meter_emit(q2_screen *s, psx_ot *ot)
{
    int i;

    if (!ot || !s->meter.enable)
        return;

    for (i = 0; i < Q2_SCREEN_METER_BARS; i++) {
        psx_prim *prim;
        s32 h;

        if (!meter_bar_live(&s->meter, i))
            continue;

        /* Every bar's height is its accumulator halved, rounded toward zero the
         * way the compiler's `srl 31; addu; sra 1` sequence does. */
        h = s->meter.value[i] / 2;

        prim = psx_ot_add_bucket(ot, (u32)Q2_SCREEN_OT_METER * PSX_OT_SUBDIV);
        if (!prim)
            break;

        prim->kind = PSX_PRIM_TILE;
        prim->xy[0].x = q2_screen_meter_x[i];
        prim->xy[0].y = meter_bar_y(&s->meter, i);
        prim->xy[2].x = (s16)(q2_screen_meter_x[i] + Q2_SCREEN_METER_WIDTH);
        prim->xy[2].y = (s16)(meter_bar_y(&s->meter, i) + h);
        prim->rgb[0].r = q2_screen_meter_rgb[i][0];
        prim->rgb[0].g = q2_screen_meter_rgb[i][1];
        prim->rgb[0].b = q2_screen_meter_rgb[i][2];
    }

    /*
     * 0x8007720C..0x80077220 zero six of the nine accumulators — the ones that
     * are summed over a frame rather than sampled as a pair of stamps — so a
     * bar shows this frame and not every frame so far.
     */
    s->meter.value[2] = 0;
    s->meter.value[3] = 0;
    s->meter.value[4] = 0;
    s->meter.value[5] = 0;
    s->meter.value[6] = 0;
    s->meter.value[8] = 0;
}

/* ------------------------------------------------------------------------- */
/* One viewport — 0x80076A74                                                  */
/* ------------------------------------------------------------------------- */
bool q2_screen_view_begin(q2_screen *s, int p, psx_ot *ot, gte_state *gte)
{
    q2_screen_view *v;
    q2_screen_ctx  *c;

    if (!s || p < 0 || p >= s->view_count)
        return false;

    v = &s->view[p];
    c = &s->ctx;

    /*
     * The water effect runs first because it WRITES the shake, and the shake is
     * read three lines further down and again by the draw env.
     *
     * On the console it is not part of the frame build at all — 0x80038460 runs
     * it during the per-view game update, which is earlier in the same loop
     * body and after the ordering table has been cleared. Running it here is
     * the same point in the frame for everything observable: the table it links
     * into is this frame's, the dt it ramps against is this frame's, and no
     * other writer of the shake exists to race with.
     */
    water_update(s, v, p, ot);

    /*
     * Everything up to the draw env is state the viewport publishes for the
     * renderers to read: the view record itself (0x800B2C24), its index
     * (0x800B2C1C), the shake lifted out of view+780 (0x800B2C34), the clip
     * extent with the shake already taken off it (0x800B2C20 / 0x800B2C22) and
     * the slice base (0x800B2D60).
     */
    c->view    = v;
    c->index   = p;
    c->overlay = false;
    c->shake_x = v->shake_x;
    c->shake_y = v->shake_y;
    c->clip_w  = (s16)(v->w - v->shake_x);
    c->clip_h  = (s16)(v->h - v->shake_y);
    c->slice_base = (u32)Q2_SCREEN_OT_VIEW_BASE
                  + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE;
    c->slice_len  = Q2_SCREEN_OT_VIEW_STRIDE;

    /* 0x80076B78 / 0x80076B90 — the GTE is reloaded per viewport, which is what
     * lets two players have different fields of view on the same frame. */
    if (gte)
        gte_set_projection(gte, (u16)v->proj, v->ofs_x, v->ofs_y);

    /*
     * 0x80076BAC: the far distance is parked whole, and a quarter of it beside
     * it. The `bgez`-guarded `addiu 3` before the `sra 2` is the compiler's
     * divide-by-four truncating toward zero, which is what C's `/ 4` already
     * does — writing the bias out again here would round a negative value twice.
     *
     * The quarter is the distance past which an entity is not drawn at all
     * (0x800698E0 and 0x800699A0); the whole value goes to the polygon emitter.
     */
    c->far_z = v->far_z;
    c->far_z_entities = c->far_z / 4;

    /* The flash is emitted before the world. A bucket is walked most-recently
     * added first, so being first in makes it last out — on top of everything
     * the world adds to the same bucket afterwards. */
    flash_emit(v, p, ot);

    /* Geometry starts one bucket past the slice's draw env.  SortData's
     * authored buckets still address the complete 51-entry slice. */
    if (ot) {
        psx_ot_set_authored_window(
            ot,
            (u32)Q2_SCREEN_OT_VIEW_BASE
                + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE,
            Q2_SCREEN_OT_VIEW_STRIDE);
        psx_ot_set_window(ot,
                          (u32)Q2_SCREEN_OT_VIEW_BASE
                              + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                              + Q2_SCREEN_OT_VIEW_ENV + 1u,
                          (u32)Q2_SCREEN_OT_VIEW_STRIDE
                              - (Q2_SCREEN_OT_VIEW_ENV + 1u));
    }

    return true;
}

void q2_screen_view_end(q2_screen *s, psx_ot *ot)
{
    if (!s || s->ctx.overlay)
        return;

    /*
     * 0x80076D44 links the env at slice + 4, i.e. slice bucket 1, AFTER the
     * world has linked its geometry. A bucket is a prepend list drawn head
     * first, so linking last is what makes the env execute first.
     *
     * The port applies envs while walking rather than carrying them as packets
     * — see q2_screen_compose — so what has to survive here is that the env
     * WAS linked. A viewport that never began has no env in the table and must
     * not have its clip installed.
     */
    if (s->ctx.index >= 0 && s->ctx.index < Q2_SCREEN_MAX_VIEWS)
        s->env_linked[s->ctx.index] = true;

    if (ot) {
        psx_ot_set_window(ot, 0, 0);
        psx_ot_set_authored_window(ot, 0, 0);
    }
}

void q2_screen_overlay_begin(q2_screen *s, psx_ot *ot, gte_state *gte)
{
    q2_screen_ctx *c;

    if (!s)
        return;

    s->overlay_armed = true;
    c = &s->ctx;

    /* 0x8007726C: the overlay publishes itself as viewport 0 — which is why the
     * index is a separate global from the view pointer. */
    c->view    = &s->overlay;
    c->index   = 0;
    c->overlay = true;

    /* 0x800772D8: the overlay copies w,h straight into the clip extent. There is
     * no shake in the overlay's path at all, so the HUD never shakes with the
     * world. */
    c->shake_x = 0;
    c->shake_y = 0;
    c->clip_w  = s->overlay.w;
    c->clip_h  = s->overlay.h;

    /* 0x80077344: the whole far value is parked, and unlike the viewport path
     * the quarter is NOT recomputed — the overlay inherits whatever the last
     * viewport left in 0x800B2CC8. */
    c->far_z = s->overlay.far_z;

    c->slice_base = Q2_SCREEN_OT_OVERLAY;
    c->slice_len  = Q2_SCREEN_OT_OVERLAY_LEN;

    if (gte)
        gte_set_projection(gte, (u16)s->overlay.proj,
                           s->overlay.ofs_x, s->overlay.ofs_y);

    if (ot) {
        psx_ot_set_authored_window(ot, Q2_SCREEN_OT_OVERLAY,
                                   Q2_SCREEN_OT_OVERLAY_LEN);
        psx_ot_set_window(ot,
                          (u32)Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_VIEW_ENV + 1u,
                          (u32)Q2_SCREEN_OT_OVERLAY_LEN
                              - (Q2_SCREEN_OT_VIEW_ENV + 1u));
    }
}

void q2_screen_overlay_end(q2_screen *s, psx_ot *ot)
{
    if (!s)
        return;

    /* `overlay_armed` is the equivalent flag; it is set when the camera comes
     * up because the original links this env unconditionally. */
    if (ot) {
        psx_ot_set_window(ot, 0, 0);
        psx_ot_set_authored_window(ot, 0, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* The frame build — 0x800780C0                                               */
/* ------------------------------------------------------------------------- */
void q2_screen_build(q2_screen *s, psx_ot *ot, gte_state *gte,
                     const q2_screen_hooks *hooks)
{
    int p;

    if (!s)
        return;

    /*
     * The clear decision, in the order the branches appear. Both armed paths
     * end by forcing every viewport's own clear off; the third leaves them
     * alone, which is the only way a per-viewport clear ever runs.
     */
    if (s->suppress_clear) {
        for (p = 0; p < s->view_count; p++)
            s->view[p].bg_enable = 0;
    } else if (s->background_enable) {
        q2_screen_background(s);
    }

    for (p = 0; p < s->view_count; p++) {
        if (!q2_screen_view_begin(s, p, ot, gte))
            continue;

        /* `if (view+144 & 1) draw the world` — 0x80076CE8. */
        if (hooks && hooks->view &&
            (s->view[p].flags & Q2_SCREEN_VIEW_DRAW_WORLD) != 0)
            hooks->view(hooks->user, s, p, ot, gte);

        q2_screen_view_end(s, ot);
    }

    /* 0x80018588 draws the meter between the frame build and PutDispEnv, into
     * viewport 0's frontmost bucket; from the table's point of view that is the
     * same place whether it is linked now or then. */
    meter_emit(s, ot);

    q2_screen_overlay_begin(s, ot, gte);
    if (hooks && hooks->overlay)
        hooks->overlay(hooks->user, s, ot, gte);
    q2_screen_overlay_end(s, ot);

    if (ot)
        psx_ot_set_window(ot, 0, 0);
}

u16 q2_screen_view_otz(const q2_screen *s, int p, u32 z)
{
    u32 usable;

    (void)s;
    if (p < 0 || p >= Q2_SCREEN_MAX_VIEWS)
        p = 0;

    /* The env packet occupies slice bucket 1, so geometry starts at 2 and the
     * slice has 51 - 2 usable buckets. Depth counts down from the far end. */
    usable = Q2_SCREEN_OT_VIEW_STRIDE - (Q2_SCREEN_OT_VIEW_ENV + 1);
    if (z >= usable)
        z = usable - 1;

    /* Real buckets out: a caller hands this straight to psx_ot_add_bucket,
     * which no longer scales. See PSX_OT_SUBDIV. */
    return (u16)((Q2_SCREEN_OT_VIEW_BASE + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                  + Q2_SCREEN_OT_VIEW_ENV + 1 + (usable - 1 - z))
                 * PSX_OT_SUBDIV);
}

u16 q2_screen_overlay_otz(const q2_screen *s, u32 z)
{
    u32 usable = Q2_SCREEN_OT_OVERLAY_LEN - (Q2_SCREEN_OT_VIEW_ENV + 1);

    (void)s;
    if (z >= usable)
        z = usable - 1;

    return (u16)((Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_VIEW_ENV + 1 + z)
                 * PSX_OT_SUBDIV);
}

/* ------------------------------------------------------------------------- */
/* Composition                                                                */
/* ------------------------------------------------------------------------- */
/*
 * One draw environment, as the walk below sees it: a clip rectangle inside the
 * buffer, a drawing offset, and an optional background fill. Built from a view
 * the way 0x80076A74 builds it, minus the VRAM buffer origin — this module's
 * framebuffer *is* the buffer, so the origin is already accounted for.
 */
typedef struct screen_env {
    s16  x, y, w, h;
    bool clear;
    u8   rgb[3];
} screen_env;

static void env_from_view(const q2_screen_view *v, screen_env *e)
{
    /*
     * 0x80076C18..0x80076C3C. The shake displaces the rectangle's origin and
     * shrinks its size by the same amount, so the far edge stays put and the
     * near edges bite into the viewport — a screen shake that never exposes
     * anything outside the viewport it belongs to.
     */
    e->x = (s16)(v->x + v->shake_x);
    e->y = (s16)(v->y + v->shake_y);
    e->w = (s16)(v->w - v->shake_x);
    e->h = (s16)(v->h - v->shake_y);
    e->clear  = (v->bg_enable != 0);
    e->rgb[0] = v->bg_rgb[0];
    e->rgb[1] = v->bg_rgb[1];
    e->rgb[2] = v->bg_rgb[2];
}

/*
 * The overlay's env, and the one place a buffer origin survives into the port.
 *
 * 0x80077350 does not consult the buffer-origin table the way the viewport draw
 * does: it tests the draw-buffer index and adds the FRAMEBUFFER WIDTH when it is
 * non-zero. Under any session layout that is the same number as buf[1].x, so
 * the two agree and nothing is observable. Under the boot layout, where both
 * origins are (0,0), it is 512 more than the viewport draw uses — so on every
 * odd frame the overlay is built into the half of VRAM the boot layout never
 * displays. The port reproduces the discrepancy rather than the constant, by
 * carrying the difference between the origin the overlay uses and the one the
 * buffer actually lives at.
 */
static void env_from_overlay(const q2_screen *s, screen_env *e)
{
    s16 assumed = (s->disp.draw_buffer != 0) ? (s16)s->disp.width : 0;
    s16 actual  = s->disp.buf[s->disp.draw_buffer].x;

    env_from_view(&s->overlay, e);
    e->x = (s16)(e->x + (assumed - actual));
}

static void env_apply(psx_framebuffer *fb, const screen_env *e,
                      psx_raster_opts *opts)
{
    opts->clip_x = e->x;
    opts->clip_y = e->y;
    opts->clip_w = e->w > 0 ? e->w : -1;
    opts->clip_h = e->h > 0 ? e->h : -1;
    opts->ofs_x  = e->x;
    opts->ofs_y  = e->y;

    if (e->clear && e->w > 0 && e->h > 0) {
        int py, px;
        int x1 = e->x + e->w;
        int y1 = e->y + e->h;
        u16 c  = psx_rgb555(e->rgb[0], e->rgb[1], e->rgb[2]);

        if (x1 > fb->width)  x1 = fb->width;
        if (y1 > fb->height) y1 = fb->height;

        for (py = e->y < 0 ? 0 : e->y; py < y1; py++)
            for (px = e->x < 0 ? 0 : e->x; px < x1; px++)
                fb->px[py * fb->width + px] = c;
    }
}

void q2_screen_compose(q2_screen *s, psx_ot *ot,
                       const psx_vram *vram, const psx_raster_opts *opts)
{
    psx_framebuffer *fb;
    psx_raster_opts  local;
    u32 buckets, b;
    int p;

    if (!s || !ot || !opts)
        return;

    /* The last viewport has no following psx_ot_area_clear to trigger retail's
     * final screen-area drain. DrawOTag is the other natural boundary. */
    psx_ot_flush_batches(ot);

    fb = &s->buf[s->disp.draw_buffer];
    if (!fb->px)
        return;

    local = *opts;
    local.clip_x = local.clip_y = local.clip_w = local.clip_h = 0;
    local.ofs_x  = local.ofs_y  = 0;

    /*
     * The table is subdivided (PSX_OT_SUBDIV), so the console's 217 entries are
     * 217 GROUPS of real buckets and the walk covers all of them. Clamping to
     * the console count here drew only the first group and left the screen
     * black — the structural packets and the geometry both live past it.
     */
    buckets = ot->bucket_count;
    if (buckets > (u32)Q2_SCREEN_OT_ENTRIES * PSX_OT_SUBDIV)
        buckets = (u32)Q2_SCREEN_OT_ENTRIES * PSX_OT_SUBDIV;

    for (b = 0; b < buckets; b++) {
        s32 idx;

        /*
         * The draw-env packets execute at the bucket they were linked into,
         * before that bucket's geometry: each is AddPrim'd after the geometry
         * that shares its bucket, and within a bucket the most recent link
         * draws first. Applying them here is that rule, not an approximation
         * of it.
         */
        if (b == (u32)Q2_SCREEN_OT_BACKGROUND * PSX_OT_SUBDIV &&
            s->background_armed) {
            screen_env e;
            e.x = 0;
            e.y = 0;
            e.w = (s16)s->disp.width;
            e.h = (s16)s->disp.height;
            e.clear  = true;                  /* isbg = 1 at 0x800769F4 */
            e.rgb[0] = s->disp.bg_rgb[0];
            e.rgb[1] = s->disp.bg_rgb[1];
            e.rgb[2] = s->disp.bg_rgb[2];
            env_apply(fb, &e, &local);
        }

        for (p = 0; p < s->view_count; p++) {
            u32 env_bucket = ((u32)Q2_SCREEN_OT_VIEW_BASE
                              + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                              + Q2_SCREEN_OT_VIEW_ENV) * PSX_OT_SUBDIV;
            if (b == env_bucket && s->env_linked[p]) {
                screen_env e;
                env_from_view(&s->view[p], &e);
                env_apply(fb, &e, &local);
            }
        }

        if (b == ((u32)Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_VIEW_ENV)
                     * PSX_OT_SUBDIV && s->overlay_armed) {
            screen_env e;
            env_from_overlay(s, &e);
            env_apply(fb, &e, &local);
        }

        for (idx = ot->bucket_head[b]; idx >= 0; idx = ot->next[idx]) {
            const psx_prim *prim = &ot->prims[idx];

            if (prim->kind == PSX_PRIM_DRAW_ENV) {
                int ep;

                /* The packet is inside one viewport slice. Its coordinates
                 * are relative to the unshaken viewport origin. */
                for (ep = 0; ep < s->view_count; ep++) {
                    u32 first = ((u32)Q2_SCREEN_OT_VIEW_BASE
                               + (u32)ep * Q2_SCREEN_OT_VIEW_STRIDE)
                              * PSX_OT_SUBDIV;
                    u32 last = first
                             + (u32)Q2_SCREEN_OT_VIEW_STRIDE * PSX_OT_SUBDIV;
                    if (b >= first && b < last) {
                        screen_env e;
                        e.x = (s16)(s->view[ep].x + prim->xy[0].x);
                        e.y = (s16)(s->view[ep].y + prim->xy[0].y);
                        e.w = prim->xy[1].x;
                        e.h = prim->xy[1].y;
                        e.clear = false; /* packet+24 is explicitly zero */
                        e.rgb[0] = e.rgb[1] = e.rgb[2] = 0;
                        env_apply(fb, &e, &local);
                        break;
                    }
                }
                continue;
            }
            psx_raster_prim(fb, prim, vram, &local);
        }
    }
}

void q2_screen_present(q2_screen *s)
{
    /*
     * PutDispEnv(db + 10940) then DrawOTag(db + 10984) at 0x800185AC and
     * 0x800185BC. The env that reaches the hardware belongs to the buffer that
     * has just been drawn, and it displays the *other* rectangle — the cross
     * pairing set up at 0x80077454. Here that reduces to: what was just drawn
     * is what is now shown.
     */
    if (!s)
        return;
    s->disp.disp_buffer = s->disp.draw_buffer;
}

bool q2_screen_single_buffered(const q2_screen *s)
{
    if (!s)
        return false;
    return s->disp.env[0].x == s->disp.env[1].x &&
           s->disp.env[0].y == s->disp.env[1].y;
}

void q2_screen_display_rect(const q2_screen *s, int *x, int *y, int *w, int *h)
{
    if (!s) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }

    /* PutDispEnv is handed the env of the buffer being drawn (0x800185AC takes
     * 0x800B2754), so this is that env's rectangle. */
    if (x) *x = s->disp.env[s->disp.draw_buffer].x;
    if (y) *y = s->disp.env[s->disp.draw_buffer].y;
    if (w) *w = s->disp.width;
    if (h) *h = s->disp.height;
}

const psx_framebuffer *q2_screen_front(const q2_screen *s)
{
    if (!s)
        return NULL;

    /* Both envs naming the same rectangle means the buffer being drawn is also
     * the one on the television. */
    if (q2_screen_single_buffered(s))
        return &s->buf[s->disp.draw_buffer];

    return &s->buf[s->disp.disp_buffer];
}

psx_framebuffer *q2_screen_back(q2_screen *s)
{
    if (!s)
        return NULL;
    return &s->buf[s->disp.draw_buffer];
}

double q2_screen_frame_period(const q2_screen *s)
{
    if (!s || s->disp.field_hz == 0)
        return 0.0;
    return (double)s->disp.vsync_divisor / (double)s->disp.field_hz;
}

s32 q2_screen_tick_dt(q2_screen *s, double seconds)
{
    s32 dt;

    if (!s)
        return Q2_SCREEN_DT_NOMINAL;

    /* 1/300 s units (§9.12). The console never sees a fractional field, but a
     * host frame loop does, so round rather than truncate. */
    dt = (s32)(seconds * 300.0 + 0.5);
    if (dt < 1)
        dt = 1;

    /* 0x800184B8: `slti 31` with 30 in the delay slot — a clamp, not an
     * average, so a long frame is simply lost time rather than a lurch. */
    if (dt >= Q2_SCREEN_DT_MAX + 1)
        dt = Q2_SCREEN_DT_MAX;

    s->dt = dt;
    return dt;
}
