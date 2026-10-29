/*
 * launcher.c — the disc chooser shown before the game. See launcher.h.
 *
 * A disc-source page in the game's own window. The software UI surface is
 * presented through the same GL display that gameplay takes over. Artwork is
 * local, and the original menu font is decoded from the player's own disc.
 */
#include "launcher.h"

#include "cddrive.h"
#include "disc.h"
#include "ident.h"
#include "launchcfg.h"
#include "version.h"
#include "menufont.h"
#include "controller_ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* ------------------------------------------------------------------------- */
/* Layout, fitted as one canvas so drawing and hit testing cannot diverge.    */
/* ------------------------------------------------------------------------- */
#define L_W         960.0f
#define L_H         600.0f
#define L_MARGIN     48.0f
#define L_LIST_W    560.0f
#define L_LIST_Y    252.0f
#define L_LIST_H    192.0f
#define L_CARD_H     60.0f
#define L_CARD_GAP    4.0f
#define L_BUTTON_Y  492.0f
#define L_BUTTON_W  270.0f
#define L_BUTTON_H   44.0f

#define L_MAX_SOURCES 48
#define L_MAX_PADS    8
#define L_DRIVE_POLL_MS 2500u
#define L_FILE_H 32.0f
#define L_FILE_ROWS 6
/* The disc font was authored for a 4:3 display of the 512 x 248 PAL buffer. */
#define L_FONT_Y (3.0f * 512.0f / (4.0f * 248.0f))

/* ------------------------------------------------------------------------- */
/* Oxidised metal from the cover; the PSX menu's blue selection light.        */
/* ------------------------------------------------------------------------- */
#define C_BG_TOP      0x0D1010
#define C_BG_BOTTOM   0x080A0C
#define C_CARD        0x141A1C
#define C_CARD_HOVER  0x1D282D
#define C_BORDER      0x394246
#define C_ACCENT      0xBA8B58
#define C_ACCENT_HI   0xE9BF80
#define C_TEXT        0xEBE5D7
#define C_TEXT_2      0xA7B9BD
#define C_TEXT_3      0x7C898C
#define C_ERROR       0xF38B6B

typedef enum src_kind {
    SRC_IMAGE,
    SRC_DRIVE
} src_kind;

typedef enum src_state {
    SRC_READY,          /* identified as Quake II PSX                       */
    SRC_UNCHECKED,      /* a drive with a disc in it, read when played       */
    SRC_NO_DISC,        /* an empty drive                                    */
    SRC_NOT_Q2,         /* a disc, but not this game                         */
    SRC_UNREADABLE      /* not a disc image at all                           */
} src_state;

typedef struct src_entry {
    src_kind  kind;
    src_state state;
    bool      last;                     /* the launcher's saved choice      */
    char      path[Q2_LAUNCH_PATH_MAX];
    char      title[64];
    char      note[96];                 /* the second line, for a drive     */
    char      tag[48];                  /* "SLES-01534 - PAL"               */
} src_entry;

enum { BTN_NONE, BTN_BROWSE, BTN_PLAY };

typedef struct file_entry {
    char name[Q2_LAUNCH_PATH_MAX];
    bool directory;
} file_entry;

typedef struct launcher_pad {
    SDL_Gamepad *device;
    int stick, direction;
    bool up, down;
    Uint64 repeat_at;
} launcher_pad;

typedef struct launcher {
    SDL_Window   *window;
    SDL_Renderer *r;
    SDL_Surface  *surface;
    SDL_Renderer *presenter;            /* SDL fallback if GL is unavailable */
    SDL_Texture  *present_texture;
    q2_gl_display *display;
    SDL_Texture  *cover;
    SDL_Surface  *cover_surface;
    SDL_Texture  *logo;
    SDL_Surface  *logo_surface;
    SDL_Texture  *font;
    SDL_Surface  *font_surface;
    char          font_cache[Q2_LAUNCH_PATH_MAX];
    float         ui, ox, oy;           /* pixels per unit, grid origin     */

    q2_launch_cfg cfg;
    const char   *cfg_path;

    src_entry     src[L_MAX_SOURCES];
    int           count;
    int           sel;
    int           hover;                /* card under the pointer, or -1    */
    int           hover_button;
    int           press_button;         /* pressed here; fires on release   */
    int           press_card;           /* double-clicked; plays on release */
    float         scroll;
    float         mx, my;               /* pointer, in units                */

    char          status[160];
    bool          status_error;

    bool          dialog_open;          /* in-window, controller-ready browser */
    char          directory[Q2_LAUNCH_PATH_MAX];
    file_entry   *files;
    int           file_count, file_capacity, file_sel, file_top;
    bool          file_error;
    SDL_Keycode   armed_key;            /* Enter/Space, waiting for release */
    SDL_JoystickID armed_pad;
    Uint8         armed_pad_button;
    launcher_pad  pads[L_MAX_PADS];
    int           prompt_pad;
    bool          unfocused;
    Uint64        drive_poll_at;
    u32           drive_signature;
} launcher;

static SDL_Gamepad *prompt_pad(const launcher *l)
{
    int i;
    if (l->pads[l->prompt_pad].device) return l->pads[l->prompt_pad].device;
    for (i = 0; i < L_MAX_PADS; i++)
        if (l->pads[i].device) return l->pads[i].device;
    return NULL;
}

