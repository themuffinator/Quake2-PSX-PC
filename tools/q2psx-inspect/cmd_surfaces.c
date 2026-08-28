/*
 * cmd_surfaces.c — surface flags, blend selection and draw order, checked
 * against every zone on the disc.
 *
 * Three claims are under test here, and each one is a claim a census can
 * falsify rather than merely illustrate:
 *
 *   1. Scene.flags08 is four fields (surface.h). If the reading is right then
 *      the on-disc values must all be drawable — bit 15 clear, draw variant
 *      never 3, object field always zero — because the other states are ones
 *      only the runtime reaches.
 *   2. MapMod.clut's low byte carries a subdivision permission in bits 2-5,
 *      not six bits of residue. If so, the nodes that select draw variant 2 —
 *      the only variant that reads it — should be the ones whose polygons
 *      actually set it.
 *   3. SortData is a self-describing bit stream (sortdata.h). A wrong reading
 *      of a bit stream does not degrade gracefully: it produces node indices
 *      outside the zone, or never terminates. So "every stream terminates on
 *      its own end opcode with every node index in range" is a real test.
 */
#include "cmd_surfaces.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "exe.h"
#include "level.h"
#include "rotator.h"
#include "scene.h"
#include "sortdata.h"
#include "surface.h"
#include "userfuncs.h"
#include "vram.h"

/* "/Q2DATA/LEVELS/BASE0/ZONE0.DAT" -> "BASE0". NULL if the path is not one. */
static const char *map_name_of(const char *path, char *buf, size_t cap)
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

/* "/Q2DATA/LEVELS/BASE0/ZONE0.DAT" -> true, and the basename starts ZONE. */
static bool is_zone_path(const char *path)
{
    const char *base = strrchr(path, '/');

    base = base ? base + 1 : path;
    return strncmp(base, "ZONE", 4) == 0 && strstr(path, "Q2DATA/LEVELS/") != NULL;
}

