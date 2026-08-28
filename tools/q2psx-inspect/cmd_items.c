#include "cmd_items.h"

#include <stdio.h>
#include <string.h>

#include "dat.h"
#include "entity.h"
#include "ident.h"
#include "item.h"
#include "itemtable.h"
#include "level.h"
#include "model.h"
#include "population.h"

static void report(void *user, const char *what, long expected, long got)
{
    unsigned *bad = (unsigned *)user;

    (*bad)++;
    if (expected == 0 && got == 0)
        printf("  MISMATCH  %s\n", what);
    else
        printf("  MISMATCH  %-28s builtin %ld, disc %ld\n", what, expected, got);
}

/* The flag word, spelled out. */
static void flag_text(u16 f, char *out, size_t n)
{
    size_t used = 0;
    int i;
    static const struct { u16 bit; const char *name; } k[] = {
        { Q2_ITEM_SPIN,          "spin"      },
        { Q2_ITEM_MATERIALISE,   "grow"      },
        { Q2_ITEM_TIMED,         "timed"     },
        { Q2_ITEM_OBJECTIVE,     "objective" },
        { Q2_ITEM_GLOW_R,        "R"         },
        { Q2_ITEM_GLOW_G,        "G"         },
        { Q2_ITEM_GLOW_B,        "B"         },
        { Q2_ITEM_NO_ANIM,       "noanim"    },
        { Q2_ITEM_NO_DROP,  "nodrop"    }
    };

    out[0] = '\0';
    for (i = 0; i < (int)(sizeof(k) / sizeof(k[0])); i++) {
        if (!(f & k[i].bit))
            continue;
        used += (size_t)snprintf(out + used, used < n ? n - used : 0, "%s%s",
                                 used ? "|" : "", k[i].name);
        if (used >= n)
            return;
    }
    if (!out[0])
        snprintf(out, n, "-");
}

/* True when `name` is in the map's COMMON bank or in any of its zones'. */
static bool map_has_model(disc *d, const q2_model_bank *common_bank,
                          const char *common_path, const char *name)
{
    char dir[160];
    const char *slash;
    int z;

    if (common_bank && q2_model_bank_find(common_bank, name) >= 0)
        return true;

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
        if (q2_model_bank_from_zone(&zbank, &zf) == Q2_OK)
            hit = q2_model_bank_find(&zbank, name) >= 0;
        q2_zone_close(&zf);
        q2_buf_free(&zbuf);
        if (hit)
            return true;
    }
    return false;
}

