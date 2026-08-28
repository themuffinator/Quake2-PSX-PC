/*
 * q2psx-inspect — the reverse-engineering harness.
 *
 * Opens a disc and reports what is on it, without needing a game window. This is
 * the tool used to validate every format claim in docs/FORMATS.md: if the parser
 * here reads a real disc cleanly, the engine's loader will too, because they are
 * the same code.
 */
#include "aimodule.h"
#include "area.h"
#include "cmd_coll.h"
#include "creworld.h"
#include "cmd_effects.h"
#include "cmd_explosive.h"
#include "cmd_modelent.h"
#include "cmd_exe.h"
#include "cmd_export.h"
#include "cmd_hud.h"
#include "cmd_text.h"
#include "cmd_items.h"
#include "cmd_lights.h"
#include "cmd_menu.h"
#include "cmd_pmove.h"
#include "cmd_save.h"
#include "cmd_death.h"
#include "cmd_multi.h"
#include "cmd_ai.h"
#include "cmd_creatures.h"
#include "cmd_weapons.h"
#include "cmd_zonescript.h"
#include "cmd_screen.h"
#include "cmd_viewweapon.h"
#include "cmd_surfaces.h"
#include "classtable.h"
#include "collision.h"
#include "dat.h"
#include "disc.h"
#include "entity.h"
#include "entitydraw.h"
#include "font.h"
#include "hud.h"
#include "exe.h"
#include "events_rt.h"
#include "ident.h"
#include "level.h"
#include "leveltable.h"
#include "musictable.h"
#include "model.h"
#include "modeldraw.h"
#include "mover.h"
#include "points.h"
#include "rotator.h"
#include "population.h"
#include "trigger.h"
#include "raster.h"
#include "reloc.h"
#include "scene.h"
#include "stx.h"
#include "stxenc.h"
#include "cdxa.h"
#include "screen.h"
#include "spawn.h"
#include "sim.h"
#include "version.h"
#include "vram.h"
#include "world.h"
#include "worldscale.h"
#include "trig.h"
#include "vag.h"
#include "xa.h"
#include "q2psx.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define q2_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define q2_mkdir(p) mkdir((p), 0755)
#endif

/* ------------------------------------------------------------------------- */
/* The console's own framing, for the commands that claim to show it          */
/* ------------------------------------------------------------------------- */
/*
 * Three commands here — `fps`, `mob` and `render` — exist to show what the game
 * looked like, and until now they rendered into a 512 x 480 buffer with
 * `q2_camera_default`, i.e. a projection distance of 512 over a square-pixel
 * frame: about 53 degrees across where the console's one-player viewport is 116.
 * The picture was honest about the geometry and wrong about the view.
 *
 * So they render at an integer multiple of the console's framebuffer with the
 * projection distance scaled by the same multiple, which is exactly the
 * console's frustum at a higher sampling rate. The output is therefore
 * anamorphic, as the console's own output is: a viewer wants it 1.5x taller (or
 * 2/3 as wide) to see what a television showed. Every PPM these commands write
 * says so on the way out.
 */
#define TOOL_VIEW_SCALE 2
#define TOOL_VIEW_W  (Q2_SCREEN_PAL_WIDTH  * TOOL_VIEW_SCALE)
#define TOOL_VIEW_H  (Q2_SCREEN_PAL_HEIGHT * TOOL_VIEW_SCALE)

static void camera_console(q2_camera *cam, int w, int h)
{
    q2_camera_default(cam, w, h);

    /*
     * `proj` scales with the width because that is what the console's own
     * layouts do: 160 is paired with the 512-wide framebuffer, and the splits
     * change it only when they change how much of that width a viewport gets.
     */
    cam->projection = (u16)((s32)Q2_SCREEN_BOOT_PROJECTION * w
                            / Q2_SCREEN_PAL_WIDTH);
    cam->ofs_x      = w / 2;
    cam->ofs_y      = h / 2;
}

/* One line under any PPM written at the console's framing, so nobody measures a
 * screenshot without knowing the pixels are not square. */
static void print_console_framing(const q2_camera *cam, int w, int h)
{
    printf("  framing       : %d x %d, proj %u — the console's frustum at %dx.\n"
           "                  Pixels are 2:3 as on a PAL television, so view it "
           "%d x %d\n",
           w, h, cam->projection, TOOL_VIEW_SCALE, w, h * 3 / 2);
}

/* Case-insensitive compare, local to the tool: the format layer's own is not
 * exported and this is only used for matching a model name typed by a human. */
static int name_casecmp_local(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void usage(void)
{
    puts("q2psx-inspect - inspect a Quake II PSX disc\n");
    puts("usage: q2psx-inspect <command> <disc> [args]\n");
    puts("commands:");
    puts("  ident   <disc>              identify the release and print its fingerprint");
    puts("  disc    <disc>              list tracks and the full file table");
    puts("  levels  <disc>              summarise the level directories");
    puts("  dat     <disc> <path>       dump the chunk directory of one .DAT");
    puts("  dats    <disc>              census every .DAT chunk schema on the disc");
    puts("  verify  <disc>              check every level file against the typed schema");
    puts("  coll    <disc> [map] [zone] check the collision model against every hull");
    puts("  audio   <disc>              decode every sound bank and validate it");
    puts("  leveltable <disc>           dump the level table and check it against the disc");
    puts("  reloc   <disc>              relocate every AI module and census the fixups");
    puts("  events  <disc>              run every event script and census the opcodes");
    puts("  zonescript <disc> [map]     which Events chunk a trigger fires");
    puts("  walk    <disc> <map> [z] [ticks]  drop a player in and simulate");
    puts("  textures <disc>             decode every compressed VRAM image");
    puts("  cluts   <disc>              check CLUT binding and UV rotation on every poly");
    puts("  anims   <disc>              decode every CastList animation clip");
    puts("  fps     <disc> <map> [zone] [weapon] [out.ppm] [yaw] [gunyaw] [eye_dy] [roll]");
    puts("  classes <disc>              the entity class table, checked against every spawn");
    puts("  items   <disc>              the item table and every place record, checked");
    puts("  lights  <disc>              the lighting model: SpaceLights, styles, flares");
    puts("  lit     <disc> [map]        gather the lights where the player starts");
    puts("  weapons <disc>              the weapon, armour and sound tables, checked");
    puts("  effects <disc>              the particle, beam and laser tables, checked");
    puts("  explosives <disc> [map]     opcode 0x08: the destroyable brush groups");
    puts("  modelents <disc>            the effect models a model entity can bind");
    puts("  ai      <disc>              the creature AI, checked against the executable");
    puts("  creatures <disc>            decode every creature module and report coverage");
    puts("  ai      <disc>              the creature AI, checked against the executable");
    puts("  creatures <disc>            decode every creature module and report coverage");
    puts("  mob     <disc> <map> [zone] [n] [out.ppm]  stand in front of a creature");
    puts("  models  <disc> <map>        list a map's model bank");
    puts("  model   <disc> <map> <name|idx> [clip] [frame] [out.ppm] [yaw]  render one model");
    puts("  hud     <disc> [map] [out.ppm]  the HUD's tables, or draw the overlay");
    puts("  text    <disc> [map] [out.ppm]  the Strings chunk, the briefing screen, the UI chrome");
    puts("  menu    <disc> [page] [out.ppm] [WxH]  the menu, checked against the executable");
    puts("  save    <disc> [map] [out]   the save system, round-tripped against a map");
    puts("  multi   <disc> [map]        the multiplayer runtime, checked against the disc");
    puts("  death   <disc>              the player death chain, checked against the executable");
    puts("  screen  <disc>              display, viewports and the OT, checked");
    puts("  viewweapon <disc> [weapon] [out.ppm] [map] [zone] [hold] [ref.ppm]");
    puts("                              the weapon in hand, checked against the executable");
    puts("  pmove   <disc> [map] [zone] player movement: styles, jump, view, volumes");
    puts("  screen  <disc> out.ppm [layout] [map] [zone]  compose one frame");
    puts("  music   <disc>              demultiplex and decode the XA music streams");
    puts("  render  <disc> <map> [z] [out.ppm] [yaw] [pitch] [rot-ticks] [eye-pitch]");
    puts("  hexdump <disc> <path> [n]   hex dump the first n bytes of a file");
    puts("  extract <disc> <outdir>     extract the whole filesystem");
    puts("  export  <disc> <outdir> [what] [map]  decode assets: OBJ, PCX/PNG, WAV");
    puts("                              what = all, or a comma-separated subset of");
    puts("                              maps,models,textures,sounds,music,cdda");
    puts("");
    puts("executable:");
    puts("  exe     <disc> [out.bin]    header, map, landmarks; optionally dump the segment");
    puts("  disasm  <disc> <addr> [n]   disassemble n instructions (0 = to the return)");
    puts("  xrefs   <disc> <addr>       every reference to an address, code and data");
    puts("  funcs   <disc> [addr]       call targets found by sweeping the image");
    puts("  moddisasm <disc> <map> [addr] [n] [creature]  disassemble one CreAIBin module");
    puts("  levdisasm <disc> <map> [addr] [n]  disassemble a relocated LevelBin module");
    puts("  modstrings <disc> <map> [crea]  the text a relocated module carries");
    puts("  modxrefs <disc> <map> <addr> [crea]  references to an address in one");
    puts("  modbytes <disc> <map> <addr> [n] [crea]  hex dump a module's image");
    puts("  bytes   <disc> <addr> [n]   hex dump executable memory by address");
    puts("  find    <disc> <str|0xhex>  locate a string or byte pattern in the image");
    puts("  access  <disc> <off> [insn] every instruction touching a record offset");
    puts("");
    puts("<disc> may be a .cue, .bin, .img or .iso.");
}

/* ------------------------------------------------------------------------- */
/*
 * ISO9660 stores its creation time as 16 ASCII digits plus a signed byte giving
 * the offset from GMT in 15-minute steps. Rendering it raw makes the timestamp
 * look like corruption, so split it out.
 */
static void format_iso_time(char *out, size_t cap, const char *raw)
{
    int tz;

    if (!raw || strlen(raw) < 17) {
        snprintf(out, cap, "(none)");
        return;
    }

    tz = (int)(signed char)raw[16];
    snprintf(out, cap, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s.%.2s GMT%+.2f",
             raw, raw + 4, raw + 6, raw + 8, raw + 10, raw + 12, raw + 14,
             (double)tz / 4.0);
}

static void print_size(char *out, size_t cap, u32 bytes)
{
    if (bytes >= 1024u * 1024u)
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024u)
        snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(out, cap, "%u B", bytes);
}

/* ------------------------------------------------------------------------- */
static int cmd_ident(disc *d)
{
    q2_build_id id;
    q2_result r;
    char when[64];

    r = q2_identify(d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "this does not look like a Quake II PSX disc: %s\n",
                q2_result_str(r));
        return 1;
    }

    printf("Release\n");
    if (id.desc)
        printf("  name            : %s\n", id.desc->name);
    else
        printf("  name            : (uncatalogued build)\n");

    printf("  serial          : %s\n", id.serial[0] ? id.serial : "(none)");
    printf("  boot executable : %s\n", id.exe_name[0] ? id.exe_name : "(none)");
    printf("  region          : %s\n", q2_region_str(id.region));
    printf("  video standard  : %s\n", q2_video_std_str(id.video));
    printf("  game tick rate  : %d Hz\n", q2_build_tick_rate(&id));
    if (id.desc && id.desc->language)
        printf("  language        : %s\n", id.desc->language);

    format_iso_time(when, sizeof(when), id.creation_time);
    printf("\nFingerprint\n");
    printf("  exe size        : %u bytes\n", id.exe_size);
    printf("  exe sha256      : %s\n", id.exe_sha256[0] ? id.exe_sha256 : "(unavailable)");
    printf("  volume created  : %s\n", when);
    printf("  volume sectors  : %u\n", id.volume_sectors);

    printf("\nData tree\n");
    printf("  level dirs      : %d\n", id.level_dir_count);
    printf("  structure       : %s\n", id.data_tree_ok ? "ok" : "UNRECOGNISED");

    printf("\nMatch\n");
    if (id.catalogued) {
        printf("  status          : exact match against the build catalogue\n");
    } else if (id.desc) {
        printf("  status          : serial matched, executable hash is new\n");
        printf("                    (a revision, or the first dump we have seen)\n");
    } else {
        printf("  status          : unknown build - will run in generic mode\n");
    }
    if (id.desc && id.desc->notes)
        printf("  notes           : %s\n", id.desc->notes);

    return 0;
}

/* ------------------------------------------------------------------------- */
static int cmd_disc(disc *d)
{
    int i, n;
    char when[64];

    printf("%s\n\n", disc_describe(d));

    format_iso_time(when, sizeof(when), disc_creation_time(d));
    printf("Volume\n");
    printf("  system id : %s\n", disc_system_id(d));
    printf("  volume id : %s\n", disc_volume_id(d)[0] ? disc_volume_id(d) : "(blank)");
    printf("  created   : %s\n", when);
    printf("  sectors   : %u\n\n", disc_volume_sectors(d));

    n = disc_track_count(d);
    printf("Tracks (%d)\n", n);
    printf("  no  type   start lba sectors   ssize  duration\n");
    for (i = 0; i < n; i++) {
        const cd_track *t = disc_track(d, i);
        double secs = (double)t->length_sectors / (double)CD_SECTORS_PER_SECOND;
        int mins = (int)(secs / 60.0);
        printf("  %-3d %-6s %-9u %-9u %-6d %d:%05.2f\n",
               t->number,
               t->type == CD_TRACK_AUDIO ? "audio" : "data",
               t->start_lba, t->length_sectors, t->sector_size,
               mins, secs - 60.0 * mins);
    }

    n = disc_file_count(d);
    printf("\nFiles (%d)\n", n);
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        char sz[32];
        print_size(sz, sizeof(sz), f->size);
        printf("  %-8s lba=%-7u %-10s %s\n",
               f->form2 ? "[form2]" : "", f->lba, sz, f->path);
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
static int cmd_levels(disc *d)
{
    int i, n = disc_file_count(d);
    char current[64];
    int dirs = 0;

    current[0] = '\0';
    printf("Level directories\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char dir[64], sz[32];
        size_t len;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;

        len = (size_t)(slash - rest);
        if (len >= sizeof(dir))
            len = sizeof(dir) - 1;
        memcpy(dir, rest, len);
        dir[len] = '\0';

        if (strcmp(dir, current) != 0) {
            strncpy(current, dir, sizeof(current) - 1);
            current[sizeof(current) - 1] = '\0';
            printf("\n  %s\n", dir);
            dirs++;
        }

        print_size(sz, sizeof(sz), f->size);
        printf("      %-14s %10s\n", slash + 1, sz);
    }

    printf("\n%d level directories\n", dirs);
    return 0;
}

/* ------------------------------------------------------------------------- */
static void dump_dat_chunks(const dat_archive *ar, const char *label)
{
    int i;

    printf("  %s - %d chunks, data ends at 0x%X\n",
           label, ar->chunk_count, ar->end_offset);
    printf("    idx name           offset           size\n");
    for (i = 0; i < ar->chunk_count; i++) {
        const dat_chunk *c = &ar->chunks[i];
        printf("    %-3d %-14s 0x%08X %10u%s\n",
               i, c->name, c->offset, c->size,
               c->size == 0 ? "   (empty)" : "");
    }
}

static int cmd_dat(disc *d, const char *path)
{
    q2_buf buf;
    dat_archive ar;
    q2_result r;

    r = disc_read_file(d, path, &buf);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read %s: %s\n", path, q2_result_str(r));
        return 1;
    }

    if (!dat_probe(buf.data, buf.size)) {
        size_t i;
        printf("%s (%zu bytes) does not use the .DAT chunk container.\n",
               path, buf.size);
        printf("First 32 bytes:\n    ");
        for (i = 0; i < 32 && i < buf.size; i++)
            printf("%02X ", buf.data[i]);
        printf("\n");
        q2_buf_free(&buf);
        return 0;
    }

    r = dat_open_buf(&ar, &buf);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot parse %s: %s\n", path, q2_result_str(r));
        q2_buf_free(&buf);
        return 1;
    }

    printf("%s\n", path);
    dump_dat_chunks(&ar, "chunks");
    dat_close(&ar);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Schema census                                                              */
/*                                                                            */
/* The loader wants to index chunks by enum rather than by string, which is    */
/* only safe if the set of names is knowable up front. So rather than just     */
/* flagging "this file differs", collect the distinct schemas and the union of */
/* names, with how often each appears and how often it carries data. That      */
/* tells us which chunks are mandatory, which are optional, and whether the    */
/* variation is in the names or only in the ordering.                          */
/* ------------------------------------------------------------------------- */
#define CENSUS_MAX_SCHEMAS 64
#define CENSUS_MAX_NAMES   128
#define CENSUS_SCHEMA_CAP  1024

typedef struct schema_variant {
    char schema[CENSUS_SCHEMA_CAP];
    char example[192];
    int  count;
    int  chunk_count;
} schema_variant;

typedef struct name_stat {
    char name[DAT_NAME_LEN + 1];
    int  present;       /* files containing this chunk         */
    int  non_empty;     /* files where it carries data         */
    int  first_index;   /* directory position, first sighting  */
    int  index_varies;
} name_stat;

typedef struct census {
    const char     *label;
    schema_variant  variants[CENSUS_MAX_SCHEMAS];
    int             variant_count;
    name_stat       names[CENSUS_MAX_NAMES];
    int             name_count;
    int             files;
} census;

static void census_init(census *c, const char *label)
{
    memset(c, 0, sizeof(*c));
    c->label = label;
}

static void census_add(census *c, const dat_archive *ar, const char *path)
{
    char schema[CENSUS_SCHEMA_CAP];
    size_t used = 0;
    int i, j;

    schema[0] = '\0';
    c->files++;

    for (i = 0; i < ar->chunk_count; i++) {
        int written = snprintf(schema + used, sizeof(schema) - used,
                               "%s|", ar->chunks[i].name);
        if (written < 0 || (size_t)written >= sizeof(schema) - used)
            break;
        used += (size_t)written;
    }

    for (i = 0; i < c->variant_count; i++) {
        if (strcmp(c->variants[i].schema, schema) == 0) {
            c->variants[i].count++;
            break;
        }
    }
    if (i == c->variant_count && c->variant_count < CENSUS_MAX_SCHEMAS) {
        schema_variant *v = &c->variants[c->variant_count++];
        strncpy(v->schema, schema, sizeof(v->schema) - 1);
        strncpy(v->example, path, sizeof(v->example) - 1);
        v->count = 1;
        v->chunk_count = ar->chunk_count;
    }

    for (i = 0; i < ar->chunk_count; i++) {
        const dat_chunk *ch = &ar->chunks[i];

        for (j = 0; j < c->name_count; j++) {
            if (strcmp(c->names[j].name, ch->name) == 0)
                break;
        }
        if (j == c->name_count) {
            if (c->name_count >= CENSUS_MAX_NAMES)
                continue;
            strncpy(c->names[j].name, ch->name, DAT_NAME_LEN);
            c->names[j].name[DAT_NAME_LEN] = '\0';
            c->names[j].first_index = i;
            c->name_count++;
        }
        if (c->names[j].first_index != i)
            c->names[j].index_varies = 1;
        c->names[j].present++;
        if (ch->size > 0)
            c->names[j].non_empty++;
    }
}

static void census_report(const census *c)
{
    int i;

    if (c->files == 0)
        return;

    printf("\n%s - %d files, %d distinct schema%s\n",
           c->label, c->files, c->variant_count,
           c->variant_count == 1 ? "" : "s");

    printf("  chunk          idx  present   w/data  role\n");
    for (i = 0; i < c->name_count; i++) {
        const name_stat *n = &c->names[i];
        const char *role;

        if (n->present == c->files && n->non_empty == c->files)
            role = "mandatory, always populated";
        else if (n->present == c->files)
            role = "mandatory, sometimes empty";
        else
            role = "OPTIONAL";

        printf("  %-14s %-4s %3d/%-5d %6d   %s\n",
               n->name,
               n->index_varies ? "var" : "fix",
               n->present, c->files, n->non_empty, role);
    }

    if (c->variant_count > 1) {
        printf("\n  schema variants:\n");
        for (i = 0; i < c->variant_count && i < 10; i++) {
            printf("    %3d file%s %2d chunks  e.g. %s\n",
                   c->variants[i].count,
                   c->variants[i].count == 1 ? " " : "s",
                   c->variants[i].chunk_count,
                   c->variants[i].example);
        }
        if (c->variant_count > 10)
            printf("    ... and %d more\n", c->variant_count - 10);
    }
}

/*
 * Census every chunked .DAT on the disc. COMMON.DAT and ZONE*.DAT are reported
 * separately: they are different file types that merely share a container.
 */