/* ------------------------------------------------------------------------- */
/* The executable's two tables, read back off the disc                        */
/* ------------------------------------------------------------------------- */
static int check_tables(const disc *d, int *failures)
{
    q2_exe exe;
    int i;
    u32 sim_blend[Q2_SURF_SELECTOR_COUNT];

    if (q2_exe_load(&exe, d, NULL) != Q2_OK) {
        printf("  (executable not loadable; skipping the table check)\n\n");
        return 0;
    }

    printf("codeTable at 0x800AE614, read back off the disc\n  ");
    for (i = 0; i < Q2_SURF_SELECTOR_COUNT; i++) {
        u8 got = 0;

        if (!q2_exe_u8(&exe, 0x800AE614u + (u32)i, &got)) {
            printf("[unmapped] ");
            (*failures)++;
            continue;
        }
        printf("%02X ", got);
        if (got != q2_surf_code_table[i]) {
            printf("\n  MISMATCH at %d: image %02X, port %02X\n",
                   i, got, q2_surf_code_table[i]);
            (*failures)++;
        }
    }
    printf("\n  -> selectors 1..4 are semi-transparent, 0 and 5..7 opaque\n\n");

    /*
     * blendTable lives in BSS, so it cannot be read out of the image — it is
     * built at run time by the loop at 0x80078378. Re-run that loop here rather
     * than trusting the transcription: this is the only way a table the disc
     * does not contain can still be checked against the disc's own code.
     *
     *     for (i = 0; i < 4; i++) tbl[i + 1] = i * 32;
     *     tbl[0] = 32;
     */
    memset(sim_blend, 0, sizeof(sim_blend));
    for (i = 0; i < 4; i++)
        sim_blend[i + 1] = (u32)i * 32u;
    sim_blend[0] = 32;

    printf("blendTable at 0x800B36D8, re-run from the loop at 0x80078378\n  ");
    for (i = 0; i < Q2_SURF_SELECTOR_COUNT; i++) {
        printf("%u ", sim_blend[i]);
        if (sim_blend[i] != q2_surf_blend_table[i]) {
            printf("\n  MISMATCH at %d: loop %u, port %u\n",
                   i, sim_blend[i], q2_surf_blend_table[i]);
            (*failures)++;
        }
    }
    printf("\n  -> ABR %u %u %u %u %u = add, half, add, sub, quarter\n\n",
           sim_blend[0] >> 5, sim_blend[1] >> 5, sim_blend[2] >> 5,
           sim_blend[3] >> 5, sim_blend[4] >> 5);

    q2_exe_free(&exe);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Rotating brush geometry                                                    */
/*                                                                            */
/* Every rotation item on the disc, built through the same code path the      */
/* the runtime uses. The check that means something is the node index: the     */
/* slots are Scene node indices on disc, so if the operand offsets are right   */
/* every one of them must be inside its own map's zones.                      */
/* ------------------------------------------------------------------------- */
static void census_rotators(const disc *d, int *failures)
{
    int i, n = disc_file_count(d);
    unsigned long long maps = 0, rotators = 0;
    unsigned long long axis_hist[4] = {0,0,0,0};
    unsigned long long speed_zero = 0, node_bad = 0;
    s32 speed_min = 0, speed_max = 0;
    bool have_speed = false;

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        q2_buf buf;
        q2_common_file cf;
        q2_events ev;
        q2_userfuncs uf;
        q2_rotator_set set;
        u32 k, max_node = 0;

        if (!strstr(f->path, "COMMON.DAT"))
            continue;
        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        maps++;

        if (q2_events_parse_common(&ev, &cf) != Q2_OK ||
            q2_userfuncs_parse(&uf, &cf) != Q2_OK) {
            q2_common_close(&cf);
            continue;
        }

        if (q2_rotators_build(&set, &ev, &uf, NULL) != Q2_OK) {
            q2_common_close(&cf);
            continue;
        }

        /* The widest zone in this map bounds every Scene node index it can
         * legally name. Reading them all would be slower and no stricter. */
        {
            char mapbuf[64];
            int z;

            if (map_name_of(f->path, mapbuf, sizeof(mapbuf))) {
                for (z = 0; z < 8; z++) {
                    char zpath[256];
                    q2_buf zbuf;
                    q2_zone_file zf;
                    q2_scene sc;

                    snprintf(zpath, sizeof(zpath),
                             "Q2DATA/LEVELS/%s/ZONE%d.DAT", mapbuf, z);
                    if (disc_read_file(d, zpath, &zbuf) != Q2_OK)
                        continue;
                    if (q2_zone_open(&zf, &zbuf) == Q2_OK) {
                        if (q2_scene_parse(&sc, &zf) == Q2_OK &&
                            sc.node_count > max_node)
                            max_node = sc.node_count;
                        q2_zone_close(&zf);
                    } else {
                        q2_buf_free(&zbuf);
                    }
                }
            }
        }

        for (k = 0; k < set.count; k++) {
            const q2_rotator *r = &set.rotators[k];

            rotators++;
            axis_hist[r->axis & 3]++;
            if (r->speed == 0)
                speed_zero++;

            if (!have_speed) {
                speed_min = speed_max = r->speed;
                have_speed = true;
            } else {
                if (r->speed < speed_min) speed_min = r->speed;
                if (r->speed > speed_max) speed_max = r->speed;
            }

            if (max_node && (r->node < 0 || (u32)r->node >= max_node))
                node_bad++;
        }

        q2_rotators_free(&set);
        q2_common_close(&cf);
    }

    printf("Rotating geometry — every SIMROT, SIMROT2, ROTHATCH and ROTBUTTON\n");
    printf("  maps scanned        %llu\n", maps);
    printf("  rotators built      %llu\n", rotators);
    printf("  axis  X %llu  Y %llu  Z %llu  (3 = unused) %llu\n",
           axis_hist[0], axis_hist[1], axis_hist[2], axis_hist[3]);
    if (have_speed)
        printf("  angular speed       %d .. %d, zero on %llu\n",
               speed_min, speed_max, speed_zero);
    printf("  node index out of range %llu\n\n", node_bad);

    /*
     * The falsifiable part. If item+12..+18 were not the object slots, the
     * values there would not be plausible Scene node indices for the map that
     * declares them.
     */
    if (node_bad != 0) {
        printf("  FAIL  a SIMROT slot names a node no zone of its map has;\n"
               "        the operand offsets are wrong\n");
        (*failures)++;
    }
}