int cmd_items(disc *d)
{
    q2_build_id id;
    q2_item_table disc_side;
    const q2_item_table *table;
    const q2_item_table *builtin = q2_item_table_builtin();
    unsigned bad = 0;
    u32 i;
    int f, n;

    /* Per-place-id census, and per-record placement count. */
    unsigned long long places = 0, resolved = 0, with_model = 0;
    unsigned long long inert = 0, scenery = 0, live = 0;
    unsigned long long shadow_places = 0;
    unsigned long long angle_over_4096 = 0, upper_bits = 0;
    unsigned long long place_flag_hist[16];
    unsigned long long place_flag_yaw_zero[16];
    unsigned long long place_allowed[3] = { 0, 0, 0 };
    unsigned place_maps = 0, marker_all_maps = 0, marker_none_maps = 0;
    unsigned marker_mixed_maps = 0;
    unsigned per_record[Q2_ITEM_COUNT];
    unsigned unknown_id[128];
    int unknown_count = 0;
    char first_shadow_path[160] = "";
    char first_shadow_group[13] = "";
    char first_shadow_model[Q2_ITEM_MODEL_LEN + 1] = "";
    q2_pop_place first_shadow_place;

    if (!d)
        return 1;

    memset(per_record, 0, sizeof(per_record));
    memset(place_flag_hist, 0, sizeof(place_flag_hist));
    memset(place_flag_yaw_zero, 0, sizeof(place_flag_yaw_zero));

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }

    if (q2_item_table_load(&disc_side, d, &id) != Q2_OK) {
        fprintf(stderr, "no item table for build %s -- using the built-in\n",
                id.serial[0] ? id.serial : "(unidentified)");
        table = builtin;
    } else {
        table = &disc_side;
        printf("Item table at 0x%08X: %u records, stride %d\n",
               Q2_ITEMTABLE_ADDR_SLES01534, table->count, Q2_ITEM_RECORD_SIZE);
        printf("Touch dispatch at 0x%08X: %d slots, failure exit 0x%08X\n\n",
               Q2_ITEM_DISPATCH_SLES01534, Q2_ITEM_EFFECT_COUNT,
               table->dispatch_default);
        q2_item_table_diff(table, builtin, report, &bad);
        if (bad)
            printf("\n");
    }

    /* ------------------------------------------------------------------ */
    printf("  %-4s %-14s %-4s %-18s %-22s %-13s %s\n",
           "id", "model", "fx", "caption", "effect", "flags", "shadow verts");
    for (i = 0; i < table->count; i++) {
        const q2_item_def *e = &table->def[i];
        char flags[64], shadow[32], what[32];
        u32 k;

        flag_text(e->flags, flags, sizeof(flags));

        shadow[0] = '\0';
        for (k = 0; k < e->shadow_vertex_count; k++)
            snprintf(shadow + strlen(shadow), sizeof(shadow) - strlen(shadow),
                     "%s%u", k ? "," : "", e->shadow_vertex[k]);
        if (!shadow[0])
            snprintf(shadow, sizeof(shadow), "-");

        if (e->effect == 0)
            snprintf(what, sizeof(what), "scenery");
        else if (!q2_item_effect_is_live(table, e->effect))
            snprintf(what, sizeof(what), "INERT (no handler)");
        else
            snprintf(what, sizeof(what), "%s", q2_item_effect_name(e->effect));

        /*
         * The CAPTION column, which is the one thing a model name does not
         * give you: `Sshotgun P` reads out on the HUD as "Super Shotgun", and
         * `Medi P` as "Health". Straight out of the 57-pointer table at
         * 0x800AC144, indexed by the same effect id as the icon rect.
         */
        printf("  %-4d %-14s %-4u %-18s %-22s %-22s %s\n",
               e->place_id, e->model, e->effect,
               q2_item_display_name(table, e->effect),
               what, flags, shadow);
    }

    /* ------------------------------------------------------------------ */
    printf("\nEffect dispatch, %d slots:\n", Q2_ITEM_EFFECT_COUNT);
    /*
     * The union of every record's flag word, and the union of the bits none of
     * `q2_item_flag` names. #27 asked what the bits beyond 0, 1 and 8 mean;
     * they are all named now, and the second number is what says whether any
     * remain to name at all.
     */
    {
        u32 ri, all = 0, unknown = 0;
        u32 known = Q2_ITEM_SPIN | Q2_ITEM_MATERIALISE | Q2_ITEM_TIMED |
                    Q2_ITEM_OBJECTIVE | Q2_ITEM_GLOW | Q2_ITEM_NO_ANIM |
                    Q2_ITEM_NO_DROP;

        for (ri = 0; ri < table->count; ri++)
            all |= table->def[ri].flags;
        unknown = all & ~known;
        printf("  flag bits set anywhere : 0x%04X;"
               " bits no q2_item_flag names : 0x%04X\n", all, unknown);
    }

    {
        u32 e;
        printf("  live  :");
        for (e = Q2_ITEM_EFFECT_FIRST; e <= (u32)Q2_ITEM_EFFECT_LAST; e++)
            if (q2_item_effect_is_live(table, e))
                printf(" %u", e);
        printf("\n  inert :");
        for (e = Q2_ITEM_EFFECT_FIRST; e <= (u32)Q2_ITEM_EFFECT_LAST; e++)
            if (!q2_item_effect_is_live(table, e))
                printf(" %u", e);
        printf("\n");
    }

    /* ------------------------------------------------------------------ */
    n = disc_file_count(d);
    for (f = 0; f < n; f++) {
        const disc_file *file = disc_file_at(d, f);
        const char *base = strrchr(file->path, '/');
        q2_buf buf;
        q2_common_file cf;
        q2_population pop;
        q2_model_bank bank;
        bool have_bank;
        unsigned map_flag_hist[16] = { 0 };
        unsigned map_places = 0;
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
                q2_pop_place pl;
                u32 k;

                if (!q2_pop_get_group(&pop, g, &grp))
                    continue;

                for (k = 0; q2_pop_get_place(&pop, &grp, k, &pl); k++) {
                    const q2_item_def *e = q2_item_find(table, (s32)pl.id);

                    places++;

                    /* 0x8007F538 reads the upper nibble before item spawn:
                     * 0x2000/0x4000/0x8000 exclude skill 0/1/2. Keep the full
                     * histogram visible so the remaining 0x1000 bit cannot be
                     * mistaken for a difficulty flag merely by association. */
                    if (Q2_POP_PLACE_ANGLE(pl.angle_flags) >= 4096u)
                        angle_over_4096++;
                    if (Q2_POP_PLACE_FLAGS(pl.angle_flags))
                        upper_bits++;
                    place_flag_hist[pl.angle_flags >> 12]++;
                    map_flag_hist[pl.angle_flags >> 12]++;
                    map_places++;
                    if (Q2_POP_PLACE_ANGLE(pl.angle_flags) == 0)
                        place_flag_yaw_zero[pl.angle_flags >> 12]++;
                    for (i = 0; i < 3; i++)
                        if (q2_pop_place_allows_skill(pl.angle_flags, (s32)i))
                            place_allowed[i]++;

                    if (!e) {
                        int seen = 0, u;
                        for (u = 0; u < unknown_count; u++)
                            if (unknown_id[u] == pl.id)
                                seen = 1;
                        if (!seen && unknown_count < 128)
                            unknown_id[unknown_count++] = pl.id;
                        continue;
                    }
                    resolved++;
                    per_record[(u32)(e - table->def)]++;

                    if (e->shadow_vertex_count) {
                        shadow_places++;
                        if (!first_shadow_path[0]) {
                            snprintf(first_shadow_path,
                                     sizeof(first_shadow_path), "%s",
                                     file->path);
                            snprintf(first_shadow_group,
                                     sizeof(first_shadow_group), "%s",
                                     grp.name);
                            snprintf(first_shadow_model,
                                     sizeof(first_shadow_model), "%s",
                                     e->model);
                            first_shadow_place = pl;
                        }
                    }

                    if (e->effect == 0)
                        scenery++;
                    else if (!q2_item_effect_is_live(table, e->effect))
                        inert++;
                    else
                        live++;

                    if (have_bank &&
                        map_has_model(d, &bank, file->path, e->model))
                        with_model++;
                }
            }
        }
        if (map_places) {
            unsigned marker_places = 0;

            place_maps++;
            for (g = 0; g < 16; g++)
                if (g & 1u)
                    marker_places += map_flag_hist[g];
            if (marker_places == map_places)
                marker_all_maps++;
            else if (marker_places == 0)
                marker_none_maps++;
            else
                marker_mixed_maps++;
        }
        q2_common_close(&cf);
        q2_buf_free(&buf);
    }

    printf("\n  place records                            : %llu\n", places);
    printf("  resolving to a table record              : %llu\n", resolved);
    printf("  whose model the same map ships            : %llu\n", with_model);
    printf("  carrying a live effect                   : %llu\n", live);
    printf("  carrying an effect with no handler       : %llu\n", inert);
    printf("  pure scenery (effect 0)                  : %llu\n", scenery);
    printf("  carrying posed shadow vertices          : %llu\n", shadow_places);
    if (first_shadow_path[0])
        printf("    first: %s / %s / %s at (%d,%d,%d) yaw %u\n",
               first_shadow_path, first_shadow_group, first_shadow_model,
               first_shadow_place.x, first_shadow_place.y,
               first_shadow_place.z,
               Q2_POP_PLACE_ANGLE(first_shadow_place.angle_flags));
    printf("  angle field >= 4096 after masking 0xFFF  : %llu\n",
           angle_over_4096);
    printf("  place records with bits 12..15 set       : %llu\n", upper_bits);
    printf("  place upper-nibble histogram (all / yaw0):\n");
    for (i = 0; i < 16; i++) {
        if (place_flag_hist[i])
            printf("    %X000  %4llu / %4llu\n", i,
                   place_flag_hist[i], place_flag_yaw_zero[i]);
    }
    printf("  bit 12 by map (all / none / mixed)       : %u / %u / %u"
           " of %u\n", marker_all_maps, marker_none_maps,
           marker_mixed_maps, place_maps);
    printf("  allowed by skill easy / medium / hard   : %llu / %llu / %llu\n",
           place_allowed[0], place_allowed[1], place_allowed[2]);

    if (unknown_count) {
        int u;
        printf("  place ids naming no record               :");
        for (u = 0; u < unknown_count; u++)
            printf(" %u", unknown_id[u]);
        printf("\n");
    }

    printf("\n  records no map ever places:");
    {
        int printed = 0;
        for (i = 0; i < table->count; i++)
            if (!per_record[i]) {
                printf(" %s", table->def[i].model);
                printed++;
            }
        if (!printed)
            printf(" (none)");
        printf("\n");
    }

    printf("\n%s\n",
           (places && resolved == places && with_model == resolved && !bad)
           ? "PASS - every place record on the disc resolves to an item whose "
             "model the map ships."
           : "PARTIAL - see the counts above.");

    return (places && resolved == places && !bad) ? 0 : 1;
}