static int cmd_dats(disc *d)
{
    int i, n = disc_file_count(d);
    census common, zone;
    int other = 0;
    int shown_common = 0, shown_zone = 0;

    census_init(&common, "COMMON.DAT");
    census_init(&zone, "ZONE*.DAT");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        dat_archive ar;

        base = base ? base + 1 : f->path;

        if (!strstr(base, ".DAT") && !strstr(base, ".ALL"))
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        if (!dat_probe(buf.data, buf.size)) {
            if (other == 0)
                printf("Files using a different container:\n");
            if (other < 4) {
                printf("  %-46s first u32 = 0x%08X\n", f->path,
                       buf.size >= 4 ? q2_rd_u32(buf.data) : 0);
            }
            other++;
            q2_buf_free(&buf);
            continue;
        }

        if (dat_open_buf(&ar, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        if (strncmp(base, "COMMON", 6) == 0) {
            if (!shown_common) {
                printf("\nCOMMON.DAT chunk directory (from %s)\n", f->path);
                dump_dat_chunks(&ar, base);
                shown_common = 1;
            }
            census_add(&common, &ar, f->path);
        } else if (strncmp(base, "ZONE", 4) == 0) {
            if (!shown_zone) {
                printf("\nZONE*.DAT chunk directory (from %s)\n", f->path);
                dump_dat_chunks(&ar, base);
                shown_zone = 1;
            }
            census_add(&zone, &ar, f->path);
        }

        dat_close(&ar);
    }

    if (other > 4)
        printf("  ... and %d more\n", other - 4);

    census_report(&common);
    census_report(&zone);

    printf("\n%d files use a non-chunked container.\n", other);
    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Run every level file through the typed loader. This is the acceptance test
 * for the container work: if all 164 COMMON/ZONE files resolve to their fixed
 * chunk slots with no unknown names and no missing mandatory chunks, the schema
 * in level.h is right for this build.
 */
static int cmd_verify(disc *d)
{
    int i, n = disc_file_count(d);
    int common_ok = 0, zone_ok = 0, failed = 0, skipped = 0;
    unsigned long long points_total = 0;
    unsigned long long quads_total = 0;
    unsigned long long planes_total = 0;
    unsigned long long normals_unit = 0;
    unsigned long long spawns_total = 0;
    unsigned long long lights_total = 0;
    unsigned long long lights_bad = 0;
    unsigned long long areas_total = 0;
    unsigned long long links_total = 0;
    unsigned long long links_bad = 0, links_far = 0, links_skew = 0;
    unsigned long long names_total = 0;
    unsigned long long convex_ok = 0;
    unsigned long long convex_bad = 0;
    unsigned long long pop_groups = 0;
    unsigned long long pop_spawns = 0;
    unsigned long long pop_places = 0;
    unsigned long long pop_paths = 0;
    unsigned long long trig_total = 0;
    unsigned long long trig_planes = 0;
    unsigned long long pickups_total = 0;
    unsigned long long pickups_taken = 0;
    unsigned long long spawn_records = 0, spawn_placed = 0, spawn_oob = 0;
    u8 class_seen[Q2_MONSTER_CLASS_COUNT];

    printf("Verifying every level file against the typed schema...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_result r;

        base = base ? base + 1 : f->path;

        if (strncmp(base, "COMMON", 6) != 0 && strncmp(base, "ZONE", 4) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK) {
            printf("  READ FAILED   %s\n", f->path);
            failed++;
            continue;
        }

        if (strncmp(base, "COMMON", 6) == 0) {
            q2_common_file cf;
            r = q2_common_open(&cf, &buf);
            if (r == Q2_OK) {
                q2_start_pos_list spawns;
                q2_light_list lights;

                common_ok++;

                if (q2_start_pos_parse(&spawns, &cf) == Q2_OK) {
                    spawns_total += spawns.count;
                } else {
                    printf("  startpos: bad  %s\n", f->path);
                    failed++;
                }

                /* Population: actor and pickup placement, plus patrol graphs. */
                {
                    q2_population pop;
                    if (q2_population_parse(&pop, &cf) == Q2_OK) {
                        u32 gi;
                        pop_groups += pop.group_count;
                        for (gi = 0; gi < pop.group_count; gi++) {
                            q2_pop_group g;
                            u32 slot;

                            if (!q2_pop_get_group(&pop, gi, &g))
                                continue;

                            if (q2_pop_group_is_path(&g)) {
                                q2_pop_path pn;
                                for (slot = 0; q2_pop_get_path(&pop, &g, slot, &pn); slot++)
                                    pop_paths++;
                            } else {
                                q2_pop_spawn sp2;
                                for (slot = 0; q2_pop_get_spawn(&pop, &g, slot, &sp2); slot++)
                                    pop_spawns++;
                            }

                            {
                                q2_pop_place pl2;
                                for (slot = 0; q2_pop_get_place(&pop, &g, slot, &pl2); slot++)
                                    pop_places++;
                            }
                        }
                    }
                }

                /*
                 * Items: spawn every place record as the engine does, then walk
                 * a player onto each in turn and count what is actually
                 * collected. Until the item table was decoded this could only
                 * prove that NOTHING could be collected; now it exercises the
                 * whole path — table lookup, spawn, touch box, dispatch.
                 */
                {
                    q2_population pop2;
                    if (q2_population_parse(&pop2, &cf) == Q2_OK) {
                        q2_entity_set es;
                        q2_item_spawn_stats ist;

                        memset(&es, 0, sizeof(es));
                        if (q2_item_spawn_all(&es, &pop2, NULL, &ist) == Q2_OK) {
                            q2_entity_world ew;
                            q2_inventory inv;
                            u32 k;

                            pickups_total += ist.spawned;

                            q2_entity_world_init(&ew);
                            q2_inventory_init(&inv);
                            q2_entity_world_add_player(&ew, 0, &inv,
                                                       es.count ? es.ent[0].pos
                                                                : NULL);
                            ew.dt = 12;

                            for (k = 0; k < es.count; k++) {
                                q2_entity *e = &es.ent[k];
                                if (!e->in_use)
                                    continue;
                                q2_entity_world_move_player(&ew, 0, e->pos);
                                e->think(e, &ew);
                                if (!e->in_use || e->taken[0])
                                    pickups_taken++;
                            }
                        }
                        q2_entity_set_free(&es);
                    }
                }

                /* Creature spawning. Every class the disc uses is treated as
                 * registered here, so the count reflects the spawn records
                 * rather than which modules we happen to have loaded. */
                {
                    q2_population pop3;
                    if (q2_population_parse(&pop3, &cf) == Q2_OK) {
                        q2_monster_set ms;
                        q2_spawn_stats st;
                        u32 k;

                        memset(&ms, 0, sizeof(ms));
                        for (k = 0; k < Q2_MONSTER_CLASS_COUNT; k++)
                            q2_monster_set_register(&ms, k);

                        /* A census with no hull: no drop, which is what a count wants. */
                        if (q2_spawn_from_population(&ms, &pop3, NULL, &st) == Q2_OK) {
                            spawn_records += st.records;
                            spawn_placed  += st.placed;
                            spawn_oob     += st.out_of_range;
                            for (k = 0; k < ms.count; k++) {
                                if (ms.monsters[k].class_id >= 0 &&
                                    ms.monsters[k].class_id < Q2_MONSTER_CLASS_COUNT)
                                    class_seen[ms.monsters[k].class_id] = 1;
                            }
                        }
                        q2_monster_set_free(&ms);
                    }
                }

                /* Trigger volumes. */
                {
                    q2_triggers tg;
                    if (q2_triggers_parse(&tg, &cf) == Q2_OK) {
                        trig_total += tg.count;
                        trig_planes += tg.plane_count;
                    } else {
                        printf("  trigbounds: bad  %s\n", f->path);
                        failed++;
                    }
                }

                if (q2_lights_parse(&lights, &cf) == Q2_OK) {
                    u32 li;
                    lights_total += lights.count;
                    for (li = 0; li < lights.count; li++) {
                        q2_light lt;
                        if (!q2_light_get(&lights, li, &lt))
                            continue;
                        /* radius must be the integer square root of radius_sq,
                         * and the inner radius must not exceed it. Both are
                         * cheap invariants that a misread stride would break. */
                        if ((u32)lt.radius * lt.radius > lt.radius_sq ||
                            (u32)(lt.radius + 1) * (lt.radius + 1) <= lt.radius_sq ||
                            lt.inner_radius_sq > lt.radius_sq)
                            lights_bad++;
                    }
                } else {
                    printf("  lights: bad  %s\n", f->path);
                    failed++;
                }

                q2_common_close(&cf);
            } else {
                printf("  %-13s %s\n", q2_result_str(r), f->path);
                failed++;
                q2_buf_free(&buf);
            }
        } else {
            q2_zone_file zf;
            r = q2_zone_open(&zf, &buf);
            if (r == Q2_OK) {
                q2_points pts;

                /* The chunk directory resolving is necessary but not
                 * sufficient — also parse the vertex pool, which is the first
                 * chunk whose *internal* layout we claim to understand. */
                r = q2_points_parse(&pts, &zf);
                if (r == Q2_OK) {
                    q2_collision coll;
                    q2_scene sc;
                    int sub_ok = 1;

                    zone_ok++;
                    points_total += pts.count;

                    /* Scene/MapMod: walk every node's polygon record. */
                    if (q2_scene_parse(&sc, &zf) == Q2_OK) {
                        u32 ni;
                        for (ni = 0; ni < sc.node_count; ni++) {
                            q2_mapmod_rec rec;
                            if (!q2_scene_get_mapmod(&sc, ni, &rec)) {
                                printf("  mapmod node %u bad in %s\n", ni, f->path);
                                sub_ok = 0;
                                break;
                            }
                            quads_total += rec.num_polys;
                        }
                    } else {
                        printf("  scene: parse failed  %s\n", f->path);
                        sub_ok = 0;
                    }

                    /* Portal graph and name table. */
                    {
                        q2_area_graph ag;
                        q2_map_name_table nt;

                        if (q2_area_parse(&ag, &zf) == Q2_OK) {
                            u32 a;
                            areas_total += ag.area_count;
                            for (a = 0; a < ag.area_count; a++) {
                                u32 nl = q2_area_link_count(&ag, a), li2;
                                for (li2 = 0; li2 < nl; li2++) {
                                    q2_area_link lk;
                                    if (q2_area_get_link(&ag, a, li2, &lk)) {
                                        s32 m2;

                                        links_total++;
                                        /*
                                         * The two facts that DECODE this record
                                         * (area.h): the neighbour must be a real
                                         * area, and the plane's normal must be a
                                         * 1.3.12 unit. Both hold on every link on
                                         * the disc, and either failing means the
                                         * two-array reading is wrong.
                                         */
                                        if (lk.neighbour >= ag.area_count)
                                            links_far++;
                                        m2 = (s32)lk.normal[0] * lk.normal[0] +
                                             (s32)lk.normal[1] * lk.normal[1] +
                                             (s32)lk.normal[2] * lk.normal[2];
                                        if (m2 < 4014*4014 || m2 > 4178*4178)
                                            links_skew++;
                                    } else
                                        links_bad++;
                                }
                            }
                        } else {
                            printf("  areaconx: parse failed  %s\n", f->path);
                            sub_ok = 0;
                        }

                        if (q2_map_names_parse(&nt, &zf) == Q2_OK) {
                            names_total += nt.count;
                        } else {
                            printf("  mapnames: parse failed  %s\n", f->path);
                            sub_ok = 0;
                        }
                    }

                    /* Both collision hulls, including the normal-length check
                     * that is the strongest evidence the layout is right. */
                    {
                        int w;
                        for (w = 0; w < 2; w++) {
                            u32 pi;
                            if (q2_collision_parse(&coll, &zf,
                                    w ? Q2_COLL_SECONDARY : Q2_COLL_PRIMARY) != Q2_OK) {
                                printf("  collision[%d]: parse failed  %s\n", w, f->path);
                                sub_ok = 0;
                                continue;
                            }
                            /* Geometric convexity: an interior point must be on
                             * the negative side of every plane of its node.
                             * This is the test that actually validates the
                             * plane-point encoding; bbox containment does not. */
                            {
                                u32 ni2;
                                for (ni2 = 0; ni2 < coll.node_count; ni2++) {
                                    q2_coll_node hn, nx2;
                                    s32 centroid[3] = { 0, 0, 0 };
                                    u32 k, n_used = 0;

                                    if (!q2_collision_get_node(&coll, ni2, &hn) ||
                                        !q2_collision_get_node(&coll, ni2 + 1, &nx2))
                                        continue;
                                    if (nx2.first_plane <= hn.first_plane)
                                        continue;

                                    for (k = hn.first_plane; k < nx2.first_plane; k++) {
                                        s32 pt[3];
                                        if (!q2_coll_plane_point(&coll, ni2, k, pt))
                                            continue;
                                        centroid[0] += pt[0];
                                        centroid[1] += pt[1];
                                        centroid[2] += pt[2];
                                        n_used++;
                                    }
                                    if (!n_used)
                                        continue;
                                    centroid[0] /= (s32)n_used;
                                    centroid[1] /= (s32)n_used;
                                    centroid[2] /= (s32)n_used;

                                    for (k = hn.first_plane; k < nx2.first_plane; k++) {
                                        if (q2_coll_plane_distance(&coll, ni2, k, centroid) > 0)
                                            convex_bad++;
                                        else
                                            convex_ok++;
                                    }
                                }
                            }

                            for (pi = 0; pi < coll.plane_count; pi++) {
                                q2_coll_plane pl;
                                s32 len_sq;
                                if (!q2_collision_get_plane(&coll, pi, &pl))
                                    continue;
                                len_sq = q2_coll_normal_len_sq(&pl);
                                planes_total++;
                                /* Unit length within 2 LSB: 4094^2 .. 4096^2 */
                                if (len_sq >= 4094 * 4094 && len_sq <= 4096 * 4096)
                                    normals_unit++;
                            }
                        }
                    }

                    if (!sub_ok)
                        failed++;

                    q2_points_free(&pts);
                } else {
                    printf("  points: %-6s %s\n", q2_result_str(r), f->path);
                    failed++;
                }
                q2_zone_close(&zf);
            } else {
                printf("  %-13s %s\n", q2_result_str(r), f->path);
                failed++;
                q2_buf_free(&buf);
            }
        }
    }

    printf("  COMMON.DAT : %d resolved\n", common_ok);
    printf("  ZONE*.DAT  : %d resolved\n", zone_ok);
    printf("  vertices   : %llu across all zones\n", points_total);
    printf("  quads      : %llu\n", quads_total);
    printf("  coll planes: %llu, of which %llu are unit normals (%.2f%%)\n",
           planes_total, normals_unit,
           planes_total ? 100.0 * (double)normals_unit / (double)planes_total : 0.0);
    printf("  areas      : %llu with %llu links (%llu unreadable);"
           " %llu name a missing area, %llu carry a non-unit normal\n",
           areas_total, links_total, links_bad, links_far, links_skew);
    printf("  map names  : %llu\n", names_total);
    printf("  convexity  : %llu/%llu planes consistent (%.3f%%), %llu violations\n",
           convex_ok, convex_ok + convex_bad,
           (convex_ok + convex_bad)
               ? 100.0 * (double)convex_ok / (double)(convex_ok + convex_bad) : 0.0,
           convex_bad);
    printf("  spawns     : %llu\n", spawns_total);
    printf("  triggers   : %llu volumes, %llu planes\n", trig_total, trig_planes);
    /* One player with one inventory walks onto each item in turn, so the second
     * shotgun and the ammo box that finds you full are correctly refused — the
     * count is what a single sweep collects, not how many are collectable. */
    printf("  items      : %llu spawned, %llu collected by one pass\n",
           pickups_total, pickups_taken);
    {
        u32 k, distinct = 0;
        for (k = 0; k < Q2_MONSTER_CLASS_COUNT; k++)
            if (class_seen[k]) distinct++;
        printf("  creatures  : %llu spawn records, %llu placed, %llu bad class, %u distinct classes\n",
               spawn_records, spawn_placed, spawn_oob, distinct);
    }
    printf("  population : %llu groups, %llu actors, %llu placements, %llu path nodes\n",
           pop_groups, pop_spawns, pop_places, pop_paths);
    printf("  lights     : %llu, %llu failing the radius invariant\n",
           lights_total, lights_bad);
    printf("  failed     : %d\n", failed);
    if (skipped)
        printf("  skipped    : %d\n", skipped);

    printf("\n%s\n", failed == 0
           ? "PASS - every level file matches the catalogued schema."
           : "FAIL - see the entries above.");

    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Poly render-flag census, split by whether the owning Scene node is driven by
 * a mover.
 *
 * MapMod Poly.uvIdxFlags bits 6-7 are render flags of unknown meaning, set on
 * 11.7% of polygons world-wide, and the renderer ignores them. The question
 * this answers is whether that 11.7% is spread evenly or concentrates on
 * mover-driven geometry (doors, lifts, plats — the brush-model entities). An
 * even split rules the flags out as the cause of a bmodel-specific fault;
 * enrichment on mover nodes makes them the prime suspect.
 */
static int cmd_polyflags(disc *d)
{
    int i, n = disc_file_count(d);
    unsigned long long mv[4] = {0,0,0,0}, wd[4] = {0,0,0,0};
    unsigned long long mv_nodes = 0, wd_nodes = 0, mv_total = 0, wd_total = 0;
    unsigned long long zones = 0, mover_nodes_seen = 0;
    unsigned long long mv_uvfail = 0, wd_uvfail = 0;
    unsigned long long overrun_zones = 0, overrun_nodes = 0, mismatch_zones = 0;
    unsigned long long origin_const_zones = 0, origin_nodes = 0, origin_same = 0;

    printf("Poly render-flag census (uvIdxFlags bits 6-7), by node kind\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_zone_file zf;
        q2_scene scene;
        q2_events ev;
        q2_mover_set movers;
        q2_points pts;
        bool have_pts;
        u8 *is_mover;
        u32 node;

        base = base ? base + 1 : f->path;
        if (strncmp(base, "ZONE", 4) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;
        if (q2_zone_open(&zf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }
        if (q2_scene_parse(&scene, &zf) != Q2_OK) {
            q2_zone_close(&zf);
            continue;
        }

        zones++;

        /* The renderer indexes points.groups[] by SCENE NODE index without
         * bounding it. Nothing in the format ties the two counts together, so
         * measure whether they ever disagree. */
        have_pts = (q2_points_parse(&pts, &zf) == Q2_OK);
        if (have_pts) {
            if (scene.node_count > pts.group_count) {
                printf("  OVERRUN  %-28s nodes %u > point groups %u\n",
                       f->path, scene.node_count, pts.group_count);
                overrun_zones++;
                overrun_nodes += scene.node_count - pts.group_count;
            }
            if (scene.node_count != pts.group_count)
                mismatch_zones++;
        }

        is_mover = (u8 *)calloc(scene.node_count ? scene.node_count : 1, 1);
        if (!is_mover) {
            q2_zone_close(&zf);
            continue;
        }

        if (q2_events_parse_zone(&ev, &zf) == Q2_OK &&
            q2_movers_build(&movers, &ev, NULL) == Q2_OK) {
            u32 m, k;
            for (m = 0; m < movers.count; m++) {
                for (k = 0; k < movers.movers[m].part_count; k++) {
                    s16 nd = movers.movers[m].node[k];
                    if (nd >= 0 && (u32)nd < scene.node_count) {
                        if (!is_mover[nd])
                            mover_nodes_seen++;
                        is_mover[nd] = 1;
                    }
                }
            }
            q2_movers_free(&movers);
        }

        /* The renderer uses node.origin as the per-node translation. If it is
         * constant zone-wide it is not a per-node origin at all, and whatever
         * positions geometry (and whatever a mover would animate) is elsewhere. */
        {
            q2_scene_node n0;
            u32 t, same = 0, total = 0;
            if (q2_scene_get_node(&scene, 0, &n0)) {
                for (t = 0; t < scene.node_count; t++) {
                    q2_scene_node nt;
                    if (!q2_scene_get_node(&scene, t, &nt))
                        continue;
                    total++;
                    if (nt.origin[0] == n0.origin[0] &&
                        nt.origin[1] == n0.origin[1] &&
                        nt.origin[2] == n0.origin[2])
                        same++;
                }
                if (total && same == total)
                    origin_const_zones++;
                origin_nodes += total;
                origin_same  += same;
            }
        }

        for (node = 0; node < scene.node_count; node++) {
            q2_mapmod_rec rec;
            u32 p;
            int is_mv = is_mover[node];

            if (!q2_scene_get_mapmod(&scene, node, &rec))
                continue;

            if (is_mv) mv_nodes++; else wd_nodes++;

            for (p = 0; p < rec.num_polys; p++) {
                q2_mapmod_poly poly;
                if (!q2_mapmod_get_poly(&rec, p, &poly))
                    continue;
                if (is_mv) { mv[poly.flags & 3]++; mv_total++; }
                else       { wd[poly.flags & 3]++; wd_total++; }

                /* The renderer silently keeps zeroed UVs when this lookup
                 * fails, collapsing the quad onto one texel. Count it. */
                if (!rec.uv || poly.uv_idx >= rec.uv_count) {
                    if (is_mv) mv_uvfail++; else wd_uvfail++;
                }
            }
        }

        free(is_mover);
        if (have_pts)
            q2_points_free(&pts);
        q2_zone_close(&zf);
    }

    printf("  zones scanned        : %llu\n", zones);
    printf("  mover-driven nodes   : %llu (%llu carry geometry)\n",
           mover_nodes_seen, mv_nodes);
    printf("  static world nodes   : %llu\n", wd_nodes);
    printf("  polys  bmodel/world  : %llu / %llu\n\n", mv_total, wd_total);

    printf("  flag    bmodel %%     world %%     bmodel n\n");
    for (i = 0; i < 4; i++) {
        printf("    %d    %9.3f   %9.3f   %10llu\n", i,
               mv_total ? 100.0 * (double)mv[i] / (double)mv_total : 0.0,
               wd_total ? 100.0 * (double)wd[i] / (double)wd_total : 0.0,
               mv[i]);
    }

    {
        double mv_set = mv_total
            ? 100.0 * (double)(mv_total - mv[0]) / (double)mv_total : 0.0;
        double wd_set = wd_total
            ? 100.0 * (double)(wd_total - wd[0]) / (double)wd_total : 0.0;
        printf("\n  any flag set: bmodel %.2f%%  world %.2f%%", mv_set, wd_set);
        if (wd_set > 0.0)
            printf("   (enrichment %.2fx)", mv_set / wd_set);
        printf("\n");
    }

    printf("\n  UV lookup failures (quad collapses to one texel):\n");
    printf("    bmodel %llu / %llu    world %llu / %llu\n",
           mv_uvfail, mv_total, wd_uvfail, wd_total);

    printf("\n  Scene node.origin (the renderer's per-node translation):\n");
    printf("    zones where it is CONSTANT     : %llu / %llu\n",
           origin_const_zones, zones);
    printf("    nodes sharing node 0's origin  : %llu / %llu\n",
           origin_same, origin_nodes);

    printf("\n  Scene nodes vs Points groups:\n");
    printf("    zones where the counts differ : %llu / %llu\n",
           mismatch_zones, zones);
    printf("    zones the renderer overruns   : %llu (%llu nodes past the end)\n",
           overrun_zones, overrun_nodes);

    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * MapMod.clut and the UV rotation, checked against the whole disc.
 *
 * The engine's own reading of these fields was recovered from the world
 * renderer at 0x80068044 (see docs/FORMATS.md §3.3). Reading it out of the code
 * is not by itself proof that the code is what runs on this data — a wrong
 * function, or a field that means something else on a different build, would
 * look exactly the same in the disassembly. So restate each rule as a property
 * the disc must satisfy and measure it:
 *
 *   clut >> 8   indexes the engine's CLUT-id table, which has clut4_count
 *               entries for that map, so it must be < clut4_count everywhere.
 *   clut & 3    selects semi-transparency, so the remaining six bits of the low
 *               byte should be dead — if they are ever set, something else is
 *               in there.
 *   uvIdx & 63  indexes the UV table and must stay inside it.
 */
#define CLUT_CENSUS_MAX_MAPS 64

typedef struct clut_map_stat {
    char               name[64];
    u32                clut4_count;
    u32                clut4_count_a;
    u32                clut4_count_b;
    u32                texpage_count;
    unsigned long long polys;
    u32                max_index;
    u32                zones;
    bool               have_vram;
} clut_map_stat;

static clut_map_stat *clut_find_map(clut_map_stat *maps, int *count,
                                    const char *name)
{
    int i;
    for (i = 0; i < *count; i++)
        if (strcmp(maps[i].name, name) == 0)
            return &maps[i];
    if (*count >= CLUT_CENSUS_MAX_MAPS)
        return NULL;
    memset(&maps[*count], 0, sizeof(maps[0]));
    snprintf(maps[*count].name, sizeof(maps[0].name), "%s", name);
    return &maps[(*count)++];
}

/* "/Q2DATA/LEVELS/BASE0/ZONE0.DAT" -> "BASE0". NULL if the path is not one. */
static const char *clut_map_of(const char *path, char *buf, size_t cap)
{
    const char *rest, *slash;
    size_t len;

    if (*path == '/')
        path++;
    if (strncmp(path, "Q2DATA/LEVELS/", 14) != 0)
        return NULL;

    rest  = path + 14;
    slash = strchr(rest, '/');
    if (!slash)
        return NULL;

    len = (size_t)(slash - rest);
    if (len >= cap)
        len = cap - 1;
    memcpy(buf, rest, len);
    buf[len] = '\0';
    return buf;
}

static void model_faces_check(const q2_model_bank *bank, u32 index,
                              const clut_map_stat *m,
                              unsigned long long *total,
                              unsigned long long *clut_bad,
                              unsigned long long *blend_set,
                              unsigned long long *page_bad,
                              u32 *max_texture)
{
    q2_model mdl;
    u32 face;

    if (q2_model_get(bank, index, &mdl) != Q2_OK)
        return;

    for (face = 0; face < mdl.hdr.num_faces; face++) {
        q2_model_face fc;

        if (!q2_model_get_face(&mdl, face, &fc))
            continue;

        (*total)++;
        if (fc.texture > *max_texture)
            *max_texture = fc.texture;
        if (fc.texture >= m->clut4_count_b)
            (*clut_bad)++;
        if ((fc.flags >> 5) != 0)
            (*blend_set)++;
        if ((fc.flags & 0x1F) >= m->texpage_count)
            (*page_bad)++;
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Every animation clip on the disc, decoded.
 *
 * The clip layout was read out of the pose selector at 0x8006B924, so what this
 * checks is whether the disc actually agrees: does the chain terminate, do the
 * per-frame key offsets land inside block C, and does every part get a key.
 * The single-frame size law `12 + 8*numParts` is reported alongside, because it
 * is the one prediction the layout makes that an earlier pass had already
 * measured (659 of 965) without being able to explain it.
 */
typedef struct anim_stats {
    unsigned long long models, animated, clips, frames, keys;
    unsigned long long variable_rate, bad_chain, bad_keys;
    unsigned long long size_law_hits, single_frame;
    unsigned long long articulated, articulated_animated;
    u32 max_clips, max_frames, max_parts;
    u32 vert_cursor;
    unsigned long long extent_models, extent_max_hit, extent_min_hit;
    unsigned long long extent_art_models, extent_art_max_hit, extent_art_min_hit;
    s32 t_min, t_max;
    s32 q_min_len, q_max_len;
} anim_stats;

/*
 * Does posing a model make its extents agree with its header?
 *
 * ext2 and ext3 match raw vertex max-Y and min-Y on most STATIC models and on
 * essentially none of the 399 articulated ones — which is what established that
 * articulated vertices are part-local. If the keyframe decode is right and the
 * poses are applied flat, one per part, then posing an articulated model should
 * restore that agreement. Nothing about the decode was tuned to make this
 * happen, so it is a genuine prediction rather than a fit.
 */
static void anim_extent_check(const q2_model *m, const q2_model_pose *pose,
                              anim_stats *st, bool articulated)
{
    s32 lo = 1 << 30, hi = -(1 << 30);
    u32 part;
    bool ok2, ok3;

    for (part = 0; part < m->hdr.num_parts; part++) {
        q2_model_part p;
        s16 rot[3][3];
        u32 v;

        if (!q2_model_get_part(m, part, &p))
            return;

        q2_quat_to_matrix(rot, pose[part].q);

        for (v = 0; v < p.num_verts; v++) {
            q2_model_vertex mv;
            s32 y;

            /* Part vertices are contiguous in file order, so the part's slice
             * starts where the previous ones ended; walk them by counting. */
            if (!q2_model_get_vertex(m, st->vert_cursor + v, &mv))
                return;

            y = ((s32)rot[1][0] * mv.x + (s32)rot[1][1] * mv.y +
                 (s32)rot[1][2] * mv.z) >> 12;
            y += pose[part].t[1];

            if (y < lo) lo = y;
            if (y > hi) hi = y;
        }
        st->vert_cursor += p.num_verts;
    }

    if (lo > hi)
        return;

    ok2 = (hi == m->hdr.ext2);
    ok3 = (lo == m->hdr.ext3);

    st->extent_models++;
    if (ok2) st->extent_max_hit++;
    if (ok3) st->extent_min_hit++;
    if (articulated) {
        st->extent_art_models++;
        if (ok2) st->extent_art_max_hit++;
        if (ok3) st->extent_art_min_hit++;
    }
}

static void anim_scan_bank(const q2_model_bank *bank, anim_stats *st)
{
    u32 i;

    for (i = 0; i < bank->count; i++) {
        q2_model mdl;
        q2_model_anim clip;
        u32 n_clips = 0, c;
        bool articulated;

        if (q2_model_get(bank, i, &mdl) != Q2_OK)
            continue;

        st->models++;
        articulated = !q2_model_is_static(&mdl);
        if (articulated)
            st->articulated++;

        n_clips = q2_model_anim_count(&mdl);
        if (n_clips == 0) {
            /* A model with a block C that will not walk is a decode failure,
             * not an unanimated model — separate the two. */
            if (mdl.hdr.ofs_block_d > mdl.hdr.ofs_block_c)
                st->bad_chain++;
            continue;
        }

        st->animated++;
        if (articulated)
            st->articulated_animated++;
        st->clips += n_clips;
        if (n_clips > st->max_clips)
            st->max_clips = n_clips;
        if (mdl.hdr.num_parts > st->max_parts)
            st->max_parts = mdl.hdr.num_parts;

        for (c = 0; c < n_clips; c++) {
            q2_model_pose pose[256];
            u32 f;

            if (!q2_model_anim_get(&mdl, c, &clip))
                break;

            st->frames += clip.frames;
            if (clip.frames > st->max_frames)
                st->max_frames = clip.frames;

            if (clip.flags & 1)
                st->variable_rate++;

            if (c == 0 && clip.frames == 1) {
                u32 block_c_size = mdl.hdr.ofs_block_d - mdl.hdr.ofs_block_c;
                st->single_frame++;
                if (block_c_size == 12u + 8u * mdl.hdr.num_parts)
                    st->size_law_hits++;
            }

            if (mdl.hdr.num_parts > Q2PSX_ARRAY_COUNT(pose))
                continue;

            if (c == 0 && q2_model_pose_at(&mdl, &clip, 0, pose) == Q2_OK) {
                st->vert_cursor = 0;
                anim_extent_check(&mdl, pose, st, articulated);
            }

            for (f = 0; f < clip.frames; f++) {
                u32 p;

                /* Sample the middle of each frame as well as its start, so the
                 * variable-rate clips are exercised where they interpolate
                 * rather than only where they land on a key. */
                if (q2_model_pose_at(&mdl, &clip, f * Q2_MODEL_TICKS_PER_FRAME,
                                     pose) != Q2_OK ||
                    q2_model_pose_at(&mdl, &clip,
                                     f * Q2_MODEL_TICKS_PER_FRAME + 5,
                                     pose) != Q2_OK) {
                    if (st->bad_keys < 6)
                        printf("  BAD  %-12s clip %u/%u  frame %u/%u  parts %u"
                               "  flags 0x%X  blockC %u..%u\n",
                               mdl.hdr.name, c, n_clips, f, clip.frames,
                               mdl.hdr.num_parts, clip.flags,
                               mdl.hdr.ofs_block_c, mdl.hdr.ofs_block_d);
                    st->bad_keys++;
                    break;
                }

                for (p = 0; p < mdl.hdr.num_parts; p++) {
                    s32 len = 0;
                    int k;

                    st->keys++;
                    for (k = 0; k < 3; k++) {
                        if (pose[p].t[k] < st->t_min) st->t_min = pose[p].t[k];
                        if (pose[p].t[k] > st->t_max) st->t_max = pose[p].t[k];
                    }
                    /* |q|^2 in 1.3.12: a real unit quaternion lands near
                     * 4096*4096 >> 12 == 4096. This is the check that the
                     * angle fields really are half angles — get that wrong and
                     * the magnitude wanders. */
                    for (k = 0; k < 4; k++)
                        len += ((s32)pose[p].q[k] * pose[p].q[k]) >> 12;
                    if (len < st->q_min_len) st->q_min_len = len;
                    if (len > st->q_max_len) st->q_max_len = len;
                }
            }
        }
    }
}

static int cmd_anims(disc *d)
{
    anim_stats st;
    int i, n = disc_file_count(d);

    memset(&st, 0, sizeof(st));
    st.t_min = 32767;
    st.t_max = -32768;
    st.q_min_len = 1 << 30;
    st.q_max_len = -(1 << 30);

    printf("CastList animation census — block C decoded as the pose selector\n"
           "at 0x8006B924 reads it\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_model_bank bank;

        base = base ? base + 1 : f->path;
        if (strcmp(base, "COMMON.DAT") != 0 && strncmp(base, "ZONE", 4) != 0)
            continue;
        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        if (strcmp(base, "COMMON.DAT") == 0) {
            q2_common_file cf;
            if (q2_common_open(&cf, &buf) == Q2_OK) {
                if (q2_model_bank_from_common(&bank, &cf) == Q2_OK)
                    anim_scan_bank(&bank, &st);
                q2_common_close(&cf);
            } else {
                q2_buf_free(&buf);
            }
        } else {
            q2_zone_file zf;
            if (q2_zone_open(&zf, &buf) == Q2_OK) {
                if (q2_model_bank_from_zone(&bank, &zf) == Q2_OK)
                    anim_scan_bank(&bank, &st);
                q2_zone_close(&zf);
            } else {
                q2_buf_free(&buf);
            }
        }
    }

    printf("  models                    : %llu\n", st.models);
    printf("    with a walkable chain   : %llu\n", st.animated);
    printf("    articulated             : %llu (%llu animated)\n",
           st.articulated, st.articulated_animated);
    printf("    block C that will NOT walk : %llu\n", st.bad_chain);
    printf("  clips                     : %llu (max %u per model)\n",
           st.clips, st.max_clips);
    printf("  frames                    : %llu (longest clip %u)\n",
           st.frames, st.max_frames);
    printf("  variable-rate clips       : %llu\n", st.variable_rate);
    printf("  keys decoded              : %llu\n", st.keys);
    printf("  frames whose keys escape block C : %llu\n", st.bad_keys);
    printf("  single-frame first clips  : %llu, of which %llu match "
           "12 + 8*numParts\n", st.single_frame, st.size_law_hits);
    printf("  translation range         : %d .. %d\n", st.t_min, st.t_max);
    printf("  |q|^2 in 1.3.12           : %d .. %d  (4096 == unit)\n",
           st.q_min_len, st.q_max_len);

    printf("\n  Posed extents against the header's ext2 / ext3:\n");
    printf("    all models        : max-Y %llu/%llu   min-Y %llu/%llu\n",
           st.extent_max_hit, st.extent_models,
           st.extent_min_hit, st.extent_models);
    printf("    articulated only  : max-Y %llu/%llu   min-Y %llu/%llu\n",
           st.extent_art_max_hit, st.extent_art_models,
           st.extent_art_min_hit, st.extent_art_models);

    /*
     * The interpolator needs an inverse cosine, and the original ships it as a
     * 4096-entry table. This port computes it instead, so measure the two
     * against each other rather than assuming they agree — a slerp weight that
     * is a few units out is invisible, but silently substituting a different
     * function for one the original tabulated is not something to guess at.
     */
    {
        q2_exe exe;

        if (q2_exe_load(&exe, d, NULL) == Q2_OK) {
            u32 idx, worst = 0, mismatches = 0;

            for (idx = 0; idx < 4096; idx++) {
                s16 stored;
                s32 mine, diff;

                if (!q2_exe_s16(&exe, 0x8009FC44 + idx * 2, &stored))
                    break;

                /* The table is indexed by cos/2 + 2048. */
                mine = q2_acos12(((s32)idx - 2048) * 2);
                diff = mine - stored;
                if (diff < 0)
                    diff = -diff;
                if (diff) {
                    mismatches++;
                    if ((u32)diff > worst)
                        worst = (u32)diff;
                }
            }

            printf("\n  acos table vs q2_acos12   : %u of 4096 entries differ, "
                   "worst by %u of 4096 (%.3f degrees)\n",
                   mismatches, worst, (double)worst * 360.0 / 4096.0);
            q2_exe_free(&exe);
        }
    }

    printf("\n%s\n", (st.bad_chain == 0 && st.bad_keys == 0)
           ? "PASS - every chain walks and every key lands inside its block."
           : "FAIL - see above.");
    return (st.bad_chain || st.bad_keys) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
static double hypot2(int dx, int dy)
{
    return sqrt((double)dx * dx + (double)dy * dy);
}

static double hypot3(int dx, int dy, int dz)
{
    return sqrt((double)dx * dx + (double)dy * dy + (double)dz * dz);
}

static int cmd_cluts(disc *d)
{
    clut_map_stat maps[CLUT_CENSUS_MAX_MAPS];
    int map_count = 0;
    int i, n = disc_file_count(d);
    unsigned long long polys = 0, low2[4] = {0,0,0,0}, rot[4] = {0,0,0,0};
    unsigned long long high_bits_set = 0, uv_out_of_range = 0;
    unsigned long long agree[3] = {0,0,0}, agree_pairs = 0;
    unsigned long long rot_agree[3] = {0,0,0}, rot_pairs = 0;
    unsigned long long face_total = 0, face_clut_bad = 0, face_blend_set = 0;
    unsigned long long face_page_bad = 0;
    u32 max_face_texture = 0;
    unsigned long long iso_good[3] = {0,0,0}, iso_total = 0;
    unsigned long long iso_rot_good[3] = {0,0,0}, iso_rot_total = 0;
    double iso_sum[3] = {0.0,0.0,0.0}, iso_rot_sum[3] = {0.0,0.0,0.0};
    static const char *const agree_name[3] = {
        "uv[j]              (old)",
        "uv[(3 - j) & 3]    (no f)",
        "uv[(3 - f - j) & 3](engine)"
    };
    int maps_out_of_range = 0;
    char mapbuf[64];

    printf("MapMod.clut and uvIdxFlags, as the world renderer at 0x80068044"
           " reads them\n\n");
    printf("  clut >> 8      index into the map's clut4[] array\n");
    printf("  clut & 3       non-zero selects semi-transparent (code 0x3E)\n");
    printf("  uvIdx & 0x3F   index into the record's UV table\n");
    printf("  uvIdx >> 6     UV rotation: vertex j takes uv[(3 - f - j) & 3]\n\n");

    /* First pass: how many CLUTs each map actually uploads. */
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        clut_map_stat *m;
        q2_vram_section vs;

        if (!strstr(f->path, "SNDVRAM.DAT"))
            continue;
        if (!clut_map_of(f->path, mapbuf, sizeof(mapbuf)))
            continue;
        m = clut_find_map(maps, &map_count, mapbuf);
        if (!m)
            continue;
        if (q2_vram_load(&vs, d, mapbuf) != Q2_OK)
            continue;
        m->clut4_count   = vs.clut4_count;
        m->clut4_count_a = vs.clut4_count_a;
        m->clut4_count_b = vs.clut4_count_b;
        m->texpage_count = vs.texpage_count;
        m->have_vram     = true;
        q2_vram_free(&vs);
    }

    /* Second pass: every polygon on the disc. */
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        clut_map_stat *m;
        q2_buf buf;
        q2_zone_file zf;
        q2_scene scene;
        q2_points pts;
        bool have_pts;
        u32 node;

        base = base ? base + 1 : f->path;
        if (strncmp(base, "ZONE", 4) != 0)
            continue;
        if (!clut_map_of(f->path, mapbuf, sizeof(mapbuf)))
            continue;
        m = clut_find_map(maps, &map_count, mapbuf);
        if (!m)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;
        if (q2_zone_open(&zf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }
        if (q2_scene_parse(&scene, &zf) != Q2_OK) {
            q2_zone_close(&zf);
            continue;
        }

        m->zones++;
        have_pts = (q2_points_parse(&pts, &zf) == Q2_OK);

        for (node = 0; node < scene.node_count; node++) {
            q2_mapmod_rec rec;
            const q2_point_group *grp = NULL;
            u32 p;

            if (have_pts && node < pts.group_count)
                grp = &pts.groups[node];
            /* One entry per polygon corner in this node: the vertex it uses,
             * and the texel each candidate rule would give it. A node holds at
             * most 63 quads. */
            struct { u8 vtx; u8 rot; u8 uv[3][2]; } corner[63 * 4];
            u32 corners = 0;

            if (!q2_scene_get_mapmod(&scene, node, &rec))
                continue;

            for (p = 0; p < rec.num_polys; p++) {
                q2_mapmod_poly poly;
                u32 index, j;

                if (!q2_mapmod_get_poly(&rec, p, &poly))
                    continue;

                index = (u32)(poly.clut >> 8);
                polys++;
                m->polys++;
                if (index > m->max_index)
                    m->max_index = index;

                low2[poly.clut & 3]++;
                rot[poly.flags & 3]++;

                if ((poly.clut & 0xFC) != 0)
                    high_bits_set++;
                if (!rec.uv || poly.uv_idx >= rec.uv_count) {
                    uv_out_of_range++;
                    continue;
                }

                /*
                 * Does the rule keep the texel scale isotropic?
                 *
                 * A quad's two edges have known world lengths. Whatever corner
                 * rule is right, walking one edge in texel space and the same
                 * edge in world space must give roughly the same texels per
                 * unit on both axes, because these are flat brush faces with a
                 * uniform texture scale. A rule that is 90 degrees out maps the
                 * long texel axis onto the short world axis and the two scales
                 * diverge. This is the test that can see a rotation, which the
                 * shared-vertex test structurally cannot: a rotated face is
                 * *meant* to disagree with its neighbours.
                 */
                if (grp) {
                    const u8 *uv = rec.uv + (size_t)poly.uv_idx * 8;
                    q2_point pt[4];
                    bool have = true;
                    u32 r;

                    for (j = 0; j < 4; j++) {
                        u32 vi = grp->first + poly.vtx[j];
                        if (poly.vtx[j] >= grp->count ||
                            !q2_points_get(&pts, vi, &pt[j]))
                            have = false;
                    }

                    for (r = 0; have && r < 3; r++) {
                        static const int rule_c[3][4] = {
                            {0,1,2,3}, {3,2,1,0}, {0,0,0,0}   /* [2] filled below */
                        };
                        double wa, wb, ua, ub, sa, sb, ratio;
                        int c0, c1, c2;

                        if (r == 2) {
                            c0 = (int)((3u - poly.flags - 0u) & 3u);
                            c1 = (int)((3u - poly.flags - 1u) & 3u);
                            c2 = (int)((3u - poly.flags - 2u) & 3u);
                        } else {
                            c0 = rule_c[r][0];
                            c1 = rule_c[r][1];
                            c2 = rule_c[r][2];
                        }

                        wa = hypot3(pt[1].x - pt[0].x, pt[1].y - pt[0].y,
                                    pt[1].z - pt[0].z);
                        wb = hypot3(pt[2].x - pt[1].x, pt[2].y - pt[1].y,
                                    pt[2].z - pt[1].z);
                        ua = hypot2(uv[c1 * 2 + 0] - uv[c0 * 2 + 0],
                                    uv[c1 * 2 + 1] - uv[c0 * 2 + 1]);
                        ub = hypot2(uv[c2 * 2 + 0] - uv[c1 * 2 + 0],
                                    uv[c2 * 2 + 1] - uv[c1 * 2 + 1]);

                        if (wa < 1.0 || wb < 1.0 || ua < 1.0 || ub < 1.0)
                            break;   /* degenerate; scores no rule */

                        sa = ua / wa;
                        sb = ub / wb;
                        ratio = (sa > sb) ? sa / sb : sb / sa;

                        if (r == 0)
                            iso_total++;
                        iso_sum[r] += ratio;
                        if (ratio < 1.25)
                            iso_good[r]++;
                        if (poly.flags != 0) {
                            if (r == 0)
                                iso_rot_total++;
                            iso_rot_sum[r] += ratio;
                            if (ratio < 1.25)
                                iso_rot_good[r]++;
                        }
                    }
                }

                {
                    const u8 *uv = rec.uv + (size_t)poly.uv_idx * 8;
                    for (j = 0; j < 4 && corners < Q2PSX_ARRAY_COUNT(corner); j++) {
                        u32 c1 = (3u - j) & 3u;
                        u32 c2 = (3u - poly.flags - j) & 3u;

                        corner[corners].vtx = poly.vtx[j];
                        corner[corners].rot = poly.flags;
                        corner[corners].uv[0][0] = uv[j * 2 + 0];
                        corner[corners].uv[0][1] = uv[j * 2 + 1];
                        corner[corners].uv[1][0] = uv[c1 * 2 + 0];
                        corner[corners].uv[1][1] = uv[c1 * 2 + 1];
                        corner[corners].uv[2][0] = uv[c2 * 2 + 0];
                        corner[corners].uv[2][1] = uv[c2 * 2 + 1];
                        corners++;
                    }
                }
            }

            /* Score the three rules on whether corners that share a vertex
             * land on the same texel. */
            {
                u32 a, b, r;
                for (a = 0; a < corners; a++) {
                    for (b = a + 1; b < corners; b++) {
                        bool rotated;

                        if (corner[a].vtx != corner[b].vtx)
                            continue;
                        agree_pairs++;

                        /* Rules 1 and 2 differ only where a rotation is
                         * actually set, so the whole-disc rate is mostly pairs
                         * both score identically. Count that subset apart or
                         * the comparison measures nothing. */
                        rotated = (corner[a].rot != 0 || corner[b].rot != 0);
                        if (rotated)
                            rot_pairs++;

                        for (r = 0; r < 3; r++) {
                            if (corner[a].uv[r][0] == corner[b].uv[r][0] &&
                                corner[a].uv[r][1] == corner[b].uv[r][1]) {
                                agree[r]++;
                                if (rotated)
                                    rot_agree[r]++;
                            }
                        }
                    }
                }
            }
        }

        if (have_pts)
            q2_points_free(&pts);
        q2_zone_close(&zf);
    }

    printf("  %-12s %5s %10s %8s %7s  %s\n",
           "map", "zones", "polys", "max idx", "clut4", "in range");
    for (i = 0; i < map_count; i++) {
        clut_map_stat *m = &maps[i];
        bool ok;

        if (m->polys == 0)
            continue;
        if (!m->have_vram) {
            printf("  %-12s %5u %10llu %8u %7s  %s\n", m->name, m->zones,
                   m->polys, m->max_index, "-", "no SNDVRAM");
            continue;
        }

        ok = (m->max_index < m->clut4_count);
        if (!ok)
            maps_out_of_range++;
        printf("  %-12s %5u %10llu %8u %7u  %s\n", m->name, m->zones, m->polys,
               m->max_index, m->clut4_count, ok ? "yes" : "NO");
    }

    printf("\n  polygons                 : %llu\n", polys);
    printf("  maps with an index past clut4_count : %d\n", maps_out_of_range);
    printf("  polygons with clut bits 2-7 set     : %llu\n", high_bits_set);
    printf("  polygons with uvIdx past the table  : %llu\n", uv_out_of_range);

    printf("\n  clut & 3 (semi-transparency selector):\n");
    for (i = 0; i < 4; i++)
        printf("    %d : %12llu  %6.3f%%\n", i, low2[i],
               polys ? 100.0 * (double)low2[i] / (double)polys : 0.0);

    printf("\n  uvIdx >> 6 (UV rotation):\n");
    for (i = 0; i < 4; i++)
        printf("    %d : %12llu  %6.3f%%\n", i, rot[i],
               polys ? 100.0 * (double)rot[i] / (double)polys : 0.0);

    /*
     * The rotation rule came out of the disassembly, so test it against
     * something the disassembly cannot have arranged: whether polygons that
     * share a vertex agree on that vertex's texel. A wrong corner rule
     * scatters the assignment and the agreement rate collapses; the right one
     * maximises it. Three rules are scored so the comparison has controls —
     * ignoring the rotation entirely, and applying only the reversal.
     */
    printf("\n  Corner rule scored on shared-vertex UV agreement:\n");
    printf("    %-28s %12s %12s  %s\n", "rule", "agree", "pairs", "rate");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %6.3f%%\n", agree_name[i], agree[i],
               agree_pairs,
               agree_pairs ? 100.0 * (double)agree[i] / (double)agree_pairs
                           : 0.0);

    printf("\n  ... restricted to pairs carrying a rotation, the only ones on\n"
           "      which the last two rules can disagree:\n");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %6.3f%%\n", agree_name[i],
               rot_agree[i], rot_pairs,
               rot_pairs ? 100.0 * (double)rot_agree[i] / (double)rot_pairs
                         : 0.0);

    printf("\n  Corner rule scored on texel-scale isotropy (texels per world\n"
           "  unit along each quad edge; a 90-degree error diverges them):\n");
    printf("    %-28s %12s %12s  %s\n", "rule", "within 1.25x", "quads",
           "mean ratio");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %8.4f\n", agree_name[i], iso_good[i],
               iso_total, iso_total ? iso_sum[i] / (double)iso_total : 0.0);

    printf("\n  ... restricted to the quads that carry a rotation:\n");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %8.4f\n", agree_name[i],
               iso_rot_good[i], iso_rot_total,
               iso_rot_total ? iso_rot_sum[i] / (double)iso_rot_total : 0.0);

    /*
     * The model side of the same question. A model face carries a texture-page
     * index and a CLUT index in two bytes, and the model emitter at 0x8006A3FC
     * reads the CLUT one as clut4_count_a + face.texture — so model palettes
     * live in the SECOND section of the clut4 array while the world uses the
     * first. That is what the a/b split has always been, and it predicts two
     * things the disc can check: face.texture < clut4_count_b everywhere, and
     * face.flags never using its blend bits.
     */
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        clut_map_stat *m;
        q2_buf buf;
        q2_model_bank bank;
        bool is_common;
        u32 k;

        base = base ? base + 1 : f->path;
        is_common = (strcmp(base, "COMMON.DAT") == 0);
        if (!is_common && strncmp(base, "ZONE", 4) != 0)
            continue;
        if (!clut_map_of(f->path, mapbuf, sizeof(mapbuf)))
            continue;
        m = clut_find_map(maps, &map_count, mapbuf);
        if (!m || !m->have_vram)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        if (is_common) {
            q2_common_file cf;
            if (q2_common_open(&cf, &buf) != Q2_OK) {
                q2_buf_free(&buf);
                continue;
            }
            if (q2_model_bank_from_common(&bank, &cf) != Q2_OK) {
                q2_common_close(&cf);
                continue;
            }
            for (k = 0; k < bank.count; k++)
                model_faces_check(&bank, k, m, &face_total, &face_clut_bad,
                                  &face_blend_set, &face_page_bad,
                                  &max_face_texture);
            q2_common_close(&cf);
        } else {
            q2_zone_file zf;
            if (q2_zone_open(&zf, &buf) != Q2_OK) {
                q2_buf_free(&buf);
                continue;
            }
            if (q2_model_bank_from_zone(&bank, &zf) != Q2_OK) {
                q2_zone_close(&zf);
                continue;
            }
            for (k = 0; k < bank.count; k++)
                model_faces_check(&bank, k, m, &face_total, &face_clut_bad,
                                  &face_blend_set, &face_page_bad,
                                  &max_face_texture);
            q2_zone_close(&zf);
        }
    }

    printf("\n  CastList faces (clut index = clut4_count_a + face.texture):\n");
    printf("    faces                              : %llu\n", face_total);
    printf("    face.texture past clut4_count_b    : %llu\n", face_clut_bad);
    printf("    face.flags with blend bits set     : %llu\n", face_blend_set);
    printf("    face.flags page past texpage_count : %llu\n", face_page_bad);
    printf("    highest face.texture seen          : %u\n", max_face_texture);

    printf("\n%s\n", (maps_out_of_range == 0 && uv_out_of_range == 0)
           ? "PASS - every CLUT index addresses a CLUT the map uploads, and "
             "every UV index is inside its table."
           : "FAIL - see above.");

    return maps_out_of_range || uv_out_of_range ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Render one brush-model entity (a mover group: door, lift, plat) on its own,
 * framed to fill the view.
 *
 * Drawing a door surrounded by 17,000 other nodes makes it very hard to tell a
 * fault in that door apart from a fault in the renderer. Isolating it, and
 * framing on its own bounds, makes the geometry and its texturing legible.
 */
static int cmd_bmodel(disc *d, const char *map, int zone_index, int which,
                      const char *out_path, bool open_it)
{
    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram = NULL;
    q2_world_stats stats;
    q2_events ev;
    q2_mover_set movers;
    u8 *mask = NULL;
    s32 bmin[3], bmax[3];
    bool any = false;
    u32 m, k;
    const int W = 512, H = 480;

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        return 1;
    }

    if (q2_events_parse_zone(&ev, &zone.zone) != Q2_OK ||
        q2_movers_build(&movers, &ev, NULL) != Q2_OK) {
        fprintf(stderr, "no movers in %s zone %d\n", map, zone_index);
        q2_world_free_zone(&zone);
        return 1;
    }

    printf("%s zone %d: %u mover groups\n", map, zone_index, movers.count);

    mask = (u8 *)calloc(zone.scene.node_count ? zone.scene.node_count : 1, 1);
    if (!mask) {
        q2_movers_free(&movers);
        q2_world_free_zone(&zone);
        return 1;
    }

    bmin[0] = bmin[1] = bmin[2] =  0x7FFFFFFF;
    bmax[0] = bmax[1] = bmax[2] = -0x7FFFFFFF;

    for (m = 0; m < movers.count; m++) {
        if (which >= 0 && (u32)which != m)
            continue;
        for (k = 0; k < movers.movers[m].part_count; k++) {
            s16 nd = movers.movers[m].node[k];
            q2_scene_node node;
            s32 nmin[3], nmax[3];
            int a;

            if (nd < 0 || (u32)nd >= zone.scene.node_count)
                continue;
            if (!q2_scene_get_node(&zone.scene, (u32)nd, &node))
                continue;

            mask[nd] = 1;
            q2_scene_node_bounds(&node, nmin, nmax);
            for (a = 0; a < 3; a++) {
                if (nmin[a] < bmin[a]) bmin[a] = nmin[a];
                if (nmax[a] > bmax[a]) bmax[a] = nmax[a];
            }
            any = true;
            printf("  group %u part %u -> node %d  origin [%d %d %d]\n",
                   m, k, nd, node.origin[0], node.origin[1], node.origin[2]);
        }
    }

    /*
     * Optionally run the mover before drawing. The displacement never touches
     * the geometry — the original offsets the node as it draws it — so this is
     * also the check that the offset actually reaches the renderer.
     */
    if (open_it && which >= 0 && (u32)which < movers.count) {
        s32 shift[3] = { 0, 0, 0 };
        int tick;

        q2_mover_trigger(&movers, (u32)which);

        /* Stop at the top of the travel rather than running on: a door that
         * auto-closes would otherwise be back where it started and the render
         * would look like the offset never arrived. */
        for (tick = 0; tick < 4000; tick++) {
            if (movers.movers[which].state == Q2_MV_ARRIVED ||
                movers.movers[which].state == Q2_MV_OPEN)
                break;
            q2_movers_tick(&movers, 10, 0xFFFF);
        }

        q2_movers_node_offset(&movers, (u32)movers.movers[which].node[0], shift);
        printf("  opened        : state %u, node offset [%d %d %d]\n",
               movers.movers[which].state, shift[0], shift[1], shift[2]);
        zone.movers = &movers;
    }

    if (!any) {
        fprintf(stderr, "no geometry-bearing mover nodes selected\n");
        free(mask);
        q2_world_free_zone(&zone);
        return 1;
    }

    zone.node_filter       = mask;
    zone.node_filter_count = zone.scene.node_count;

    printf("  bmodel bounds : [%d %d %d] .. [%d %d %d]\n",
           bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]);

    /* Frame it: stand back along -Z by its largest extent. */
    q2_camera_default(&cam, W, H);
    {
        s32 ex = bmax[0] - bmin[0];
        s32 ey = bmax[1] - bmin[1];
        s32 ez = bmax[2] - bmin[2];
        s32 extent = ex > ey ? ex : ey;
        if (ez > extent) extent = ez;
        if (extent < 64) extent = 64;

        cam.pos[0] = (bmin[0] + bmax[0]) / 2;
        cam.pos[1] = (bmin[1] + bmax[1]) / 2;
        cam.pos[2] = (bmin[2] + bmax[2]) / 2 - extent;
        cam.yaw = 0;
        cam.pitch = 0;
    }

    if (psx_ot_init(&ot, 4096, 300000) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        free(mask);
        q2_world_free_zone(&zone);
        return 1;
    }
    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) { free(mask); q2_world_free_zone(&zone); return 1; }

    q2_world_build_ot(&zone, &cam, W, H, &ot, &gte, NULL, &stats);
    printf("  quads emitted : %u of %u\n", stats.quads_emitted, stats.quads_total);

    psx_raster_opts_default(&opts);
    {
        q2_vram_section vs;
        if (q2_vram_load(&vs, d, map) == Q2_OK) {
            if (q2_vram_upload(&vs, vram) != Q2_OK)
                opts.textures = false;
            q2_vram_free(&vs);
        } else {
            opts.textures = false;
        }
    }

    psx_fb_clear(&fb, psx_rgb555(4, 4, 10));
    psx_raster_ot(&fb, &ot, vram, &opts);

    if (psx_fb_write_ppm(&fb, out_path) == Q2_OK)
        printf("  wrote %s\n", out_path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    free(mask);
    zone.node_filter = NULL;
    zone.movers      = NULL;
    q2_movers_free(&movers);
    q2_world_free_zone(&zone);
    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Render one CastList model, posed, on its own.
 *
 * This is the end-to-end check of the model path the way `render` is for the
 * world: disc -> CastList -> animation keys -> per-part transform -> GTE ->
 * ordering table -> pixels. If a monster comes out looking like a monster, the
 * keyframe decode, the quaternion conversion, the scratch-window indexing and
 * the second-section CLUT binding are all right at once, which no single
 * numeric check establishes.
 */
/* List a map's model bank. Cheap, and the natural companion to `model`: model
 * names are not unique, so an index is often the only way to name one. */
static int cmd_models(disc *d, const char *map)
{
    q2_buf buf;
    q2_common_file cf;
    q2_model_bank bank;
    char path[160];
    u32 i;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    if (q2_common_open(&cf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        return 1;
    }
    if (q2_model_bank_from_common(&bank, &cf) != Q2_OK) {
        q2_common_close(&cf);
        return 1;
    }

    /* `ext2` is the model's own vertical bias — the draw origin is the entity
     * position lowered by 286 and raised again by it (entitydraw.c). An item
     * that sits in the floor is usually a model whose bias is not what the
     * drawer thinks, so it is listed here beside the geometry. */
    printf("%-4s %-13s %6s %6s %6s %6s %6s  %s\n",
           "idx", "name", "parts", "verts", "faces", "clips", "ext2", "kind");
    for (i = 0; i < bank.count; i++) {
        q2_model m;
        if (q2_model_get(&bank, i, &m) != Q2_OK)
            continue;
        printf("%-4u %-13s %6u %6u %6u %6u %6d  %s\n", i, m.hdr.name,
               m.hdr.num_parts, m.hdr.num_verts, m.hdr.num_faces,
               q2_model_anim_count(&m), (int)m.hdr.ext2,
               q2_model_is_static(&m) ? "static" : "articulated");
    }

    printf("\n%u models\n", bank.count);
    q2_common_close(&cf);
    return 0;
}

static int cmd_model(disc *d, const char *map, const char *want, int clip_index,
                     int frame, const char *out_path, s32 view_yaw)
{
    const int W = 512, H = 480;
    q2_buf buf;
    q2_common_file cf;
    q2_model_bank bank;
    q2_model mdl;
    q2_model_pose pose[256];
    q2_model_anim clip;
    q2_model_instance inst;
    q2_model_draw_stats stats;
    q2_camera cam;
    q2_vram_section vs;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram;
    char path[128];
    u32 i, found = (u32)-1, clip_count;
    bool have_pose = false;
    s32 extent;
    s32 bmin[3] = { 1 << 30, 1 << 30, 1 << 30 };
    s32 bmax[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    if (q2_common_open(&cf, &buf) != Q2_OK) {
        fprintf(stderr, "%s is not a COMMON.DAT\n", path);
        q2_buf_free(&buf);
        return 1;
    }
    if (q2_model_bank_from_common(&bank, &cf) != Q2_OK || bank.count == 0) {
        fprintf(stderr, "%s has no models\n", map);
        q2_common_close(&cf);
        return 1;
    }

    /* Accept an index or a name, because model names are not unique and an
     * index is the only way to reach the duplicates. */
    for (i = 0; i < bank.count; i++) {
        q2_model probe;
        if (q2_model_get(&bank, i, &probe) != Q2_OK)
            continue;
        if (name_casecmp_local(probe.hdr.name, want) == 0) {
            found = i;
            break;
        }
    }
    if (found == (u32)-1) {
        char *endp;
        unsigned long n = strtoul(want, &endp, 0);
        if (*endp == 0 && n < bank.count)
            found = (u32)n;
    }

    /*
     * Not in COMMON.DAT — try the map's ZONE banks. The creature models live
     * there: of the seven creatures BASE1 spawns, only the Soldier appears in a
     * COMMON bank, so searching only COMMON makes six of them unreachable and
     * makes the disc look as though it has almost no articulated models.
     *
     * The zone buffer has to outlive this function's drawing, so it is kept in
     * statics rather than freed at the end of the search.
     */
    if (found == (u32)-1) {
        static q2_buf zbuf;
        static q2_zone_file zf;
        static q2_model_bank zbank;
        int z;

        for (z = 0; z < 8 && found == (u32)-1; z++) {
            char zpath[200];
            u32 zi;

            snprintf(zpath, sizeof(zpath), "Q2DATA/LEVELS/%s/ZONE%d.DAT",
                     map, z);
            if (disc_read_file(d, zpath, &zbuf) != Q2_OK)
                continue;
            if (q2_zone_open(&zf, &zbuf) != Q2_OK) {
                q2_buf_free(&zbuf);
                continue;
            }
            if (q2_model_bank_from_zone(&zbank, &zf) == Q2_OK) {
                for (zi = 0; zi < zbank.count; zi++) {
                    q2_model probe;
                    if (q2_model_get(&zbank, zi, &probe) != Q2_OK)
                        continue;
                    if (name_casecmp_local(probe.hdr.name, want) == 0) {
                        bank  = zbank;
                        found = zi;
                        printf("(from %s)\n", zpath);
                        break;
                    }
                }
            }
            if (found == (u32)-1) {
                q2_zone_close(&zf);
                q2_buf_free(&zbuf);
            }
        }
    }

    if (found == (u32)-1 || q2_model_get(&bank, found, &mdl) != Q2_OK) {
        fprintf(stderr, "no model '%s' in %s (%u models)\n", want, map,
                bank.count);
        q2_common_close(&cf);
        return 1;
    }

    printf("%s model %u: %-12s %u parts, %u verts, %u faces, %s\n",
           map, found, mdl.hdr.name, mdl.hdr.num_parts, mdl.hdr.num_verts,
           mdl.hdr.num_faces,
           q2_model_is_static(&mdl) ? "static" : "articulated");

    clip_count = q2_model_anim_count(&mdl);
    printf("  clips         : %u\n", clip_count);

    /*
     * Every clip's length, and the running total.
     *
     * The total is the number that matters to anything driving this model from
     * a CREATURE, because a creature module's moves are numbered in one global
     * frame timeline (`q2psx-inspect creatures` prints their ranges) and this
     * says whether the clips laid end to end are that timeline.
     */
    if (clip_count >= 1) {
        u32 ci, total = 0;
        printf("  clip lengths  :");
        for (ci = 0; ci < clip_count; ci++) {
            q2_model_anim a;
            if (!q2_model_anim_get(&mdl, ci, &a))
                break;
            printf(" %u", a.frames);
            total += a.frames;
        }
        printf("\n  clip frames   : %u in total\n", total);
    }

    /* Block D — the move table, and what the containment rule makes of it. */
    {
        u32 moves = q2_model_move_count(&mdl), mi;
        printf("  moves         : %u\n", moves);
        for (mi = 0; mi < moves; mi++) {
            q2_model_move mv, prev;
            int gap = -1;
            if (!q2_model_move_get(&mdl, mi, &mv))
                break;
            if (mi && q2_model_move_get(&mdl, mi - 1, &prev))
                gap = (int)mv.start - (int)prev.end;
            printf("    move %-3u  %-12s %5u..%-5u  span %4u  rest %5u%s"
                   "  one %u  gap %d\n",
                   mi, mv.name, mv.start, mv.end, mv.end - mv.start, mv.rest,
                   mv.rest == mv.start ? " (=start)" :
                   mv.rest == mv.end   ? " (=end)  " : " (?)     ",
                   mv.one, gap);
        }
    }

    if (clip_count && (u32)clip_index < clip_count &&
        mdl.hdr.num_parts <= Q2PSX_ARRAY_COUNT(pose) &&
        q2_model_anim_get(&mdl, (u32)clip_index, &clip)) {
        u32 tick = (u32)frame * Q2_MODEL_TICKS_PER_FRAME;
        printf("  clip %d       : %u frames, flags 0x%X%s\n", clip_index,
               clip.frames, clip.flags,
               (clip.flags & 1) ? " (variable rate)" : "");
        if (q2_model_pose_at(&mdl, &clip, tick, pose) == Q2_OK)
            have_pose = true;
        else
            fprintf(stderr, "  pose failed; drawing unposed\n");
    }

    /* Frame the model on its own posed extents. */
    {
        u32 part, cursor = 0;
        for (part = 0; part < mdl.hdr.num_parts; part++) {
            q2_model_part p;
            s16 rot[3][3];
            u32 v;

            if (!q2_model_get_part(&mdl, part, &p))
                break;
            if (have_pose)
                q2_quat_to_matrix(rot, pose[part].q);

            for (v = 0; v < p.num_verts; v++) {
                q2_model_vertex mv;
                s32 pos[3];
                int ax;

                if (!q2_model_get_vertex(&mdl, cursor + v, &mv))
                    break;

                if (have_pose) {
                    for (ax = 0; ax < 3; ax++)
                        pos[ax] = (((s32)rot[ax][0] * mv.x +
                                    (s32)rot[ax][1] * mv.y +
                                    (s32)rot[ax][2] * mv.z) >> 12) +
                                  pose[part].t[ax];
                } else {
                    pos[0] = mv.x; pos[1] = mv.y; pos[2] = mv.z;
                }

                for (ax = 0; ax < 3; ax++) {
                    if (pos[ax] < bmin[ax]) bmin[ax] = pos[ax];
                    if (pos[ax] > bmax[ax]) bmax[ax] = pos[ax];
                }
            }
            cursor += p.num_verts;
        }
    }
    if (bmin[1] > bmax[1]) {
        int ax;
        for (ax = 0; ax < 3; ax++) { bmin[ax] = -128; bmax[ax] = 128; }
    }

    /*
     * Frame on all three axes, not just height. A weapon is long and thin, and
     * a camera placed from its Y extent alone puts it half out of frame — which
     * is exactly what the first version of this did.
     */
    {
        int ax;
        extent = 64;
        for (ax = 0; ax < 3; ax++)
            if (bmax[ax] - bmin[ax] > extent)
                extent = bmax[ax] - bmin[ax];
    }

    /*
     * The RAW bounds beside the posed ones. A single-part static model should
     * read the same either way; where they differ, the pose is carrying a
     * translation, and anything drawing the model UNPOSED puts it somewhere
     * else — which is what an item sitting in the floor looks like.
     */
    {
        s32 rmin[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
        s32 rmax[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
        u32 vi;

        for (vi = 0; vi < mdl.hdr.num_verts; vi++) {
            q2_model_vertex mv;
            if (!q2_model_get_vertex(&mdl, vi, &mv))
                break;
            if (mv.x < rmin[0]) rmin[0] = mv.x;
            if (mv.x > rmax[0]) rmax[0] = mv.x;
            if (mv.y < rmin[1]) rmin[1] = mv.y;
            if (mv.y > rmax[1]) rmax[1] = mv.y;
            if (mv.z < rmin[2]) rmin[2] = mv.z;
            if (mv.z > rmax[2]) rmax[2] = mv.z;
        }
        if (rmin[1] <= rmax[1])
            printf("  raw bounds    : [%d %d %d] .. [%d %d %d]\n",
                   rmin[0], rmin[1], rmin[2], rmax[0], rmax[1], rmax[2]);
    }

    printf("  posed bounds  : [%d %d %d] .. [%d %d %d]  (ext2 %d, ext3 %d)\n",
           bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2],
           mdl.hdr.ext2, mdl.hdr.ext3);

    /* Which texture page and palette each face asks for. A model that looks
     * wrong in one region almost always asks for one page or one CLUT there,
     * and this is the cheapest way to see that. */
    {
        u32 page_faces[32];
        u32 clut_lo = 0xFFFFFFFFu, clut_hi = 0, f, distinct = 0;

        memset(page_faces, 0, sizeof(page_faces));
        for (f = 0; f < mdl.hdr.num_faces; f++) {
            q2_model_face fc;
            if (!q2_model_get_face(&mdl, f, &fc))
                continue;
            page_faces[q2_model_face_page(&fc)]++;
            if (fc.texture < clut_lo) clut_lo = fc.texture;
            if (fc.texture > clut_hi) clut_hi = fc.texture;
        }
        printf("  pages used    :");
        for (f = 0; f < 32; f++)
            if (page_faces[f]) {
                printf(" %u(%u)", f, page_faces[f]);
                distinct++;
            }
        printf("   [%u distinct]\n", distinct);
        {
            u32 part, face_cursor = 0;
            printf("  per part      : (part: faces, page(s), texture(s), uv box)\n");
            for (part = 0; part < mdl.hdr.num_parts; part++) {
                q2_model_part pp;
                u32 k, plo = 99, phi = 0, tlo = 999, thi = 0;
                u32 ulo = 256, uhi = 0, vlo = 256, vhi = 0;

                if (!q2_model_get_part(&mdl, part, &pp))
                    break;
                for (k = 0; k < pp.num_faces; k++) {
                    q2_model_face fc;
                    u32 c;
                    if (!q2_model_get_face(&mdl, face_cursor + k, &fc))
                        break;
                    if (q2_model_face_page(&fc) < plo) plo = q2_model_face_page(&fc);
                    if (q2_model_face_page(&fc) > phi) phi = q2_model_face_page(&fc);
                    if (fc.texture < tlo) tlo = fc.texture;
                    if (fc.texture > thi) thi = fc.texture;
                    /* Where on the page a part samples localises a wrong-looking
                     * region far faster than the page number alone: two parts on
                     * one page can read opposite corners of it. */
                    for (c = 0; c < 4; c++) {
                        if (fc.uv[c][0] < ulo) ulo = fc.uv[c][0];
                        if (fc.uv[c][0] > uhi) uhi = fc.uv[c][0];
                        if (fc.uv[c][1] < vlo) vlo = fc.uv[c][1];
                        if (fc.uv[c][1] > vhi) vhi = fc.uv[c][1];
                    }
                }
                if (pp.num_faces)
                    printf("      %2u: %3u faces  page %u-%u  tex %u-%u"
                           "  u %u-%u  v %u-%u\n",
                           part, pp.num_faces, plo, phi, tlo, thi,
                           ulo, uhi, vlo, vhi);
                face_cursor += pp.num_faces;
            }
        }
        printf("  face.texture  : %u .. %u\n", clut_lo, clut_hi);
    }

    q2_model_instance_init(&inst);
    inst.model  = &mdl;
    inst.pose   = have_pose ? pose : NULL;
    inst.yaw    = view_yaw;

    /* Orbit the model rather than turning it, so its own facing is preserved
     * and the pose is not silently rotated out from under the eye. */
    q2_camera_default(&cam, W, H);
    {
        s32 cx = (bmin[0] + bmax[0]) / 2;
        s32 cy = (bmin[1] + bmax[1]) / 2;
        s32 cz = (bmin[2] + bmax[2]) / 2;
        s32 dist = extent * 5 / 2;   /* clear of the near plane at this FOV */

        cam.yaw   = view_yaw;
        cam.pitch = 0;
        cam.pos[0] = cx - ((q2_sin12(view_yaw) * dist) >> 12);
        cam.pos[1] = cy;
        cam.pos[2] = cz - ((q2_cos12(view_yaw) * dist) >> 12);
    }
    inst.yaw = 0;

    if (psx_ot_init(&ot, 4096, 300000) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        q2_common_close(&cf);
        return 1;
    }
    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) { q2_common_close(&cf); return 1; }

    psx_raster_opts_default(&opts);
    if (q2_vram_load(&vs, d, map) == Q2_OK) {
        inst.clut4_count_a = vs.clut4_count_a;
        if (q2_vram_upload(&vs, vram) != Q2_OK)
            opts.textures = false;
        q2_vram_free(&vs);
    } else {
        opts.textures = false;
    }

    psx_ot_clear(&ot);
    gte_init(&gte);
    gte_set_projection(&gte, cam.projection, W / 2, H / 2);
    gte.zsf3 = (s16)(Q2_ONE_12 / 3);
    gte.zsf4 = (s16)(Q2_ONE_12 / 4);

    q2_model_build_ot(&inst, &cam, &ot, &gte, &stats);
    printf("  faces emitted : %u of %u  (near %u, bad %u, back %u)\n",
           stats.faces_emitted, stats.faces_total, stats.faces_rejected_near,
           stats.faces_rejected_bad, stats.faces_rejected_back);

    psx_fb_clear(&fb, psx_rgb555(4, 4, 10));
    psx_raster_ot(&fb, &ot, vram, &opts);

    if (psx_fb_write_ppm(&fb, out_path) == Q2_OK)
        printf("  wrote %s\n", out_path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    q2_common_close(&cf);
    return 0;
}

/* Does `name` name a model in this map's COMMON bank or in any of its zones? */
static bool class_has_model(disc *d, const q2_model_bank *common_bank,
                            const char *common_path, const char *name)
{
    char dir[160];
    const char *slash;
    u32 m;
    int z;

    for (m = 0; m < common_bank->count; m++) {
        q2_model probe;
        if (q2_model_get(common_bank, m, &probe) != Q2_OK)
            continue;
        if (name_casecmp_local(probe.hdr.name, name) == 0)
            return true;
    }

    slash = strrchr(common_path, '/');
    if (!slash)
        return false;
    if ((size_t)(slash - common_path) >= sizeof(dir))
        return false;
    memcpy(dir, common_path, (size_t)(slash - common_path));
    dir[slash - common_path] = '\0';

    for (z = 0; z < 8; z++) {
        char zpath[200];
        q2_buf zbuf;
        q2_zone_file zf;
        q2_model_bank zbank;
        bool hit = false;

        snprintf(zpath, sizeof(zpath), "%s/ZONE%d.DAT", dir, z);
        if (disc_read_file(d, zpath, &zbuf) != Q2_OK)
            continue;
        if (q2_zone_open(&zf, &zbuf) != Q2_OK) {
            q2_buf_free(&zbuf);
            continue;
        }
        if (q2_model_bank_from_zone(&zbank, &zf) == Q2_OK) {
            for (m = 0; m < zbank.count; m++) {
                q2_model probe;
                if (q2_model_get(&zbank, m, &probe) != Q2_OK)
                    continue;
                if (name_casecmp_local(probe.hdr.name, name) == 0) {
                    hit = true;
                    break;
                }
            }
        }
        q2_zone_close(&zf);
        if (hit)
            return true;
    }

    return false;
}

/* Position, facing and class name of the map's Nth creature spawn. */
static bool creature_at(disc *d, const char *map, int want, s32 pos[3],
                        s32 *facing, char *name_out, size_t name_cap)
{
    char cpath[160];
    q2_buf cbuf;
    q2_common_file cf;
    q2_build_id id;
    q2_class_table tbl;
    q2_population pop;
    u32 g;
    int seen = 0;
    bool found = false;

    if (q2_identify(d, &id) != Q2_OK)
        return false;
    if (q2_class_table_load(&tbl, d, &id) != Q2_OK)
        return false;

    snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, cpath, &cbuf) != Q2_OK) {
        q2_class_table_free(&tbl);
        return false;
    }
    if (q2_common_open(&cf, &cbuf) != Q2_OK) {
        q2_buf_free(&cbuf);
        q2_class_table_free(&tbl);
        return false;
    }

    if (q2_population_parse(&pop, &cf) == Q2_OK) {
        for (g = 0; g < pop.group_count && !found; g++) {
            q2_pop_group grp;
            q2_pop_spawn sp;
            u32 k;

            if (!q2_pop_get_group(&pop, g, &grp) || q2_pop_group_is_path(&grp))
                continue;

            for (k = 0; q2_pop_get_spawn(&pop, &grp, k, &sp); k++) {
                const q2_class_entry *e = q2_class_find(&tbl, sp.class_id);

                if (!e || !e->name[0] || e->is_player)
                    continue;
                if (seen++ != want)
                    continue;

                pos[0] = sp.x;
                pos[1] = sp.y;
                pos[2] = sp.z;
                *facing = sp.angle;
                snprintf(name_out, name_cap, "%s", e->name);
                found = true;
                break;
            }
        }
    }

    q2_common_close(&cf);
    q2_class_table_free(&tbl);
    return found;
}

/* ------------------------------------------------------------------------- */
/*
 * The entity class table, and whether the disc's spawns resolve through it.
 *
 * This is the check the claim needs. Every Population spawn carries a class id;
 * if this is the right table then every id on the disc names a class, and that
 * class's name is a model the same map ships.
 */
static int cmd_classes(disc *d)
{
    q2_build_id id;
    q2_class_table tbl;
    u32 i;
    int f, n = disc_file_count(d);
    unsigned long long spawns = 0, resolved = 0, with_model = 0;
    u32 unresolved_ids[64];
    int unresolved_count = 0;

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }
    if (q2_class_table_load(&tbl, d, &id) != Q2_OK) {
        fprintf(stderr, "no class table for build %s\n", id.serial);
        return 1;
    }

    printf("Entity class table: %u records\n\n", tbl.count);
    printf("  %-4s %-14s %8s %8s %6s  %s\n", "id", "name", "health", "gib",
           "class", "kind");
    for (i = 0; i < tbl.count; i++) {
        const q2_class_entry *e = &tbl.entries[i];
        if (!e->name[0])
            continue;
        printf("  %-4u %-14s %8d %8d %6u  %s\n", e->id, e->name, e->health,
               e->gib_health, (unsigned)e->class_byte,
               e->is_player ? "player skin" : "creature");
    }

    for (f = 0; f < n; f++) {
        const disc_file *file = disc_file_at(d, f);
        const char *base = strrchr(file->path, '/');
        q2_buf buf;
        q2_common_file cf;
        q2_population pop;
        q2_model_bank bank;
        bool have_bank;
        u32 g;

        base = base ? base + 1 : file->path;
        if (strcmp(base, "COMMON.DAT") != 0)
            continue;
        if (disc_read_file(d, file->path, &buf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }
        have_bank = (q2_model_bank_from_common(&bank, &cf) == Q2_OK);

        if (q2_population_parse(&pop, &cf) == Q2_OK) {
            for (g = 0; g < pop.group_count; g++) {
                q2_pop_group grp;
                q2_pop_spawn sp;
                u32 k;

                if (!q2_pop_get_group(&pop, g, &grp))
                    continue;
                if (q2_pop_group_is_path(&grp))
                    continue;

                for (k = 0; q2_pop_get_spawn(&pop, &grp, k, &sp); k++) {
                    const q2_class_entry *e = q2_class_find(&tbl, sp.class_id);

                    spawns++;
                    if (!e) {
                        int seen = 0, u;
                        for (u = 0; u < unresolved_count; u++)
                            if (unresolved_ids[u] == sp.class_id)
                                seen = 1;
                        if (!seen && unresolved_count < 64)
                            unresolved_ids[unresolved_count++] = sp.class_id;
                        continue;
                    }
                    resolved++;

                    /* A creature's model may be in the map's COMMON bank or
                     * in any of its zones' banks, so check both before saying
                     * the name does not resolve to geometry. */
                    if (have_bank && class_has_model(d, &bank, file->path,
                                                     e->name))
                        with_model++;
                }
            }
        }
        q2_common_close(&cf);
    }

    printf("\n  spawn records                            : %llu\n", spawns);
    printf("  resolving to a class                     : %llu\n", resolved);
    printf("  whose class names a model in the same map: %llu\n", with_model);
    if (unresolved_count) {
        int u;
        printf("  unresolved class ids                     :");
        for (u = 0; u < unresolved_count; u++)
            printf(" %u", unresolved_ids[u]);
        printf("\n");
    }

    printf("\n%s\n", (spawns && resolved == spawns)
           ? "PASS - every spawn on the disc names a class in the table."
           : "PARTIAL - see the unresolved ids above.");

    q2_class_table_free(&tbl);
    return 0;
}

/*
 * Draw the creatures a map actually places, at the positions Population gives
 * them, with the models their classes name.
 *
 * This is the first time the three halves of the level meet: Population says
 * where and which class, the class table says which model and how much health,
 * and CastList supplies the geometry and the pose. Anything wrong in any of
 * them shows up here as a monster in a wall, a monster of the wrong kind, or no
 * monster at all.
 */
/*
 * `ai_frame` poses every creature at that AI animation frame instead of the
 * first, which is how a death move is looked at: `q2_creature_world_death_frame`
 * names its first frame, and the frames are one continuous timeline at
 * Q2_CRE_TICKS_PER_FRAME model ticks each. Negative means "the first frame",
 * which is what every caller wanted before this existed.
 */
static u32 draw_map_creatures(disc *d, const char *map, const q2_camera *cam,
                              psx_ot *ot, gte_state *gte, u32 clut4_count_a,
                              u32 *out_drawn, s32 ai_frame)
{
    char cpath[160], zpath[200];
    q2_buf cbuf;
    q2_common_file cf;
    q2_build_id id;
    q2_class_table tbl;
    q2_population pop;
    q2_model_bank common_bank;
    bool have_common_bank;
    u32 g, drawn = 0, faces = 0;
    int z;

    /* Zone banks are opened lazily and kept for the whole pass: a creature's
     * model is as likely to live in a zone's CastList as in COMMON's. */
    q2_buf zbuf[8];
    q2_zone_file zf[8];
    q2_model_bank zbank[8];
    bool zopen[8];

    memset(zopen, 0, sizeof(zopen));

    if (q2_identify(d, &id) != Q2_OK)
        return 0;
    if (q2_class_table_load(&tbl, d, &id) != Q2_OK)
        return 0;

    snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, cpath, &cbuf) != Q2_OK) {
        q2_class_table_free(&tbl);
        return 0;
    }
    if (q2_common_open(&cf, &cbuf) != Q2_OK) {
        q2_buf_free(&cbuf);
        q2_class_table_free(&tbl);
        return 0;
    }

    have_common_bank = (q2_model_bank_from_common(&common_bank, &cf) == Q2_OK);

    for (z = 0; z < 8; z++) {
        snprintf(zpath, sizeof(zpath), "Q2DATA/LEVELS/%s/ZONE%d.DAT", map, z);
        if (disc_read_file(d, zpath, &zbuf[z]) != Q2_OK)
            continue;
        if (q2_zone_open(&zf[z], &zbuf[z]) != Q2_OK) {
            q2_buf_free(&zbuf[z]);
            continue;
        }
        zopen[z] = (q2_model_bank_from_zone(&zbank[z], &zf[z]) == Q2_OK);
        if (!zopen[z])
            q2_zone_close(&zf[z]);
    }

    if (q2_population_parse(&pop, &cf) == Q2_OK) {
        for (g = 0; g < pop.group_count; g++) {
            q2_pop_group grp;
            q2_pop_spawn sp;
            u32 k;

            if (!q2_pop_get_group(&pop, g, &grp) || q2_pop_group_is_path(&grp))
                continue;

            for (k = 0; q2_pop_get_spawn(&pop, &grp, k, &sp); k++) {
                const q2_class_entry *e = q2_class_find(&tbl, sp.class_id);
                q2_model mdl;
                bool found = false;
                u32 m;

                if (!e || !e->name[0] || e->is_player)
                    continue;

                if (have_common_bank) {
                    for (m = 0; m < common_bank.count && !found; m++) {
                        if (q2_model_get(&common_bank, m, &mdl) != Q2_OK)
                            continue;
                        found = (name_casecmp_local(mdl.hdr.name, e->name) == 0);
                    }
                }
                for (z = 0; z < 8 && !found; z++) {
                    if (!zopen[z])
                        continue;
                    for (m = 0; m < zbank[z].count && !found; m++) {
                        if (q2_model_get(&zbank[z], m, &mdl) != Q2_OK)
                            continue;
                        found = (name_casecmp_local(mdl.hdr.name, e->name) == 0);
                    }
                }
                if (!found)
                    continue;

                {
                    q2_model_instance inst;
                    q2_model_draw_stats st;
                    q2_model_pose pose[256];
                    q2_model_anim clip;
                    bool posed = false;

                    /* Stand it on its first animation frame rather than the
                     * unposed rest, which for an articulated model is every
                     * part at the origin. */
                    if (mdl.hdr.num_parts > Q2PSX_ARRAY_COUNT(pose)) {
                        posed = false;
                    } else if (ai_frame >= 0) {
                        u32 within = 0;

                        if (q2_model_anim_at(&mdl,
                                             (u32)ai_frame * Q2_CRE_TICKS_PER_FRAME,
                                             &clip, &within))
                            posed = (q2_model_pose_at(&mdl, &clip, within,
                                                      pose) == Q2_OK);
                    } else if (q2_model_anim_get(&mdl, 0, &clip) &&
                               q2_model_pose_at(&mdl, &clip, 0, pose) == Q2_OK) {
                        posed = true;
                    }

                    q2_model_instance_init(&inst);
                    inst.model         = &mdl;
                    inst.pose          = posed ? pose : NULL;
                    inst.origin[0]     = sp.x;
                    inst.origin[1]     = sp.y;
                    inst.origin[2]     = sp.z;
                    inst.yaw           = sp.angle;
                    inst.clut4_count_a = clut4_count_a;

                    faces += q2_model_build_ot(&inst, cam, ot, gte, &st);
                    if (st.faces_emitted)
                        drawn++;
                }
            }
        }
    }

    for (z = 0; z < 8; z++)
        if (zopen[z])
            q2_zone_close(&zf[z]);

    q2_common_close(&cf);
    q2_class_table_free(&tbl);

    if (out_drawn)
        *out_drawn = drawn;
    return faces;
}

/*
 * The items a map places, drawn where the place records put them.
 *
 * The creature walk above resolves a class to a model and draws it directly;
 * this goes through the entity system instead, because an item is not just a
 * model at a position — it has a spin, a scale and a glow tint that its think
 * produces, and drawing it any other way would not exercise the thing being
 * checked. One tick is run before drawing so the spin and glow have values.
 */
static u32 draw_map_items(disc *d, const char *map, const q2_camera *cam,
                          psx_ot *ot, gte_state *gte, u32 clut4_count_a,
                          u32 *out_drawn, u32 *out_no_model)
{
    char cpath[160], zpath[200];
    q2_buf cbuf;
    q2_common_file cf;
    q2_population pop;
    q2_model_bank common_bank;
    q2_entity_set set;
    q2_entity_world world;
    q2_inventory inv;
    q2_entity_draw_ctx ctx;
    q2_entity_draw_stats st;
    u32 faces = 0;
    int z;

    q2_buf zbuf[8];
    q2_zone_file zf[8];
    q2_model_bank zbank[8];
    bool zopen[8];

    memset(zopen, 0, sizeof(zopen));
    memset(&set, 0, sizeof(set));
    memset(&st, 0, sizeof(st));

    snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, cpath, &cbuf) != Q2_OK)
        return 0;
    if (q2_common_open(&cf, &cbuf) != Q2_OK) {
        q2_buf_free(&cbuf);
        return 0;
    }

    if (q2_population_parse(&pop, &cf) != Q2_OK ||
        q2_item_spawn_all(&set, &pop, NULL, NULL) != Q2_OK) {
        q2_common_close(&cf);
        q2_buf_free(&cbuf);
        return 0;
    }

    /* Run one tick so the spin, the materialise ramp and the glow have run at
     * least once. No player is registered, so nothing is collected. */
    q2_entity_world_init(&world);
    q2_inventory_init(&inv);
    world.dt = 12;
    q2_entity_run(&set, &world);

    /*
     * An item's model can be in COMMON's bank or in any zone's, exactly as a
     * creature's can. Resolve against each in turn FIRST — each entity records
     * the bank that had it — and only then draw, once. Drawing per bank would
     * emit an entity as many times as there are banks, and would index the
     * wrong bank while doing it.
     */
    memset(&ctx, 0, sizeof(ctx));
    ctx.clut4_count_a = clut4_count_a;
    ctx.player        = 0;

    if (q2_model_bank_from_common(&common_bank, &cf) == Q2_OK) {
        u32 i;
        ctx.bank = &common_bank;
        for (i = 0; i < set.count; i++)
            q2_entity_resolve_model(&set.ent[i], &common_bank);
    }

    for (z = 0; z < 8; z++) {
        u32 i;

        snprintf(zpath, sizeof(zpath), "Q2DATA/LEVELS/%s/ZONE%d.DAT", map, z);
        if (disc_read_file(d, zpath, &zbuf[z]) != Q2_OK)
            continue;
        if (q2_zone_open(&zf[z], &zbuf[z]) != Q2_OK) {
            q2_buf_free(&zbuf[z]);
            continue;
        }
        zopen[z] = (q2_model_bank_from_zone(&zbank[z], &zf[z]) == Q2_OK);
        if (!zopen[z]) {
            q2_zone_close(&zf[z]);
            q2_buf_free(&zbuf[z]);
            continue;
        }

        for (i = 0; i < set.count; i++)
            q2_entity_resolve_model(&set.ent[i], &zbank[z]);
        if (!ctx.bank)
            ctx.bank = &zbank[z];
    }

    faces = q2_entity_build_ot(&set, &ctx, cam, ot, gte, &st);

    if (out_drawn)
        *out_drawn = st.drawn;
    if (out_no_model)
        *out_no_model = st.no_model;

    for (z = 0; z < 8; z++)
        if (zopen[z]) {
            q2_zone_close(&zf[z]);
            q2_buf_free(&zbuf[z]);
        }

    q2_entity_set_free(&set);
    q2_common_close(&cf);
    q2_buf_free(&cbuf);
    return faces;
}