/* ------------------------------------------------------------------------- */
int cmd_surfaces(disc *d)
{
    int i, n = disc_file_count(d);
    int failures = 0;

    unsigned long long nodes = 0, polys = 0;
    unsigned long long flag_hist[8] = {0,0,0,0,0,0,0,0};
    unsigned long long variant_hist[4] = {0,0,0,0};
    unsigned long long nodraw = 0, deferred = 0, object_set = 0;
    unsigned long long flags_unexpected = 0;

    unsigned long long semi = 0, subdiv_tagged = 0, clut_bits67 = 0;
    unsigned long long tagged_in_v2 = 0, tagged_elsewhere = 0;
    unsigned long long polys_in_v2 = 0;

    unsigned long long zones = 0, zones_with_sort = 0;
    unsigned long long streams_ok = 0, streams_overrun = 0, streams_bad_node = 0;
    unsigned long long sort_nodes = 0, sort_entities = 0;
    unsigned long long sort_node_max = 0, sort_node_bad = 0, sort_bytes = 0;

    /* Sealing geometry: the all-CLUT-0 nodes, and whether any stream draws one. */
    unsigned long long sealing_nodes = 0, sealing_polys = 0, sealing_odd_flags = 0;
    unsigned long long sealing_testable = 0, sealing_drawn = 0;
    unsigned long long other_nodes = 0, other_drawn = 0;
    unsigned long long clut_zero = 0, clut_zero_mixed = 0;
    u8 *sealing = NULL, *drawn = NULL;

    printf("Surface flags, blend selection and draw order\n");
    printf("============================================\n\n");

    check_tables(d, &failures);
    census_rotators(d, &failures);

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        q2_buf buf;
        q2_zone_file zf;
        q2_scene scene;
        q2_sortdata sd;
        u32 node;

        if (!is_zone_path(f->path))
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

        /*
         * Which nodes a draw stream ever names. Allocated per zone because the
         * answer is what decides whether "sealing geometry" is a real category
         * or a coincidence of the palette field; see the report below.
         */
        sealing = (u8 *)calloc(scene.node_count ? scene.node_count : 1, 1);
        drawn   = (u8 *)calloc(scene.node_count ? scene.node_count : 1, 1);
        if (!sealing || !drawn) {
            free(sealing);
            free(drawn);
            q2_zone_close(&zf);
            continue;
        }

        for (node = 0; node < scene.node_count; node++) {
            q2_scene_node sn;
            q2_mapmod_rec rec;
            q2_surf_variant variant;
            u32 p;

            if (!q2_scene_get_node(&scene, node, &sn))
                continue;

            nodes++;
            variant = q2_scene_flags_variant(sn.flags);
            variant_hist[variant]++;

            if (q2_scene_flags_nodraw(sn.flags))   nodraw++;
            if (q2_scene_flags_deferred(sn.flags)) deferred++;
            if (q2_scene_flags_object(sn.flags) >= 0) object_set++;

            /* The eight values the format notes record. Anything else would
             * mean the field has states this census has never seen. */
            switch (sn.flags) {
            case 0x0000: flag_hist[0]++; break;
            case 0x0400: flag_hist[1]++; break;
            case 0x0800: flag_hist[2]++; break;
            case 0x1000: flag_hist[3]++; break;
            case 0x1400: flag_hist[4]++; break;
            case 0x4000: flag_hist[5]++; break;
            case 0x4400: flag_hist[6]++; break;
            case 0x4800: flag_hist[7]++; break;
            default:     flags_unexpected++; break;
            }

            if (!q2_scene_get_mapmod(&scene, node, &rec))
                continue;

            if (q2_mapmod_rec_is_sealing(&rec)) {
                sealing[node] = 1;
                sealing_nodes++;
                sealing_polys += rec.num_polys;
                if (sn.flags != 0x0000)
                    sealing_odd_flags++;
            }

            for (p = 0; p < rec.num_polys; p++) {
                q2_mapmod_poly poly;
                bool tagged;

                if (!q2_mapmod_get_poly(&rec, p, &poly))
                    continue;

                polys++;
                if (q2_mapmod_clut_semi(poly.clut))
                    semi++;
                if (q2_mapmod_clut_index(poly.clut) == 0) {
                    clut_zero++;
                    if (!sealing[node])
                        clut_zero_mixed++;
                }
                if ((poly.clut & 0xC0u) != 0)
                    clut_bits67++;

                tagged = q2_surf_poly_may_subdivide(poly.clut);
                if (tagged)
                    subdiv_tagged++;

                if (variant == Q2_SURF_VARIANT_TAGGED) {
                    polys_in_v2++;
                    if (tagged)
                        tagged_in_v2++;
                } else if (tagged) {
                    tagged_elsewhere++;
                }
            }
        }

        /* ----------------------------------------------------------------- */
        /* SortData                                                          */
        /* ----------------------------------------------------------------- */
        if (q2_sortdata_parse(&sd, &zf) == Q2_OK && sd.size >= 8) {
            u32 offset = 0;

            zones_with_sort++;
            sort_bytes += sd.size;

            /*
             * TILING, which is what makes this a test rather than an anecdote.
             *
             * The start offset of a stream lives in a per-viewport runtime
             * record the disc does not carry, so it cannot simply be looked up.
             * But if the chunk is a concatenation of self-delimiting streams,
             * then starting at zero, running to the end opcode, rounding up to
             * the next byte and starting again must tile the whole chunk — and
             * every node index in every stream must be inside the zone.
             *
             * That is a strong claim about a bit-packed format: one wrong field
             * width anywhere desynchronises the reader, and a desynchronised
             * reader does not land back on a byte boundary with a valid header
             * hundreds of times in a row. Whatever this reports is worth more
             * than the single-stream case, which ends after a handful of items
             * and proves almost nothing.
             */
            while (offset < sd.size) {
                q2_sort_reader r;
                q2_sort_item it;
                unsigned long long guard = 0;
                bool bad = false;
                u32 end_bit;

                if (!q2_sort_begin(&r, &sd, offset, Q2_SORT_BUCKET_START)) {
                    streams_overrun++;
                    break;
                }

                while (q2_sort_next(&r, &it)) {
                    if (++guard > 1000000ull)
                        break;

                    if (it.kind == Q2_SORT_NODE) {
                        sort_nodes++;
                        if (it.node > sort_node_max)
                            sort_node_max = it.node;
                        if (it.node >= scene.node_count) {
                            bad = true;
                            sort_node_bad++;
                        } else {
                            drawn[it.node] = 1;
                        }
                    } else if (it.kind == Q2_SORT_ENTITY) {
                        sort_entities++;
                        q2_sort_entity_resolve(&r, false);
                    }
                }

                if (r.overrun) {
                    streams_overrun++;
                    break;
                }
                if (bad)
                    streams_bad_node++;
                else
                    streams_ok++;

                end_bit = q2_sort_bit_position(&r);
                offset  = (end_bit + 7u) / 8u;

                /* A stream that consumed nothing would loop for ever. */
                if (offset <= (end_bit / 8u) && offset == 0)
                    break;
            }

            /* Cross the two: is being sealing geometry the same thing as never
             * being drawn? A zone with no streams cannot answer, so this only
             * counts where the chunk was actually read. */
            for (node = 0; node < scene.node_count; node++) {
                if (sealing[node]) {
                    sealing_testable++;
                    if (drawn[node]) sealing_drawn++;
                } else {
                    other_nodes++;
                    if (drawn[node]) other_drawn++;
                }
            }
        }

        free(sealing);
        free(drawn);
        q2_zone_close(&zf);
    }

    /* --------------------------------------------------------------------- */
    printf("Scene.flags08 over %llu nodes in %llu zones\n", nodes, zones);
    printf("  0x0000 %6llu   0x0400 %6llu   0x0800 %6llu   0x1000 %6llu\n",
           flag_hist[0], flag_hist[1], flag_hist[2], flag_hist[3]);
    printf("  0x1400 %6llu   0x4000 %6llu   0x4400 %6llu   0x4800 %6llu\n",
           flag_hist[4], flag_hist[5], flag_hist[6], flag_hist[7]);
    printf("  outside that set: %llu\n\n", flags_unexpected);

    printf("  draw variant  0 subdivide %6llu   1 flat %6llu\n",
           variant_hist[0], variant_hist[1]);
    printf("                2 tagged    %6llu   3 hidden %5llu\n",
           variant_hist[2], variant_hist[3]);
    printf("  bit 15 nodraw   %llu\n", nodraw);
    printf("  bit 14 deferred %llu\n", deferred);
    printf("  object bound    %llu\n\n", object_set);

    /*
     * Three things the reading predicts, all of them falsifiable. A hide flag
     * and a hidden variant are runtime states, so an authored map must never
     * carry them; and the object field is filled in by the script at load, so
     * it must be empty on disc. Any of these being non-zero would mean the
     * field boundaries are wrong.
     */
    if (nodraw != 0) {
        printf("  FAIL  bit 15 is set on disc; it should be runtime-only\n");
        failures++;
    }
    if (variant_hist[3] != 0) {
        printf("  FAIL  draw variant 3 occurs on disc; it should be runtime-only\n");
        failures++;
    }
    if (object_set != 0) {
        printf("  FAIL  the object field is non-zero on disc; the loader clears it\n");
        failures++;
    }
    if (flags_unexpected != 0) {
        printf("  NOTE  %llu nodes carry a flags value outside the known set\n",
               flags_unexpected);
    }

    printf("MapMod.clut low byte over %llu polygons\n", polys);
    printf("  bits 0-1 semi-transparent      %llu (%.1f%%)\n",
           semi, polys ? 100.0 * (double)semi / (double)polys : 0.0);
    printf("  bits 2-5 subdivision gate      %llu (%.1f%%)\n",
           subdiv_tagged, polys ? 100.0 * (double)subdiv_tagged / (double)polys : 0.0);
    printf("  bits 6-7 set (no reader known) %llu\n\n", clut_bits67);

    /*
     * The discriminating measurement, and it comes out AGAINST the bits having
     * been authored. Only draw variant 2 reads them, so a deliberate permission
     * would concentrate in the nodes that select it. Residue would not.
     */
    {
        double in_rate  = polys_in_v2
            ? 100.0 * (double)tagged_in_v2 / (double)polys_in_v2 : 0.0;
        double out_rate = (polys - polys_in_v2)
            ? 100.0 * (double)tagged_elsewhere / (double)(polys - polys_in_v2) : 0.0;

        printf("  where the gate bits actually fall:\n");
        printf("    in variant-2 nodes  %llu of %llu (%.1f%%)\n",
               tagged_in_v2, polys_in_v2, in_rate);
        printf("    elsewhere           %llu of %llu (%.1f%%)\n",
               tagged_elsewhere, polys - polys_in_v2, out_rate);

        if (in_rate <= out_rate) {
            printf("    -> NOT concentrated where they are read. The engine does\n"
                   "       read them (0x800AFBD4), but the exporter did not author\n"
                   "       them for it: they are residue the engine consults.\n\n");
        } else {
            printf("    -> concentrated where they are read, which is what an\n"
                   "       authored permission would look like.\n\n");
        }
    }

    printf("SortData, tiled end-to-end across every chunk\n");
    printf("  zones with the chunk      %llu of %llu, %llu bytes total\n",
           zones_with_sort, zones, sort_bytes);
    printf("  streams reaching an end   %llu\n", streams_ok);
    printf("  streams that overran      %llu\n", streams_overrun);
    printf("  streams naming a bad node %llu\n", streams_bad_node);
    printf("  node references           %llu (%llu out of range), highest %llu\n",
           sort_nodes, sort_node_bad, sort_node_max);
    printf("  entity records            %llu\n", sort_entities);
    printf("  scene nodes in these zones %llu, so %.2f references per node\n\n",
           nodes, nodes ? (double)sort_nodes / (double)nodes : 0.0);

    /* --------------------------------------------------------------------- */
    /*
     * Sealing geometry — the nodes the streams leave out.
     *
     * A node whose every polygon binds CLUT index 0 draws as opaque black,
     * because index 0 is one of the sixteen reserved all-0x8000 palettes. If
     * such nodes were ordinary surface the streams would name them at the same
     * rate as anything else. The comparison below is the whole argument, and it
     * needs both halves: "no stream names them" means nothing without "streams
     * name nearly everything else".
     */
    printf("Sealing geometry — nodes whose every polygon binds CLUT index 0\n");
    printf("  such nodes                %llu, holding %llu polygons\n",
           sealing_nodes, sealing_polys);
    printf("  polygons on CLUT index 0  %llu, of which in a mixed node %llu\n",
           clut_zero, clut_zero_mixed);
    printf("  their Scene flags are not 0x0000 on %llu of them\n", sealing_odd_flags);
    printf("  named by some SortData stream:\n");
    printf("    sealing nodes           %llu of %llu (%.1f%%)\n",
           sealing_drawn, sealing_testable,
           sealing_testable ? 100.0 * (double)sealing_drawn / (double)sealing_testable : 0.0);
    printf("    every other node        %llu of %llu (%.1f%%)\n\n",
           other_drawn, other_nodes,
           other_nodes ? 100.0 * (double)other_drawn / (double)other_nodes : 0.0);

    /*
     * Two claims, both falsifiable. If a polygon anywhere mixed index 0 with a
     * real palette then "sealing" would be a property of polygons rather than
     * of nodes, and the renderer's node-level gate would drop real surface. If
     * any stream named a sealing node then the console draws them after all and
     * the port must not filter them.
     */
    if (clut_zero_mixed != 0) {
        printf("  FAIL  %llu polygons bind CLUT index 0 inside a node that also\n"
               "        carries real palettes; the marker is not per-node\n",
               clut_zero_mixed);
        failures++;
    }
    if (sealing_drawn != 0) {
        printf("  FAIL  %llu sealing nodes ARE named by a draw stream; the console\n"
               "        draws them and world.c must not filter them out\n",
               sealing_drawn);
        failures++;
    }

    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