static void control_hint(const launcher *l, char *line, size_t cap)
{
    SDL_Gamepad *pad = prompt_pad(l);
    if (pad) {
        snprintf(line, cap, "%s %s   %s %s   %s %s   %s refresh",
            q2_ui_button(pad, SDL_GAMEPAD_BUTTON_SOUTH), l->dialog_open ? "open" : "start",
            q2_ui_button(pad, SDL_GAMEPAD_BUTTON_WEST), l->dialog_open ? "up" : "browse",
            q2_ui_button(pad, SDL_GAMEPAD_BUTTON_EAST), l->dialog_open ? "cancel" : "quit",
            q2_ui_button(pad, SDL_GAMEPAD_BUTTON_NORTH));
    } else SDL_strlcpy(line, l->dialog_open
        ? "ENTER open   BACKSPACE up   ESC cancel   F5 refresh"
        : "ENTER start   O browse   ESC quit   F5 refresh", cap);
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (!dst || cap == 0)
        return;
    n = src ? strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *path_tail(const char *path)
{
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *t = !a ? b : !b ? a : a > b ? a : b;

    return t ? t + 1 : path;
}

static const char *path_ext(const char *path)
{
    const char *tail = path_tail(path);
    const char *dot = strrchr(tail, '.');

    return dot ? dot + 1 : "";
}

static bool ext_is(const char *path, const char *ext)
{
    return SDL_strcasecmp(path_ext(path), ext) == 0;
}

static bool file_exists(const char *path)
{
    SDL_PathInfo info;

    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static void set_status(launcher *l, bool error, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(l->status, sizeof(l->status), fmt, ap);
    va_end(ap);
    l->status_error = error;
}

/* ------------------------------------------------------------------------- */
/* The sources                                                                */
/* ------------------------------------------------------------------------- */
static int find_src(const launcher *l, const char *path)
{
    int i;

    for (i = 0; i < l->count; i++)
        if (q2_launch_path_same(l->src[i].path, path))
            return i;
    return -1;
}

/* Open it, ask what it is, close it. The disc layer reports a file that is
 * not a disc as an error, which is the point of asking, so only real faults
 * reach the console while a list is being built. */
static void identify(src_entry *s)
{
    q2_log_level level = q2_log_get_level();
    q2_build_id id;
    disc *d = NULL;

    q2_log_set_level(Q2_LOG_ERROR);
    if (disc_open(&d, s->path) != Q2_OK) {
        s->state = s->kind == SRC_DRIVE ? SRC_NO_DISC : SRC_UNREADABLE;
    } else if (q2_identify(d, &id) != Q2_OK) {
        s->state = SRC_NOT_Q2;
    } else {
        s->state = SRC_READY;
        if (s->kind == SRC_IMAGE)
            copy_str(s->title, sizeof(s->title),
                     id.desc ? id.desc->name : "Quake II (uncatalogued build)");
        snprintf(s->tag, sizeof(s->tag), "%s - %s", id.serial,
                 id.video == Q2_VIDEO_NTSC ? "NTSC" : "PAL");
    }
    disc_close(d);
    q2_log_set_level(level);
}

static src_entry *add_image(launcher *l, const char *path)
{
    src_entry *s;
    int at = find_src(l, path);

    if (at >= 0)
        return &l->src[at];
    if (l->count >= L_MAX_SOURCES || !file_exists(path))
        return NULL;

    s = &l->src[l->count++];
    memset(s, 0, sizeof(*s));
    s->kind = SRC_IMAGE;
    copy_str(s->path, sizeof(s->path), path);
    copy_str(s->title, sizeof(s->title), path_tail(path));
    identify(s);
    return s;
}

static src_entry *add_drive(launcher *l, const cd_drive_info *info)
{
    src_entry *s;
    int at = find_src(l, info->path);

    if (at >= 0)
        s = &l->src[at];
    else if (l->count < L_MAX_SOURCES)
        s = &l->src[l->count++];
    else
        return NULL;

    memset(s, 0, sizeof(*s));
    s->kind  = SRC_DRIVE;
    s->state = info->has_media ? SRC_UNCHECKED : SRC_NO_DISC;
    copy_str(s->path, sizeof(s->path), info->path);
    snprintf(s->title, sizeof(s->title), "CD drive %s", info->name);
    if (!info->has_media)
        copy_str(s->note, sizeof(s->note), "Empty - insert the Quake II disc");
    else if (info->label[0])
        snprintf(s->note, sizeof(s->note), "Disc in the drive: %s", info->label);
    else
        copy_str(s->note, sizeof(s->note), "A disc is in the drive");
    return s;
}

/* Images in one directory. A .bin or .img that sits beside a .cue is that
 * sheet's track and is not offered on its own. */
static void scan_dir(launcher *l, const char *dir)
{
    char **names;
    int n = 0, i;
    bool has_cue = false;
    size_t len;

    if (!dir || !dir[0])
        return;
    names = SDL_GlobDirectory(dir, "*", SDL_GLOB_CASEINSENSITIVE, &n);
    if (!names)
        return;

    for (i = 0; i < n; i++)
        has_cue |= ext_is(names[i], "cue");

    len = strlen(dir);
    for (i = 0; i < n; i++) {
        char full[Q2_LAUNCH_PATH_MAX];
        bool want = ext_is(names[i], "cue") || ext_is(names[i], "iso") ||
                    (!has_cue && (ext_is(names[i], "bin") ||
                                  ext_is(names[i], "img")));

        if (!want)
            continue;
        snprintf(full, sizeof(full), "%s%s%s", dir,
                 (len && (dir[len - 1] == '/' || dir[len - 1] == '\\'))
                     ? "" : "/",
                 names[i]);
        add_image(l, full);
    }
    SDL_free(names);
}

static u32 drives_signature(const cd_drive_info *d, int n)
{
    u32 h = 2166136261u;
    int i;
    const char *p;

    for (i = 0; i < n; i++) {
        for (p = d[i].path; *p; p++) h = (h ^ (u8)*p) * 16777619u;
        for (p = d[i].label; *p; p++) h = (h ^ (u8)*p) * 16777619u;
        h = (h ^ (d[i].has_media ? 1u : 2u)) * 16777619u;
    }
    return h;
}

/*
 * The whole list, in the order a player looks for it: the disc played last,
 * the others played before it, images beside the executable, then the
 * drives. `keep` is reselected if it is still there; otherwise the last disc
 * played, otherwise the first one that will play.
 */
static void build_sources(launcher *l, const char *keep)
{
    cd_drive_info drives[CD_DRIVE_LIST_MAX];
    char want[Q2_LAUNCH_PATH_MAX];
    int nd, i;

    copy_str(want, sizeof(want), keep ? keep : "");
    l->count = 0;
    nd = cd_drive_list(drives, CD_DRIVE_LIST_MAX);
    l->drive_signature = drives_signature(drives, nd);

    for (i = -1; i < l->cfg.recent_count; i++) {
        const char *p = i < 0 ? l->cfg.last : l->cfg.recent[i];
        int k;

        if (!p[0])
            continue;
        if (cd_drive_parse(p, NULL, 0, NULL, 0)) {
            for (k = 0; k < nd; k++)
                if (q2_launch_path_same(drives[k].path, p))
                    add_drive(l, &drives[k]);
        } else {
            add_image(l, p);
        }
    }

    {
        const char *base = SDL_GetBasePath();
        char *cwd = SDL_GetCurrentDirectory();
        char sub[Q2_LAUNCH_PATH_MAX];

        if (base) {
            snprintf(sub, sizeof(sub), "%sdisc", base);
            scan_dir(l, sub);
            scan_dir(l, base);
        }
        scan_dir(l, cwd);
        SDL_free(cwd);
    }

    for (i = 0; i < nd; i++)
        add_drive(l, &drives[i]);

    for (i = 0; i < l->count; i++)
        l->src[i].last = l->cfg.last[0] &&
                         q2_launch_path_same(l->src[i].path, l->cfg.last);

    l->sel = want[0] ? find_src(l, want) : -1;
    if (l->sel < 0 && l->cfg.last[0])
        l->sel = find_src(l, l->cfg.last);
    for (i = 0; l->sel < 0 && i < l->count; i++)
        if (l->src[i].state == SRC_READY || l->src[i].state == SRC_UNCHECKED)
            l->sel = i;
    if (l->sel < 0)
        l->sel = 0;
}

/* ------------------------------------------------------------------------- */
/* Local artwork and disc-derived letterforms                                 */
/* ------------------------------------------------------------------------- */
static void load_logo(launcher *l)
{
    SDL_Surface *cover = SDL_ConvertSurface(l->cover_surface, SDL_PIXELFORMAT_RGBA32);
    int x, y, left, top, w, h;
    if (!cover) return;
    /* Original white wordmark from the same PAL scan as the tank. This is a
     * matte of the printed pixels, not a replacement font or redrawn logo.
     * Coordinates scale with the full 775 x 800 cover, including the Q tail. */
    left = cover->w * 169 / 775; top = cover->h * 282 / 800;
    w = cover->w * 598 / 775; h = cover->h * 109 / 800;
    l->logo_surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (l->logo_surface) {
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            const Uint8 *p = (Uint8 *)cover->pixels + (top+y)*cover->pitch + (left+x)*4;
            Uint8 *d = (Uint8 *)l->logo_surface->pixels + y*l->logo_surface->pitch + x*4;
            int low = SDL_min(p[0], SDL_min(p[1], p[2]));
            int high = SDL_max(p[0], SDL_max(p[1], p[2]));
            int alpha = SDL_clamp((low-155)*255/85, 0, 255);
            /* Below the baseline only the Q descender belongs to the title. */
            if ((y*109/h > 77 && x*598/w > 62) || high-low > 65) alpha = 0;
            d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = (Uint8)alpha;
        }
        l->logo = SDL_CreateTextureFromSurface(l->r, l->logo_surface);
        if (l->logo) SDL_SetTextureScaleMode(l->logo, SDL_SCALEMODE_LINEAR);
    }
    SDL_DestroySurface(cover);
}

static void load_art(launcher *l)
{
    char path[Q2_LAUNCH_PATH_MAX];
    const char *override = SDL_getenv("Q2PSX_LAUNCHER_ART");
    SDL_Surface *s;
    if (override) copy_str(path, sizeof(path), override);
    else snprintf(path, sizeof(path), "%slauncher/cover.bmp", SDL_GetBasePath());
    s = SDL_LoadBMP(path);
    if (s) {
        l->cover = SDL_CreateTextureFromSurface(l->r, s);
        if (l->cover) SDL_SetTextureScaleMode(l->cover, SDL_SCALEMODE_LINEAR);
        l->cover_surface = s;
        load_logo(l);
    }
}

/* Normalised ASCII cells, three faces stacked vertically. This is a runtime
 * conversion of the disc's own glyphs, never artwork compiled into the port. */
static bool font_from_disc(launcher *l, const char *path)
{
    disc *d = NULL;
    q2_build_id id;
    q2_hud_tables tab;
    q2_menu_font font;
    q2_vram_section vs = {0};
    psx_vram *vram = NULL;
    SDL_Surface *s = NULL;
    int face, ch, x, y;
    bool ok = false;
    q2_log_level level = q2_log_get_level();
    q2_log_set_level(Q2_LOG_ERROR);
    if (disc_open(&d, path) != Q2_OK || q2_identify(d, &id) != Q2_OK ||
        q2_hud_tables_load(&tab, d, &id) != Q2_OK ||
        q2_vram_load(&vs, d, "QFRONT") != Q2_OK) goto done;
    vram = calloc(1, sizeof(*vram));
    if (!vram || q2_menu_font_upload(&font, &tab, &vs, vram, false, 1) != Q2_OK ||
        !font.item_resident || !font.small_resident) goto done;
    s = SDL_CreateSurface(512, 768, SDL_PIXELFORMAT_RGBA32);
    if (!s) goto done;
    SDL_ClearSurface(s, 0, 0, 0, 0);
    for (face = 0; face < 3; face++) {
        int size = 8 << face;
        const q2_menu_face *f = q2_menu_face_get(size);
        u16 page = face ? font.tpage_item : font.tpage_small;
        u16 clut = face ? font.clut_title_hi : font.clut_small;
        int tx = (page & 15) * 64, ty = ((page >> 4) & 1) * 256;
        int cx = (clut & 63) * 16, cy = clut >> 6;
        for (ch = 32; ch < 124; ch++) {
            u8 u = 0, v = 0;
            if (face && !q2_menu_glyph_defined(size, (char)ch)) continue;
            q2_menu_glyph(&font, size, (char)ch, &u, &v);
            for (y = 0; y < f->cell_h; y++) for (x = 0; x < f->cell_w; x++) {
                unsigned uu = (u + x) & 255, vv = (v + y) & 255;
                u16 packed = vram->px[ty + vv][tx + uu / 4];
                u16 color = vram->px[cy][cx + ((packed >> ((uu & 3) * 4)) & 15)];
                u8 red = (u8)((color & 31) * 255 / 31);
                u8 green = (u8)(((color >> 5) & 31) * 255 / 31);
                u8 blue = (u8)(((color >> 10) & 31) * 255 / 31);
                u8 bright = red > green ? red : green;
                u8 *pixel = (u8 *)s->pixels + (face * 256 + ch / 16 * 32 + y) * s->pitch
                         + (ch % 16 * 32 + x) * 4;
                if (blue > bright) bright = blue;
                /* The retail palette is dim because the PSX modulates it on
                 * a bright CRT. Retain the bevel but give host UI text contrast. */
                pixel[0] = pixel[1] = pixel[2] = (u8)q2_clamp_s32(bright * 2 + 48, 0, 255);
                pixel[3] = (color & 0x7fff) ? 255 : 0;
            }
        }
    }
    l->font = SDL_CreateTextureFromSurface(l->r, s);
    if (l->font) {
        SDL_SetTextureScaleMode(l->font, SDL_SCALEMODE_NEAREST);
        l->font_surface = s;
        s = NULL;
        ok = true;
    }
done:
    SDL_DestroySurface(s);
    free(vram);
    q2_vram_free(&vs);
    disc_close(d);
    q2_log_set_level(level);
    return ok;
}

static void load_font(launcher *l)
{
    int i;
    if (l->font) return;
    for (i = 0; i < l->count; i++)
        if (l->src[i].state == SRC_READY && font_from_disc(l, l->src[i].path)) return;
    if (l->font_cache[0]) {
        SDL_Surface *s = SDL_LoadBMP(l->font_cache);
        if (s && s->w == 512 && s->h == 768) {
            l->font = SDL_CreateTextureFromSurface(l->r, s);
            if (l->font) {
                SDL_SetTextureScaleMode(l->font, SDL_SCALEMODE_NEAREST);
                l->font_surface = s;
                s = NULL;
            }
        }
        SDL_DestroySurface(s);
    }
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
static SDL_FColor rgb(u32 hex, float a)
{
    SDL_FColor c = { ((hex >> 16) & 255) / 255.0f,
                    ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a };
    return c;
}
static float px_x(const launcher *l, float x) { return l->ox + x * l->ui; }
static float px_y(const launcher *l, float y) { return l->oy + y * l->ui; }

static void rect(launcher *l, float x, float y, float w, float h, SDL_FColor c)
{
    SDL_FRect r = { px_x(l, x), px_y(l, y), w * l->ui, h * l->ui };
    SDL_SetRenderDrawColorFloat(l->r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(l->r, &r);
}

/* TL, TR, BL, BR colours. Used for the PSX-style blue selection band and
 * for fading the unmodified cover crop into the page's metal background. */
static void gradient(launcher *l, float x, float y, float w, float h,
                     SDL_FColor tl, SDL_FColor tr, SDL_FColor bl, SDL_FColor br)
{
    SDL_Vertex v[4] = {0};
    static const int ix[] = {0, 1, 2, 2, 1, 3};
    v[0].position = (SDL_FPoint){px_x(l, x), px_y(l, y)};
    v[1].position = (SDL_FPoint){px_x(l, x+w), px_y(l, y)};
    v[2].position = (SDL_FPoint){px_x(l, x), px_y(l, y+h)};
    v[3].position = (SDL_FPoint){px_x(l, x+w), px_y(l, y+h)};
    v[0].color = tl; v[1].color = tr; v[2].color = bl; v[3].color = br;
    SDL_RenderGeometry(l->r, NULL, v, 4, ix, 6);
}

static float text_w(const launcher *l, const char *s, int size, float track)
{
    size_t n = strlen(s);
    (void)l;
    return n ? (float)n * (8.0f * size + track) - track : 0;
}

static float text(launcher *l, float x, float y, const char *s, int size,
                  float track, SDL_FColor c)
{
    float start = x;
    if (l->font) {
        int face = size == 1 ? 0 : size == 2 ? 1 : 2;
        int native = 8 << face;
        const q2_menu_face *f = q2_menu_face_get(native);
        SDL_SetTextureColorModFloat(l->font, c.r, c.g, c.b);
        SDL_SetTextureAlphaModFloat(l->font, c.a);
        for (; *s; s++) {
            unsigned ch = (unsigned char)*s;
            SDL_FRect src, dst;
            if (ch >= 124 || ch < 32) ch = '?';
            if (face && ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
            src = (SDL_FRect){ (float)(ch % 16 * 32), (float)(face*256 + ch/16*32),
                               (float)f->cell_w, (float)f->cell_h };
            dst = (SDL_FRect){ floorf(px_x(l, x)), floorf(px_y(l, y)),
                               size * 8.0f * l->ui,
                               f->cell_h * (size * 8.0f / native) * L_FONT_Y * l->ui };
            SDL_RenderTexture(l->r, l->font, &src, &dst);
            x += 8.0f * size + track;
        }
    } else {
        float scale = size * l->ui;
        SDL_Rect clip = {0};
        bool clipped = SDL_RenderClipEnabled(l->r);
        if (clipped) SDL_GetRenderClipRect(l->r, &clip);
        SDL_SetRenderDrawColorFloat(l->r, c.r, c.g, c.b, c.a);
        SDL_SetRenderScale(l->r, scale, scale);
        if (clipped) {
            SDL_Rect scaled = { (int)floorf(clip.x / scale), (int)floorf(clip.y / scale),
                (int)ceilf(clip.w / scale), (int)ceilf(clip.h / scale) };
            SDL_SetRenderClipRect(l->r, &scaled);
        }
        for (; *s; s++) {
            char one[2] = {*s, 0};
            SDL_RenderDebugText(l->r, px_x(l, x)/scale, px_y(l, y)/scale, one);
            x += 8.0f * size + track;
        }
        SDL_SetRenderScale(l->r, 1, 1);
        if (clipped) SDL_SetRenderClipRect(l->r, &clip);
    }
    return x - start;
}

static void fit_middle(const launcher *l, const char *s, int size, float max_w,
                       char *out, size_t cap)
{
    size_t fit = (size_t)fmaxf(0, max_w / (8.0f * size));
    size_t n = strlen(s), head, tail;
    (void)l;
    if (fit >= cap) fit = cap - 1;
    if (n <= fit || fit < 8) {
        copy_str(out, cap, s);
        if (n > fit) out[fit] = '\0';
        return;
    }
    head = (fit-3)/3; tail = fit-3-head;
    memcpy(out, s, head);
    memcpy(out+head, "...", 3);
    memcpy(out+head+3, s+n-tail, tail);
    out[fit] = '\0';
}

static float list_top(const launcher *l, int i)
{ return L_LIST_Y + i * (L_CARD_H + L_CARD_GAP) - l->scroll; }
static float content_h(const launcher *l)
{ return l->count ? l->count * (L_CARD_H + L_CARD_GAP) - L_CARD_GAP : 0; }
static bool can_play(const src_entry *s)
{ return s->state == SRC_READY || s->state == SRC_UNCHECKED; }

static void selection_band(launcher *l, float x, float y, float w, float h, bool hot)
{
    SDL_FColor edge = rgb(0x142334, 0.9f), centre = rgb(hot ? 0x284AC0 : 0x213D85, 0.85f);
    gradient(l, x, y, w/2, h, edge, centre, edge, centre);
    gradient(l, x+w/2, y, w/2, h, centre, edge, centre, edge);
    rect(l, x, y, 2, h, rgb(C_ACCENT_HI, 1));
}

static void draw_card(launcher *l, int i)
{
    const src_entry *s = &l->src[i];
    float x = L_MARGIN, y = list_top(l, i), right = x+L_LIST_W-16;
    char line[160], label[80];
    const char *state = s->last ? "LAST PLAYED" : "READY";
    bool enabled = can_play(s);
    rect(l, x, y, L_LIST_W, L_CARD_H, rgb(i == l->hover ? C_CARD_HOVER : C_CARD, 0.86f));
    if (i == l->sel) selection_band(l, x, y, L_LIST_W, L_CARD_H, i == l->hover);
    rect(l, x, y+L_CARD_H-1, L_LIST_W, 1, rgb(C_BORDER, 0.65f));
    if (s->state == SRC_NO_DISC) state = "NO DISC";
    else if (s->state == SRC_NOT_Q2) state = "WRONG DISC";
    else if (s->state == SRC_UNREADABLE) state = "UNREADABLE";
    else if (s->state == SRC_UNCHECKED) state = "CHECK DISC";
    snprintf(label, sizeof(label), "%02d", i+1);
    text(l, x+16, y+15, label, 1, 1, rgb(i == l->sel ? C_ACCENT_HI : C_TEXT_3, 1));
    fit_middle(l, s->title, 2, L_LIST_W-64, line, sizeof(line));
    text(l, x+48, y+12, line, 2, 0, rgb(enabled ? C_TEXT : C_TEXT_2, 1));
    snprintf(label, sizeof(label), "%s%s%s", s->kind == SRC_DRIVE ? "CD DRIVE" : "DISC IMAGE",
             s->tag[0] ? " / " : "", s->tag);
    fit_middle(l, label, 1, L_LIST_W-64-text_w(l, state, 1, 0)-16, line, sizeof(line));
    text(l, x+48, y+39, line, 1, 0, rgb(C_TEXT_2, 1));
    text(l, right-text_w(l, state, 1, 0), y+39, state, 1, 0,
         rgb(enabled ? C_ACCENT_HI : C_TEXT_3, 1));
}

static bool button_rect(int which, SDL_FRect *out)
{
    *out = (SDL_FRect){ L_MARGIN, L_BUTTON_Y, L_BUTTON_W, L_BUTTON_H };
    if (which == BTN_PLAY) out->x += L_LIST_W-L_BUTTON_W;
    else if (which != BTN_BROWSE) return false;
    return true;
}

static void draw_button(launcher *l, int which, const char *label, bool enabled)
{
    SDL_FRect b;
    bool hot = enabled && l->hover_button == which;
    bool pressed = hot && l->press_button == which;
    float tw = text_w(l, label, 2, 0);
    button_rect(which, &b);
    rect(l, b.x, b.y, b.w, b.h, rgb(C_BORDER, 1));
    if (which == BTN_PLAY && enabled) {
        SDL_FColor top = rgb(hot ? 0x765633 : 0x543F2A, 1), bottom = rgb(0x231F19, 1);
        gradient(l, b.x+1, b.y+1, b.w-2, b.h-2, top, top, bottom, bottom);
        rect(l, b.x, b.y, b.w, 1, rgb(C_ACCENT, 1));
    } else rect(l, b.x+1, b.y+1, b.w-2, b.h-2, rgb(hot ? C_CARD_HOVER : C_BG_TOP, 1));
    text(l, b.x+(b.w-tw)*0.5f, b.y+15+(pressed ? 1 : 0), label, 2, 0,
         rgb(enabled ? (which == BTN_PLAY ? C_ACCENT_HI : C_TEXT) : C_TEXT_3, 1));
}

static void draw_controls(launcher *l)
{
    char line[256], *at;
    float x = L_MARGIN;
    control_hint(l, line, sizeof(line));
    at = line;
    while (*at) {
        char *action = strchr(at, ' '), *next;
        float width;
        if (!action) break;
        *action++ = 0;
        next = strstr(action, "   ");
        if (next) *next = 0;
        width = text_w(l, at, 1, 0)+12;
        rect(l, x, 571, width, 19, rgb(C_BORDER, 1));
        rect(l, x+1, 572, width-2, 17, rgb(0x22282B, 1));
        text(l, x+6, 575, at, 1, 0, rgb(C_ACCENT_HI, 1));
        text(l, x+width+8, 575, action, 1, 0, rgb(C_TEXT_2, 1));
        x += width+8+text_w(l, action, 1, 0)+20;
        if (!next) break;
        at = next+3;
    }
}

static void draw_files(launcher *l)
{
    int i;
    char name[256];
    rect(l, L_MARGIN, L_LIST_Y, L_LIST_W, L_LIST_H, rgb(C_CARD, 0.85f));
    for (i = l->file_top; i < l->file_count && i < l->file_top+L_FILE_ROWS; i++) {
        float y = L_LIST_Y + (i-l->file_top)*L_FILE_H;
        if (i == l->file_sel) {
            gradient(l, L_MARGIN, y, L_LIST_W, L_FILE_H,
                     rgb(0x192D68, 1), rgb(0x294781, 0.3f),
                     rgb(0x192D68, 1), rgb(0x294781, 0.3f));
            rect(l, L_MARGIN, y, 2, L_FILE_H, rgb(C_ACCENT_HI, 1));
        }
        text(l, L_MARGIN+16, y+12, l->files[i].directory ? "DIR" : "DISC",
             1, 0, rgb(C_ACCENT_HI, 1));
        fit_middle(l, l->files[i].name, 1, L_LIST_W-88, name, sizeof(name));
        text(l, L_MARGIN+72, y+12, name, 1, 0, rgb(C_TEXT, 1));
    }
    if (!l->file_count)
        text(l, L_MARGIN+16, L_LIST_Y+28, "No folders or disc images here.",
             1, 0, rgb(C_TEXT_2, 1));
    if (l->file_count > L_FILE_ROWS) {
        float knob = L_LIST_H*L_FILE_ROWS/l->file_count;
        float at = (L_LIST_H-knob)*l->file_top/(l->file_count-L_FILE_ROWS);
        rect(l, L_MARGIN+L_LIST_W+8, L_LIST_Y, 2, L_LIST_H, rgb(C_BORDER, 0.5f));
        rect(l, L_MARGIN+L_LIST_W+8, L_LIST_Y+at, 2, knob, rgb(C_ACCENT, 1));
    }
}

static void draw(launcher *l)
{
    int ow = 0, oh = 0, i;
    SDL_Rect canvas;
    char line[256];
    SDL_GetCurrentRenderOutputSize(l->r, &ow, &oh);
    if (ow <= 0 || oh <= 0) return;
    l->ui = fminf(ow/L_W, oh/L_H);
    l->ox = (ow-L_W*l->ui)*0.5f; l->oy = (oh-L_H*l->ui)*0.5f;
    SDL_SetRenderDrawBlendMode(l->r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(l->r, 8, 10, 12, 255);
    SDL_RenderClear(l->r);
    canvas = (SDL_Rect){ (int)floorf(l->ox), (int)floorf(l->oy),
        (int)ceilf(L_W*l->ui), (int)ceilf(L_H*l->ui) };
    SDL_SetRenderClipRect(l->r, &canvas);
    rect(l, 0, 0, L_W, L_H, rgb(C_BG_TOP, 1));
    if (l->cover) {
        float w, h;
        SDL_FRect src, dst;
        SDL_GetTextureSize(l->cover, &w, &h);
        /* PAL cover: only the tank's head/shoulders, above the printed logo.
         * Keep original pixels and proportions; no reconstructed illustration. */
        src = (SDL_FRect){w*0.195f, 0, w*0.805f, h*0.348f};
        dst = (SDL_FRect){px_x(l, 370), px_y(l, 0), 830*l->ui,
                          830*src.h/src.w*l->ui};
        SDL_SetTextureColorMod(l->cover, 192, 192, 180);
        SDL_RenderTexture(l->r, l->cover, &src, &dst);
        gradient(l, 370, 0, 330, 374, rgb(C_BG_TOP, 1), rgb(C_BG_TOP, 0),
                 rgb(C_BG_TOP, 1), rgb(C_BG_TOP, 0));
        gradient(l, 370, 190, 830, 180, rgb(C_BG_TOP, 0), rgb(C_BG_TOP, 0),
                 rgb(C_BG_TOP, 1), rgb(C_BG_TOP, 1));
    }
    gradient(l, 0, 380, L_W, 220, rgb(C_BG_TOP, 1), rgb(C_BG_TOP, 1),
             rgb(C_BG_BOTTOM, 1), rgb(C_BG_BOTTOM, 1));
    /* Restrained steel seams and rivets, with no desktop-style rounded cards. */
    rect(l, L_MARGIN, 34, 34, 2, rgb(C_ACCENT, 1));
    text(l, L_MARGIN+48, 31, "PLAYSTATION EDITION", 1, 2, rgb(C_ACCENT_HI, 1));
    if (l->logo) {
        SDL_FRect dst = {px_x(l, L_MARGIN), px_y(l, 66), 360*l->ui,
                         360.0f*l->logo_surface->h/l->logo_surface->w*l->ui};
        SDL_RenderTexture(l->r, l->logo, NULL, &dst);
    } else {
        text(l, L_MARGIN+2, 83, "QUAKE II", 5, 1, rgb(0x030404, 1));
        text(l, L_MARGIN, 80, "QUAKE II", 5, 1, rgb(C_TEXT, 1));
    }
    text(l, L_MARGIN, 137, "Q2PSX-PC", 1, 3, rgb(C_TEXT_3, 1));
    rect(l, L_MARGIN, 182, 240, 1, rgb(C_BORDER, 1));
    rect(l, L_MARGIN, 182, 56, 1, rgb(C_ACCENT, 1));
    text(l, L_MARGIN, 208, l->dialog_open ? "BROWSE DISC IMAGE" : "SELECT GAME SOURCE",
         2, 0, rgb(C_TEXT, 1));
    if (l->dialog_open) {
        fit_middle(l, l->directory[0] ? l->directory : "Computer", 1, L_LIST_W, line, sizeof(line));
        text(l, L_MARGIN, 232, line, 1, 0, rgb(C_TEXT_2, 1));
    } else text(l, L_MARGIN, 232, "Choose a disc image or insert your PlayStation CD.", 1, 0, rgb(C_TEXT_2, 1));

    if (l->dialog_open) draw_files(l);
    else if (l->count) {
        SDL_Rect clip = { (int)floorf(px_x(l, L_MARGIN)), (int)floorf(px_y(l, L_LIST_Y)),
            (int)ceilf(L_LIST_W*l->ui), (int)ceilf(L_LIST_H*l->ui) };
        SDL_SetRenderClipRect(l->r, &clip);
        for (i = 0; i < l->count; i++)
            if (list_top(l, i)+L_CARD_H >= L_LIST_Y && list_top(l, i) < L_LIST_Y+L_LIST_H)
                draw_card(l, i);
        SDL_SetRenderClipRect(l->r, &canvas);
        if (content_h(l) > L_LIST_H) {
            float knob = L_LIST_H*L_LIST_H/content_h(l);
            float at = (L_LIST_H-knob)*l->scroll/(content_h(l)-L_LIST_H);
            rect(l, L_MARGIN+L_LIST_W+8, L_LIST_Y, 2, L_LIST_H, rgb(C_BORDER, 0.5f));
            rect(l, L_MARGIN+L_LIST_W+8, L_LIST_Y+at, 2, knob, rgb(C_ACCENT, 1));
        }
    } else {
        rect(l, L_MARGIN, L_LIST_Y, L_LIST_W, 128, rgb(C_CARD, 0.85f));
        rect(l, L_MARGIN, L_LIST_Y, 2, 128, rgb(C_ACCENT, 1));
        text(l, L_MARGIN+24, L_LIST_Y+27, "INSERT DISC", 2, 0, rgb(C_ACCENT_HI, 1));
        text(l, L_MARGIN+24, L_LIST_Y+63, "Browse for a CUE, ISO, BIN or IMG file,", 1, 0, rgb(C_TEXT_2, 1));
        text(l, L_MARGIN+24, L_LIST_Y+81, "or drop a disc image into this window.", 1, 0, rgb(C_TEXT_2, 1));
    }
    if (l->status[0]) {
        fit_middle(l, l->status, 1, L_LIST_W, line, sizeof(line));
        text(l, L_MARGIN, 464, line, 1, 0, rgb(l->status_error ? C_ERROR : C_TEXT_2, 1));
    } else if (!l->dialog_open && l->count && l->sel >= 0 && l->sel < l->count) {
        const src_entry *s = &l->src[l->sel];
        fit_middle(l, s->kind == SRC_IMAGE ? s->path : s->note, 1, L_LIST_W, line, sizeof(line));
        for (i = 0; line[i]; i++) if (line[i] == '\\') line[i] = '/';
        text(l, L_MARGIN, 464, line, 1, 0, rgb(C_TEXT_2, 1));
    }
    draw_button(l, BTN_BROWSE, l->dialog_open ? "UP ONE FOLDER" : "BROWSE IMAGE", true);
    draw_button(l, BTN_PLAY, l->dialog_open ? "OPEN" : "START GAME",
                l->dialog_open ? l->file_count > 0 : l->count && can_play(&l->src[l->sel]));
    rect(l, L_MARGIN, 560, L_W-2*L_MARGIN, 1, rgb(C_BORDER, 0.6f));
    draw_controls(l);
    snprintf(line, sizeof(line), "Q2PSX-PC %s", Q2PSX_VERSION_STRING);
    text(l, L_W-L_MARGIN-text_w(l, line, 1, 0), 575, line, 1, 0, rgb(C_TEXT_3, 1));
    SDL_SetRenderClipRect(l->r, NULL);
}

/* The UI always renders into its own surface. No OS capture is involved, and
 * the GL context survives selection so the first game frame uses the same host. */
static bool present(launcher *l)
{
    SDL_RenderPresent(l->r);
    if (l->display) {
        q2_crt frame = {0};
        int w, h;
        frame.pixels = (u32 *)l->surface->pixels;
        frame.width = l->surface->w;
        frame.height = l->surface->h;
        SDL_GetWindowSizeInPixels(l->window, &w, &h);
        return q2_gl_upload(l->display, &frame) && q2_gl_present(l->display, w, h, 0, 0);
    }
    if (l->presenter) {
        SDL_UpdateTexture(l->present_texture, NULL, l->surface->pixels, l->surface->pitch);
        SDL_RenderTexture(l->presenter, l->present_texture, NULL, NULL);
        return SDL_RenderPresent(l->presenter);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */
static void scroll_to_selection(launcher *l)
{
    float top, bottom, max_scroll = content_h(l) - L_LIST_H;

    if (l->sel < 0 || l->sel >= l->count)
        return;
    top = (float)l->sel * (L_CARD_H + L_CARD_GAP);
    bottom = top + L_CARD_H;
    if (top < l->scroll)
        l->scroll = top;
    if (bottom > l->scroll + L_LIST_H)
        l->scroll = bottom - L_LIST_H;
    if (l->scroll > max_scroll) l->scroll = max_scroll;
    if (l->scroll < 0) l->scroll = 0;
}

static void move_selection(launcher *l, int by)
{
    if (l->dialog_open) {
        l->file_sel = SDL_clamp(l->file_sel+by, 0, SDL_max(0, l->file_count-1));
        if (l->file_sel < l->file_top) l->file_top = l->file_sel;
        if (l->file_sel >= l->file_top+L_FILE_ROWS) l->file_top = l->file_sel-L_FILE_ROWS+1;
        return;
    }
    if (!l->count)
        return;
    l->sel += by;
    if (l->sel < 0) l->sel = 0;
    if (l->sel >= l->count) l->sel = l->count - 1;
    l->status[0] = '\0';
    scroll_to_selection(l);
}

static void hit_test(launcher *l)
{
    int i;
    SDL_FRect b;

    l->hover = -1;
    l->hover_button = BTN_NONE;

    for (i = BTN_BROWSE; i <= BTN_PLAY; i++) {
        button_rect(i, &b);
        if (l->mx >= b.x && l->mx < b.x + b.w &&
            l->my >= b.y && l->my < b.y + b.h)
            l->hover_button = i;
    }
    if (l->my < L_LIST_Y || l->my >= L_LIST_Y + L_LIST_H ||
        l->mx < L_MARGIN || l->mx >= L_MARGIN + L_LIST_W)
        return;
    if (l->dialog_open) {
        i = l->file_top + (int)((l->my-L_LIST_Y)/L_FILE_H);
        if (i < l->file_count) l->hover = i;
        return;
    }
    for (i = 0; i < l->count; i++) {
        float y = list_top(l, i);
        if (l->my >= y && l->my < y + L_CARD_H)
            l->hover = i;
    }
}

static bool file_add(launcher *l, const char *name, bool directory)
{
    file_entry *p;
    if (strlen(name) >= sizeof(l->files[0].name)) return true;
    if (l->file_count == l->file_capacity) {
        int capacity = l->file_capacity ? l->file_capacity*2 : 64;
        p = SDL_realloc(l->files, sizeof(*p)*(size_t)capacity);
        if (!p) { l->file_error = true; return false; }
        l->files = p; l->file_capacity = capacity;
    }
    p = &l->files[l->file_count++];
    copy_str(p->name, sizeof(p->name), name); p->directory = directory;
    return true;
}

static SDL_EnumerationResult SDLCALL file_found(void *user, const char *dir, const char *name)
{
    launcher *l = user;
    char path[Q2_LAUNCH_PATH_MAX];
    SDL_PathInfo info;
    const char *ext = path_ext(name);
    bool directory;
    int n = snprintf(path, sizeof(path), "%s%s", dir, name);
    if (n < 0 || n >= (int)sizeof(path) || !strcmp(name, ".") || !strcmp(name, ".."))
        return SDL_ENUM_CONTINUE;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    directory = info.type == SDL_PATHTYPE_DIRECTORY;
    if (!directory && (info.type != SDL_PATHTYPE_FILE ||
        (SDL_strcasecmp(ext, "cue") && SDL_strcasecmp(ext, "iso") &&
         SDL_strcasecmp(ext, "bin") && SDL_strcasecmp(ext, "img")))) return SDL_ENUM_CONTINUE;
    return file_add(l, name, directory) ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
}

static int file_compare(const void *a, const void *b)
{
    const file_entry *x = a, *y = b;
    if (!strcmp(x->name, y->name)) return 0;
    if (!strcmp(x->name, "..")) return -1;
    if (!strcmp(y->name, "..")) return 1;
    if (x->directory != y->directory) return y->directory-x->directory;
    return SDL_strcasecmp(x->name, y->name);
}

static void read_directory(launcher *l)
{
    l->file_count = l->file_sel = l->file_top = 0;
    l->file_error = false;
    l->armed_key = 0; l->armed_pad = 0;
    l->hover = l->press_card = -1;
    l->status[0] = 0;
    if (!l->directory[0]) {
#ifdef _WIN32
        DWORD drives = GetLogicalDrives();
        int i;
        for (i = 0; i < 26; i++) if (drives & (1u << i)) {
            char root[] = "A:/";
            root[0] += (char)i;
            if (!file_add(l, root, true)) break;
        }
#else
        file_add(l, "/", true);
#endif
    } else {
        if (file_add(l, "..", true) &&
            !SDL_EnumerateDirectory(l->directory, file_found, l)) l->file_error = true;
    }
    if (l->file_error) set_status(l, true, "Could not read this folder. Go up to choose another.");
    if (l->file_count > 1) qsort(l->files, (size_t)l->file_count, sizeof(*l->files), file_compare);
}

static void browser_parent(launcher *l)
{
    size_t n = strlen(l->directory);
    char previous[Q2_LAUNCH_PATH_MAX];
    int i;
    if (n && (l->directory[n-1] == '/' || l->directory[n-1] == '\\')) l->directory[--n] = 0;
    copy_str(previous, sizeof(previous), path_tail(l->directory));
    l->directory[path_tail(l->directory)-l->directory] = 0;
    /* A drive root's parent is the list of drives. */
    if (n == 2 && l->directory[1] == ':') l->directory[0] = 0;
    read_directory(l);
    for (i = 0; i < l->file_count; i++)
        if (!SDL_strcasecmp(l->files[i].name, previous)) { move_selection(l, i); break; }
}

static void browse(launcher *l)
{
    if (l->dialog_open) { browser_parent(l); return; }
    copy_str(l->directory, sizeof(l->directory), SDL_GetBasePath());
    if (l->count && l->src[l->sel].kind == SRC_IMAGE) {
        copy_str(l->directory, sizeof(l->directory), l->src[l->sel].path);
        l->directory[path_tail(l->directory)-l->directory] = 0;
        if (!l->directory[0]) copy_str(l->directory, sizeof(l->directory), "./");
    }
    l->dialog_open = true;
    read_directory(l);
}

/* A file from the picker or dropped on the window. */
static void take_path(launcher *l, const char *path)
{
    src_entry *s;

    l->dialog_open = false;
    l->armed_key = 0; l->armed_pad = 0;

    if (cd_drive_parse(path, NULL, 0, NULL, 0)) {
        build_sources(l, path);
        return;
    }
    s = add_image(l, path);
    if (!s) {
        set_status(l, true, "That file cannot be opened.");
        return;
    }
    l->sel = (int)(s - l->src);
    load_font(l);
    scroll_to_selection(l);
    if (s->state == SRC_READY)
        set_status(l, false, "%s is ready.", s->title);
    else
        set_status(l, true, "%s is %s.", path_tail(s->path),
                   s->state == SRC_NOT_Q2 ? "not a Quake II PlayStation disc"
                                          : "not a disc image this can read");
}

static void browser_choose(launcher *l)
{
    char path[Q2_LAUNCH_PATH_MAX];
    file_entry *e;
    int n;
    if (!l->file_count) return;
    e = &l->files[l->file_sel];
    if (!strcmp(e->name, "..")) { browser_parent(l); return; }
    n = snprintf(path, sizeof(path), "%s%s%s", l->directory, e->name,
                 e->directory && e->name[strlen(e->name)-1] != '/' ? "/" : "");
    if (n < 0 || n >= (int)sizeof(path)) {
        set_status(l, true, "This path is too long."); return;
    }
    if (e->directory) {
        copy_str(l->directory, sizeof(l->directory), path);
        read_directory(l);
    } else take_path(l, path);
}

static void forget(launcher *l)
{
    char keep[Q2_LAUNCH_PATH_MAX];
    src_entry *s;

    if (!l->count)
        return;
    s = &l->src[l->sel];
    copy_str(keep, sizeof(keep), s->path);
    q2_launch_cfg_forget(&l->cfg, keep);
    q2_launch_cfg_save(&l->cfg, l->cfg_path);
    build_sources(l, NULL);
    /* Found again beside the executable or in a drive: it stays, just not
     * as the remembered one. */
    set_status(l, false, "Forgot %s.", path_tail(keep));
    scroll_to_selection(l);
}

/* Read the selection once more and, if it is the game, remember it. */
static bool activate(launcher *l)
{
    src_entry *s;

    if (l->dialog_open || !l->count || l->sel < 0 || l->sel >= l->count)
        return false;
    s = &l->src[l->sel];

    if (s->kind == SRC_DRIVE) {
        if (s->state == SRC_NO_DISC) {
            set_status(l, true, "There is no disc in %s.", s->path);
            return false;
        }
        /* A drive can take seconds to spin up; say so before blocking. */
        set_status(l, false, "Reading the disc in %s...", s->path);
        draw(l);
        present(l);
        identify(s);
        l->status[0] = '\0';
    } else if (s->state == SRC_READY) identify(s);

    switch (s->state) {
    case SRC_READY:
        break;
    case SRC_NO_DISC:
        set_status(l, true, "%s has no readable disc in it.", s->path);
        return false;
    case SRC_NOT_Q2:
        set_status(l, true, "That is not a Quake II PlayStation disc.");
        return false;
    case SRC_UNREADABLE:
    case SRC_UNCHECKED:
        set_status(l, true, "That cannot be read as a disc image.");
        return false;
    }

    q2_launch_cfg_use(&l->cfg, s->path);
    if (!q2_launch_cfg_save(&l->cfg, l->cfg_path))
        Q2_WARN("launcher: could not save %s", l->cfg_path);
    if (l->font_surface && l->font_cache[0])
        SDL_SaveBMP(l->font_surface, l->font_cache);
    set_status(l, false, "Starting Quake II...");
    draw(l);
    present(l);
    return true;
}

static void open_pad(launcher *l, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < L_MAX_PADS; i++)
        if (l->pads[i].device && SDL_GetGamepadID(l->pads[i].device) == id) return;
    for (i = 0; i < L_MAX_PADS; i++) {
        if (!l->pads[i].device) {
            l->pads[i].device = SDL_OpenGamepad(id);
            return;
        }
    }
}

static int pad_index(const launcher *l, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < L_MAX_PADS; i++)
        if (l->pads[i].device && SDL_GetGamepadID(l->pads[i].device) == id) return i;
    return -1;
}

static void pad_direction(launcher *l, int i, Uint64 now)
{
    launcher_pad *p = &l->pads[i];
    int direction = p->up || p->down ? (int)p->down-(int)p->up : p->stick;
    if (direction != p->direction) {
        p->direction = direction;
        p->repeat_at = now+350;
        if (direction) move_selection(l, direction);
    } else if (direction && now >= p->repeat_at) {
        move_selection(l, direction);
        p->repeat_at = now+90;
    }
}

static void refresh(launcher *l)
{
    if (l->dialog_open) read_directory(l);
    else {
        build_sources(l, l->count ? l->src[l->sel].path : NULL);
        scroll_to_selection(l);
        set_status(l, false, "Looked again: %d source%s.", l->count,
                   l->count == 1 ? "" : "s");
    }
}

static void cancel_browser(launcher *l)
{
    l->dialog_open = false;
    l->armed_key = 0; l->armed_pad = 0;
    l->hover = l->press_card = -1;
    l->status[0] = 0;
}

static bool choose(launcher *l)
{
    if (l->dialog_open) { browser_choose(l); return false; }
    return activate(l);
}

/* The drives change under the launcher — a disc goes in, a USB drive comes
 * out — and nobody should have to press anything to see it. */
static void poll_drives(launcher *l)
{
    cd_drive_info drives[CD_DRIVE_LIST_MAX];
    Uint64 now = SDL_GetTicks();
    int n;

    if (now < l->drive_poll_at)
        return;
    l->drive_poll_at = now + L_DRIVE_POLL_MS;
    n = cd_drive_list(drives, CD_DRIVE_LIST_MAX);
    if (drives_signature(drives, n) != l->drive_signature) {
        char keep[Q2_LAUNCH_PATH_MAX];

        copy_str(keep, sizeof(keep), l->count ? l->src[l->sel].path : "");
        build_sources(l, keep);
        scroll_to_selection(l);
    }
}

/* ------------------------------------------------------------------------- */
/* The loop                                                                   */
/* ------------------------------------------------------------------------- */
enum { RUN_ON, RUN_PLAY, RUN_QUIT };

static int handle(launcher *l, SDL_Event *e)
{
    int pi;
    float density = l->window ? SDL_GetWindowPixelDensity(l->window) : 1.0f;

    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return RUN_QUIT;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        l->armed_key = 0;
        l->armed_pad = false;
        l->press_button = BTN_NONE;
        l->press_card = -1;
        l->unfocused = true;
        for (pi = 0; pi < L_MAX_PADS; pi++) {
            l->pads[pi].stick = l->pads[pi].direction = 0;
            l->pads[pi].up = l->pads[pi].down = false;
        }
        break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        l->drive_poll_at = 0;
        l->unfocused = false;
        break;

    case SDL_EVENT_DROP_FILE:
        if (e->drop.data)
            take_path(l, e->drop.data);
        break;

    case SDL_EVENT_MOUSE_MOTION:
        l->mx = (e->motion.x * density - l->ox) / l->ui;
        l->my = (e->motion.y * density - l->oy) / l->ui;
        hit_test(l);
        break;

    case SDL_EVENT_MOUSE_WHEEL: {
        float max_scroll = content_h(l) - L_LIST_H;
        if (l->dialog_open) {
            if (e->wheel.y != 0) move_selection(l, e->wheel.y > 0 ? -3 : 3);
            break;
        }
        l->scroll -= e->wheel.y * (L_CARD_H + L_CARD_GAP) * 0.5f;
        if (l->scroll > max_scroll) l->scroll = max_scroll;
        if (l->scroll < 0) l->scroll = 0;
        hit_test(l);
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button != SDL_BUTTON_LEFT)
            break;
        l->mx = (e->button.x * density - l->ox) / l->ui;
        l->my = (e->button.y * density - l->oy) / l->ui;
        hit_test(l);
        l->press_button = l->hover_button;
        l->press_card = -1;
        if (l->hover >= 0) {
            if (l->dialog_open) {
                l->file_sel = l->hover;
                if (e->button.clicks >= 2) l->press_card = l->hover;
                break;
            }
            if (l->sel != l->hover)
                l->status[0] = '\0';
            l->sel = l->hover;
            if (e->button.clicks >= 2)
                l->press_card = l->hover;
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int pressed = l->press_button, card = l->press_card;

        if (e->button.button != SDL_BUTTON_LEFT)
            break;
        l->mx = (e->button.x * density - l->ox) / l->ui;
        l->my = (e->button.y * density - l->oy) / l->ui;
        hit_test(l);
        l->press_button = BTN_NONE;
        l->press_card = -1;
        if (card >= 0 && card == l->hover && choose(l))
            return RUN_PLAY;
        if (pressed != BTN_NONE && pressed == l->hover_button) {
            if (pressed == BTN_BROWSE)
                browse(l);
            else if (pressed == BTN_PLAY && choose(l))
                return RUN_PLAY;
        }
        break;
    }

    case SDL_EVENT_KEY_DOWN:
        switch (e->key.key) {
        /* The keypad's keys too: with NumLock off they are the arrows. */
        case SDLK_ESCAPE:
            if (l->dialog_open) { cancel_browser(l); break; }
            return RUN_QUIT;
        case SDLK_UP:
        case SDLK_KP_8:     move_selection(l, -1); break;
        case SDLK_DOWN:
        case SDLK_KP_2:     move_selection(l, 1); break;
        case SDLK_PAGEUP:
        case SDLK_KP_9:     move_selection(l, -4); break;
        case SDLK_PAGEDOWN:
        case SDLK_KP_3:     move_selection(l, 4); break;
        case SDLK_HOME:
        case SDLK_KP_7:     move_selection(l, -(l->dialog_open ? l->file_count : l->count)); break;
        case SDLK_END:
        case SDLK_KP_1:     move_selection(l, l->dialog_open ? l->file_count : l->count); break;
        case SDLK_O:        browse(l); break;
        case SDLK_F5:
            refresh(l);
            break;
        case SDLK_DELETE:
            if (!l->dialog_open) forget(l);
            break;
        case SDLK_BACKSPACE:
            if (l->dialog_open) browser_parent(l); else forget(l);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            /* On the release, so the key is up before the game starts
             * reading the keyboard — a held Enter would confirm its menu. */
            if (!e->key.repeat)
                l->armed_key = e->key.key;
            break;
        default:
            break;
        }
        break;

    case SDL_EVENT_KEY_UP:
        if (l->armed_key && e->key.key == l->armed_key) {
            l->armed_key = 0;
            if (choose(l))
                return RUN_PLAY;
        }
        break;

    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(l, e->gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_REMOVED: {
        pi = pad_index(l, e->gdevice.which);
        if (pi >= 0) {
            SDL_CloseGamepad(l->pads[pi].device);
            SDL_zero(l->pads[pi]);
        }
        if (l->armed_pad == e->gdevice.which) l->armed_pad = 0;
        break;
    }

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        pi = pad_index(l, e->gaxis.which);
        if (pi < 0 || l->unfocused || e->gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) break;
        l->pads[pi].stick = q2_ui_axis(e->gaxis.value, l->pads[pi].stick);
        if (l->pads[pi].stick) l->prompt_pad = pi;
        pad_direction(l, pi, SDL_GetTicks());
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        pi = pad_index(l, e->gbutton.which);
        if (pi < 0 || l->unfocused) break;
        l->prompt_pad = pi;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP ||
            e->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
            bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            if (e->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) l->pads[pi].up = down;
            else l->pads[pi].down = down;
            pad_direction(l, pi, SDL_GetTicks());
            break;
        }
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            if (l->armed_pad == e->gbutton.which && l->armed_pad_button == e->gbutton.button) {
                l->armed_pad = 0;
                if (choose(l)) return RUN_PLAY;
            }
            break;
        }
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_WEST:      browse(l); break;
        case SDL_GAMEPAD_BUTTON_NORTH:     refresh(l); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: move_selection(l, -L_FILE_ROWS); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: move_selection(l, L_FILE_ROWS); break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_BACK:
            if (l->dialog_open) { cancel_browser(l); break; }
            return RUN_QUIT;
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_START:
            l->armed_pad = e->gbutton.which;
            l->armed_pad_button = e->gbutton.button;
            break;
        default: break;
        }
        break;

    default:
        break;
    }
    return RUN_ON;
}

void q2_launcher_host_close(q2_launcher_host *host)
{
    if (!host) return;
    q2_gl_destroy(host->display);
    SDL_DestroyWindow(host->window);
    memset(host, 0, sizeof(*host));
}

static void shutdown_ui(launcher *l, bool keep_host)
{
    int i;
    for (i = 0; i < L_MAX_PADS; i++)
        if (l->pads[i].device) SDL_CloseGamepad(l->pads[i].device);
    SDL_free(l->files);
    SDL_DestroyRenderer(l->r);
    SDL_DestroySurface(l->surface);
    SDL_DestroySurface(l->font_surface);
    SDL_DestroySurface(l->cover_surface);
    SDL_DestroySurface(l->logo_surface);
    SDL_DestroyRenderer(l->presenter);
    if (!keep_host) {
        q2_gl_destroy(l->display);
        SDL_DestroyWindow(l->window);
    }
    if (l->window) {
        SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_DROP_POSITION);
    }
}

static bool resize_surface(launcher *l, int w, int h)
{
    if (w < 1 || h < 1) return l->r != NULL;
    if (l->surface && l->surface->w == w && l->surface->h == h) return true;
    SDL_DestroyRenderer(l->r);
    SDL_DestroySurface(l->surface);
    l->cover = l->font = l->logo = NULL;
    l->surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    l->r = l->surface ? SDL_CreateSoftwareRenderer(l->surface) : NULL;
    if (!l->r) return false;
    if (l->cover_surface) {
        l->cover = SDL_CreateTextureFromSurface(l->r, l->cover_surface);
        if (l->cover) SDL_SetTextureScaleMode(l->cover, SDL_SCALEMODE_LINEAR);
    }
    if (l->font_surface) {
        l->font = SDL_CreateTextureFromSurface(l->r, l->font_surface);
        if (l->font) SDL_SetTextureScaleMode(l->font, SDL_SCALEMODE_NEAREST);
    }
    if (l->logo_surface) {
        l->logo = SDL_CreateTextureFromSurface(l->r, l->logo_surface);
        if (l->logo) SDL_SetTextureScaleMode(l->logo, SDL_SCALEMODE_LINEAR);
    }
    if (l->presenter) {
        SDL_DestroyTexture(l->present_texture);
        l->present_texture = SDL_CreateTexture(l->presenter, SDL_PIXELFORMAT_RGBA32,
                                                SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!l->present_texture) return false;
    }
    return true;
}

int q2_launcher_run(char *out, size_t cap, q2_launcher_host *host,
                    const q2_launcher_options *options)
{
    launcher *l;
    int result = RUN_ON, outcome = Q2_LAUNCH_ERROR;
    int w, h;
    if (host) memset(host, 0, sizeof(*host));
    if (!out || !cap || !host || !options ||
        (options->headless && (!options->shot || !options->shot[0]))) return Q2_LAUNCH_ERROR;
    w = options->width > 0 ? options->width : (int)L_W;
    h = options->height > 0 ? options->height : (int)L_H;
    if (!SDL_Init(options->headless ? 0 : SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return Q2_LAUNCH_ERROR;
    l = SDL_calloc(1, sizeof(*l));
    if (!l) return Q2_LAUNCH_ERROR;
    l->hover = l->press_card = -1;
    l->cfg_path = SDL_getenv("Q2PSX_LAUNCHER_CFG");
    if (!l->cfg_path || !l->cfg_path[0]) l->cfg_path = q2_launch_cfg_default_path();
    q2_launch_cfg_load(&l->cfg, l->cfg_path);
    copy_str(l->font_cache, sizeof(l->font_cache), l->cfg_path);
    l->font_cache[path_tail(l->font_cache)-l->font_cache] = '\0';
    {
        size_t n = strlen(l->font_cache);
        snprintf(l->font_cache+n, sizeof(l->font_cache)-n, "launcher-font.bmp");
    }
    if (!options->headless) {
        SDL_Rect usable;
        float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if (!options->width && scale > 1) { w = (int)(w*scale); h = (int)(h*scale); }
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
            if (w > usable.w*9/10) { h = h*(usable.w*9/10)/w; w = usable.w*9/10; }
            if (h > usable.h*9/10) { w = w*(usable.h*9/10)/h; h = usable.h*9/10; }
        }
        if (!options->cpu) {
            q2_gl_attributes();
            l->window = SDL_CreateWindow("Quake II - PlayStation", w, h,
                SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
            if (l->window) l->display = q2_gl_create(l->window);
            if (!l->display) { SDL_DestroyWindow(l->window); l->window = NULL; }
        }
        if (!l->window) {
            l->window = SDL_CreateWindow("Quake II - PlayStation", w, h,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
            if (l->window) l->presenter = SDL_CreateRenderer(l->window, NULL);
            if (!l->presenter) goto done;
            SDL_SetRenderVSync(l->presenter, 1);
        }
        SDL_SetWindowMinimumSize(l->window, 480, 300);
        SDL_GetWindowSizeInPixels(l->window, &w, &h);
        {
            int count = 0, i;
            SDL_JoystickID *ids = SDL_GetGamepads(&count);
            for (i = 0; i < count; i++) open_pad(l, ids[i]);
            SDL_free(ids);
        }
    }
    if (!resize_surface(l, w, h)) goto done;
    load_art(l);
    build_sources(l, options->disc);
    if (options->disc) {
        add_image(l, options->disc);
        l->sel = find_src(l, options->disc);
        if (l->sel < 0) l->sel = 0;
    }
    load_font(l);
    scroll_to_selection(l);
    l->drive_poll_at = SDL_GetTicks() + L_DRIVE_POLL_MS;
    draw(l);
    if (options->headless) {
        SDL_RenderPresent(l->r);
        if (SDL_SaveBMP(l->surface, options->shot)) outcome = Q2_LAUNCH_CAPTURE;
        goto done;
    }
    if (!present(l)) goto done;
    SDL_ShowWindow(l->window);
    while (result == RUN_ON) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 30)) {
            do {
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    SDL_GetWindowSizeInPixels(l->window, &w, &h);
                    if (!resize_surface(l, w, h)) goto done;
                    draw(l); /* refresh coordinates before the next pointer event */
                }
                result = handle(l, &e);
            } while (result == RUN_ON && SDL_PollEvent(&e));
        }
        if (result == RUN_ON) {
            int i;
            if (!l->unfocused) for (i = 0; i < L_MAX_PADS; i++)
                if (l->pads[i].device) pad_direction(l, i, SDL_GetTicks());
            poll_drives(l);
            load_font(l);
            draw(l);
            if (!present(l)) goto done;
        }
    }
    outcome = result == RUN_PLAY ? Q2_LAUNCH_PLAY : Q2_LAUNCH_QUIT;
    if (outcome == Q2_LAUNCH_PLAY) {
        copy_str(out, cap, l->src[l->sel].path);
        host->window = l->window;
        host->display = l->display;
    }
done:
    if (outcome == Q2_LAUNCH_ERROR) fprintf(stderr, "disc selector: %s\n", SDL_GetError());
    shutdown_ui(l, outcome == Q2_LAUNCH_PLAY);
    SDL_free(l);
    return outcome;
}