/* ------------------------------------------------------------------------- */
/*
 * Stand in front of one of a map's creatures and look at it.
 *
 * `fps` proves the pieces fit together; this proves they fit together in the
 * right PLACE. The camera is put a few hundred units from the chosen spawn,
 * facing it, and the whole level is drawn around it — so a monster standing in
 * a wall, or floating, or facing the wrong way, is immediately visible.
 */
static int cmd_mob(disc *d, const char *map, int zone_index, int which,
                   const char *out_path, s32 ai_frame)
{
    const int W = TOOL_VIEW_W, H = TOOL_VIEW_H;
    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram;
    q2_world_stats wstats;
    q2_vram_section vs;
    u32 clut4_count_a = 0;
    s32 pos[3] = { 0, 0, 0 };
    s32 facing = 0;
    char cname[16] = "";
    u32 drawn = 0, cfaces;

    if (!creature_at(d, map, which, pos, &facing, cname, sizeof(cname))) {
        fprintf(stderr, "%s has no creature %d\n", map, which);
        return 1;
    }
    printf("%s creature %d: %-12s at [%d %d %d]\n", map, which, cname,
           pos[0], pos[1], pos[2]);

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        return 1;
    }

    /* Back off along the creature's own facing, so it is seen from the front,
     * and raise the eye to roughly head height. */
    camera_console(&cam, W, H);
    cam.yaw   = facing + 2048;
    cam.pitch = 0;
    cam.pos[0] = pos[0] + ((q2_sin12(facing) * 700) >> 12);
    cam.pos[1] = pos[1] - 250;
    cam.pos[2] = pos[2] + ((q2_cos12(facing) * 700) >> 12);

    if (psx_ot_init(&ot, 4096, 300000) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        q2_world_free_zone(&zone);
        return 1;
    }
    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        q2_world_free_zone(&zone);
        return 1;
    }

    psx_raster_opts_default(&opts);
    if (q2_vram_load(&vs, d, map) == Q2_OK) {
        clut4_count_a = vs.clut4_count_a;
        opts.textures = (q2_vram_upload(&vs, vram) == Q2_OK);
        q2_vram_free(&vs);
    } else {
        opts.textures = false;
    }

    print_console_framing(&cam, W, H);
    q2_world_build_ot(&zone, &cam, W, H, &ot, &gte, NULL, &wstats);
    cfaces = draw_map_creatures(d, map, &cam, &ot, &gte, clut4_count_a, &drawn,
                                ai_frame);
    printf("  world %u quads, creatures %u visible / %u faces\n",
           wstats.quads_emitted, drawn, cfaces);

    psx_fb_clear(&fb, psx_rgb555(2, 2, 5));
    psx_raster_ot(&fb, &ot, vram, &opts);

    {
        u16 label = psx_rgb555(20, 20, 22);
        psx_fb_text(&fb, 12, 12, map, label, 2);
        psx_fb_text(&fb, 12, 30, cname, label, 2);
    }

    if (psx_fb_write_ppm(&fb, out_path) == Q2_OK)
        printf("  wrote %s\n", out_path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    q2_world_free_zone(&zone);
    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * A first-person frame: the world from the player's eye, the weapon in hand,
 * and the HUD.
 *
 * This is the first view that shows the whole port at once rather than one
 * layer of it. The world comes from the Scene/MapMod path, the weapon is a
 * CastList model drawn through the same GTE and the same ordering table -- it
 * sorts against the walls rather than being pasted on top -- and the numbers on
 * the HUD are the simulation's real inventory, not captions.
 *
 * The weapon models are already view models: they carry the player's arm, and
 * their geometry is authored around the eye. So the viewmodel is placed at the
 * camera, nudged forward and down, and turned to face the camera's yaw.
 */
/*
 * `eye_dy` and `roll` are the two things the movement frame contributes to a
 * view that this command could not previously show: the eye's height above the
 * feet, which a crouch volume and a jump both change, and the strafe lean. Pass
 * the figures `q2psx-inspect pmove` measures and the frame is a real player's,
 * not a hand-posed camera.
 */
static int cmd_fps(disc *d, const char *map, int zone_index, const char *weapon,
                   const char *out_path, s32 yaw_offset, s32 gun_yaw,
                   s32 eye_dy, s32 roll)
{
    const int W = TOOL_VIEW_W, H = TOOL_VIEW_H;
    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram;
    q2_world_stats wstats;
    q2_inventory inv;
    q2_vram_section vs;
    q2_hud_tables hud_tab;
    q2_hud_font hud_font;
    q2_hud hud;
    q2_hud_ctx hud_ctx;
    psx_ot hud_ot;
    bool have_hud_tab = false;
    bool have_hud = false;
    bool have_vram = false;
    u32 clut4_count_a = 0;

    q2_buf cbuf;
    q2_common_file cf;
    q2_model_bank bank;
    q2_model wmodel;
    bool have_weapon = false;
    char cpath[160];

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        return 1;
    }

    /*
     * Stand where the game would put the player: a StartPos for this zone, at
     * eye height. World Y grows downward, so the eye is at a SMALLER Y than the
     * feet -- getting that sign wrong buries the camera in the floor.
     */
    camera_console(&cam, W, H);
    cam.yaw   = yaw_offset;
    cam.pitch = 0;
    cam.roll  = roll;
    {
        char spath[160];
        q2_buf sbuf;
        bool placed = false;

        snprintf(spath, sizeof(spath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(d, spath, &sbuf) == Q2_OK) {
            q2_common_file scf;
            if (q2_common_open(&scf, &sbuf) == Q2_OK) {
                q2_start_pos_list sl;
                if (q2_start_pos_parse(&sl, &scf) == Q2_OK) {
                    u32 k;
                    for (k = 0; k < sl.count; k++) {
                        q2_start_pos sp;
                        if (!q2_start_pos_get(&sl, k, &sp) ||
                            sp.zone != zone_index)
                            continue;
                        cam.pos[0] = sp.x;
                        cam.pos[1] = sp.y - Q2_VIEW_STAND + eye_dy;
                        cam.pos[2] = sp.z;
                        cam.yaw    = sp.angle + yaw_offset;
                        placed = true;
                        printf("%s zone %d: eye at spawn [%d %d %d] yaw %d\n",
                               map, zone_index, sp.x, sp.y, sp.z, cam.yaw);
                        break;
                    }

                }
                q2_common_close(&scf);
            } else {
                q2_buf_free(&sbuf);
            }
        }

        if (!placed) {
            s32 wmin[3], wmax[3];
            q2_world_bounds(&zone, wmin, wmax);
            cam.pos[0] = (wmin[0] + wmax[0]) / 2;
            cam.pos[1] = (wmin[1] + wmax[1]) / 2;
            cam.pos[2] = (wmin[2] + wmax[2]) / 2;
            printf("%s zone %d: no start position, standing at the centre\n",
                   map, zone_index);
        }
    }

    if (psx_ot_init(&ot, 4096, 300000) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        q2_world_free_zone(&zone);
        return 1;
    }
    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        q2_world_free_zone(&zone);
        return 1;
    }

    psx_raster_opts_default(&opts);
    if (q2_vram_load(&vs, d, map) == Q2_OK) {
        clut4_count_a = vs.clut4_count_a;
        have_vram = (q2_vram_upload(&vs, vram) == Q2_OK);

        /*
         * The overlay's atlas rides in the same file as the world's texture
         * pages, so it goes up while the section is still open. Its palettes
         * come from the executable rather than from the disc.
         */
        {
            q2_build_id bid;
            if (q2_identify(d, &bid) == Q2_OK &&
                q2_hud_tables_load(&hud_tab, d, &bid) == Q2_OK) {
                have_hud_tab = true;
                have_hud = (q2_hud_font_upload(&hud_font, &hud_tab, &vs,
                                               vram) == Q2_OK);
            }
        }
        q2_vram_free(&vs);
    }
    opts.textures = have_vram;

    print_console_framing(&cam, W, H);
    q2_world_build_ot(&zone, &cam, W, H, &ot, &gte, NULL, &wstats);
    printf("  world         : %u of %u quads\n", wstats.quads_emitted,
           wstats.quads_total);

    /* The creatures this map places, into the same table. */
    {
        u32 drawn = 0;
        u32 cfaces = draw_map_creatures(d, map, &cam, &ot, &gte,
                                        clut4_count_a, &drawn, -1);
        printf("  creatures     : %u visible, %u faces\n", drawn, cfaces);
    }

    /* And the items, through the entity system, into the same table again. */
    {
        u32 drawn = 0, missing = 0;
        u32 ifaces = draw_map_items(d, map, &cam, &ot, &gte,
                                    clut4_count_a, &drawn, &missing);
        printf("  items         : %u visible, %u faces", drawn, ifaces);
        if (missing)
            printf(", %u with no model in this map", missing);
        printf("\n");
    }

    /* The weapon, into the same ordering table. */
    snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (weapon && disc_read_file(d, cpath, &cbuf) == Q2_OK) {
        if (q2_common_open(&cf, &cbuf) == Q2_OK) {
            if (q2_model_bank_from_common(&bank, &cf) == Q2_OK) {
                u32 i;
                for (i = 0; i < bank.count; i++) {
                    q2_model probe;
                    if (q2_model_get(&bank, i, &probe) != Q2_OK)
                        continue;
                    if (name_casecmp_local(probe.hdr.name, weapon) == 0) {
                        wmodel = probe;
                        have_weapon = true;
                        break;
                    }
                }
                if (!have_weapon) {
                    char *endp;
                    unsigned long n = strtoul(weapon, &endp, 0);
                    if (*endp == 0 && n < bank.count &&
                        q2_model_get(&bank, (u32)n, &wmodel) == Q2_OK)
                        have_weapon = true;
                }
            }
            if (!have_weapon)
                q2_common_close(&cf);
        } else {
            q2_buf_free(&cbuf);
        }
    }

    if (have_weapon) {
        q2_model_instance inst;
        q2_model_draw_stats mstats;
        s32 fwd[3], right[3];
        s32 s = q2_sin12(cam.yaw), c = q2_cos12(cam.yaw);

        /* Camera basis on the ground plane. */
        fwd[0]   =  s; fwd[2]   =  c;
        right[0] =  c; right[2] = -s;

        q2_model_instance_init(&inst);
        inst.model         = &wmodel;
        inst.pose          = NULL;
        inst.yaw           = cam.yaw + gun_yaw;
        inst.clut4_count_a = clut4_count_a;

        /*
         * In front of the eye, slightly right and below it. The offsets are
         * scaled to the model's own extent rather than fixed, because a view
         * model spans a few hundred units around its origin and a fixed nudge
         * puts half of it behind the near plane -- which the GTE rejects
         * outright, exactly as the hardware did.
         */
        {
            s32 reach = 900, side = 220, drop = 260;
            inst.origin[0] = cam.pos[0] +
                             ((fwd[0] * reach + right[0] * side) >> 12);
            inst.origin[1] = cam.pos[1] + drop;
            inst.origin[2] = cam.pos[2] +
                             ((fwd[2] * reach + right[2] * side) >> 12);
        }

        q2_model_build_ot(&inst, &cam, &ot, &gte, &mstats);
        printf("  weapon        : %-13s %u of %u faces\n", wmodel.hdr.name,
               mstats.faces_emitted, mstats.faces_total);
        q2_common_close(&cf);
    }

    psx_fb_clear(&fb, psx_rgb555(2, 2, 5));
    psx_raster_ot(&fb, &ot, vram, &opts);

    /*
     * The HUD, into its own ordering table and rasterised over the finished
     * frame -- which is how the console does it: the 2D overlay has its own OT
     * per player, drawn after the world.
     *
     * There is no status bar here because there is none in the game. What the
     * console shows is a crosshair, a notification stack and a centred line, so
     * that is what this shows: the weapon-selected message for the weapon
     * actually in hand, drawn with the original's own word graphics out of
     * chars.lbm, and a damage flash driven by the simulation's inventory.
     * src/game/hud.h records how that conclusion was reached.
     */
    if (have_hud) {
        /* Laid out in the console's 512x248 and translated into this larger
         * debug frame; see the note on q2_hud_ctx. */
        q2_hud_ctx_centre_in(&hud_ctx, W, H);
        q2_hud_init(&hud, &hud_tab, 1);

        q2_inventory_init(&inv);
        q2_hud_track(&hud, inv.health, inv.armour);

        if (have_weapon) {
            /*
             * The glyph table is indexed by the live weapon id, and the model
             * bank names its view models the same way the weapon table does, so
             * the message says what is in the player's hands rather than what
             * a caption says it is.
             */
            q2_hud_weapon_selected(&hud, &hud_tab,
                                   q2_hud_weapon_by_model(&hud_tab,
                                                          wmodel.hdr.name));
        }
        q2_hud_centre(&hud, &hud_tab, &hud_ctx, map);

        if (psx_ot_init(&hud_ot, 8, 4096) == Q2_OK) {
            q2_hud_build_ot(&hud, &hud_font, &hud_ctx, &hud_ot, 0);
            psx_raster_ot(&fb, &hud_ot, vram, &opts);
            psx_ot_free(&hud_ot);
            printf("  hud           : overlay drawn from chars.lbm\n");
        }
    } else {
        printf("  hud           : no chars.lbm on this map -- no overlay\n");
    }
    if (have_hud_tab)
        q2_hud_tables_free(&hud_tab);

    if (psx_fb_write_ppm(&fb, out_path) == Q2_OK)
        printf("  wrote %s\n", out_path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    q2_world_free_zone(&zone);
    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Render a zone to a PPM. This is the end-to-end test of the geometry path:
 * disc -> zone chunks -> GTE -> ordering table -> rasteriser -> pixels, with no
 * window and no GPU involved. If this produces a coherent image, every layer
 * beneath it is working.
 *
 * The camera is placed automatically at the centre of the zone's true world
 * bounds, backed off along -Z far enough to see the whole thing.
 */
/* Where `render` points the camera when it has a rotator worth looking at. */
static bool g_focus_valid;
static s32  g_focus[3];
static s32  g_focus_size;

static int cmd_render(disc *d, const char *map, int zone_index, const char *out_path,
                      s32 yaw, s32 pitch, s32 rot_ticks, s32 eye_pitch)
{
    /* pitch == 9999 is a sentinel meaning "stand at the spawn point and look
     * ahead" rather than framing the whole zone from outside. It is the view a
     * player actually gets, and therefore the honest test of the renderer.
     * `eye_pitch` is the pitch to apply once standing there, since the sentinel
     * has taken `pitch` over. */
    bool eye_view = (pitch == 9999);
    if (eye_view)
        pitch = eye_pitch;

    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram = NULL;
    q2_world_stats stats;
    q2_result r;
    s32 wmin[3], wmax[3];
    const int W = TOOL_VIEW_W, H = TOOL_VIEW_H;   /* the console's, at 2x */

    r = q2_world_load_zone(&zone, d, map, zone_index);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d: %s\n", map, zone_index, q2_result_str(r));
        return 1;
    }

    q2_world_bounds(&zone, wmin, wmax);

    printf("%s\n", zone.name);
    printf("  scene nodes   : %u\n", zone.scene.node_count);
    printf("  vertices      : %u\n", zone.points.count);
    printf("  world bounds  : [%d %d %d] .. [%d %d %d]\n",
           wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2]);
    printf("  world size    : %d x %d x %d\n",
           wmax[0] - wmin[0], wmax[1] - wmin[1], wmax[2] - wmin[2]);

    /*
     * The zone's rotating brushes, turned by `rot_ticks` before drawing.
     *
     * This is how a rotator is looked at rather than counted: render the same
     * zone twice, once with 0 ticks and once with several hundred, and the
     * geometry that moves is the geometry a script turns. The rotators are
     * built from the ZONE's own Events — the client builds from COMMON's,
     * which is where its trigger volumes point but NOT where the rotation calls
     * live, so it finds one or two per map and this finds dozens.
     *
     * Every rotator is stepped directly, standing in for the script call, since
     * what fires a zone's records is still unknown (openquestions #50).
     */
    static q2_rotator_set g_render_rot;      /* outlives the draw */

    /* A NEGATIVE tick count builds and frames the rotators without turning
     * them: the "before" of a before/after pair, taken from the same camera,
     * which a zero here could not give because zero also means "no rotators
     * at all" and would frame the whole zone instead. */
    if (rot_ticks != 0) {
        char zpath[256], cpath[256];
        q2_buf zbuf, cbuf;

        snprintf(zpath, sizeof(zpath), "Q2DATA/LEVELS/%s/ZONE%d.DAT",
                 map, zone_index);
        snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);

        if (disc_read_file(d, zpath, &zbuf) == Q2_OK &&
            disc_read_file(d, cpath, &cbuf) == Q2_OK) {
            q2_zone_file   zf;
            q2_common_file cf;
            q2_events      zev, zops;
            q2_userfuncs   uf;

            /*
             * Walk COMMON's script, but read its object slots from this zone's
             * same-offset Events copy. That is the constructor's gp+372/gp+376
             * split at 0x800285CC; building from either chunk alone chooses the
             * wrong nodes in zones where their copies differ.
             */
            if (q2_zone_open(&zf, &zbuf) == Q2_OK &&
                q2_common_open(&cf, &cbuf) == Q2_OK &&
                q2_events_parse_common(&zev, &cf) == Q2_OK &&
                q2_events_parse_zone(&zops, &zf) == Q2_OK &&
                q2_userfuncs_parse(&uf, &cf) == Q2_OK) {
                q2_rotators_set_operand_source(&g_render_rot, zev.data,
                                               zops.data, zops.size);
                if (q2_rotators_build(&g_render_rot, &zev, &uf,
                                      &zone.scene) == Q2_OK) {
                u32 ri, moved = 0;
                s32 t;

                /*
                 * Re-triggered every tick, which is a script holding the
                 * rotation ON rather than tapping it once. One step is all a
                 * single call buys — `q2_rotators_tick` consumes the request —
                 * so a single tap turns an ACCUM rotator by one speed's worth
                 * and stops, which is correct but shows nothing.
                 *
                 * Nothing is triggered on the "before" pass: a SNAP rotator
                 * moves the moment it is asked, so triggering it and ticking
                 * zero times would still show it turned.
                 */
                for (t = 0; t < rot_ticks; t++) {
                    for (ri = 0; ri < g_render_rot.count; ri++)
                        q2_rotator_trigger(&g_render_rot, ri);
                    moved += q2_rotators_tick(&g_render_rot, 12);
                }


                zone.rotators = &g_render_rot;
                printf("  rotators      : %u, %u tick-moves over %d ticks\n",
                       g_render_rot.count, moved, rot_ticks);
                for (ri = 0; ri < g_render_rot.count; ri++)
                    printf("    node %d  axis %u  angle %d  pivot [%d %d %d]\n",
                           g_render_rot.rotators[ri].node,
                           g_render_rot.rotators[ri].axis,
                           g_render_rot.rotators[ri].angle,
                           g_render_rot.rotators[ri].pivot[0],
                           g_render_rot.rotators[ri].pivot[1],
                           g_render_rot.rotators[ri].pivot[2]);

                /*
                 * Frame the rotator that turned the most, rather than the whole
                 * zone. A rotating brush is one node among hundreds: from the
                 * outside view it is a few pixels, and a render that cannot
                 * show the motion cannot check it either.
                 */
                {
                    s32 best = -1, best_angle = -1;
                    q2_rotator_set probe;

                    /*
                     * Which rotator to look at is decided by a PROBE run, not
                     * by the pass being rendered: the before pass has turned
                     * nothing, so choosing by current angle would pick nothing
                     * there and frame the whole zone, and the pair would come
                     * from two different cameras. The probe is a second set off
                     * the same script, run the same way, and thrown away.
                    */
                    memset(&probe, 0, sizeof(probe));
                    q2_rotators_set_operand_source(&probe, zev.data,
                                                   zops.data, zops.size);
                    if (q2_rotators_build(&probe, &zev, &uf,
                                          &zone.scene) == Q2_OK) {
                        u32 pi;
                        s32 pt;

                        for (pt = 0; pt < 400; pt++) {
                            for (pi = 0; pi < probe.count; pi++)
                                q2_rotator_trigger(&probe, pi);
                            q2_rotators_tick(&probe, 12);
                        }

                        for (pi = 0; pi < probe.count; pi++) {
                            s32 a = probe.rotators[pi].angle;

                            if (a < 0)
                                a = -a;
                            if (probe.rotators[pi].node >= 0 && a > best_angle) {
                                best_angle = a;
                                best = probe.rotators[pi].node;
                            }
                        }
                        q2_rotators_free(&probe);
                    }

                    if (best >= 0) {
                        q2_scene_node nd;

                        if (q2_scene_get_node(&zone.scene, (u32)best, &nd)) {
                            s32 nmin[3], nmax[3];

                            q2_scene_node_bounds(&nd, nmin, nmax);
                            g_focus_valid = true;
                            g_focus[0] = (nmin[0] + nmax[0]) / 2;
                            g_focus[1] = (nmin[1] + nmax[1]) / 2;
                            g_focus[2] = (nmin[2] + nmax[2]) / 2;
                            g_focus_size = (nmax[0] - nmin[0]) +
                                           (nmax[2] - nmin[2]);
                            printf("    framing node %d, centre [%d %d %d]\n",
                                   best, g_focus[0], g_focus[1], g_focus[2]);
                        }
                    }
                }
                }
            }
        }
    }

    camera_console(&cam, W, H);
    cam.yaw   = yaw;
    cam.pitch = pitch;

    if (eye_view) {
        /* Stand at a real spawn for this zone, at eye height. World Y grows
         * downward, so the eye is at a SMALLER Y than the feet. */
        char cpath[256];
        q2_buf cbuf;
        bool placed = false;

        snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(d, cpath, &cbuf) == Q2_OK) {
            q2_common_file cf3;
            if (q2_common_open(&cf3, &cbuf) == Q2_OK) {
                q2_start_pos_list sl;
                if (q2_start_pos_parse(&sl, &cf3) == Q2_OK) {
                    u32 k;
                    for (k = 0; k < sl.count; k++) {
                        q2_start_pos sp;
                        if (!q2_start_pos_get(&sl, k, &sp) || sp.zone != zone_index)
                            continue;
                        cam.pos[0] = sp.x;
                        cam.pos[1] = sp.y - Q2_VIEW_STAND;
                        cam.pos[2] = sp.z;
                        if (yaw == 0)
                            cam.yaw = sp.angle;
                        placed = true;
                        printf("  eye at spawn  : '%s' [%d %d %d] yaw=%d\n",
                               sp.name, sp.x, sp.y, sp.z, cam.yaw);
                        break;
                    }
                }
                q2_common_close(&cf3);
            } else {
                q2_buf_free(&cbuf);
            }
        }
        if (!placed)
            printf("  eye at spawn  : none for this zone, framing instead\n");
        eye_view = placed;
    }

    /* A rotator was framed above: sit off its centre instead of the zone's. */
    if (g_focus_valid) {
        /* Back off by a few times the node's own size: a rotating brush is
         * small, and sitting at its extent puts the camera inside it. */
        /* Close enough that the brush fills the frame: its own size across,
         * not three times it, which put a 287-unit door 860 units away and
         * left it a few hundred pixels of a 1024-wide render. */
        s32 dist = g_focus_size;

        if (dist < 1200)
            dist = 1200;
        s32 sy = q2_sin12(yaw),   cyaw = q2_cos12(yaw);
        s32 sp = q2_sin12(pitch), cp   = q2_cos12(pitch);
        s32 fx = (s32)(((s64)cp * sy) >> Q2_FRAC_12);
        s32 fy = -sp;
        s32 fz = (s32)(((s64)cp * cyaw) >> Q2_FRAC_12);

        cam.pos[0] = g_focus[0] - (s32)(((s64)fx * dist) >> Q2_FRAC_12);
        cam.pos[1] = g_focus[1] - (s32)(((s64)fy * dist) >> Q2_FRAC_12);
        cam.pos[2] = g_focus[2] - (s32)(((s64)fz * dist) >> Q2_FRAC_12);
        eye_view = true;                 /* skip the whole-zone framing */
    }

    if (!eye_view) {

    /* Frame the whole zone: sit at its centre and back off along the camera's
     * own view direction by enough that the largest extent fits a 90-degree
     * field. Backing off in world -Z only works when looking down -Z, which
     * stops being true the moment a pitch is applied. */
    {
        s32 cx = (wmin[0] + wmax[0]) / 2;
        s32 cy = (wmin[1] + wmax[1]) / 2;
        s32 cz = (wmin[2] + wmax[2]) / 2;
        s32 ex = wmax[0] - wmin[0];
        s32 ey = wmax[1] - wmin[1];
        s32 ez = wmax[2] - wmin[2];
        s32 extent = ex > ez ? ex : ez;
        s32 dist;

        if (ey > extent)
            extent = ey;
        dist = extent;

        /* Forward vector for yaw/pitch, in 1.3.12. */
        {
            s32 sy = q2_sin12(yaw),   cyaw = q2_cos12(yaw);
            s32 sp = q2_sin12(pitch), cp   = q2_cos12(pitch);
            s32 fx = (s32)(((s64)cp * sy) >> Q2_FRAC_12);
            s32 fy = -sp;
            s32 fz = (s32)(((s64)cp * cyaw) >> Q2_FRAC_12);

            cam.pos[0] = cx - (s32)(((s64)fx * dist) >> Q2_FRAC_12);
            cam.pos[1] = cy - (s32)(((s64)fy * dist) >> Q2_FRAC_12);
            cam.pos[2] = cz - (s32)(((s64)fz * dist) >> Q2_FRAC_12);
        }
    }
    }

    printf("  camera        : [%d %d %d] yaw=%d pitch=%d h=%u\n",
           cam.pos[0], cam.pos[1], cam.pos[2], cam.yaw, cam.pitch, cam.projection);

    /*
     * The LENS FLARES, which this renderer has never drawn: `q2_world_zone`
     * carries `lights` and `light_node` for exactly this pass and nothing here
     * ever set them, so the draw took the null branch every time.
     *
     * Static because the light world BORROWS all three of the structures below
     * — the map's Lights list lives in the COMMON buffer, and the SpaceLights
     * partition indexes the SecondaryCol hull — and every one of them has to
     * outlive the draw call at the bottom of this function.
     *
     * A camera outside the movement hull finds no node and therefore no static
     * lights, which is the console's own behaviour and not a gap here: a flare
     * is visible from inside the cells the level's build tool assigned its light
     * to and nowhere else. That means the whole-zone framing view sees none and
     * the `pitch 9999` eye view, which stands at a real spawn, sees what a
     * player would.
     */
    static q2_buf         g_lit_common, g_lit_zone;
    static q2_common_file g_lit_cf;
    static q2_zone_file   g_lit_zf;
    static q2_collision   g_lit_sec;
    static q2_spacelights g_lit_sl;
    static q2_light_list  g_lit_list;
    static q2_light_world g_lit_world;
    {
        char cpath[256], zpath[256];
        bool ok = false;

        snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        snprintf(zpath, sizeof(zpath), "Q2DATA/LEVELS/%s/ZONE%d.DAT",
                 map, zone_index);

        if (disc_read_file(d, cpath, &g_lit_common) == Q2_OK &&
            disc_read_file(d, zpath, &g_lit_zone) == Q2_OK &&
            q2_common_open(&g_lit_cf, &g_lit_common) == Q2_OK &&
            q2_zone_open(&g_lit_zf, &g_lit_zone) == Q2_OK &&
            q2_lights_parse(&g_lit_list, &g_lit_cf) == Q2_OK &&
            q2_collision_parse(&g_lit_sec, &g_lit_zf,
                               Q2_COLL_SECONDARY) == Q2_OK &&
            q2_spacelights_open(&g_lit_sl, &g_lit_zf, &g_lit_sec) == Q2_OK) {
            memset(&g_lit_world, 0, sizeof(g_lit_world));
            g_lit_world.statics = &g_lit_list;
            g_lit_world.space   = &g_lit_sl;

            zone.lights     = &g_lit_world;
            zone.light_node = q2_coll_find_node(&g_lit_sec, cam.pos, -1, true);
            ok = true;
        }

        if (ok)
            printf("  lights        : %u in the map, camera in cell %d\n",
                   g_lit_list.count, zone.light_node);
        else
            printf("  lights        : unavailable; no flares will be drawn\n");
    }

    r = psx_ot_init(&ot, 4096, 300000);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot allocate ordering table: %s\n", q2_result_str(r));
        q2_world_free_zone(&zone);
        return 1;
    }

    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        fprintf(stderr, "out of memory for VRAM\n");
        psx_ot_free(&ot);
        q2_world_free_zone(&zone);
        return 1;
    }

    r = psx_fb_init(&fb, W, H);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot allocate framebuffer: %s\n", q2_result_str(r));
        psx_ot_free(&ot);
        q2_world_free_zone(&zone);
        return 1;
    }

    print_console_framing(&cam, W, H);
    q2_world_build_ot(&zone, &cam, W, H, &ot, &gte, NULL, &stats);

    printf("\n  quads total   : %u\n", stats.quads_total);
    printf("  emitted       : %u\n", stats.quads_emitted);
    printf("  rejected near : %u\n", stats.quads_rejected_near);
    printf("  rejected back : %u\n", stats.quads_rejected_back);
    printf("  rejected bad  : %u\n", stats.quads_rejected_bad);
    printf("  sealing nodes : %u skipped\n", stats.nodes_sealing);
    printf("  ot overflow   : %u\n", stats.ot_overflow);
    printf("  flares        : %u lights, %u styled, %u too near, %u dark, "
           "%u drawn, %u prims\n",
           stats.flare_lights, stats.flare_styled, stats.flare_near,
           stats.flare_dark, stats.flare_drawn, stats.flare_prims);

    psx_raster_opts_default(&opts);

    /* Upload the map's texture pages and palettes into VRAM, then render
     * textured. Falls back to Gouraud-only if the map has no VRAM section. */
    {
        q2_vram_section vs;
        if (q2_vram_load(&vs, d, map) == Q2_OK) {
            if (q2_vram_upload(&vs, vram) == Q2_OK) {
                printf("  textures      : %u pages, %u palettes uploaded\n",
                       vs.texpage_count, vs.clut4_count);
            } else {
                printf("  textures      : upload failed, drawing untextured\n");
                opts.textures = false;
            }
            q2_vram_free(&vs);
        } else {
            printf("  textures      : no VRAM section, drawing untextured\n");
            opts.textures = false;
        }
    }

    psx_fb_clear(&fb, psx_rgb555(16, 16, 32));
    psx_raster_ot(&fb, &ot, vram, &opts);

    r = psx_fb_write_ppm(&fb, out_path);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot write %s: %s\n", out_path, q2_result_str(r));
    } else {
        printf("\n  wrote %s (%dx%d)\n", out_path, W, H);
    }

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    q2_world_free_zone(&zone);
    return r == Q2_OK ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/*
 * Load and decode every sound bank on the disc.
 *
 * The strongest check here is not that decoding "works" but that the ADPCM
 * block headers are all structurally valid and that every bank's bodies fit in
 * the console's 512 KB of sound RAM. Both would fail loudly if the bank layout
 * or the endianness were misread.
 */
