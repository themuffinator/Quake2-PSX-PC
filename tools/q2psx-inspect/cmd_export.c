/*
 * cmd_export.c — decode the disc's assets into interchange formats.
 *
 * `extract` writes the files back out byte for byte and leaves you with .DAT
 * containers nothing else can read. This command goes the other way: every
 * format the port understands is written out as something a modelling package,
 * an image viewer or a media player can open.
 *
 *     zone geometry   -> Wavefront OBJ + MTL
 *     CastList models -> Wavefront OBJ + MTL, posed
 *     textures        -> indexed PNG with a tRNS block
 *     sound banks     -> RIFF WAV, 16-bit mono
 *     XA music        -> RIFF WAV, 16-bit stereo, 37800 Hz
 *     CD-DA track     -> RIFF WAV, 16-bit stereo, 44100 Hz
 *
 * Three conventions are worth stating up front, because they are choices this
 * file makes rather than facts the disc states.
 *
 * WORLD UNITS ARE NOT SCALED. Points are integers on the disc and stay integers
 * here. worldscale.h puts the PC Quake II factor at ten and marks it INFERRED;
 * baking an inferred constant into an export would launder a guess into data.
 * Divide by ten downstream if you want id units.
 *
 * +Y IS DOWN ON THE DISC AND UP IN THE FILE. Every exported Y is negated, which
 * reverses handedness, so every face is also emitted in reverse order. The two
 * together leave the surface facing the way it faced on the console. Getting
 * only one of them right turns every wall inside out, and the symptom — a level
 * that looks fine until you notice you are seeing its back faces — is subtle
 * enough to be worth naming.
 *
 * A TEXTURE IS A (PAGE, PALETTE) PAIR, NOT A PAGE. The console samples one
 * 256x256 4bpp page through whichever 16-entry CLUT the polygon names, so the
 * same page is a dozen different images depending on who is drawing it. There
 * is no way to express that in a material system, so a material here is one
 * page already resolved through one palette, and pairs are emitted only when
 * something actually uses them.
 *
 * IMAGES ARE INDEXED PNG. An earlier version wrote PCX as well, on the grounds
 * that it is Quake II's own texture format. It is also a format most modern
 * viewers and every modelling package cannot open, and it has no way to express
 * the PlayStation's transparent texel — which is a real part of the art on any
 * grate, ladder, decal or menu. An indexed PNG carries the same palette, carries
 * the transparency in a tRNS block, and opens everywhere.
 */
#include "cmd_export.h"

#include "fixed.h"
#include "level.h"
#include "model.h"
#include "points.h"
#include "scene.h"
#include "trig.h"
#include "vag.h"
#include "vram.h"
#include "xa.h"
#include "q2psx.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define export_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define export_mkdir(p) mkdir((p), 0755)
#endif

/* poly.tpage is masked to five bits by the engine, so this is the ceiling on
 * anything a polygon can name — not on what a map actually carries. */
#define EXPORT_MAX_PAGES  32

/* A texture page is 256x256 texels: 128 bytes per row at two texels a byte. */
#define EXPORT_PAGE_TEXELS 256

typedef struct export_stats {
    u32 maps;
    u32 zones;
    u32 quads;
    u32 quads_untextured;
    u32 models;
    u32 model_faces;
    u32 textures;
    u32 images;
    u32 sounds;
    u32 music;
    u32 cdda;
    u32 zone_failed;
    u32 model_failed;
    u32 texture_missing;
} export_stats;

/* ------------------------------------------------------------------------- */
/* Paths and names                                                            */
/* ------------------------------------------------------------------------- */
static void make_dir(const char *path)
{
    char tmp[1024];
    size_t i;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (i = 0; tmp[i]; i++) {
        if ((tmp[i] == '/' || tmp[i] == '\\') && i > 0) {
            char saved = tmp[i];
            tmp[i] = '\0';
            export_mkdir(tmp);
            tmp[i] = saved;
        }
    }
    export_mkdir(tmp);
}

/*
 * The disc's names are ASCII but not all of them are legal file names, and a
 * VAG name is only NUL-padded rather than NUL-terminated in every build. Fold
 * anything unusual to an underscore rather than trusting the filesystem to
 * reject it cleanly.
 */
static void sanitize(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c) && c != '-' && c != '_' && c != '.')
            *s = '_';
    }
}

static bool name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* /Q2DATA/LEVELS/<MAP>/COMMON.DAT -> <MAP> */
static bool map_name_of(const char *path, char *out, size_t cap)
{
    const char *p = strstr(path, "/LEVELS/");
    const char *q;
    size_t n;

    if (!p)
        return false;
    p += 8;
    q = strchr(p, '/');
    if (!q)
        return false;

    n = (size_t)(q - p);
    if (n == 0 || n + 1 > cap)
        return false;

    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* 5-bit colour to 8-bit. Replicating the high bits rather than shifting left
 * matters at the top of the range: 31 must come back as 255, not 248. */
static u8 expand5(u16 v)
{
    return (u8)(((v & 0x1F) << 3) | ((v & 0x1F) >> 2));
}

/*
 * Turn `count` BGR555 CLUT entries into a PNG palette plus its alpha table. An
 * entry of zero is the hardware's TRANSPARENT texel rather than black, which is
 * the whole reason the output carries a tRNS block.
 */
static void build_palette(const u16 *entries, u32 count, u8 pal[768],
                          u8 *alpha)
{
    u32 e;

    memset(pal, 0, 768);
    for (e = 0; e < count; e++) {
        u16 c = entries[e];

        alpha[e] = (c == 0) ? 0 : 255;
        pal[e * 3 + 0] = expand5((u16)(c));
        pal[e * 3 + 1] = expand5((u16)(c >> 5));
        pal[e * 3 + 2] = expand5((u16)(c >> 10));
    }
}

/* ------------------------------------------------------------------------- */
/* PNG — indexed colour, with tRNS for the transparent texel                  */
/* ------------------------------------------------------------------------- */
static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 crc32_update(u32 crc, const u8 *data, size_t n)
{
    static u32 table[256];
    static bool ready = false;
    size_t i;

    if (!ready) {
        u32 k;
        int bit;
        for (k = 0; k < 256; k++) {
            u32 c = k;
            for (bit = 0; bit < 8; bit++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[k] = c;
        }
        ready = true;
    }

    for (i = 0; i < n; i++)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static void png_chunk(FILE *fp, const char *type, const u8 *data, size_t n)
{
    u8 hdr[8], tail[4];
    u32 crc;

    put_be32(hdr, (u32)n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, sizeof(hdr), fp);
    if (n)
        fwrite(data, 1, n, fp);

    crc = crc32_update(0xFFFFFFFFu, hdr + 4, 4);
    if (n)
        crc = crc32_update(crc, data, n);
    put_be32(tail, crc ^ 0xFFFFFFFFu);
    fwrite(tail, 1, sizeof(tail), fp);
}

/*
 * A PNG needs a zlib stream, and deflate's STORED block is a legal one that
 * costs five bytes per 64 KB and no compression code at all. The file comes out
 * about the size of the raw indices, which is the right trade for an
 * intermediate asset — a real deflate would be a few hundred lines of Huffman
 * for an image nobody ships.
 */
static u8 *zlib_store(const u8 *src, size_t n, size_t *out_size)
{
    size_t blocks = (n + 65534) / 65535;
    size_t i, o = 0;
    u32 a = 1, b = 0;
    u8 *out;

    if (blocks == 0)
        blocks = 1;

    out = (u8 *)malloc(2 + blocks * 5 + n + 4);
    if (!out)
        return NULL;

    out[o++] = 0x78;                 /* CM 8, CINFO 7 */
    out[o++] = 0x01;                 /* FCHECK, no dictionary, fastest */

    for (i = 0; i < blocks; i++) {
        size_t off = i * 65535;
        u32    len = (u32)((n - off > 65535) ? 65535 : (n - off));

        out[o++] = (u8)((i + 1 == blocks) ? 1 : 0);
        out[o++] = (u8)(len & 0xFF);
        out[o++] = (u8)(len >> 8);
        out[o++] = (u8)(~len & 0xFF);
        out[o++] = (u8)((~len >> 8) & 0xFF);
        if (len)
            memcpy(out + o, src + off, len);
        o += len;
    }

    for (i = 0; i < n; i++) {
        a = (a + src[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(out + o, (b << 16) | a);
    o += 4;

    *out_size = o;
    return out;
}

static bool write_png8(const char *path, u32 w, u32 h, const u8 *px,
                       const u8 *pal, u32 pal_count, const u8 *alpha)
{
    static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    FILE *fp;
    u8 ihdr[13];
    u8 *raw, *z;
    size_t raw_size, z_size, y;
    bool need_trns = false;
    u32 i;

    if (!w || !h || !pal_count || pal_count > 256)
        return false;

    raw_size = (size_t)h * ((size_t)w + 1);
    raw = (u8 *)malloc(raw_size);
    if (!raw)
        return false;

    for (y = 0; y < h; y++) {
        raw[y * (w + 1)] = 0;                          /* filter type: none */
        memcpy(raw + y * (w + 1) + 1, px + y * (size_t)w, w);
    }

    z = zlib_store(raw, raw_size, &z_size);
    free(raw);
    if (!z)
        return false;

    fp = fopen(path, "wb");
    if (!fp) {
        free(z);
        return false;
    }

    fwrite(sig, 1, sizeof(sig), fp);

    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8]  = 8;      /* bit depth        */
    ihdr[9]  = 3;      /* colour type: indexed */
    ihdr[10] = 0;      /* compression      */
    ihdr[11] = 0;      /* filter           */
    ihdr[12] = 0;      /* no interlace     */
    png_chunk(fp, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(fp, "PLTE", pal, (size_t)pal_count * 3);

    if (alpha) {
        for (i = 0; i < pal_count; i++)
            if (alpha[i] != 255)
                need_trns = true;
        if (need_trns)
            png_chunk(fp, "tRNS", alpha, pal_count);
    }

    png_chunk(fp, "IDAT", z, z_size);
    png_chunk(fp, "IEND", NULL, 0);

    free(z);
    return fclose(fp) == 0;
}

/* ------------------------------------------------------------------------- */
/* WAV — 16-bit PCM, written incrementally so a 30-minute track never has to   */
/* be held in memory                                                          */
/* ------------------------------------------------------------------------- */
typedef struct wav_out {
    FILE *fp;
    u32   samples;      /* total across all channels */
    u16   channels;
    u32   rate;
} wav_out;

static void put_le16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void wav_header(u8 h[44], u32 samples, u16 channels, u32 rate)
{
    u32 data_bytes = samples * 2u;

    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36u + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);                              /* fmt chunk size   */
    put_le16(h + 20, 1);                               /* PCM              */
    put_le16(h + 22, channels);
    put_le32(h + 24, rate);
    put_le32(h + 28, rate * channels * 2u);            /* byte rate        */
    put_le16(h + 32, (u16)(channels * 2u));            /* block align      */
    put_le16(h + 34, 16);                              /* bits per sample  */
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

static bool wav_open(wav_out *w, const char *path, u16 channels, u32 rate)
{
    u8 head[44];

    w->fp = fopen(path, "wb");
    if (!w->fp)
        return false;

    w->samples  = 0;
    w->channels = channels;
    w->rate     = rate;

    wav_header(head, 0, channels, rate);
    fwrite(head, 1, sizeof(head), w->fp);
    return true;
}

/* Host-endian samples, converted on the way out so the file is correct on a
 * big-endian host too. */
static void wav_samples(wav_out *w, const s16 *pcm, u32 count)
{
    u8 stage[4096];
    u32 done = 0;

    while (done < count) {
        u32 n = count - done;
        u32 i;

        if (n > (u32)(sizeof(stage) / 2))
            n = (u32)(sizeof(stage) / 2);
        for (i = 0; i < n; i++)
            put_le16(stage + i * 2, (u16)pcm[done + i]);
        fwrite(stage, 1, (size_t)n * 2, w->fp);
        done += n;
    }
    w->samples += count;
}

/* Bytes that are already little-endian 16-bit PCM — a CD-DA sector is exactly
 * that, so it goes straight through. */
static void wav_raw(wav_out *w, const u8 *bytes, u32 n)
{
    fwrite(bytes, 1, n, w->fp);
    w->samples += n / 2u;
}

static bool wav_close(wav_out *w)
{
    u8 head[44];

    if (!w->fp)
        return false;

    wav_header(head, w->samples, w->channels, w->rate);
    if (fseek(w->fp, 0, SEEK_SET) == 0)
        fwrite(head, 1, sizeof(head), w->fp);

    return fclose(w->fp) == 0;
}

static bool write_wav16(const char *path, const s16 *pcm, u32 samples,
                        u16 channels, u32 rate)
{
    wav_out w;

    if (!wav_open(&w, path, channels, rate))
        return false;
    wav_samples(&w, pcm, samples);
    return wav_close(&w);
}

/* ------------------------------------------------------------------------- */
/* The material set of one OBJ                                                */
/* ------------------------------------------------------------------------- */
#define EXPORT_MTL_NAME 24

typedef struct mtl_set {
    char (*names)[EXPORT_MTL_NAME];
    u32 count, cap;
} mtl_set;

static void mtl_add(mtl_set *s, const char *name)
{
    u32 i;

    for (i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0)
            return;

    if (s->count == s->cap) {
        u32 cap = s->cap ? s->cap * 2 : 32;
        void *grown = realloc(s->names, (size_t)cap * EXPORT_MTL_NAME);
        if (!grown)
            return;
        s->names = (char (*)[EXPORT_MTL_NAME])grown;
        s->cap = cap;
    }

    snprintf(s->names[s->count], EXPORT_MTL_NAME, "%s", name);
    s->count++;
}

static void mtl_free(mtl_set *s)
{
    free(s->names);
    s->names = NULL;
    s->count = s->cap = 0;
}

/*
 * `prefix` is the path from the OBJ to the texture directory, which differs
 * between a zone (textures/) and a model (../textures/). Relative paths are
 * what an MTL is supposed to carry, so the whole tree stays movable.
 */
static void mtl_write(const mtl_set *s, const char *path, const char *prefix)
{
    FILE *fp = fopen(path, "wb");
    u32 i;

    if (!fp)
        return;

    fprintf(fp, "# Q2PSX-PC asset export\n");
    fprintf(fp, "# One material is one 256x256 texture page resolved through\n");
    fprintf(fp, "# one 16-entry CLUT: p<page>_c<clut>.\n\n");

    for (i = 0; i < s->count; i++) {
        fprintf(fp, "newmtl %s\n", s->names[i]);
        fprintf(fp, "Ka 1.000 1.000 1.000\n");
        fprintf(fp, "Kd 1.000 1.000 1.000\n");
        fprintf(fp, "Ks 0.000 0.000 0.000\n");
        fprintf(fp, "d 1.000\n");
        fprintf(fp, "illum 1\n");
        if (strcmp(s->names[i], "untextured") != 0) {
            fprintf(fp, "map_Kd %s%s.png\n", prefix, s->names[i]);
            fprintf(fp, "map_d %s%s.png\n", prefix, s->names[i]);
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
}

/* ------------------------------------------------------------------------- */
/* The per-map texture bank                                                   */
/* ------------------------------------------------------------------------- */
typedef struct tex_ref {
    u8  page;
    u16 clut;
} tex_ref;

typedef struct tex_bank {
    const q2_vram_section *vs;
    char      dir[768];
    u8       *page[EXPORT_MAX_PAGES];
    bool      page_failed[EXPORT_MAX_PAGES];
    tex_ref  *seen;
    u32       seen_count, seen_cap;
    u32       written;
    u32       missing;
} tex_bank;

static void tex_bank_free(tex_bank *tb)
{
    u32 i;

    for (i = 0; i < EXPORT_MAX_PAGES; i++)
        free(tb->page[i]);
    free(tb->seen);
    memset(tb, 0, sizeof(*tb));
}

static const u8 *tex_page_data(tex_bank *tb, u32 page)
{
    size_t need, got;
    u8 *buf;

    if (!tb->vs || page >= EXPORT_MAX_PAGES || page >= tb->vs->texpage_count)
        return NULL;
    if (tb->page[page])
        return tb->page[page];
    if (tb->page_failed[page])
        return NULL;

    need = q2_vram_decoded_size(tb->vs, page);
    if (need != (size_t)Q2_VRAM_TEXPAGE_W * Q2_VRAM_TEXPAGE_H) {
        tb->page_failed[page] = true;
        return NULL;
    }

    buf = (u8 *)malloc(need);
    if (!buf) {
        tb->page_failed[page] = true;
        return NULL;
    }
    if (q2_vram_decode(tb->vs, page, buf, need, &got) != Q2_OK || got != need) {
        free(buf);
        tb->page_failed[page] = true;
        return NULL;
    }

    tb->page[page] = buf;
    return buf;
}

/*
 * Expand one page through one palette and write it both ways.
 *
 * The nibble order is the hardware's: a VRAM halfword holds four texels with
 * texel u in bits (u & 3) * 4, so in the decoded byte stream the LOW nibble of
 * a byte is the left-hand texel. Getting it backwards produces an image that is
 * still recognisable but shifted by one texel on every other column, which is
 * exactly the kind of fault that survives a glance.
 */
static bool tex_emit(tex_bank *tb, u32 page, u32 clut, const char *name)
{
    const u8 *src = tex_page_data(tb, page);
    u16 entries[Q2_VRAM_CLUT4_ENTRIES];
    u8  pal[768], alpha[Q2_VRAM_CLUT4_ENTRIES], *px;
    char path[1024];
    u32 x, y;
    bool ok;

    if (!src)
        return false;
    if (!q2_vram_get_clut4(tb->vs, clut, entries))
        return false;

    build_palette(entries, Q2_VRAM_CLUT4_ENTRIES, pal, alpha);

    px = (u8 *)malloc((size_t)EXPORT_PAGE_TEXELS * EXPORT_PAGE_TEXELS);
    if (!px)
        return false;

    for (y = 0; y < EXPORT_PAGE_TEXELS; y++) {
        const u8 *row = src + (size_t)y * Q2_VRAM_TEXPAGE_W;
        for (x = 0; x < EXPORT_PAGE_TEXELS; x++)
            px[y * EXPORT_PAGE_TEXELS + x] =
                (u8)((row[x >> 1] >> ((x & 1u) * 4)) & 0x0Fu);
    }

    snprintf(path, sizeof(path), "%s/%s.png", tb->dir, name);
    ok = write_png8(path, EXPORT_PAGE_TEXELS, EXPORT_PAGE_TEXELS, px, pal,
                    Q2_VRAM_CLUT4_ENTRIES, alpha);

    free(px);
    return ok;
}

/*
 * Name the material for one (page, palette) pair, emitting the image the first
 * time the pair is seen. Returns false when the map does not carry that page,
 * which is a fault rather than a style — the caller counts it and falls back to
 * an untextured material so the geometry still comes out.
 */
static bool tex_material(tex_bank *tb, u32 page, u32 clut, char *out, size_t cap)
{
    u32 i;

    snprintf(out, cap, "p%02u_c%03u", page, clut);

    for (i = 0; i < tb->seen_count; i++)
        if (tb->seen[i].page == (u8)page && tb->seen[i].clut == (u16)clut)
            return true;

    if (!tex_emit(tb, page, clut, out)) {
        tb->missing++;
        return false;
    }

    if (tb->seen_count == tb->seen_cap) {
        u32 cap_new = tb->seen_cap ? tb->seen_cap * 2 : 64;
        tex_ref *grown = (tex_ref *)realloc(tb->seen,
                                            (size_t)cap_new * sizeof(tex_ref));
        if (!grown)
            return true;      /* the image is written; only the memo is lost */
        tb->seen = grown;
        tb->seen_cap = cap_new;
    }

    tb->seen[tb->seen_count].page = (u8)page;
    tb->seen[tb->seen_count].clut = (u16)clut;
    tb->seen_count++;
    tb->written++;
    return true;
}

/*
 * How many of a 256-entry block's sixteen 16-entry sub-CLUTs are distinct.
 *
 * This is what tells a 4bpp image apart from an 8bpp one, and it is the only
 * test found that separates them without an exception. See tex_emit_images().
 */
static u32 clut8_distinct_subs(const u16 entries[Q2_VRAM_CLUT8_ENTRIES])
{
    u32 i, j, distinct = 0;

    for (i = 0; i < 16; i++) {
        bool dup = false;
        for (j = 0; j < i; j++) {
            if (memcmp(&entries[i * 16], &entries[j * 16],
                       16 * sizeof(u16)) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup)
            distinct++;
    }
    return distinct;
}

/* Above this many distinct sub-CLUTs the block is one real 256-entry palette;
 * at or below it, it is a handful of 4bpp palettes padded out with filler. The
 * measured gap is 6 against 12, so the threshold is not near anything. */
#define EXPORT_SUBCLUT_4BPP_MAX 8

/*
 * The standalone images — HUD, menu, globe and title art — are the records past
 * the texture pages, and each carries a 512-byte palette block.
 *
 * THEIR BIT DEPTH IS NOT UNIFORM, and assuming it is was wrong. The record's
 * `width` is BYTES per row, so the same payload is either `width` texels of
 * 8bpp or `width * 2` texels of 4bpp, and only one of the two is a picture. Read
 * at the wrong depth the menu sheets come out as magenta noise while still
 * being structured enough to look like a palette problem rather than a depth
 * one, which is what made this worth pinning down properly.
 *
 * What separates them is the shape of the palette block, not any statistic of
 * the pixels. A 4bpp image's 512 bytes are sixteen 16-entry CLUTs of which only
 * a few are real and the rest are repeated filler; an 8bpp image's are one
 * 256-entry palette with all sixteen sub-blocks distinct. Over the disc's 222
 * images that measure is 2, 4 or 6 for every image whose 4bpp reading is a
 * picture, and 12 or 16 for every image whose 8bpp reading is — 15 of 15 on the
 * images checked by eye, with no overlap.
 *
 * Three statistics that look like they should work and do not, recorded so
 * nobody spends the afternoon again: adjacency coherence (degenerate on sparse
 * icon sheets, which are mostly flat background), the share of pixels landing on
 * dead palette entries (flags the DualShock art, which is genuinely 8bpp), and
 * the double-nibble rate (a large flat black background inflates it, so
 * control.lbm scores above qkm_menu.lbm).
 *
 * A 4bpp image gets one file per DISTINCT sub-CLUT, the same way a texture page
 * does, because those really are alternate palettes for one sheet — qk_menu's
 * icons exist in both a green and a red set.
 */
static u32 tex_emit_images(tex_bank *tb)
{
    u32 i, done = 0;

    if (!tb->vs)
        return 0;

    for (i = tb->vs->texpage_count; i < tb->vs->image_count; i++) {
        const q2_vram_image *img = &tb->vs->images[i];
        u16 entries[Q2_VRAM_CLUT8_ENTRIES];
        u8  pal[768], alpha[Q2_VRAM_CLUT8_ENTRIES];
        u8 *px;
        char name[64], path[1024];
        size_t need, got;

        need = q2_vram_decoded_size(tb->vs, i);
        if (!need || need != (size_t)img->width * img->height)
            continue;

        px = (u8 *)malloc(need);
        if (!px)
            continue;
        if (q2_vram_decode(tb->vs, i, px, need, &got) != Q2_OK || got != need) {
            free(px);
            continue;
        }

        if (!q2_vram_get_clut8(tb->vs, i - tb->vs->texpage_count, entries)) {
            free(px);
            continue;
        }

        snprintf(name, sizeof(name), "%s", img->name ? img->name : "unnamed");
        sanitize(name);

        if (clut8_distinct_subs(entries) <= EXPORT_SUBCLUT_4BPP_MAX) {
            u32 texw = (u32)img->width * 2;
            u8 *expanded = (u8 *)malloc((size_t)texw * img->height);
            u32 sub;

            if (!expanded) {
                free(px);
                continue;
            }

            /* Low nibble first, as the hardware packs it. */
            {
                u32 x, y;
                for (y = 0; y < img->height; y++) {
                    const u8 *row = px + (size_t)y * img->width;
                    for (x = 0; x < texw; x++)
                        expanded[(size_t)y * texw + x] =
                            (u8)((row[x >> 1] >> ((x & 1u) * 4)) & 0x0Fu);
                }
            }

            for (sub = 0; sub < 16; sub++) {
                bool dup = false, flat = true;
                u32 j;

                for (j = 0; j < sub; j++) {
                    if (memcmp(&entries[sub * 16], &entries[j * 16],
                               16 * sizeof(u16)) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup)
                    continue;

                /* A sub-CLUT whose sixteen entries are one repeated colour is
                 * padding, not a palette — it can only render a solid block. */
                for (j = 1; j < 16; j++) {
                    if (entries[sub * 16 + j] != entries[sub * 16]) {
                        flat = false;
                        break;
                    }
                }
                if (flat)
                    continue;

                build_palette(&entries[sub * 16], Q2_VRAM_CLUT4_ENTRIES, pal,
                              alpha);
                snprintf(path, sizeof(path), "%s/img_%02u_%s_c%u.png",
                         tb->dir, i, name, sub);
                if (write_png8(path, texw, img->height, expanded, pal,
                               Q2_VRAM_CLUT4_ENTRIES, alpha))
                    done++;
            }

            free(expanded);
        } else {
            build_palette(entries, Q2_VRAM_CLUT8_ENTRIES, pal, alpha);
            snprintf(path, sizeof(path), "%s/img_%02u_%s.png", tb->dir, i,
                     name);
            if (write_png8(path, img->width, img->height, px, pal,
                           Q2_VRAM_CLUT8_ENTRIES, alpha))
                done++;
        }

        free(px);
    }

    return done;
}

/* ------------------------------------------------------------------------- */
/* Zone geometry                                                              */
/* ------------------------------------------------------------------------- */
static void obj_preamble(FILE *fp, const char *mtl_name, const char *object)
{
    fprintf(fp, "# Q2PSX-PC asset export\n");
    fprintf(fp, "# Units are the disc's own; +Y is up here and down on the\n");
    fprintf(fp, "# disc, so Y is negated and every face is reversed to match.\n");
    fprintf(fp, "# Vertex lines carry r g b after the position.\n");
    fprintf(fp, "mtllib %s\n", mtl_name);
    fprintf(fp, "o %s\n", object);
}

static int export_zone(disc *d, const char *map, u32 zone_index,
                       tex_bank *tb, const char *mapdir, export_stats *st)
{
    char path[256], obj_path[1024], mtl_path[1024], base[64], cur[EXPORT_MTL_NAME];
    q2_buf buf;
    q2_zone_file zf;
    q2_points pts;
    q2_scene sc;
    mtl_set mtl;
    FILE *fp;
    u32 node, vbase = 1, tbase = 1;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/ZONE%u.DAT", map, zone_index);
    if (disc_read_file(d, path, &buf) != Q2_OK)
        return 0;                       /* the map simply has fewer zones */

    if (q2_zone_open(&zf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        st->zone_failed++;
        return -1;
    }
    if (q2_points_parse(&pts, &zf) != Q2_OK) {
        q2_zone_close(&zf);
        st->zone_failed++;
        return -1;
    }
    if (q2_scene_parse(&sc, &zf) != Q2_OK) {
        q2_points_free(&pts);
        q2_zone_close(&zf);
        st->zone_failed++;
        return -1;
    }

    snprintf(base, sizeof(base), "%s_ZONE%u", map, zone_index);
    snprintf(obj_path, sizeof(obj_path), "%.*s/%s.obj",
             (int)(sizeof(obj_path) - sizeof(base) - 8), mapdir, base);
    snprintf(mtl_path, sizeof(mtl_path), "%.*s/%s.mtl",
             (int)(sizeof(mtl_path) - sizeof(base) - 8), mapdir, base);

    fp = fopen(obj_path, "wb");
    if (!fp) {
        q2_points_free(&pts);
        q2_zone_close(&zf);
        st->zone_failed++;
        return -1;
    }

    memset(&mtl, 0, sizeof(mtl));
    cur[0] = '\0';

    {
        char mtl_base[80];
        snprintf(mtl_base, sizeof(mtl_base), "%s.mtl", base);
        obj_preamble(fp, mtl_base, base);
    }

    for (node = 0; node < sc.node_count; node++) {
        q2_scene_node n;
        q2_mapmod_rec rec;
        const q2_point_group *grp;
        u32 p;

        if (!q2_scene_get_node(&sc, node, &n))
            continue;
        if (!q2_scene_get_mapmod(&sc, node, &rec))
            continue;
        if (node >= pts.group_count)
            continue;
        grp = &pts.groups[node];

        if (rec.num_polys == 0)
            continue;

        fprintf(fp, "g node%04u\n", node);

        for (p = 0; p < rec.num_polys; p++) {
            q2_mapmod_poly poly;
            q2_point pt[4];
            char name[EXPORT_MTL_NAME];
            bool textured;
            bool ok = true;
            u32 i;

            if (!q2_mapmod_get_poly(&rec, p, &poly))
                continue;

            for (i = 0; i < 4; i++) {
                if (poly.vtx[i] >= grp->count ||
                    !q2_points_get(&pts, grp->first + poly.vtx[i], &pt[i])) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;

            textured = (rec.uv != NULL) && (poly.uv_idx < rec.uv_count) &&
                       tex_material(tb, poly.tpage,
                                    q2_mapmod_clut_index(poly.clut),
                                    name, sizeof(name));
            if (!textured) {
                snprintf(name, sizeof(name), "untextured");
                st->quads_untextured++;
            }

            if (strcmp(name, cur) != 0) {
                fprintf(fp, "usemtl %s\n", name);
                mtl_add(&mtl, name);
                snprintf(cur, sizeof(cur), "%s", name);
            }

            for (i = 0; i < 4; i++) {
                u32 ci = poly.col[i];
                double r = 0.5, g = 0.5, b = 0.5;

                if (rec.rgb && ci < rec.rgb_count) {
                    r = rec.rgb[ci * 3 + 0] / 255.0;
                    g = rec.rgb[ci * 3 + 1] / 255.0;
                    b = rec.rgb[ci * 3 + 2] / 255.0;
                }

                fprintf(fp, "v %d %d %d %.4f %.4f %.4f\n",
                        (int)(pt[i].x + n.origin[0]),
                        (int)-(pt[i].y + n.origin[1]),
                        (int)(pt[i].z + n.origin[2]), r, g, b);
            }

            if (textured) {
                const u8 *uv = rec.uv + (size_t)poly.uv_idx * 8;
                for (i = 0; i < 4; i++) {
                    /* The UV corners are rotated and reversed against the
                     * vertices; the rule is the world renderer's own, and
                     * world.c carries the derivation. */
                    u32 c = (3u - poly.flags - i) & 3u;
                    fprintf(fp, "vt %.6f %.6f\n",
                            (uv[c * 2 + 0] + 0.5) / 256.0,
                            1.0 - (uv[c * 2 + 1] + 0.5) / 256.0);
                }
                fprintf(fp, "f %u/%u %u/%u %u/%u %u/%u\n",
                        vbase + 3, tbase + 3, vbase + 2, tbase + 2,
                        vbase + 1, tbase + 1, vbase, tbase);
                tbase += 4;
            } else {
                fprintf(fp, "f %u %u %u %u\n",
                        vbase + 3, vbase + 2, vbase + 1, vbase);
            }

            vbase += 4;
            st->quads++;
        }
    }

    fclose(fp);
    mtl_write(&mtl, mtl_path, "textures/");
    mtl_free(&mtl);

    q2_points_free(&pts);
    q2_zone_close(&zf);
    st->zones++;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Models                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * A model's vertices are PART-LOCAL, so an unposed export piles every part on
 * the origin. Frame 0 of clip 0 is the closest thing the format has to a rest
 * pose, and it is what the engine itself shows before an entity has selected an
 * animation. Models with no animation chain are already in model space and go
 * out as they are.
 *
 * Faces index a shared 96-slot scratch window that later parts overwrite, so a
 * face is emitted with its own four corners rather than into a shared vertex
 * list — the aliasing is the format, and flattening it would be a lie about
 * which vertex a face means.
 */
static bool export_model(const q2_model *m, const char *dir, const char *base,
                         tex_bank *tb, u32 clut4_count_a, export_stats *st)
{
    s32  wpos[Q2_MODEL_SCRATCH_MAX][3];
    s32  wnrm[Q2_MODEL_SCRATCH_MAX][3];
    bool wvalid[Q2_MODEL_SCRATCH_MAX];
    q2_model_pose pose[Q2_MODEL_SCRATCH_MAX * 2];
    q2_model_anim clip;
    mtl_set mtl;
    FILE *fp;
    char obj_path[1024], mtl_path[1024], cur[EXPORT_MTL_NAME];
    bool have_pose = false;
    u32 part, face_index = 0, vertex_cursor = 0, vbase = 1;

    if (m->hdr.num_parts && m->hdr.num_parts <= (u32)Q2PSX_ARRAY_COUNT(pose) &&
        q2_model_anim_count(m) > 0 && q2_model_anim_get(m, 0, &clip) &&
        q2_model_pose_at(m, &clip, 0, pose) == Q2_OK)
        have_pose = true;

    snprintf(obj_path, sizeof(obj_path), "%s/%s.obj", dir, base);
    snprintf(mtl_path, sizeof(mtl_path), "%s/%s.mtl", dir, base);

    fp = fopen(obj_path, "wb");
    if (!fp)
        return false;

    memset(&mtl, 0, sizeof(mtl));
    memset(wvalid, 0, sizeof(wvalid));
    cur[0] = '\0';

    {
        char mtl_base[80];
        snprintf(mtl_base, sizeof(mtl_base), "%s.mtl", base);
        obj_preamble(fp, mtl_base, base);
        fprintf(fp, "# %s: %u parts, %u vertices, %u faces, %s%s\n",
                m->hdr.name, m->hdr.num_parts, m->hdr.num_verts,
                m->hdr.num_faces,
                q2_model_is_static(m) ? "static" : "articulated",
                have_pose ? ", posed at clip 0 frame 0" : ", unposed");
    }

    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;
        s16 rot[3][3];
        s32 t[3] = { 0, 0, 0 };
        u32 v;

        if (!q2_model_get_part(m, part, &p))
            break;

        if (have_pose) {
            q2_quat_to_matrix(rot, pose[part].q);
            t[0] = pose[part].t[0];
            t[1] = pose[part].t[1];
            t[2] = pose[part].t[2];
        } else {
            memset(rot, 0, sizeof(rot));
            rot[0][0] = rot[1][1] = rot[2][2] = (s16)Q2_ONE_12;
        }

        for (v = 0; v < p.num_verts; v++) {
            q2_model_vertex mv;
            u32 slot = p.vert_base + v;
            int ax;

            if (slot >= Q2_MODEL_SCRATCH_MAX)
                break;
            if (!q2_model_get_vertex(m, vertex_cursor + v, &mv))
                break;

            for (ax = 0; ax < 3; ax++) {
                wpos[slot][ax] = (((s32)rot[ax][0] * mv.x +
                                   (s32)rot[ax][1] * mv.y +
                                   (s32)rot[ax][2] * mv.z) >> Q2_FRAC_12) + t[ax];
                wnrm[slot][ax] = ((s32)rot[ax][0] * mv.nx +
                                  (s32)rot[ax][1] * mv.ny +
                                  (s32)rot[ax][2] * mv.nz) >> Q2_FRAC_12;
            }
            wvalid[slot] = true;
        }

        vertex_cursor += p.num_verts;

        for (v = 0; v < p.num_faces; v++, face_index++) {
            q2_model_face f;
            char name[EXPORT_MTL_NAME];
            bool ok = true, textured;
            u32 i;

            if (!q2_model_get_face(m, face_index, &f))
                break;

            for (i = 0; i < 4; i++) {
                if (f.v[i] >= Q2_MODEL_SCRATCH_MAX || !wvalid[f.v[i]]) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;

            textured = tex_material(tb, q2_model_face_page(&f),
                                    q2_model_face_clut_index(&f, clut4_count_a),
                                    name, sizeof(name));
            if (!textured)
                snprintf(name, sizeof(name), "untextured");

            if (strcmp(name, cur) != 0) {
                fprintf(fp, "usemtl %s\n", name);
                mtl_add(&mtl, name);
                snprintf(cur, sizeof(cur), "%s", name);
            }

            for (i = 0; i < 4; i++) {
                const s32 *pos = wpos[f.v[i]];
                const s32 *nrm = wnrm[f.v[i]];

                fprintf(fp, "v %d %d %d\n", (int)pos[0], (int)-pos[1],
                        (int)pos[2]);
                fprintf(fp, "vt %.6f %.6f\n",
                        (f.uv[i][0] + 0.5) / 256.0,
                        1.0 - (f.uv[i][1] + 0.5) / 256.0);
                fprintf(fp, "vn %.4f %.4f %.4f\n",
                        nrm[0] / (double)Q2_ONE_12,
                        -nrm[1] / (double)Q2_ONE_12,
                        nrm[2] / (double)Q2_ONE_12);
            }

            fprintf(fp, "f %u/%u/%u %u/%u/%u %u/%u/%u %u/%u/%u\n",
                    vbase + 3, vbase + 3, vbase + 3,
                    vbase + 2, vbase + 2, vbase + 2,
                    vbase + 1, vbase + 1, vbase + 1,
                    vbase, vbase, vbase);

            vbase += 4;
            st->model_faces++;
        }
    }

    fclose(fp);
    mtl_write(&mtl, mtl_path, "../textures/");
    mtl_free(&mtl);

    st->models++;
    return true;
}

static void export_model_bank(const q2_model_bank *bank, const char *dir,
                              const char *prefix, tex_bank *tb,
                              u32 clut4_count_a, export_stats *st)
{
    u32 i;

    for (i = 0; i < bank->count; i++) {
        q2_model m;
        char name[96], base[128];

        if (q2_model_get(bank, i, &m) != Q2_OK) {
            st->model_failed++;
            continue;
        }

        snprintf(name, sizeof(name), "%s", m.hdr.name);
        sanitize(name);
        /* Model names are not unique — the index is what makes the file name
         * one, and it is also how `q2psx-inspect model` addresses them. */
        snprintf(base, sizeof(base), "%s%03u_%s", prefix, i, name);

        export_model(&m, dir, base, tb, clut4_count_a, st);
    }
}

static void export_models(disc *d, const char *map, tex_bank *tb,
                          const char *mapdir, export_stats *st)
{
    char path[256], dir[1024];
    q2_buf buf;
    u32 zone;

    snprintf(dir, sizeof(dir), "%.*s/models", (int)(sizeof(dir) - 16), mapdir);

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) == Q2_OK) {
        q2_common_file cf;
        if (q2_common_open(&cf, &buf) == Q2_OK) {
            q2_model_bank bank;
            if (q2_model_bank_from_common(&bank, &cf) == Q2_OK && bank.count) {
                make_dir(dir);
                export_model_bank(&bank, dir, "", tb,
                                  tb->vs ? tb->vs->clut4_count_a : 0, st);
            }
            q2_common_close(&cf);
        } else {
            q2_buf_free(&buf);
        }
    }

    /* A zone can carry its own CastList as well; 98 of 115 do. */
    for (zone = 0; zone < 16; zone++) {
        q2_zone_file zf;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/ZONE%u.DAT", map, zone);
        if (disc_read_file(d, path, &buf) != Q2_OK)
            continue;
        if (q2_zone_open(&zf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }
        {
            q2_model_bank bank;
            if (q2_model_bank_from_zone(&bank, &zf) == Q2_OK && bank.count) {
                char prefix[32];
                snprintf(prefix, sizeof(prefix), "zone%u_", zone);
                make_dir(dir);
                export_model_bank(&bank, dir, prefix, tb,
                                  tb->vs ? tb->vs->clut4_count_a : 0, st);
            }
        }
        q2_zone_close(&zf);
    }
}

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */
/* ------------------------------------------------------------------------- */
static u32 export_sounds(disc *d, const char *map, const char *mapdir)
{
    q2_sound_bank bank;
    char dir[1024];
    u32 i, done = 0;

    if (q2_sound_bank_load(&bank, d, map) != Q2_OK)
        return 0;

    snprintf(dir, sizeof(dir), "%.*s/sounds", (int)(sizeof(dir) - 16), mapdir);
    make_dir(dir);

    for (i = 0; i < bank.count; i++) {
        q2_vag vag;
        s16 *pcm;
        u32 cap, got;
        char name[32], path[1024];

        if (!q2_sound_bank_get(&bank, i, &vag))
            continue;

        cap = (vag.data_size / SPU_BLOCK_SIZE) * SPU_SAMPLES_PER_BLOCK;
        if (!cap)
            continue;

        pcm = (s16 *)malloc((size_t)cap * sizeof(s16));
        if (!pcm)
            continue;

        got = q2_spu_adpcm_decode(vag.body, vag.data_size, pcm, cap);

        snprintf(name, sizeof(name), "%s", vag.name[0] ? vag.name : "unnamed");
        sanitize(name);

        /* The runtime sound id is the bank index plus one — slot 0 is a null
         * placeholder — so the file name carries the id, not the index. */
        snprintf(path, sizeof(path), "%.*s/%03u_%.*s.wav",
                 (int)(sizeof(path) - sizeof(name) - 24), dir,
                 i + 1, (int)(sizeof(name) - 1), name);
        if (got && write_wav16(path, pcm, got, 1, vag.sample_rate))
            done++;

        free(pcm);
    }

    q2_sound_bank_free(&bank);
    return done;
}

static u32 export_music(disc *d, const char *outdir)
{
    char dir[1024];
    char letter;
    u32 done = 0;

    snprintf(dir, sizeof(dir), "%s/music", outdir);
    make_dir(dir);

    for (letter = 'A'; letter <= 'E'; letter++) {
        u8 channel;

        for (channel = 0; channel < XAI_CHANNEL_COUNT; channel++) {
            q2_xa_track track;
            q2_xa_decoder dec;
            wav_out w;
            s16 pcm[XA_FRAMES_PER_SECTOR * XA_CHANNELS];
            u32 cursor = 0, n;
            char path[1024];

            if (q2_xa_track_open(&track, d, letter, channel) != Q2_OK)
                continue;

            snprintf(path, sizeof(path), "%.*s/QUAKE_%c_ch%u.wav",
                     (int)(sizeof(path) - 32), dir, letter, channel);
            if (!wav_open(&w, path, XA_CHANNELS, XA_SAMPLE_RATE))
                continue;

            q2_xa_decoder_reset(&dec);
            while ((n = q2_xa_track_read(&track, &dec, &cursor, pcm,
                                         (u32)Q2PSX_ARRAY_COUNT(pcm))) > 0)
                wav_samples(&w, pcm, n);

            wav_close(&w);
            printf("  music QUAKE_%c channel %u: %.1f s\n", letter, channel,
                   (double)w.samples / (XA_CHANNELS * (double)XA_SAMPLE_RATE));
            done++;
        }
    }

    return done;
}

/* The Red Book track is already 16-bit stereo LE at 44100; a WAV of it is the
 * same bytes with a header in front. */
static u32 export_cdda(disc *d, const char *outdir)
{
    char dir[1024];
    int i, n = disc_track_count(d);
    u32 done = 0;

    snprintf(dir, sizeof(dir), "%s/cdda", outdir);

    for (i = 0; i < n; i++) {
        const cd_track *t = disc_track(d, i);
        wav_out w;
        char path[1024];
        u32 s;

        if (!t || t->type != CD_TRACK_AUDIO)
            continue;

        make_dir(dir);
        snprintf(path, sizeof(path), "%.*s/track%02d.wav",
                 (int)(sizeof(path) - 32), dir, t->number);
        if (!wav_open(&w, path, 2, 44100))
            continue;

        for (s = 0; s < t->length_sectors; s++) {
            u8 raw[2352];
            if (disc_read_raw_sector(d, t->start_lba + s, raw) != Q2_OK)
                break;
            wav_raw(&w, raw, sizeof(raw));
        }

        wav_close(&w);
        printf("  cdda track %d: %.1f s\n", t->number, w.samples / (2.0 * 44100.0));
        done++;
    }

    return done;
}

/* ------------------------------------------------------------------------- */
/* The manifest                                                               */
/* ------------------------------------------------------------------------- */
static void write_readme(const char *outdir, const disc *d,
                         const export_stats *st)
{
    char path[1024];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/README.txt", outdir);
    fp = fopen(path, "wb");
    if (!fp)
        return;

    fprintf(fp,
"Quake II PSX asset export\n"
"=========================\n"
"\n"
"Produced by `q2psx-inspect export` from %s.\n"
"Nothing here is authored: every file is a decode of the disc.\n"
"\n"
"Layout\n"
"------\n"
"  <MAP>/<MAP>_ZONE<n>.obj|.mtl   one streamed zone of world geometry\n"
"  <MAP>/models/<nnn>_<NAME>.obj  one CastList model, posed\n"
"  <MAP>/models/zone<z>_...       models from that zone's own CastList\n"
"  <MAP>/textures/p<pp>_c<ccc>.png  texture page pp through palette ccc\n"
"  <MAP>/textures/img_<nn>_<N>.png  standalone 8bpp image (logo, screenshot)\n"
"  <MAP>/textures/img_<nn>_<N>_c<k>.png  standalone 4bpp image, sub-CLUT k\n"
"  <MAP>/sounds/<id>_<NAME>.wav   one SPU sample; <id> is the runtime id\n"
"  music/QUAKE_<L>_ch<n>.wav      one demultiplexed XA music stream\n"
"  cdda/track<nn>.wav             the disc's Red Book audio track\n"
"\n"
"Geometry conventions\n"
"--------------------\n"
"UNITS ARE THE DISC'S OWN, unscaled. The factor against PC Quake II is ten,\n"
"but docs/worldscale.h marks it INFERRED, so nothing here bakes it in.\n"
"Divide by ten if you want id units.\n"
"\n"
"+Y IS UP IN THESE FILES AND DOWN ON THE DISC. Every Y is negated, which\n"
"reverses handedness, so every face is emitted in reverse order too. The two\n"
"together leave each surface facing the way it faced on the console.\n"
"\n"
"Faces are quads, as the data stores them. The engine splits them as the fan\n"
"(0,1,2) + (0,2,3) -- the corners are a perimeter winding, not the PSX packet's\n"
"Z order -- so any importer's own triangulation agrees with the console's.\n"
"\n"
"Zone vertex lines carry `v x y z r g b`: the per-corner Gouraud colour that\n"
"holds this game's baked lighting. 0.502 (128/255) is the engine's unity, so a\n"
"surface at that value is unlit rather than half-dark.\n"
"\n"
"Models are exported at frame 0 of clip 0, which is the closest thing the\n"
"format has to a rest pose; a model with no animation chain is already in model\n"
"space and comes out untransformed. Vertices are part-local on disc and faces\n"
"index a shared 96-slot window that later parts overwrite, so each face carries\n"
"its own four corners rather than sharing a vertex list.\n"
"\n"
"Textures\n"
"--------\n"
"A material is one 256x256 4bpp texture page ALREADY RESOLVED through one\n"
"16-entry CLUT, because that is what the console samples: the same page is a\n"
"different image under every palette that indexes it. Only pairs some polygon\n"
"or model face actually uses are written.\n"
"\n"
"Every image is an indexed PNG with a tRNS block. A CLUT entry of zero is the\n"
"hardware's TRANSPARENT texel rather than black, so the alpha is part of the\n"
"art, and PNG is the format that both carries it and opens everywhere.\n"
"\n"
"Standalone images -- the HUD, menu, globe and title art in img_* -- are NOT\n"
"all one bit depth, and treating them as 8bpp was wrong. The record's width is\n"
"BYTES per row, so a payload is either `width` texels of 8bpp or `width * 2` of\n"
"4bpp, and only one of the two is a picture.\n"
"\n"
"Which one is settled by the shape of the 512-byte palette block, not by any\n"
"statistic of the pixels: a 4bpp image's block is sixteen 16-entry CLUTs of\n"
"which only a few are real and the rest repeat as filler, while an 8bpp image's\n"
"is one 256-entry palette with all sixteen sub-blocks distinct. Across the 222\n"
"images that count is 2, 4 or 6 whenever the 4bpp reading is a picture and 12 or\n"
"16 whenever the 8bpp reading is -- no overlap, and 15 of 15 against the images\n"
"checked by eye.\n"
"\n"
"A 4bpp image therefore gets ONE FILE PER DISTINCT SUB-CLUT, named _c<n>, the\n"
"same way a texture page does -- those really are alternate palettes for one\n"
"sheet, which is how qk_menu's icons exist in both a green and a red set.\n"
"\n"
"A material named c000 comes out BLACK, and that is the disc's answer rather\n"
"than a decode fault: palette 0 is one of the sixteen reserved all-0x8000 CLUTs\n"
"the engine uploads, so every texel it resolves is black with the transparency\n"
"bit set. The texels underneath are real art -- 11,255 polygons index it -- but\n"
"nothing on the console gives them a colour.\n"
"\n"
"Audio\n"
"-----\n"
"Sound-bank samples are SPU-ADPCM decoded to 16-bit mono at their stored rate\n"
"(11025 or 22050 Hz). Some banks alias -- two table slots naming one sample --\n"
"so a few files in a map are byte-identical by design.\n"
"\n"
"Music is CD-XA ADPCM, four streams per .XAI multiplexed one sector each in\n"
"round-robin, decoded to 16-bit stereo at 37800 Hz. Twenty streams are written\n"
"and two of them are byte-identical, so there are 19 distinct tracks.\n"
"\n"
"Not exported\n"
"------------\n"
"The .STX movies in Q2DATA/MOVIES are an unimplemented format and are skipped;\n"
"`q2psx-inspect extract` will write them out raw. /SILENCE3.WAV is already a\n"
"WAV on the disc and is left alone for the same reason.\n"
"\n"
"Collision is not here either, and deliberately. The PrimaryColl and\n"
"SecondaryCol chunks are convex hulls stored as PLANES, not as meshes, so an\n"
"OBJ of them would be a polytope this tool computed rather than anything the\n"
"disc holds -- and 148 of the 22,773 nodes are not actually convex. The visible\n"
"world in the OBJs is the drawn geometry, which is a separate representation.\n"
"\n"
"Counts\n"
"------\n"
"  maps                : %u\n"
"  zones               : %u  (%u quads, %u of them with no UV set)\n"
"  models              : %u  (%u faces)\n"
"  textures            : %u  page/palette pairs\n"
"  standalone images   : %u\n"
"  sounds              : %u\n"
"  music streams       : %u\n"
"  CD-DA tracks        : %u\n"
"  zones that failed   : %u\n"
"  models that failed  : %u\n"
"  texture lookups that named a page the map does not carry : %u\n",
            disc_describe(d), st->maps, st->zones, st->quads,
            st->quads_untextured, st->models, st->model_faces, st->textures,
            st->images, st->sounds, st->music, st->cdda, st->zone_failed,
            st->model_failed, st->texture_missing);

    fclose(fp);
}

/* ------------------------------------------------------------------------- */
static bool wants(const char *what, const char *part)
{
    if (!what || strcmp(what, "all") == 0)
        return true;
    return strstr(what, part) != NULL;
}

int cmd_export(disc *d, const char *outdir, const char *what,
               const char *only_map)
{
    export_stats st;
    char maps[64][32];
    u32 map_count = 0, i;
    int f, n = disc_file_count(d);

    memset(&st, 0, sizeof(st));

    /* The map list is whatever COMMON.DAT files the disc actually has, not a
     * table from the executable — a build with a map the table forgot still
     * exports. */
    for (f = 0; f < n && map_count < (u32)Q2PSX_ARRAY_COUNT(maps); f++) {
        const disc_file *file = disc_file_at(d, f);
        const char *base = strrchr(file->path, '/');
        char name[32];

        base = base ? base + 1 : file->path;
        if (strcmp(base, "COMMON.DAT") != 0)
            continue;
        if (!map_name_of(file->path, name, sizeof(name)))
            continue;
        if (only_map && !name_ieq(name, only_map))
            continue;

        snprintf(maps[map_count], sizeof(maps[0]), "%s", name);
        map_count++;
    }

    make_dir(outdir);
    printf("exporting %u map%s to %s\n\n", map_count,
           map_count == 1 ? "" : "s", outdir);

    for (i = 0; i < map_count; i++) {
        const char *map = maps[i];
        char mapdir[1024];
        tex_bank tb;
        q2_vram_section vs;
        bool have_vram;
        u32 zone, sounds, zones_before = st.zones, models_before = st.models;

        snprintf(mapdir, sizeof(mapdir), "%s/%s", outdir, map);
        make_dir(mapdir);

        memset(&tb, 0, sizeof(tb));
        have_vram = (q2_vram_load(&vs, d, map) == Q2_OK);
        if (have_vram) {
            tb.vs = &vs;
            snprintf(tb.dir, sizeof(tb.dir), "%.*s/textures",
                     (int)(sizeof(tb.dir) - 16), mapdir);
            if (wants(what, "texture") || wants(what, "map") ||
                wants(what, "model"))
                make_dir(tb.dir);
            if (wants(what, "texture"))
                st.images += tex_emit_images(&tb);
        }

        if (wants(what, "map"))
            for (zone = 0; zone < 16; zone++)
                export_zone(d, map, zone, &tb, mapdir, &st);

        if (wants(what, "model"))
            export_models(d, map, &tb, mapdir, &st);

        if (wants(what, "sound")) {
            sounds = export_sounds(d, map, mapdir);
            st.sounds += sounds;
        } else {
            sounds = 0;
        }

        st.textures        += tb.written;
        st.texture_missing += tb.missing;

        printf("  %-10s %2u zones  %4u models  %4u textures  %3u sounds%s\n",
               map, st.zones - zones_before, st.models - models_before,
               tb.written, sounds,
               have_vram ? "" : "   (no VRAM section)");
        fflush(stdout);

        tex_bank_free(&tb);
        if (have_vram)
            q2_vram_free(&vs);

        st.maps++;
    }

    if (wants(what, "music"))
        st.music += export_music(d, outdir);
    if (wants(what, "cdda"))
        st.cdda += export_cdda(d, outdir);

    write_readme(outdir, d, &st);

    printf("\n"
           "  maps      : %u\n"
           "  zones     : %u  (%u quads, %u untextured)\n"
           "  models    : %u  (%u faces, %u failed)\n"
           "  textures  : %u  (+%u standalone images, %u lookups missed)\n"
           "  sounds    : %u\n"
           "  music     : %u\n"
           "  cdda      : %u\n",
           st.maps, st.zones, st.quads, st.quads_untextured,
           st.models, st.model_faces, st.model_failed,
           st.textures, st.images, st.texture_missing,
           st.sounds, st.music, st.cdda);

    return (st.zone_failed || st.model_failed) ? 1 : 0;
}