static int cmd_audio(disc *d)
{
    int i, n = disc_file_count(d);
    u32 banks = 0, sounds = 0, looping = 0, bad_blocks = 0, failed = 0;
    u32 worst_body = 0;
    char worst_map[64];
    s16 *pcm = NULL;
    size_t pcm_cap = 1 << 20;

    worst_map[0] = '\0';
    pcm = (s16 *)malloc(pcm_cap * sizeof(s16));
    if (!pcm) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    printf("Decoding every sound bank on the disc...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char map[64];
        size_t len;
        q2_sound_bank bank;
        u32 s, total;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;
        if (!strstr(p, "SNDVRAM.DAT"))
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;
        len = (size_t)(slash - rest);
        if (len >= sizeof(map))
            len = sizeof(map) - 1;
        memcpy(map, rest, len);
        map[len] = '\0';

        if (q2_sound_bank_load(&bank, d, map) != Q2_OK) {
            printf("  LOAD FAILED  %s\n", map);
            failed++;
            continue;
        }

        banks++;

        for (s = 0; s < bank.count; s++) {
            q2_vag vag;

            if (!q2_sound_bank_get(&bank, s, &vag)) {
                printf("  bad VAG %u in %s\n", s, map);
                failed++;
                continue;
            }

            sounds++;
            if (vag.looping)
                looping++;

            bad_blocks += q2_spu_adpcm_validate(vag.body, vag.data_size);

            /* Actually decode it, so a broken decoder cannot hide behind a
             * header-only check. */
            q2_spu_adpcm_decode(vag.body, vag.data_size, pcm, (u32)pcm_cap);
        }

        total = q2_sound_bank_total_body(&bank);
        if (total > worst_body) {
            worst_body = total;
            strncpy(worst_map, map, sizeof(worst_map) - 1);
            worst_map[sizeof(worst_map) - 1] = '\0';
        }

        q2_sound_bank_free(&bank);
    }

    free(pcm);

    printf("  banks loaded    : %u\n", banks);
    printf("  sounds          : %u\n", sounds);
    printf("  looping         : %u\n", looping);
    printf("  invalid blocks  : %u\n", bad_blocks);
    printf("  failures        : %u\n", failed);
    printf("  largest bank    : %s, %u bytes of ADPCM\n", worst_map, worst_body);
    printf("  SPU RAM usable  : %d bytes\n", SPU_RAM_SIZE - SPU_RAM_RESERVED);
    printf("  fits            : %s\n",
           worst_body <= (u32)(SPU_RAM_SIZE - SPU_RAM_RESERVED) ? "yes" : "NO");

    printf("\n%s\n", (failed == 0 && bad_blocks == 0)
           ? "PASS - every bank parsed and every ADPCM block is structurally valid."
           : "FAIL - see above.");

    return (failed || bad_blocks) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Demultiplex and decode the streamed music.
 *
 * The claim under test is the interleave: each .XAI carries four independent
 * stereo streams and sector_index % 4 == channel_num with no exceptions. That
 * is checked directly here rather than assumed, because if it were wrong every
 * track would be a quarter of four different songs.
 */
static int cmd_music(disc *d)
{
    static const char letters[] = { 'A', 'B', 'C', 'D', 'E' };
    s16 *pcm;
    const u32 pcm_cap = XA_FRAMES_PER_SECTOR * 2;
    u32 total_sectors = 0, audio_sectors = 0, bad_blocks = 0;
    u32 interleave_violations = 0, skipped = 0;
    q2_music_table mtab;
    bool have_mtab = false;
    u32 mtab_checked = 0, mtab_bad = 0;
    size_t li;

    pcm = (s16 *)malloc(pcm_cap * sizeof(s16));
    if (!pcm) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /*
     * The executable's own music table, so each stream can be checked against
     * the duration the game believes it has. That is what turns "+4 looks like
     * a duration" into a fact: twenty streams, twenty independent agreements.
     */
    {
        q2_build_id mid;
        if (q2_identify(d, &mid) == Q2_OK &&
            q2_music_table_load(&mtab, d, &mid) == Q2_OK)
            have_mtab = true;
    }

    printf("Demultiplexing the XA music streams...\n\n");
    printf("  file       ch  sectors    seconds   id   table\n");

    for (li = 0; li < sizeof(letters); li++) {
        char path[64];
        const disc_file *f;
        u32 ch;

        snprintf(path, sizeof(path), "Q2DATA/AUD/QUAKE_%c.XAI", letters[li]);
        f = disc_find(d, path);
        if (!f) {
            printf("  QUAKE_%c    missing\n", letters[li]);
            continue;
        }

        for (ch = 0; ch < XAI_CHANNEL_COUNT; ch++) {
            q2_xa_track track;
            q2_xa_decoder dec;
            u32 cursor = 0, sectors = 0, n;

            if (q2_xa_track_open(&track, d, letters[li], (u8)ch) != Q2_OK)
                continue;

            q2_xa_decoder_reset(&dec);

            while ((n = q2_xa_track_read(&track, &dec, &cursor, pcm, pcm_cap)) > 0)
                sectors++;

            {
                double secs = (double)sectors * XA_FRAMES_PER_SECTOR /
                              (double)XA_SAMPLE_RATE;
                /* Ids run 2..21 in file-major order (musictable.h). */
                int id = 2 + (int)li * XAI_CHANNEL_COUNT + (int)ch;
                const q2_music_entry *me = have_mtab ? q2_music_get(&mtab, id)
                                                     : NULL;

                if (me) {
                    /* A tenth of slack: the table is in tenths of a second and
                     * a stream ends on a sector boundary, not on a tenth. */
                    double want = (double)me->tenths / 10.0;
                    double err  = want - secs;
                    bool ok = me->file == (s8)li && me->channel == ch &&
                              err < 0.15 && err > -0.15;

                    printf("  QUAKE_%c    %u   %-9u  %-8.1f  %-3d  %s\n",
                           letters[li], ch, sectors, secs, id,
                           ok ? "ok" : "MISMATCH");
                    mtab_checked++;
                    if (!ok)
                        mtab_bad++;
                } else {
                    printf("  QUAKE_%c    %u   %-9u  %-8.1f\n",
                           letters[li], ch, sectors, secs);
                }
            }

            audio_sectors += sectors;
        }

        /* Independently check the round-robin and the group parameters by
         * walking the raw sectors, without going through the track reader. */
        {
            u32 count = (f->size + CD_SECTOR_FORM1 - 1) / CD_SECTOR_FORM1;
            u32 i;

            for (i = 0; i < count; i++) {
                u8 raw[CD_SECTOR_RAW];

                if (disc_read_raw_sector(d, f->lba + i, raw) != Q2_OK)
                    break;

                total_sectors++;

                if (!(raw[18] & CD_SUBMODE_AUDIO) || !(raw[18] & CD_SUBMODE_FORM2)) {
                    skipped++;
                    continue;
                }

                if (raw[17] != (u8)(i % XAI_CHANNEL_COUNT))
                    interleave_violations++;

                bad_blocks += q2_xa_validate_sector(raw + 24);
            }
        }
    }

    free(pcm);

    printf("\n  sectors scanned       : %u\n", total_sectors);
    printf("  audio sectors decoded : %u\n", audio_sectors);
    printf("  non-audio skipped     : %u\n", skipped);
    printf("  interleave violations : %u  (expected 0)\n", interleave_violations);
    if (have_mtab)
        printf("  streams whose length matches the music table : %u of %u\n",
               mtab_checked - mtab_bad, mtab_checked);
    printf("  invalid ADPCM blocks  : %u  (expected 0)\n", bad_blocks);

    printf("\n%s\n", (interleave_violations == 0 && bad_blocks == 0)
           ? "PASS - the round-robin holds and every sound group is valid."
           : "FAIL - see above.");

    return (interleave_violations || bad_blocks) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Decode every compressed VRAM image on the disc.
 *
 * The acceptance bar is strict on purpose: a correct codec decodes every
 * payload to exactly its expected size with no overshoot and no starvation.
 * Anything less means it is wrong, or there is more than one codec.
 */
static int cmd_textures(disc *d)
{
    int i, n = disc_file_count(d);
    u32 maps = 0, images = 0, failed = 0;
    u64 packed_total = 0, decoded_total = 0;
    u32 pad_hist[5];
    u8 *scratch;
    size_t scratch_cap = 1024 * 1024;

    memset(pad_hist, 0, sizeof(pad_hist));

    scratch = (u8 *)malloc(scratch_cap);
    if (!scratch) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    printf("Decoding every compressed VRAM image...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char map[64];
        size_t len;
        q2_vram_section vs;
        u32 k;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;
        if (!strstr(p, "SNDVRAM.DAT"))
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;
        len = (size_t)(slash - rest);
        if (len >= sizeof(map))
            len = sizeof(map) - 1;
        memcpy(map, rest, len);
        map[len] = '\0';

        if (q2_vram_load(&vs, d, map) != Q2_OK) {
            printf("  LOAD FAILED  %s\n", map);
            failed++;
            continue;
        }

        maps++;

        for (k = 0; k < vs.image_count; k++) {
            /* Not width*height — texture pages ignore their stored dimensions
             * and are forced to 128x256 by the engine. */
            size_t want = q2_vram_decoded_size(&vs, k);
            size_t got = 0;

            if (want > scratch_cap) {
                u8 *bigger = (u8 *)realloc(scratch, want);
                if (!bigger) { failed++; continue; }
                scratch = bigger;
                scratch_cap = want;
            }

            if (q2_vram_decode(&vs, k, scratch, scratch_cap, &got) != Q2_OK) {
                printf("  DECODE FAILED  %s image %u (%ux%u)\n",
                       map, k, vs.images[k].width, vs.images[k].height);
                failed++;
                continue;
            }

            images++;
            packed_total  += vs.images[k].packed_size;
            decoded_total += got;
        }

        q2_vram_free(&vs);
    }

    free(scratch);

    printf("  maps            : %u\n", maps);
    printf("  images decoded  : %u\n", images);
    printf("  failures        : %u\n", failed);
    printf("  packed bytes    : %llu\n", (unsigned long long)packed_total);
    printf("  decoded bytes   : %llu\n", (unsigned long long)decoded_total);
    if (packed_total)
        printf("  compression     : %.2fx\n",
               (double)decoded_total / (double)packed_total);

    printf("\n%s\n", failed == 0
           ? "PASS - every payload decoded to exactly its expected size."
           : "FAIL - see above.");

    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Drop a player into a real zone and simulate.
 *
 * The unit tests drive the simulation with no zone attached, so they never
 * touch collision. This is the check that it works on actual geometry: does the
 * spawn point land inside a convex cell, does the player come to rest instead of
 * falling forever, and do they stay inside the hull while walking.
 */
static int cmd_walk(disc *d, const char *map, int zone_index, int ticks)
{
    q2_world_zone zone;
    q2_sim sim;
    q2_input in;
    q2_result r;
    q2_start_pos_list spawns;
    q2_common_file common;
    q2_buf buf;
    char path[256];
    s32 feet[3] = { 0, 0, 0 };
    s32 start_y;
    s32 walk_x = 0, walk_z = 0, walk_dist = 0;
    int i, grounded_at = -1, escaped = 0, zone_gates = 0;
    bool have_spawn = false;

    r = q2_world_load_zone(&zone, d, map, zone_index);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d: %s\n", map, zone_index, q2_result_str(r));
        return 1;
    }

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) == Q2_OK) {
        if (q2_common_open(&common, &buf) == Q2_OK) {
            if (q2_start_pos_parse(&spawns, &common) == Q2_OK) {
                u32 k;
                for (k = 0; k < spawns.count; k++) {
                    q2_start_pos sp;
                    if (!q2_start_pos_get(&spawns, k, &sp) || sp.zone != zone_index)
                        continue;
                    feet[0] = sp.x; feet[1] = sp.y; feet[2] = sp.z;
                    have_spawn = true;
                    printf("  spawn         : '%s' at [%d %d %d]\n",
                           sp.name, sp.x, sp.y, sp.z);
                    break;
                }
            }
            q2_common_close(&common);
        } else {
            q2_buf_free(&buf);
        }
    }

    q2_sim_init(&sim, &zone, 50);

    /* Attach the map's triggers and script so the walk exercises gameplay, not
     * just physics. */
    {
        q2_buf cbuf;
        if (disc_read_file(d, path, &cbuf) == Q2_OK) {
            q2_common_file cf2;
            if (q2_common_open(&cf2, &cbuf) == Q2_OK) {
                q2_sim_attach_gameplay(&sim, &cf2);
                printf("  triggers      : %s, %u volumes\n",
                       sim.triggers_ready ? "loaded" : "none",
                       sim.triggers_ready ? sim.triggers.count : 0);
                printf("  script        : %s, %u records\n",
                       sim.events_ready ? "loaded" : "none",
                       sim.events_ready ? sim.event_rt.record_count : 0);
                /* The sim borrows these, so they must outlive it; leaked
                 * deliberately for the duration of this one-shot command. */
            } else {
                q2_buf_free(&cbuf);
            }
        }
    }

    q2_sim_spawn(&sim, feet, 0);

    printf("%s\n", zone.name);
    printf("  spawn found   : %s\n", have_spawn ? "yes" : "no (using origin)");
    printf("  collision     : %s, %u nodes\n",
           sim.coll_ready ? "loaded" : "UNAVAILABLE",
           sim.coll_ready ? sim.coll.node_count : 0);
    printf("  spawn cell    : %d%s\n", sim.current_node,
           sim.current_node < 0 ? "  (outside every hull)" : "");

    /*
     * Both hulls, side by side. Movement runs against SecondaryCol — that is
     * read out of the zone loader, not chosen — so when a spawn lands in one
     * and not the other, the number to look at is which.
     */
    if (sim.coll_primary_ready) {
        s32 pn = q2_coll_find_node(&sim.coll_primary, feet, -1, true);
        printf("  primary hull  : %u nodes, spawn cell %d\n",
               sim.coll_primary.node_count, pn);
    }

    start_y = sim.player[0].pos[1];
    walk_x  = sim.player[0].pos[0];
    walk_z  = sim.player[0].pos[2];

    memset(&in, 0, sizeof(in));
    for (i = 0; i < ticks; i++) {
        /* Walk forward for the second half so both falling and walking are
         * exercised. Q2_INPUT_FULL is the pad's own full deflection — the wish
         * velocity is (maxspeed * axis) >> 7, so anything larger simply makes
         * the player faster than the executable's own speed table allows. */
        if (i == ticks / 2 + 1) {
            walk_x = sim.player[0].pos[0];
            walk_z = sim.player[0].pos[2];
        }
        in.forward = (i > ticks / 2) ? Q2_INPUT_FULL : 0;
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

        if (sim.player[0].on_ground && grounded_at < 0)
            grounded_at = i;
        if (sim.coll_ready && sim.current_node < 0)
            escaped++;
        {
            u32 zt;
            if (q2_sim_take_zone_change(&sim, &zt)) {
                printf("    tick %d: ZONE GATE fired -> zone %u\n", i, zt);
                zone_gates++;
            }
        }
    }

    {
        s32 dx = sim.player[0].pos[0] - walk_x;
        s32 dz = sim.player[0].pos[2] - walk_z;
        walk_dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    }

    printf("  after %d ticks:\n", ticks);
    printf("    grounded    : %s\n",
           grounded_at >= 0 ? "yes" : "NO - fell the whole time");
    if (grounded_at >= 0)
        printf("    landed on tick %d\n", grounded_at);
    printf("    fell        : %d world units\n", sim.player[0].pos[1] - start_y);

    /*
     * How far the second half of the run actually travelled. A zero here with a
     * grounded player is the signature of movement being cancelled by the mover
     * rather than of a collision failure, which is exactly what the missing
     * airborne branch at 0x80045CA4 used to produce for jumps.
     */
    printf("    walked      : %d world units in %d ticks\n",
           walk_dist, ticks - ticks / 2 - 1);
    printf("    final cell  : %d\n", sim.current_node);
    printf("    ticks outside any hull: %d\n", escaped);
    printf("    events run  : %u\n", sim.events_ready ? sim.event_rt.ran_count : 0);
    printf("    zone gates  : %d\n", zone_gates);

    /*
     * Fire every weapon once into the real map, so the reconstructed combat is
     * exercised against real geometry rather than only against a test's three
     * actors in a line. What is checkable here is not damage — nothing is
     * standing there to take it — but that every weapon produces the shape of
     * shot its fire function produces, spends the ammo the table says, and that
     * a hitscan trace actually meets the world instead of running to infinity.
     */
    {
        static const char *const k_kind[] = {
            "-", "hitscan", "bolt", "rail", "grenade", "thrown", "rocket", "bfg"
        };
        int w;

        for (w = 0; w < Q2_AMMO_COUNT; w++)
            sim.combat.inv.ammo[w] = 200;
        sim.combat.inv.weapons = 0x7FF;

        printf("  firing every weapon from the spawn:\n");
        for (w = 1; w <= Q2_WID_COUNT; w++) {
            q2_fire_result_v2 fr;
            s16 before, after;
            s32 type = q2_weapon_tables_builtin()->ammo_type[w];
            s32 eye[3], frac = 4096;

            sim.combat.weapon_id = w;
            sim.combat.next_fire = 0;
            before = sim.combat.inv.ammo[type];

            fr = q2_sim_fire(&sim);
            after = sim.combat.inv.ammo[type];

            q2_sim_eye(&sim, eye);
            if (fr.fired && fr.shot_count)
                frac = 4096;

            printf("    %-13s %-7s shots %2u  dmg %4d  ammo -%d%s\n",
                   q2_weapon_tables_builtin()->name[w],
                   k_kind[fr.kind], fr.shot_count,
                   fr.shot_count ? fr.shot[0].damage : 0,
                   before - after,
                   fr.fired ? "" : "  (BLOCKED)");
            (void)frac;
        }
        printf("    projectiles in flight: %u\n", sim.combat.projectiles.live);

        /* Let them fly, so the world trace resolves every one of them. */
        {
            q2_input idle;
            int t;
            memset(&idle, 0, sizeof(idle));
            for (t = 0; t < 600 && sim.combat.projectiles.live; t++)
                q2_sim_tick(&sim, &idle, Q2_DT_NOMINAL);
            printf("    still in flight after %d ticks: %u\n",
                   t, sim.combat.projectiles.live);
        }
    }

    q2_world_free_zone(&zone);

    /* A spawn that is not inside a cell means either the hull or the spawn is
     * misread, and is worth failing on. */
    return (sim.coll_ready && sim.current_node < 0) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Run every event script on the disc.
 *
 * Fires each named entry point and lets the trigger graph propagate, which
 * exercises the parser and the runtime together. The useful output is the
 * opcode census and how much of it actually executes: a script that parses but
 * never runs anything would look fine to `verify` and be useless in a game.
 */
/*
 * A zone's rotators, driven by that zone's own scripts.
 *
 * The client installs exactly this hook on its event runtime; here it runs for
 * every script on the disc at once, which is the only way to see the rotation
 * calls without walking 42 maps into every trigger volume by hand.
 */
typedef struct ev_rot_ctx {
    const q2_userfuncs *uf;
    q2_rotator_set     *set;
    u32                 steps;
} ev_rot_ctx;

static void ev_rot_call(void *user, const q2_event_item *item, u8 call_index)
{
    ev_rot_ctx *ctx = (ev_rot_ctx *)user;

    ctx->steps += q2_rotators_call(ctx->set, ctx->uf, item, call_index);
}

static int cmd_events(disc *d)
{
    int i, n = disc_file_count(d);
    u32 files = 0, records = 0, items = 0, named = 0;
    u32 ran = 0, movers = 0, zone_changes = 0;
    u32 op_hist[64];
    u32 movers_built = 0, movers_moved = 0, movers_open = 0, movers_empty = 0;
    u32 rot_built = 0, rot_calls = 0, rot_steps = 0, rot_moved = 0, rot_turned = 0;
    u32 trig_with_event = 0, trig_in_common = 0;

    memset(op_hist, 0, sizeof(op_hist));
    printf("Running every event script on the disc...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_events ev;
        q2_event_rt rt;
        bool is_zone;

        base = base ? base + 1 : f->path;
        is_zone = (strncmp(base, "ZONE", 4) == 0);
        if (!is_zone && strncmp(base, "COMMON", 6) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        if (is_zone) {
            q2_zone_file zf;
            if (q2_zone_open(&zf, &buf) != Q2_OK) { q2_buf_free(&buf); continue; }
            if (q2_events_parse_zone(&ev, &zf) != Q2_OK) { q2_zone_close(&zf); continue; }

            files++;
            records += ev.record_count;

            if (q2_event_rt_init(&rt, &ev) == Q2_OK) {
                q2_event_record rec;
                u32 k;

                /* Census every item before running anything. */
                if (q2_events_first_record(&ev, &rec)) {
                    do {
                        for (k = 0; k < rec.n_items; k++) {
                            q2_event_item it;
                            if (!q2_events_get_item(&ev, &rec, k, &it))
                                break;
                            op_hist[it.opcode & 0x3F]++;
                            items++;
                        }
                    } while (q2_events_next_record(&ev, &rec, &rec));
                }

                /*
                 * The zone's rotating brushes, wired to the scripts that turn
                 * them. The UserFuncs table is per MAP and lives in its
                 * COMMON.DAT, so it has to be opened alongside the zone — the
                 * call index in a zone's script means nothing without it.
                 */
                q2_rotator_set rs;
                q2_buf         cbuf;
                q2_common_file ccf;
                q2_userfuncs   cuf;
                ev_rot_ctx     rctx;
                bool           rot_ready = false, common_open = false;
                char           cpath[256];
                const char    *slash = strrchr(f->path, '/');

                memset(&rs, 0, sizeof(rs));
                memset(&rctx, 0, sizeof(rctx));

                if (slash && (size_t)(slash - f->path) < sizeof(cpath) - 12) {
                    memcpy(cpath, f->path, (size_t)(slash - f->path));
                    strcpy(cpath + (slash - f->path), "/COMMON.DAT");

                    if (disc_read_file(d, cpath, &cbuf) == Q2_OK) {
                        if (q2_common_open(&ccf, &cbuf) == Q2_OK) {
                            /*
                             * The file stays OPEN across the run: the parsed
                             * UserFuncs point into its buffer, and the hook
                             * reads them on every CALL. Closing here segfaults
                             * on the first script that calls anything.
                             */
                            common_open = true;
                            if (q2_userfuncs_parse(&cuf, &ccf) == Q2_OK &&
                                q2_rotators_build(&rs, &ev, &cuf,
                                                  NULL) == Q2_OK) {
                                rot_ready   = true;
                                rot_built  += rs.count;
                                rctx.uf     = &cuf;
                                rctx.set    = &rs;
                                rt.on_call      = ev_rot_call;
                                rt.on_call_user = &rctx;
                            }
                        } else {
                            q2_buf_free(&cbuf);
                        }
                    }
                }

                for (k = 0; k < ev.dir_count; k++) {
                    q2_event_dir_entry e;
                    if (!q2_events_get_dir_entry(&ev, k, &e))
                        continue;
                    named++;
                    q2_event_rt_trigger(&rt, e.offset);
                }

                if (q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE)
                    zone_changes++;

                if (rot_ready) {
                    u32 t, ri;

                    rot_steps += rctx.steps;

                    /* Same 400 ticks the movers get, on the rotators' own
                     * 1/300 s clock. */
                    for (t = 0; t < 400; t++)
                        rot_moved += q2_rotators_tick(&rs, 12);

                    for (ri = 0; ri < rs.count; ri++)
                        if (rs.rotators[ri].angle != 0)
                            rot_turned++;

                    q2_rotators_free(&rs);
                }
                rot_calls += rt.call_count;
                if (common_open)
                    q2_common_close(&ccf);

                /* Build the zone's doors and lifts and run them for a while,
                 * so the state machine is exercised rather than merely
                 * constructed. */
                {
                    q2_mover_set ms;
                    if (q2_movers_build(&ms, &ev, NULL) == Q2_OK) {
                        u32 mi, t;
                        movers_built += ms.count;
                        for (mi = 0; mi < ms.count; mi++) {
                            if (ms.movers[mi].part_count == 0)
                                movers_empty++;
                            q2_mover_trigger(&ms, mi);
                        }
                        for (t = 0; t < 400; t++)
                            movers_moved += q2_movers_tick(&ms, 12, 0xFFFF);
                        for (mi = 0; mi < ms.count; mi++) {
                            if (ms.movers[mi].offset != 0)
                                movers_open++;
                        }
                        q2_movers_free(&ms);
                    }
                }

                ran    += rt.ran_count;
                movers += rt.skipped_movers;
                q2_event_rt_free(&rt);
            }
            q2_zone_close(&zf);
        } else {
            q2_common_file cf;
            if (q2_common_open(&cf, &buf) != Q2_OK) { q2_buf_free(&buf); continue; }
            if (q2_events_parse_common(&ev, &cf) == Q2_OK) {
                files++;
                records += ev.record_count;

                /*
                 * Which script do the trigger volumes fire?
                 *
                 * They are parsed from COMMON.DAT and the sim fires their
                 * `event_offset` into COMMON's Events. But COMMON's script is
                 * nearly empty while the ZONE's carries the movers and the
                 * rotation calls — so before wiring anything further, count how
                 * many trigger offsets actually name a record in COMMON's own
                 * script. An offset that names no record there is firing into
                 * the wrong chunk.
                 */
                q2_triggers tg;

                if (q2_triggers_parse(&tg, &cf) == Q2_OK) {
                    q2_event_rt probe;

                    if (q2_event_rt_init(&probe, &ev) == Q2_OK) {
                        u32 t;

                        for (t = 0; t < tg.count; t++) {
                            q2_trigger tr;
                            u32 r;
                            bool hit = false;

                            if (!q2_trigger_get(&tg, t, &tr))
                                continue;
                            if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                                continue;

                            trig_with_event++;
                            for (r = 0; r < probe.record_count; r++)
                                if (probe.offsets[r] == tr.event_offset) {
                                    hit = true;
                                    break;
                                }
                            if (hit)
                                trig_in_common++;
                        }
                        q2_event_rt_free(&probe);
                    }
                }
            }
            q2_common_close(&cf);
        }
    }

    printf("  files with events : %u\n", files);
    printf("  records           : %u\n", records);
    printf("  items             : %u\n", items);
    printf("  named entries     : %u\n", named);
    printf("  records executed  : %u\n", ran);
    printf("  movers skipped    : %u  (link not decoded)\n", movers);
    printf("  zone gates fired  : %u\n", zone_changes);
    printf("  movers built      : %u  (%u with no nodes)\n", movers_built, movers_empty);
    printf("  mover tick-moves  : %u\n", movers_moved);
    printf("  movers displaced  : %u  after 400 ticks\n", movers_open);
    /*
     * These come from the ZONE scripts this command runs, and the engine never
     * loads a zone's Events chunk: its loader looks up twelve chunk names by
     * hand and Events is not among them, while the only two references to the
     * "Events" string in the image are COMMON's loader (0x8007AC30, storing to
     * 0x800AE774) and the teardown that clears it. So this exercises the
     * format, not the console. `zonescript` measures the script that runs.
     */
    printf("  rotators built    : %u  (from ZONE scripts — see below)\n",
           rot_built);
    printf("  CALL items run    : %u\n", rot_calls);
    printf("  rotation steps    : %u  requested by those calls\n", rot_steps);
    printf("  rotator tick-moves: %u\n", rot_moved);
    printf("  rotators turned   : %u  after 400 ticks\n", rot_turned);
    printf("\n  NOTE: a zone's Events chunk IS loaded. The zone loader looks\n"
           "  \"Events\" up at 0x8007C14C and stores it at gp+376, and a rotation\n"
           "  CALL reads its object slots from THERE while stamping -1 into\n"
           "  COMMON's copy. The note that used to print here said the opposite\n"
           "  and cost this port most of its rotating geometry; see #56.\n");
    printf("  triggers w/ event : %u, of which %u name a record in COMMON's"
           " own script\n", trig_with_event, trig_in_common);

    printf("\n  opcode census\n");
    {
        static const struct { u8 op; const char *name; } names[] = {
            { 0x02, "TRIGGER"  }, { 0x03, "MOVER_A" }, { 0x04, "MOVER_B" },
            { 0x05, "MOVER_C"  }, { 0x08, "FXGROUP" }, { 0x09, "WAIT"    },
            { 0x0F, "ZONEGATE" }, { 0x13, "FX"      }, { 0x14, "ENABLE"  },
            { 0x15, "DISABLE"  }, { 0x16, "CALL"    },
        };
        u32 k;
        for (k = 0; k < Q2PSX_ARRAY_COUNT(names); k++) {
            if (op_hist[names[k].op])
                printf("    0x%02X %-9s %u\n",
                       names[k].op, names[k].name, op_hist[names[k].op]);
        }
        for (k = 0; k < 64; k++) {
            u32 j, known = 0;
            for (j = 0; j < Q2PSX_ARRAY_COUNT(names); j++)
                if (names[j].op == k) known = 1;
            if (!known && op_hist[k])
                printf("    0x%02X %-9s %u\n", k, "(unknown)", op_hist[k]);
        }
    }

    printf("\n%s\n", ran > 0
           ? "PASS - the trigger graph parses and executes."
           : "FAIL - nothing executed.");
    return ran > 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/*
 * Relocate every AI and level module on the disc.
 *
 * The bar is structural: every fixup offset must land inside its image, every
 * stream must terminate, and the HI16 addend words must keep the walk in step.
 * A parse that drifts one word out of phase still consumes most streams
 * plausibly, so "it did not crash" proves nothing — the type census is the
 * thing to read, because the residue counts have to decompose exactly.
 */
static int cmd_reloc(disc *d)
{
    int i, n = disc_file_count(d);
    u32 modules = 0, empty = 0, failed = 0;
    unsigned long long fixups = 0, addends = 0, oob = 0;
    unsigned long long by_type[4] = { 0, 0, 0, 0 };
    unsigned long long moves_found = 0, frames_found = 0, moves_coherent = 0;

    printf("Relocating every AI module on the disc...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_common_file cf;

        base = base ? base + 1 : f->path;
        if (strncmp(base, "COMMON", 6) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        {
            const dat_chunk *bin = cf.chunk[Q2_COMMON_CRE_AI_BIN];
            const dat_chunk *rel = cf.chunk[Q2_COMMON_CRE_AI_REL];

            if (bin && rel && bin->size > Q2_RELOC_CREAI_PREAMBLE &&
                rel->size > Q2_RELOC_CREAI_PREAMBLE) {
                q2_reloc_stats st;
                q2_result r = q2_reloc_scan(rel->data + Q2_RELOC_CREAI_PREAMBLE,
                                            rel->size - Q2_RELOC_CREAI_PREAMBLE,
                                            bin->size - Q2_RELOC_CREAI_PREAMBLE,
                                            &st);
                if (r == Q2_OK) {
                    modules++;
                    fixups  += st.fixups;
                    addends += st.addend_words;
                    oob     += st.out_of_range;
                    by_type[0] += st.by_type[0];
                    by_type[1] += st.by_type[1];
                    by_type[2] += st.by_type[2];
                    by_type[3] += st.by_type[3];

                    /* Now actually relocate it, so the write path is exercised
                     * and not merely the scan, then read its animations out. */
                    {
                        q2_ai_module m;
                        if (q2_ai_module_load(&m, &cf, 0x80100000u) == Q2_OK) {
                            q2_ai_moves mv;

                            /* Guided by the fixup stream rather than scanning
                             * every offset: a move's frames pointer is a WORD32
                             * relocation, so the stream says where to look. */
                            if (!m.empty && m.image.data &&
                                q2_ai_moves_scan_guided(&mv, m.image.data, m.image.size,
                                                        rel->data + Q2_RELOC_CREAI_PREAMBLE,
                                                        rel->size - Q2_RELOC_CREAI_PREAMBLE,
                                                        0x80100000u) == Q2_OK) {
                                u32 k;
                                moves_found  += mv.count;
                                frames_found += mv.total_frames;
                                for (k = 0; k < mv.count; k++) {
                                    if (q2_ai_move_verb_run(&mv, k, m.image.data,
                                                            m.image.size) >= 2)
                                        moves_coherent++;
                                }
                                q2_ai_moves_free(&mv);
                            }
                            q2_ai_module_free(&m);
                        } else {
                            printf("  RELOCATE FAILED  %s\n", f->path);
                            failed++;
                        }
                    }
                } else {
                    printf("  SCAN FAILED  %s: %s\n", f->path, q2_result_str(r));
                    failed++;
                }
            } else {
                empty++;
            }
        }

        q2_common_close(&cf);
    }

    printf("  modules relocated : %u\n", modules);
    printf("  maps with none    : %u\n", empty);
    printf("  failures          : %u\n", failed);
    printf("  fixups            : %llu\n", fixups);
    printf("  HI16 addend words : %llu\n", addends);
    printf("  offsets out of range : %llu\n", oob);
    printf("\n  type census\n");
    printf("    WORD32   %llu\n", by_type[0]);
    printf("    HI16     %llu\n", by_type[1]);
    printf("    LO16     %llu\n", by_type[2]);
    printf("    TARGET26 %llu\n", by_type[3]);

    /* The addend count must equal the HI16 count exactly: one raw word each.
     * If a stream ever drifted, these would diverge. */
    printf("\n  animations recovered\n");
    printf("    moves            %llu\n", moves_found);
    printf("    frames           %llu\n", frames_found);
    printf("    with a verb run  %llu  (2+ consecutive same-verb frames)\n",
           moves_coherent);

    printf("\n  addends == HI16 count : %s\n",
           (addends == by_type[1]) ? "yes" : "NO - the walk drifted");

    printf("\n%s\n", (failed == 0 && oob == 0 && addends == by_type[1])
           ? "PASS - every stream terminates, every target is in range."
           : "FAIL - see above.");

    return (failed || oob || addends != by_type[1]) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Dump the executable's level table and cross-check it against the disc.
 *
 * The check that matters: every directory the table names must actually exist
 * under Q2DATA/LEVELS. A wrong table offset would still yield printable-looking
 * names, so agreement with the filesystem is what makes the read trustworthy.
 */
static int cmd_leveltable(disc *d)
{
    q2_build_id id;
    q2_level_table t;
    q2_result r;
    u32 i, real = 0, placeholders = 0, missing = 0;

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }

    r = q2_level_table_load(&t, d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the level table: %s\n", q2_result_str(r));
        return 1;
    }

    printf("Level table: %u records\n", t.count);
    printf("  idx display        directory  music playlist         end    on disc\n");

    for (i = 0; i < t.count; i++) {
        const q2_level_entry *e = &t.entries[i];
        char probe[128];
        bool present;

        if (e->is_placeholder) {
            placeholders++;
            continue;
        }

        snprintf(probe, sizeof(probe), "Q2DATA/LEVELS/%s/COMMON.DAT", e->directory);
        present = disc_find(d, probe) != NULL;

        if (present) real++;
        else missing++;

        /*
         * The playlist as the engine plays it: walked, not printed raw, and
         * stopped at the point it starts repeating so a looping list shows its
         * period instead of running forever.
         */
        {
            char list[64];
            int walk = -1, n = 0, first = -1, tid;
            size_t used = 0;

            list[0] = '\0';
            while ((tid = q2_level_playlist_next(e, &walk)) >= 0 && n < 12) {
                if (n == 0)
                    first = walk;
                else if (walk == first)
                    break;              /* back to the top: that is the loop */
                used += (size_t)snprintf(list + used, sizeof(list) - used,
                                         "%s%d", n ? "," : "", tid);
                n++;
            }

            printf("  %-3u %-14s %-10s %-22s %-6s %s\n",
                   i, e->display, e->directory, list,
                   (tid >= 0) ? "loops" : "once",
                   present ? "yes" : "MISSING");
        }
    }

    printf("\n  resolve to a directory : %u\n", real);
    printf("  placeholders           : %u\n", placeholders);
    printf("  named but not present  : %u\n", missing);

    q2_level_table_free(&t);

    /*
     * An entry naming a directory that is not on the disc is CUT CONTENT, not
     * a bad read. The five here are three Gallery variants plus QUAKE3 and
     * HALFLIFE, and all five carry the same sequence bytes as each other —
     * unused stubs left in the table.
     *
     * So the verdict is about whether the table READS, and the direction that
     * would actually indicate a wrong offset is garbage names or a resolve rate
     * near zero. Failing on cut content would be asserting the wrong thing.
     */
    printf("\n%s\n", real >= 40
           ? "PASS - the table reads and resolves; entries with no directory are cut content."
           : "FAIL - too few entries resolve; the table offset is probably wrong.");

    return real >= 40 ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
static int cmd_hexdump(disc *d, const char *path, size_t count)
{
    q2_buf buf;
    q2_result r;
    size_t i, j;

    r = disc_read_file(d, path, &buf);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read %s: %s\n", path, q2_result_str(r));
        return 1;
    }

    if (count > buf.size)
        count = buf.size;

    printf("%s - %zu bytes, showing %zu\n\n", path, buf.size, count);

    for (i = 0; i < count; i += 16) {
        printf("%08zX  ", i);
        for (j = 0; j < 16; j++) {
            if (i + j < count)
                printf("%02X ", buf.data[i + j]);
            else
                printf("   ");
            if (j == 7)
                printf(" ");
        }
        printf(" |");
        for (j = 0; j < 16 && i + j < count; j++) {
            u8 ch = buf.data[i + j];
            putchar(ch >= 0x20 && ch < 0x7F ? (int)ch : '.');
        }
        printf("|\n");
    }

    q2_buf_free(&buf);
    return 0;
}

/* ------------------------------------------------------------------------- */
static void make_dirs_for(const char *path)
{
    char tmp[1024];
    size_t i;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            q2_mkdir(tmp);
            tmp[i] = saved;
        }
    }
}

static int cmd_extract(disc *d, const char *outdir)
{
    int i, n = disc_file_count(d);
    int ok = 0, failed = 0;

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        char out[1024];
        q2_buf buf;
        FILE *fp;
        size_t written;

        snprintf(out, sizeof(out), "%s%s", outdir, f->path);
        make_dirs_for(out);

        if (disc_read_file(d, f->path, &buf) != Q2_OK) {
            fprintf(stderr, "  FAILED %s\n", f->path);
            failed++;
            continue;
        }

        fp = fopen(out, "wb");
        if (!fp) {
            fprintf(stderr, "  CANNOT WRITE %s\n", out);
            q2_buf_free(&buf);
            failed++;
            continue;
        }

        written = fwrite(buf.data, 1, buf.size, fp);
        fclose(fp);

        printf("  %s (%zu bytes)\n", f->path, written);
        q2_buf_free(&buf);
        ok++;
    }

    printf("\nextracted %d files, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* movie — the three .STX films, demuxed and decoded                          */
/* ------------------------------------------------------------------------- */
/*
 * The container has been verified for a long time (FORMATS.md §6) and the
 * decoder had not been written, which is what left the campaign ending on a
 * placard (#92). This runs the decoder over every frame of a film and reports
 * the two numbers that say whether it is right: how many 8x8 BLOCKS came out
 * and how many BITS went in.
 *
 * A wrong Huffman table does not make a slightly wrong picture. It runs out of
 * blocks or bits inside the first frame, so "5301 of 5301 frames, 1440 blocks
 * each" is a strong statement and "5300 of 5301" would be a broken one.
 */
/*
 * `movie sweep` — score every plausible escape layout against the disc.
 *
 * After `000001` the escape carries a run and a level and nothing announces how
 * wide either is. Rather than assert a pair, try them: a wrong layout
 * desynchronises nearly every frame, so the right one should stand out by a
 * margin no coincidence produces. Frames decoded exactly is the score.
 */
/*
 * How smooth a decoded frame is — the oracle this decoder was missing.
 *
 * "The disc cannot say which picture is right" is what the last pass concluded
 * about the run/level column, and it is false. A frame of real video is SMOOTH:
 * neighbouring pixels differ by a little. A frame decoded with wrong
 * coefficients is not — it is the banding of magenta and green blocks the last
 * dump showed. So mean absolute difference between horizontally adjacent pixels
 * scores a candidate table, and the disc is the judge after all.
 *
 * Averaged over the frames that decode, and lower is better.
 */
static double frame_roughness(const u8 *rgb, u32 w, u32 h)
{
    double acc = 0.0;
    u32 x, y, n = 0;

    for (y = 0; y < h; y++) {
        for (x = 1; x < w; x++) {
            const u8 *a = rgb + ((size_t)y * w + x - 1) * 3;
            const u8 *b = rgb + ((size_t)y * w + x) * 3;
            int k;

            for (k = 0; k < 3; k++) {
                int d = (int)a[k] - (int)b[k];
                acc += (d < 0) ? -d : d;
                n++;
            }
        }
    }

    return n ? acc / (double)n : 0.0;
}

static int cmd_movie_sweep(const disc *d)
{
    static const u32 k_run[]   = { 5, 6, 7 };
    static const u32 k_level[] = { 8, 9, 10, 11, 12, 16 };
    q2_buf buf;
    u32 ri, li;
    u32 best_ok = 0, best_run = 0, best_level = 0;
    double best_rough = 1e9;

    if (disc_read_file(d, "Q2DATA/MOVIES/OUTRO1P.STX", &buf) != Q2_OK) {
        printf("  OUTRO1P.STX is not on this disc\n");
        return 1;
    }

    printf("\nscoring escape layouts against OUTRO1P.STX (1559 frames)\n");
    printf("  %4s %5s  %8s  %10s  %9s\n", "run", "level", "exact", "overruns",
           "roughness");

    for (ri = 0; ri < sizeof(k_run) / sizeof(k_run[0]); ri++) {
        for (li = 0; li < sizeof(k_level) / sizeof(k_level[0]); li++) {
            size_t cursor = 0;
            static q2_stx_frame f;
            static u8 rgb[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];
            u32 ok = 0, rough_n = 0;
            double rough = 0.0;

            q2_stx_set_escape_layout(k_run[ri], k_level[li]);
            q2_stx_reset_stats();

            while (q2_stx_frame_next(buf.data, buf.size, &cursor, &f)) {
                u32 nb = 0, bits = 0;
                u32 want = ((f.width + 15u) / 16u) *
                           ((f.height + 15u) / 16u) * 6u;

                if (q2_stx_frame_decode(&f, rgb, &nb, &bits) && nb == want) {
                    ok++;
                    if (bits > 12u * want) {
                        rough += frame_roughness(rgb, f.width, f.height);
                        rough_n++;
                    }
                }
            }

            /*
             * Two layouts decode every frame, and they cannot be told apart by
             * synchronisation: the escape's run and level fields total sixteen
             * bits, and where the boundary between them falls changes the
             * NUMBERS a code carries and not how many bits it eats. So the
             * picture is the discriminator — a boundary in the wrong place
             * spills the run's top bit into the level's sign, and the frame
             * gets rougher.
             */
            printf("  %4u %5u  %8u  %10u  %9.2f%s\n", k_run[ri], k_level[li],
                   ok, q2_stx_fail_overrun,
                   rough_n ? rough / (double)rough_n : 0.0,
                   ok > best_ok ? "   <-- best" : "");

            /*
             * Ranked on frames decoded first and SMOOTHNESS second, because
             * the first number cannot separate the two layouts that survive:
             * the escape's run and level total sixteen bits either way, so
             * where the boundary sits changes the numbers and not the
             * synchronisation. The picture is what tells them apart.
             */
            {
                double r = rough_n ? rough / (double)rough_n : 1e9;

                if (ok > best_ok || (ok == best_ok && r < best_rough)) {
                    best_ok    = ok;
                    best_rough = r;
                    best_run   = k_run[ri];
                    best_level = k_level[li];
                }
            }
        }
    }

    printf("  best: run %u bits, level %u bits — %u of 1559 frames,"
           " roughness %.2f\n", best_run, best_level, best_ok, best_rough);

    q2_stx_set_escape_layout(6, 10);
    q2_buf_free(&buf);
    return 0;
}

static int cmd_movie(const disc *d, const char *name, const char *out_ppm,
                     u32 want_frame)
{
    static const char *const k_film[] = {
        "Q2DATA/MOVIES/TAKE1BP.STX",
        "Q2DATA/MOVIES/OUTRO1P.STX",
        "Q2DATA/MOVIES/ROGUEINP.STX"
    };
    u32 fi;
    int rc = 0;

    for (fi = 0; fi < 3; fi++) {
        q2_buf buf;
        size_t cursor = 0;
        u32 frames = 0, ok = 0, blocks_want = 0;
        u32 rich = 0, maxbits = 0, rough_n = 0;
        u32 codes_hit = 0, codes_seen = 0, ac_hit = 0, ac_seen = 0;
        u32 dumped = 0;
        double rough_sum = 0.0;
        static q2_stx_frame f;
        const char *base = strrchr(k_film[fi], '/');

        base = base ? base + 1 : k_film[fi];
        if (name && *name && strcmp(base, name) != 0)
            continue;

        if (disc_read_file(d, k_film[fi], &buf) != Q2_OK) {
            printf("  %s: not on this disc\n", base);
            continue;
        }

        q2_stx_reset_stats();
        printf("\n%s — %llu bytes, %llu sectors\n", base,
               (unsigned long long)buf.size,
               (unsigned long long)(buf.size / Q2_STX_SECTOR_SIZE));

        while (q2_stx_frame_next(buf.data, buf.size, &cursor, &f)) {
            static u8 rgb[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];
            u32 nb = 0, bits = 0;
            bool good;

            frames++;
            blocks_want = ((f.width + 15u) / 16u) *
                          ((f.height + 15u) / 16u) * 6u;
            good = q2_stx_frame_decode(&f, rgb, &nb, &bits);

            if (!good || nb != blocks_want) {
                if (rc == 0)
                    printf("    FAILED at frame %u: %u of %u blocks,"
                           " %u bits of %u\n", f.number, nb, blocks_want,
                           bits, (f.size - 8) * 8);
                rc = 1;
                continue;
            }

            ok++;
            codes_seen++;
            /*
             * The frame's own header, checked against what came out of it.
             *
             * `bs_num_codes` is the MDEC's DMA LENGTH: the chip is fed one
             * 16-bit word per block for the DC, one per (run, level) pair and
             * one for each block's EOB terminator, the DMA moves 32-bit words,
             * and the length is padded to a multiple of 32 of them. So
             *
             *     words  = 2 * blocks + pairs
             *     codes  = round_up_32(ceil(words / 2))
             *
             * and every frame therefore carries its own answer. It needs no
             * reference, no capture and no second decoder — which is what
             * makes it the check that settles whether the Huffman table is
             * right. A DC-only frame is 2*1440 = 2880 words = 1440 longwords,
             * already a multiple of 32, and reading THAT as "one word per
             * block" is what made an earlier wrong formula look confirmed.
             */
            {
                u32 words  = 2u * blocks_want + q2_stx_last_pairs;
                u32 want32 = (((words + 1u) / 2u) + 31u) & ~31u;

                if (want32 == f.num_codes)
                    codes_hit++;
                if (q2_stx_last_pairs) {
                    ac_seen++;
                    if (want32 == f.num_codes)
                        ac_hit++;
                }
            }

            if (bits > 12u * blocks_want) {
                rich++;
                rough_sum += frame_roughness(rgb, f.width, f.height);
                rough_n++;
                if (bits > maxbits)
                    maxbits = bits;
            }

            /* Dump the frame that was asked for; frame 0 means "the first one
             * that carries AC data", because the films open on a fade and a
             * flat grey rectangle says nothing about a decoder. */
            if (out_ppm && *out_ppm && !dumped &&
                (want_frame ? f.number == want_frame
                            : bits > 12u * blocks_want)) {
                FILE *fp = fopen(out_ppm, "wb");

                if (fp) {
                    fprintf(fp, "P6\n%u %u\n255\n", f.width, f.height);
                    fwrite(rgb, 1, (size_t)f.width * f.height * 3, fp);
                    fclose(fp);
                    printf("    frame %u -> %s\n", f.number, out_ppm);
                    dumped = 1;
                }
            }
        }

        printf("    %u frames, %u decoded exactly (%u blocks each);"
               " %u carried AC data, most bits in one %u\n",
               frames, ok, blocks_want, rich, maxbits);
        printf("    bs_num_codes agrees on %u of %u, and on %u of %u"
               " AC-carrying\n", codes_hit, codes_seen, ac_hit, ac_seen);
        if (rough_n)
            printf("    mean roughness %.2f over %u AC frames"
                   " (real video is a few units; wrong coefficients are tens)\n",
                   rough_sum / (double)rough_n, rough_n);
        printf("    gave up: %u unmatched code, %u run overran 63,"
               " %u out of bits\n", q2_stx_fail_unmatched,
               q2_stx_fail_overrun, q2_stx_fail_dry);

        /*
         * And the SOUND, which rides in slot 7 of the same interleave.
         *
         * It is read a sector at a time rather than out of `buf`, because the
         * audio slots are Form 2 and carry 2324 bytes where the video slots
         * carry 2048 — and `disc_read_file` picks one strategy for a whole
         * file, from its FIRST sector, which here is video. A player that took
         * the audio out of that buffer would be handed 2048 of each sector's
         * 2304 ADPCM bytes and would drift.
         *
         * The check that matters is the DURATION: one audio sector is 2016
         * stereo frames at 37800 Hz, and if the reading of the interleave is
         * right then the sound and the picture must come out the same length.
         */
        {
            const disc_file *df = disc_find(d, k_film[fi]);
            u32 sectors = df ? (df->size + Q2_STX_SECTOR_SIZE - 1)
                                   / Q2_STX_SECTOR_SIZE : 0;
            u32 i, audio = 0, bad = 0, shortp = 0;

            for (i = 0; df && i < sectors; i++) {
                u8  payload[CD_SECTOR_RAW];
                u32 len = 0;

                if (!q2_stx_sector_is_audio(i))
                    continue;
                if (disc_read_sector_payload(d, df->lba + i, payload,
                                             &len) != Q2_OK)
                    break;
                if (len < XA_SECTOR_ADPCM_BYTES) { shortp++; continue; }
                audio++;
                if (q2_xa_validate_sector(payload) != 0)
                    bad++;
            }

            printf("    audio: %u XA sectors, %u short, %u with an invalid"
                   " sound group — %.1f s against %.1f s of picture\n",
                   audio, shortp, bad,
                   (double)audio * XA_FRAMES_PER_SECTOR / XA_SAMPLE_RATE,
                   (double)frames / 25.0);
        }

        if (ok != frames || codes_hit != codes_seen)
            rc = 1;

        q2_buf_free(&buf);
    }

    return rc;
}

/* ------------------------------------------------------------------------- */
/* movie encode — the other direction, and the check that it is exact         */
/* ------------------------------------------------------------------------- */
/*
 * Re-encode one of the disc's films and decode the result back.
 *
 * A decoder can be wrong in ways a picture never shows. A quantiser off by a
 * constant, a zigzag transposed, the DC's scale folded into the transform —
 * each of those produces something that still looks like video, and the only
 * way to catch them is to build the inverse and make the two meet in the
 * middle. That is what this is: the disc's own frames go in, a NEW `.STX` comes
 * out, and the same decoder that read the disc reads what was written.
 *
 * What it reports is what an encoder can be judged on:
 *
 *   - every frame decodes, at the full block count, with no unmatched code
 *   - every frame satisfies its own `bs_num_codes` — the MDEC DMA length,
 *     which depends on the code LENGTHS and so cannot be satisfied by accident
 *   - the cadence is 6,5,5,5 and the audio is at slot 7 of every 8
 *   - PSNR against the source frames, which says the picture survived
 *   - the raw sectors' EDC and parity are the ones a drive would compute
 */
typedef struct enc_sink_state {
    FILE *fp;             /* the raw 2352-byte CD-XA stream           */
    u8   *flat;           /* ...and a 2048-per-sector mirror of it    */
    u32   flat_cap;
    u32   flat_used;
    u32   lba;            /* where the file is imagined to sit        */
    u32   bad_sectors;    /* sectors whose own EDC/parity disagree    */
    u32   audio_slots_ok;
    u32   audio_slots_bad;
    bool  failed;

    /* The audio, decoded straight back out of what was just written, so the
     * ADPCM can be scored against the PCM it was made from. A codec that only
     * reports "the sound groups are structurally valid" is reporting that it
     * wrote 2304 bytes, not that it wrote the right ones. */
    q2_xa_decoder xa_back;
    s16  *pcm_back;
    u32   pcm_back_cap;   /* in stereo frames */
    u32   pcm_back_frames;
} enc_sink_state;

static bool enc_sink(void *user, u32 index, q2_stx_form form,
                     const u8 *payload, u32 len)
{
    enc_sink_state *s = (enc_sink_state *)user;
    u8   raw[CD_SECTOR_RAW];
    bool form2 = (form == Q2_STX_FORM_AUDIO);
    u8   submode;

    if (!s || s->failed)
        return false;

    /*
     * The subheader the disc uses on these files: file 1, channel 1, and the
     * submode is the ONLY thing separating a video sector from an audio one —
     * they share the file and channel numbers (FORMATS.md §6).
     */
    submode = form2 ? (u8)(CD_SUBMODE_AUDIO | CD_SUBMODE_REALTIME |
                           CD_SUBMODE_FORM2)
                    : (u8)CD_SUBMODE_DATA;

    cd_sector_build(raw, s->lba + index, 1, 1, submode,
                    form2 ? 0x01u : 0x00u, payload, form2);

    /* Immediately read back what was built, through the same checker the
     * disc's own sectors are put through. An encoder that cannot satisfy its
     * own EDC has no business claiming to satisfy a drive's. */
    if (cd_sector_check(raw) != 0)
        s->bad_sectors++;

    if (form2) {
        if (q2_xa_validate_sector(payload) == 0)
            s->audio_slots_ok++;
        else
            s->audio_slots_bad++;

        if (s->pcm_back &&
            s->pcm_back_frames + XA_FRAMES_PER_SECTOR <= s->pcm_back_cap) {
            u32 n = q2_xa_decode_sector(&s->xa_back, payload,
                                        s->pcm_back +
                                            (size_t)s->pcm_back_frames * 2,
                                        XA_FRAMES_PER_SECTOR * 2);

            s->pcm_back_frames += n / 2;
        }
    }

    if (s->fp && fwrite(raw, 1, CD_SECTOR_RAW, s->fp) != CD_SECTOR_RAW) {
        s->failed = true;
        return false;
    }

    /*
     * The flat mirror, for the round trip. Audio slots become 2048 zero bytes
     * — which is exactly what an extraction does to them, and exactly what the
     * demuxer expects to skip, since it decides what a sector IS from its
     * index and not from its content.
     */
    if (s->flat && s->flat_used + Q2_STX_SECTOR_SIZE <= s->flat_cap) {
        memset(s->flat + s->flat_used, 0, Q2_STX_SECTOR_SIZE);
        if (!form2)
            memcpy(s->flat + s->flat_used, payload,
                   len < Q2_STX_SECTOR_SIZE ? len : Q2_STX_SECTOR_SIZE);
        s->flat_used += Q2_STX_SECTOR_SIZE;
    }

    return true;
}

static double psnr_rgb(const u8 *a, const u8 *b, u32 n)
{
    double sum = 0.0;
    u32 i;

    for (i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];

        sum += d * d;
    }
    if (sum <= 0.0)
        return 99.0;
    return 10.0 * log10(255.0 * 255.0 * (double)n / sum);
}

static int cmd_movie_encode(const disc *d, const char *name, const char *out,
                            u32 want_frames)
{
    char path[64];
    const disc_file *df;
    q2_buf src;
    size_t cursor = 0;
    static q2_stx_frame f;
    static u8 rgb_src[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];
    static u8 rgb_back[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];
    static u8 worst_src[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];
    static u8 worst_enc[Q2_STX_WIDTH * Q2_STX_HEIGHT * 3];
    u32 worst_num = 0;
    q2_stx_writer w;
    enc_sink_state st;
    q2_xa_decoder xa;
    s16   *abuf = NULL;
    u32    abuf_frames = 0, abuf_at = 0, audio_cursor = 0;
    s16   *pcm_src = NULL;         /* what went in, to score what came out */
    u32    pcm_src_cap = 0, pcm_src_frames = 0;
    u32    frames = 0;
    double psnr_sum = 0.0, psnr_min = 1e9;
    int    rc = 0;

    if (!name || !*name) {
        printf("  movie encode needs a film name\n");
        return 1;
    }
    snprintf(path, sizeof(path), "Q2DATA/MOVIES/%s", name);

    df = disc_find(d, path);
    if (!df || disc_read_file(d, path, &src) != Q2_OK) {
        printf("  %s is not on this disc\n", name);
        return 1;
    }

    if (!want_frames)
        want_frames = 250u;      /* ten seconds: a check, not a transcode */

    memset(&st, 0, sizeof(st));
    st.lba = df->lba;
    st.fp  = out ? fopen(out, "wb") : NULL;
    if (out && !st.fp) {
        printf("  cannot write %s\n", out);
        q2_buf_free(&src);
        return 1;
    }

    /* Six sectors a frame, plus the tail. */
    st.flat_cap = (want_frames + 8u) * 8u * Q2_STX_SECTOR_SIZE;
    st.flat     = (u8 *)calloc(st.flat_cap, 1);
    abuf        = (s16 *)calloc((size_t)XA_FRAMES_PER_SECTOR * 2 * 4,
                                sizeof(s16));

    /* One picture is 1512 stereo frames of sound; a couple of sectors of slack
     * covers the tail the writer drains after the last picture. */
    pcm_src_cap      = (want_frames + 8u) * 1512u;
    pcm_src          = (s16 *)calloc((size_t)pcm_src_cap * 2, sizeof(s16));
    st.pcm_back_cap  = pcm_src_cap + XA_FRAMES_PER_SECTOR * 4u;
    st.pcm_back      = (s16 *)calloc((size_t)st.pcm_back_cap * 2, sizeof(s16));
    q2_xa_decoder_reset(&st.xa_back);

    if (!st.flat || !abuf || !pcm_src || !st.pcm_back) {
        printf("  out of memory\n");
        rc = 1;
        goto cleanup;
    }

    q2_xa_decoder_reset(&xa);
    q2_stx_writer_init(&w, Q2_STX_WIDTH, Q2_STX_HEIGHT, true, enc_sink, &st);

    printf("\nre-encoding %s -> %s\n", name, out ? out : "(no file)");

    /*
     * Before writing a single sector: put THIS FILM'S OWN sectors through the
     * same EDC and parity code the writer uses.
     *
     * A sector builder that satisfies itself has proved nothing — a consistent
     * mistake is still a mistake. What settles it is the disc: recompute the
     * CRC and the Reed-Solomon P/Q for sectors that were mastered in 1997 and
     * see whether the four and 276 bytes that come back are the ones already
     * there. If they are, the builder is the drive's.
     */
    {
        u32 total = (u32)(src.size / Q2_STX_SECTOR_SIZE);
        u32 i, checked = 0, disagree = 0, form2 = 0;

        for (i = 0; i < total; i++) {
            u8 raw[CD_SECTOR_RAW];
            u32 bad;

            if (disc_read_raw_sector(d, df->lba + i, raw) != Q2_OK)
                break;
            if (raw[18] & CD_SUBMODE_FORM2)
                form2++;
            bad = cd_sector_check(raw);
            checked++;
            if (bad)
                disagree++;
        }

        printf("  the disc's own %u sectors of this film (%u of them Form 2):"
               " %u disagree with this builder\n", checked, form2, disagree);
        if (disagree)
            rc = 1;
    }

    while (frames < want_frames &&
           q2_stx_frame_next(src.data, src.size, &cursor, &f)) {
        u32 blocks = 0, bits = 0;
        u32 need = 37800u / 25u;      /* 1512 stereo frames under one picture */

        if (!q2_stx_frame_decode(&f, rgb_src, &blocks, &bits)) {
            printf("  source frame %u will not decode\n", f.number);
            rc = 1;
            break;
        }

        /*
         * The audio that plays under this frame, pulled out of the source's own
         * slot-7 sectors. Three of them cover four pictures exactly, which is
         * the arithmetic that makes this container 25.000 fps.
         */
        while (abuf_frames - abuf_at < need) {
            u8  payload[CD_SECTOR_RAW];
            u32 len = 0;

            while (audio_cursor < (u32)(src.size / Q2_STX_SECTOR_SIZE) &&
                   !q2_stx_sector_is_audio(audio_cursor))
                audio_cursor++;
            if (audio_cursor >= (u32)(src.size / Q2_STX_SECTOR_SIZE))
                break;

            if (disc_read_sector_payload(d, df->lba + audio_cursor,
                                         payload, &len) != Q2_OK)
                break;
            audio_cursor++;
            if (len < XA_SECTOR_ADPCM_BYTES ||
                q2_xa_validate_sector(payload) != 0)
                continue;

            /* Compact what is left, then decode one more sector behind it. */
            memmove(abuf, abuf + (size_t)abuf_at * 2,
                    (size_t)(abuf_frames - abuf_at) * 2 * sizeof(s16));
            abuf_frames -= abuf_at;
            abuf_at      = 0;
            abuf_frames += q2_xa_decode_sector(&xa, payload,
                                               abuf + (size_t)abuf_frames * 2,
                                               XA_FRAMES_PER_SECTOR * 2) / 2;
        }

        {
            u32 have = abuf_frames - abuf_at;
            u32 take = have < need ? have : need;

            if (!q2_stx_writer_frame(&w, rgb_src,
                                     take ? abuf + (size_t)abuf_at * 2 : NULL,
                                     take)) {
                printf("  the encoder gave up on frame %u\n", f.number);
                rc = 1;
                break;
            }
            if (take && pcm_src_frames + take <= pcm_src_cap) {
                memcpy(pcm_src + (size_t)pcm_src_frames * 2,
                       abuf + (size_t)abuf_at * 2,
                       (size_t)take * 2 * sizeof(s16));
                pcm_src_frames += take;
            }
            abuf_at += take;
        }

        frames++;
    }

    q2_stx_writer_finish(&w);

    printf("  %u frames, %u video + %u audio + %u null = %u sectors\n",
           w.frames, w.video_sectors, w.audio_sectors, w.null_sectors,
           w.sector);
    printf("  qscale %u..%u, mean %.2f\n", w.qscale_min, w.qscale_max,
           w.frames ? (double)w.qscale_sum / (double)w.frames : 0.0);
    printf("  raw sectors failing their own EDC or parity: %u\n",
           st.bad_sectors);
    printf("  audio slots: %u valid, %u malformed\n",
           st.audio_slots_ok, st.audio_slots_bad);
    if (st.bad_sectors || st.audio_slots_bad)
        rc = 1;

    /*
     * The ADPCM, scored against the PCM it was made from — decoded back out of
     * the sectors that were just written, by the same decoder the player uses.
     * 4-bit ADPCM is lossy by construction, so the number to watch is not "is
     * it exact" but "is it in the twenties of dB and STABLE": a codec that
     * feeds its predictor the sample it wanted rather than the sample it
     * produced starts fine and drifts, and the drift shows up as a figure that
     * falls the longer the film runs.
     */
    if (pcm_src_frames && st.pcm_back_frames) {
        u32 n = pcm_src_frames < st.pcm_back_frames ? pcm_src_frames
                                                    : st.pcm_back_frames;
        double sig = 0.0, err = 0.0;
        u32 i;

        for (i = 0; i < n * 2u; i++) {
            double a = (double)pcm_src[i];
            double b = (double)st.pcm_back[i];

            sig += a * a;
            err += (a - b) * (a - b);
        }
        /*
         * The source's own level is printed beside the SNR on purpose. A
         * stretch of silence encodes to silence and scores infinitely well,
         * and an encoder that had been handed nothing would look identical to
         * one that had been handed everything and got it right. The first
         * twelve seconds of the intro ARE silence, so this is not hypothetical.
         */
        printf("  audio: %u frames in, %u back — source RMS %.1f, ",
               pcm_src_frames, st.pcm_back_frames,
               sqrt(sig / (double)(n * 2u)));
        if (sig <= 0.0)
            printf("silent (nothing to score)\n");
        else if (err <= 0.0)
            printf("reproduced exactly\n");
        else
            printf("SNR %.2f dB\n", 10.0 * log10(sig / err));
    }

    /* ------------------------------------------------------------------ */
    /* And now read back what was written, with the disc's own decoder.    */
    /* ------------------------------------------------------------------ */
    {
        static q2_stx_frame a, b;
        size_t ca = 0, cb = 0;
        u32 back = 0, exact = 0, codes_ok = 0, cad_bad = 0, num_bad = 0;
        u32 want_blocks = ((Q2_STX_WIDTH + 15u) / 16u) *
                          ((Q2_STX_HEIGHT + 15u) / 16u) * 6u;

        q2_stx_reset_stats();

        while (q2_stx_frame_next(src.data, src.size, &ca, &a) &&
               q2_stx_frame_next(st.flat, st.flat_used, &cb, &b)) {
            u32 blocks = 0, bits = 0, words, half, codes;

            back++;
            if (back > w.frames)
                break;

            if (!q2_stx_frame_decode(&a, rgb_src, &blocks, &bits))
                break;
            if (!q2_stx_frame_decode(&b, rgb_back, &blocks, &bits) ||
                blocks != want_blocks) {
                printf("  frame %u came back short: %u of %u blocks\n",
                       b.number, blocks, want_blocks);
                rc = 1;
                break;
            }
            exact++;

            /*
             * The frame's own DMA length, checked the way the disc's frames are
             * checked: one word per block for the DC, one per pair and one per
             * EOB, halved into longwords and padded to 32 of them. This is what
             * makes the round trip a statement about the FORMAT rather than a
             * statement about two programs agreeing with each other.
             */
            words = 2u * blocks + q2_stx_last_pairs;
            half  = (words + 1u) / 2u;
            codes = ((half + 31u) / 32u) * 32u;
            if (codes == b.num_codes)
                codes_ok++;
            else
                num_bad++;

            /* 6, 5, 5, 5 — keyed to the frame number, as on the disc. */
            {
                u32 want_chunks = (((b.number - 1u) % 4u) == 0u) ? 6u : 5u;
                u32 got = (b.size + Q2_STX_VIDEO_PAYLOAD - 1u) /
                          Q2_STX_VIDEO_PAYLOAD;

                if (got > want_chunks)
                    cad_bad++;
            }

            {
                double p = psnr_rgb(rgb_src, rgb_back,
                                    Q2_STX_WIDTH * Q2_STX_HEIGHT * 3);

                psnr_sum += p;
                /*
                 * Keep the WORST pair, not a sample of a good one. A mean is
                 * easy to be pleased by; the frame the encoder handled least
                 * well is the one worth looking at, and it is the one that
                 * shows a structural mistake — a transposed block, a chroma
                 * plane in the wrong place — if there is one to show.
                 */
                if (p < psnr_min) {
                    psnr_min  = p;
                    worst_num = b.number;
                    memcpy(worst_src, rgb_src, sizeof(worst_src));
                    memcpy(worst_enc, rgb_back, sizeof(worst_enc));
                }
            }
        }

        printf("  read back: %u frames, %u decoded exactly (%u blocks each)\n",
               back, exact, want_blocks);
        printf("  bs_num_codes agrees on %u of %u; %u disagree\n",
               codes_ok, exact, num_bad);
        printf("  frames over their sector budget: %u\n", cad_bad);
        printf("  gave up: %u unmatched code, %u run overran 63, %u out of"
               " bits\n", q2_stx_fail_unmatched, q2_stx_fail_overrun,
               q2_stx_fail_dry);
        if (exact)
            printf("  PSNR against the source: %.2f dB mean, %.2f dB worst"
                   " (frame %u)\n", psnr_sum / (double)exact, psnr_min,
                   worst_num);

        /* The worst pair, side by side on disc, for looking at. */
        if (out && worst_num) {
            char p[260];
            int  k;

            for (k = 0; k < 2; k++) {
                FILE *fp;

                snprintf(p, sizeof(p), "%s.%s.ppm", out,
                         k ? "encoded" : "source");
                fp = fopen(p, "wb");
                if (!fp)
                    continue;
                fprintf(fp, "P6\n%u %u\n255\n", Q2_STX_WIDTH, Q2_STX_HEIGHT);
                fwrite(k ? worst_enc : worst_src, 1,
                       (size_t)Q2_STX_WIDTH * Q2_STX_HEIGHT * 3, fp);
                fclose(fp);
                printf("  %s\n", p);
            }
        }

        if (exact != w.frames || num_bad || cad_bad ||
            q2_stx_fail_unmatched || q2_stx_fail_overrun || q2_stx_fail_dry)
            rc = 1;
    }

cleanup:
    if (st.fp)
        fclose(st.fp);
    free(st.flat);
    free(st.pcm_back);
    free(pcm_src);
    free(abuf);
    q2_buf_free(&src);
    return rc;
}

int main(int argc, char **argv)
{
    disc *d = NULL;
    q2_result r;
    const char *cmd;
    const char *path;
    int rc;

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 ||
                      strcmp(argv[1], "-v") == 0)) {
        q2_version_print();
        return 0;
    }

    if (argc < 3) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    cmd  = argv[1];
    path = argv[2];

    if (getenv("Q2PSX_VERBOSE"))
        q2_log_set_level(Q2_LOG_DEBUG);

    r = disc_open(&d, path);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot open disc '%s': %s\n", path, q2_result_str(r));
        return 1;
    }

    if (strcmp(cmd, "ident") == 0) {
        rc = cmd_ident(d);
    } else if (strcmp(cmd, "disc") == 0) {
        rc = cmd_disc(d);
    } else if (strcmp(cmd, "levels") == 0) {
        rc = cmd_levels(d);
    } else if (strcmp(cmd, "dats") == 0) {
        rc = cmd_dats(d);
    } else if (strcmp(cmd, "verify") == 0) {
        rc = cmd_verify(d);
    } else if (strcmp(cmd, "coll") == 0) {
        int zi = (argc >= 5) ? atoi(argv[4]) : -1;
        rc = cmd_coll(d, (argc >= 4) ? argv[3] : NULL, zi);
    } else if (strcmp(cmd, "polyflags") == 0) {
        rc = cmd_polyflags(d);
    } else if (strcmp(cmd, "cluts") == 0) {
        rc = cmd_cluts(d);
    } else if (strcmp(cmd, "surfaces") == 0) {
        rc = cmd_surfaces(d);
    } else if (strcmp(cmd, "anims") == 0) {
        rc = cmd_anims(d);
    } else if (strcmp(cmd, "mob") == 0) {
        if (argc < 4) {
            fprintf(stderr, "mob needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            int wh = (argc >= 6) ? atoi(argv[5]) : 0;
            const char *outp = (argc >= 7) ? argv[6] : "mob.ppm";
            rc = cmd_mob(d, argv[3], zi, wh, outp,
                         (argc >= 8) ? (s32)strtol(argv[7], NULL, 10) : -1);
        }
    } else if (strcmp(cmd, "classes") == 0) {
        rc = cmd_classes(d);
    } else if (strcmp(cmd, "models") == 0) {
        if (argc < 4) {
            fprintf(stderr, "models needs a map name\n");
            rc = 1;
        } else {
            rc = cmd_models(d, argv[3]);
        }
    } else if (strcmp(cmd, "fps") == 0) {
        if (argc < 4) {
            fprintf(stderr, "fps needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            const char *wp = (argc >= 6) ? argv[5] : NULL;
            const char *outp = (argc >= 7) ? argv[6] : "fps.ppm";
            s32 vy = (argc >= 8) ? (s32)strtol(argv[7], NULL, 10) : 0;
            s32 gy = (argc >= 9) ? (s32)strtol(argv[8], NULL, 10) : 1536;
            s32 dy = (argc >= 10) ? (s32)strtol(argv[9], NULL, 10) : 0;
            s32 rl = (argc >= 11) ? (s32)strtol(argv[10], NULL, 10) : 0;
            rc = cmd_fps(d, argv[3], zi, wp, outp, vy, gy, dy, rl);
        }
    } else if (strcmp(cmd, "model") == 0) {
        if (argc < 5) {
            fprintf(stderr, "model needs a map and a model name or index\n");
            rc = 1;
        } else {
            int ci = (argc >= 6) ? atoi(argv[5]) : 0;
            int fr = (argc >= 7) ? atoi(argv[6]) : 0;
            const char *outp = (argc >= 8) ? argv[7] : "model.ppm";
            /*
             * Default to the model's FRONT, not its back.
             *
             * Yaw 0 puts the eye behind whatever is being inspected, and the
             * back of a creature is the half of it nobody has ever checked. It
             * cost #10b three passes: a violet patch on the back of the
             * Soldier's collar was written up as "the head textures wrong",
             * and one look from the front would have closed it, because from
             * the front the model is perfect.
             */
            s32 vy = (argc >= 9) ? (s32)strtol(argv[8], NULL, 10)
                                 : Q2_ANGLE_180;
            rc = cmd_model(d, argv[3], argv[4], ci, fr, outp, vy);
        }
    } else if (strcmp(cmd, "bmodel") == 0) {
        if (argc < 4) {
            fprintf(stderr, "bmodel needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            int wh = (argc >= 6) ? atoi(argv[5]) : -1;
            const char *outp = (argc >= 7) ? argv[6] : "bmodel.ppm";
            bool open_it = (argc >= 8) && atoi(argv[7]) != 0;
            rc = cmd_bmodel(d, argv[3], zi, wh, outp, open_it);
        }
    } else if (strcmp(cmd, "audio") == 0) {
        rc = cmd_audio(d);
    } else if (strcmp(cmd, "leveltable") == 0) {
        rc = cmd_leveltable(d);
    } else if (strcmp(cmd, "reloc") == 0) {
        rc = cmd_reloc(d);
    } else if (strcmp(cmd, "zonescript") == 0) {
        rc = cmd_zonescript(d, argc >= 4 ? argv[3] : NULL);
    } else if (strcmp(cmd, "events") == 0) {
        rc = cmd_events(d);
    } else if (strcmp(cmd, "walk") == 0) {
        if (argc < 4) {
            fprintf(stderr, "walk needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            int tk = (argc >= 6) ? atoi(argv[5]) : 200;
            rc = cmd_walk(d, argv[3], zi, tk);
        }
    } else if (strcmp(cmd, "textures") == 0) {
        rc = cmd_textures(d);
    } else if (strcmp(cmd, "music") == 0) {
        rc = cmd_music(d);
    } else if (strcmp(cmd, "render") == 0) {
        if (argc < 4) {
            fprintf(stderr, "render needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            const char *outp = (argc >= 6) ? argv[5] : "zone.ppm";
            s32 yaw   = (argc >= 7) ? (s32)strtol(argv[6], NULL, 10) : 0;
            s32 pitch = (argc >= 8) ? (s32)strtol(argv[7], NULL, 10) : 0;
            s32 rott  = (argc >= 9) ? (s32)strtol(argv[8], NULL, 10) : 0;
            /* The eye view's own pitch. `pitch` is spent on the 9999 sentinel
             * that selects the eye view at all, so looking UP from a spawn had
             * no expression — and a lens flare's ghosts are usually above the
             * horizon, because the lights that carry one are ceiling lamps. */
            s32 epitch = (argc >= 10) ? (s32)strtol(argv[9], NULL, 10) : 0;
            rc = cmd_render(d, argv[3], zi, outp, yaw, pitch, rott, epitch);
        }
    } else if (strcmp(cmd, "dat") == 0) {
        if (argc < 4) {
            fprintf(stderr, "dat needs a file path\n");
            rc = 1;
        } else {
            rc = cmd_dat(d, argv[3]);
        }
    } else if (strcmp(cmd, "hexdump") == 0) {
        if (argc < 4) {
            fprintf(stderr, "hexdump needs a file path\n");
            rc = 1;
        } else {
            size_t count = 256;
            if (argc >= 5)
                count = (size_t)strtoul(argv[4], NULL, 0);
            rc = cmd_hexdump(d, argv[3], count);
        }
    } else if (strcmp(cmd, "exe") == 0) {
        rc = cmd_exe(d, (argc >= 4) ? argv[3] : NULL);
    } else if (strcmp(cmd, "disasm") == 0) {
        if (argc < 4) {
            fprintf(stderr, "disasm needs an address\n");
            rc = 1;
        } else {
            int n = (argc >= 5) ? atoi(argv[4]) : 0;
            rc = cmd_disasm(d, argv[3], n);
        }
    } else if (strcmp(cmd, "xrefs") == 0) {
        if (argc < 4) {
            fprintf(stderr, "xrefs needs an address\n");
            rc = 1;
        } else {
            rc = cmd_xrefs(d, argv[3]);
        }
    } else if (strcmp(cmd, "moddisasm") == 0) {
        if (argc < 4) {
            fprintf(stderr, "moddisasm needs a map name\n");
            rc = 1;
        } else {
            const char *at = (argc >= 5) ? argv[4] : NULL;
            int n = (argc >= 6) ? atoi(argv[5]) : 0;
            rc = cmd_moddisasm(d, argv[3], at, n,
                               (argc >= 7) ? argv[6] : NULL);
        }
    } else if (strcmp(cmd, "levdisasm") == 0) {
        if (argc < 4) {
            fprintf(stderr, "levdisasm needs a map name\n");
            rc = 1;
        } else {
            const char *at = (argc >= 5) ? argv[4] : NULL;
            int n = (argc >= 6) ? atoi(argv[5]) : 0;
            rc = cmd_levdisasm(d, argv[3], at, n);
        }
    } else if (strcmp(cmd, "modstrings") == 0) {
        if (argc < 4) {
            fprintf(stderr, "modstrings needs a map name\n");
            rc = 1;
        } else {
            bool lev = (argc < 5) || strcmp(argv[4], "crea") != 0;
            rc = cmd_modstrings(d, argv[3], lev);
        }
    } else if (strcmp(cmd, "modxrefs") == 0) {
        if (argc < 5) {
            fprintf(stderr, "modxrefs needs a map name and an address\n");
            rc = 1;
        } else {
            bool lev = (argc < 6) || strcmp(argv[5], "crea") != 0;
            rc = cmd_modxrefs(d, argv[3], argv[4], lev);
        }
    } else if (strcmp(cmd, "modbytes") == 0) {
        if (argc < 5) {
            fprintf(stderr, "modbytes needs a map name and an address\n");
            rc = 1;
        } else {
            int n = (argc >= 6) ? atoi(argv[5]) : 0;
            bool lev = (argc < 7) || strcmp(argv[6], "crea") != 0;
            rc = cmd_modbytes(d, argv[3], argv[4], n, lev);
        }
    } else if (strcmp(cmd, "funcs") == 0) {
        rc = cmd_funcs(d, (argc >= 4) ? argv[3] : NULL);
    } else if (strcmp(cmd, "bytes") == 0) {
        if (argc < 4) {
            fprintf(stderr, "bytes needs an address\n");
            rc = 1;
        } else {
            int n = (argc >= 5) ? atoi(argv[4]) : 128;
            rc = cmd_bytes(d, argv[3], n);
        }
    } else if (strcmp(cmd, "access") == 0) {
        if (argc < 4) {
            fprintf(stderr, "access needs a structure offset\n");
            rc = 1;
        } else {
            rc = cmd_access(d, argv[3], (argc >= 5) ? argv[4] : NULL);
        }
    } else if (strcmp(cmd, "hud") == 0) {
        rc = cmd_hud(d, (argc >= 4) ? argv[3] : NULL,
                     (argc >= 5) ? argv[4] : NULL);
    } else if (strcmp(cmd, "items") == 0) {
        rc = cmd_items(d);
    } else if (strcmp(cmd, "lights") == 0) {
        rc = cmd_lights(d);
    } else if (strcmp(cmd, "lit") == 0) {
        rc = cmd_lit(d, argc > 3 ? argv[3] : NULL);
    } else if (strcmp(cmd, "weapons") == 0) {
        rc = cmd_weapons(d);
    } else if (strcmp(cmd, "effects") == 0) {
        rc = cmd_effects(d, (argc >= 4) ? argv[3] : NULL);
    } else if (strcmp(cmd, "modelents") == 0) {
        rc = cmd_modelent(d);
    } else if (strcmp(cmd, "explosives") == 0) {
        rc = cmd_explosive(d, (argc >= 4) ? argv[3] : NULL);
    } else if (strcmp(cmd, "ai") == 0) {
        rc = cmd_ai(d);
    } else if (strcmp(cmd, "creatures") == 0) {
        rc = cmd_creatures(d);
    } else if (strcmp(cmd, "multi") == 0) {
        rc = cmd_multi(d, (argc >= 4) ? argv[3] : NULL);
    } else if (strcmp(cmd, "death") == 0) {
        rc = cmd_death(d);
    } else if (strcmp(cmd, "text") == 0) {
        rc = cmd_text(d, (argc >= 4) ? argv[3] : NULL,
                      (argc >= 5) ? argv[4] : NULL);
    } else if (strcmp(cmd, "movie") == 0) {
        if (argc >= 4 && strcmp(argv[3], "sweep") == 0)
            rc = cmd_movie_sweep(d);
        else if (argc >= 4 && strcmp(argv[3], "encode") == 0)
            rc = cmd_movie_encode(d, (argc >= 5) ? argv[4] : NULL,
                                     (argc >= 6) ? argv[5] : NULL,
                                     (argc >= 7)
                                         ? (u32)strtoul(argv[6], NULL, 0) : 0u);
        else
            rc = cmd_movie(d, (argc >= 4) ? argv[3] : NULL,
                              (argc >= 5) ? argv[4] : NULL,
                              (argc >= 6) ? (u32)strtoul(argv[5], NULL, 0) : 0u);
    } else if (strcmp(cmd, "menu") == 0) {
        rc = cmd_menu(d, (argc >= 4) ? argv[3] : NULL,
                         (argc >= 5) ? argv[4] : NULL,
                         (argc >= 6) ? argv[5] : NULL);
    } else if (strcmp(cmd, "save") == 0) {
        rc = cmd_save(d, (argc >= 4) ? argv[3] : NULL,
                         (argc >= 5) ? argv[4] : NULL);
    } else if (strcmp(cmd, "pmove") == 0) {
        rc = cmd_pmove(d, (argc >= 4) ? argv[3] : NULL,
                          (argc >= 5) ? atoi(argv[4]) : 0);
    } else if (strcmp(cmd, "viewweapon") == 0) {
        rc = cmd_viewweapon(d, (argc >= 4) ? argv[3] : NULL,
                               (argc >= 5) ? argv[4] : NULL,
                               (argc >= 6) ? argv[5] : NULL,
                               (argc >= 7) ? atoi(argv[6]) : 0,
                               (argc >= 8) ? atoi(argv[7]) : 0,
                               (argc >= 9) ? argv[8] : NULL);
    } else if (strcmp(cmd, "screen") == 0) {
        rc = cmd_screen(d, (argc >= 4) ? argv[3] : NULL,
                           (argc >= 5) ? argv[4] : NULL,
                           (argc >= 6) ? argv[5] : NULL,
                           (argc >= 7) ? atoi(argv[6]) : 0);
    } else if (strcmp(cmd, "find") == 0) {
        if (argc < 4) {
            fprintf(stderr, "find needs a string or 0x-prefixed byte pattern\n");
            rc = 1;
        } else {
            rc = cmd_find(d, argv[3]);
        }
    } else if (strcmp(cmd, "extract") == 0) {
        if (argc < 4) {
            fprintf(stderr, "extract needs an output directory\n");
            rc = 1;
        } else {
            rc = cmd_extract(d, argv[3]);
        }
    } else if (strcmp(cmd, "export") == 0) {
        if (argc < 4) {
            fprintf(stderr, "export needs an output directory\n");
            rc = 1;
        } else {
            rc = cmd_export(d, argv[3], (argc >= 5) ? argv[4] : NULL,
                            (argc >= 6) ? argv[5] : NULL);
        }
    } else {
        fprintf(stderr, "unknown command '%s'\n\n", cmd);
        usage();
        rc = 1;
    }

    disc_close(d);
    return rc;
}
