/*
 * cmd_screen.c — the screen reconstruction, checked against the disc.
 *
 * `src/screen/screen.[ch]` claims a great many literal numbers: that the
 * framebuffer is 512 x 248, that the ordering table has 217 entries carved into
 * four 51-entry viewport slices and an eleven-entry overlay slice, that the
 * quad split puts its top-right viewport at x = 257, that the vertical split's
 * projection distance is 175 while the horizontal split's is 160. Every one of
 * those came out of a single MIPS instruction in the boot executable.
 *
 * So rather than assert them in a comment, this reads that instruction back off
 * a real disc and compares its immediate field. Nothing here needs a
 * disassembler: each check names an address whose instruction is known to be an
 * `addiu rt, rs, imm` (or one `slti`), and the immediate is the low sixteen
 * bits, sign-extended. If a transcription is wrong, or a different build moves
 * a function, the command fails and says which number disagreed.
 *
 * The negative results are checked too, because they are the load-bearing part
 * of two answered questions:
 *
 *   - the 512 x 256 display envs at 0x8006DFF8 are followed four instructions
 *     later by a call to the 512 x 248 setup that overwrites them, which is why
 *     the game never displays 512 x 256 (openquestions #19);
 *   - the `VSync(3)` at 0x80069188 sits between `CdlSetmode(0x80)` and the
 *     drive bring-up that follows it, so it is a settling delay and not a frame
 *     rate (openquestions #20).
 */
#include "cmd_screen.h"

#include "entity.h"
#include "exe.h"
#include "hud.h"          /* the flash's own colours and mode — hud.h owns the
                           * raise, screen.h owns the tile */
#include "screen.h"
#include "vram.h"
#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
typedef struct check {
    u32         addr;      /* the instruction the constant lives in           */
    s32         expect;    /* what src/screen says it is                      */
    const char *what;
} check;

/* The immediate of an I-type instruction, sign-extended as the hardware does. */
static bool imm_at(const q2_exe *e, u32 addr, s32 *out)
{
    u32 word;

    if (!q2_exe_u32(e, addr, &word))
        return false;

    *out = (s32)(s16)(u16)(word & 0xFFFFu);
    return true;
}

static int g_total;

static int run_checks(const q2_exe *e, const char *title,
                      const check *list, size_t n, int *failed)
{
    size_t i;
    int bad = 0;

    g_total += (int)n;

    printf("\n%s\n", title);

    for (i = 0; i < n; i++) {
        s32 got;

        if (!imm_at(e, list[i].addr, &got)) {
            printf("  %08X  %-44s  UNMAPPED\n", list[i].addr, list[i].what);
            bad++;
            continue;
        }

        if (got == list[i].expect) {
            printf("  %08X  %-44s  %6d  ok\n",
                   list[i].addr, list[i].what, got);
        } else {
            printf("  %08X  %-44s  %6d  MISMATCH (port says %d)\n",
                   list[i].addr, list[i].what, got, list[i].expect);
            bad++;
        }
    }

    *failed += bad;
    return bad;
}

/* ------------------------------------------------------------------------- */
/* The shift amount of an R-type shift — `sll`, `srl`, `sra`.                  */
/*                                                                             */
/* Only the water effect needs this, and it needs it badly: four of the numbers */
/* that define its shape are shift distances rather than immediates, so a table */
/* of `addiu` operands cannot see them. Getting one wrong changes how far the   */
/* picture moves without changing anything a compiler would complain about.     */
/* ------------------------------------------------------------------------- */
static bool sa_at(const q2_exe *e, u32 addr, s32 *out)
{
    u32 word;

    if (!q2_exe_u32(e, addr, &word))
        return false;
    if ((word >> 26) != 0)                  /* SPECIAL */
        return false;
    if ((word & 0x3Fu) > 0x03u)             /* sll, srl, sra                  */
        return false;

    *out = (s32)((word >> 6) & 0x1Fu);
    return true;
}

/* ------------------------------------------------------------------------- */
/* A `jal` to a known target, encoded as the hardware encodes it. Used for the  */
/* one structural claim that is about control flow rather than a constant.     */
/* ------------------------------------------------------------------------- */
static bool jal_targets(const q2_exe *e, u32 addr, u32 target)
{
    u32 word;

    if (!q2_exe_u32(e, addr, &word))
        return false;
    if ((word >> 26) != 0x03u)          /* jal */
        return false;

    return ((word & 0x03FFFFFFu) << 2) == (q2_exe_norm(target) & 0x0FFFFFFFu);
}

/* ------------------------------------------------------------------------- */
static void print_view(const q2_screen *s, const char *tag,
                       const q2_screen_view *v)
{
    q2_screen_fov fov;

    printf("  %-8s  %4d,%-4d %4dx%-4d  ofs %4d,%-4d  proj %4d  far %5d"
           "  aspect %5d  kick %5d\n",
           tag, v->x, v->y, v->w, v->h, v->ofs_x, v->ofs_y,
           v->proj, v->far_z, v->aspect, v->kick_scale);

    /*
     * What those constants MEAN, which is the thing a table of halfwords cannot
     * show: the field of view they describe, the shape the viewport is actually
     * displayed at once the television has narrowed the pixels, and the ratio
     * between the two. The squeeze is 1.5 in every layout — one projection
     * distance cannot be anamorphic and nothing in the image compensates — so a
     * layout that reported anything else would mean a constant had been
     * mistranscribed.
     */
    q2_screen_view_fov(s, v, &fov);
    printf("            fov %6.1f x %5.1f deg   shown at %5.3f:1"
           "   squeeze %4.2f\n",
           fov.horizontal, fov.vertical, fov.display_aspect, fov.squeeze);
}

static void print_layout(q2_screen *s, q2_screen_layout l, int players)
{
    int i;
    /* "view " plus an int: eleven digits and a terminator, so sixteen was
     * one short of what a hostile view_count could print. */
    char tag[24];

    q2_screen_set_layout(s, l, players);

    printf("\n%s (%d viewport%s)\n",
           q2_screen_layout_name(l), s->view_count,
           s->view_count == 1 ? "" : "s");

    for (i = 0; i < s->view_count; i++) {
        snprintf(tag, sizeof(tag), "view %d", i);
        print_view(s, tag, &s->view[i]);
    }

    printf("  buffers   VRAM (%d,%d) and (%d,%d)%s\n",
           s->disp.buf[0].x, s->disp.buf[0].y,
           s->disp.buf[1].x, s->disp.buf[1].y,
           (s->disp.buf[0].x == s->disp.buf[1].x &&
            s->disp.buf[0].y == s->disp.buf[1].y) ? "  -- single buffered" : "");

    for (i = 0; i < s->view_count; i++) {
        printf("  view %d    ordering table [%3u..%3u], draw env at %3u\n", i,
               (unsigned)(Q2_SCREEN_OT_VIEW_BASE + i * Q2_SCREEN_OT_VIEW_STRIDE),
               (unsigned)(Q2_SCREEN_OT_VIEW_BASE + (i + 1) * Q2_SCREEN_OT_VIEW_STRIDE - 1),
               (unsigned)(Q2_SCREEN_OT_VIEW_BASE + i * Q2_SCREEN_OT_VIEW_STRIDE
                          + Q2_SCREEN_OT_VIEW_ENV));
    }
}

/* ------------------------------------------------------------------------- */
/* Composing one real frame                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The same sequence the client runs and the same sequence 0x800182C8 runs:
 * swap, one background clear, each viewport into its own slice with its own
 * projection distance, then one walk of the table. Every viewport gets the same
 * camera because there is only one of them to place — what this renders is the
 * screen, not a multiplayer session.
 */
static int render_frame(disc *d, q2_screen *s, const char *map, int zone_index,
                        const char *out, bool submerged, bool flash)
{
    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_raster_opts opts;
    psx_vram *vram = NULL;
    q2_world_stats stats;
    q2_world_render render;
    q2_result r;
    int p;

    r = q2_world_load_zone(&zone, d, map, zone_index);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d: %s\n",
                map, zone_index, q2_result_str(r));
        return 1;
    }

    q2_camera_default(&cam, s->disp.width, s->disp.height);
    q2_world_render_init(&render);
    psx_raster_opts_default(&opts);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 300000);
    memset(&gte, 0, sizeof(gte));

    /* Stand at a spawn for this zone, eye height above the feet. */
    {
        char cpath[256];
        q2_buf cbuf;
        bool placed = false;

        snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(d, cpath, &cbuf) == Q2_OK) {
            q2_common_file cf;
            if (q2_common_open(&cf, &cbuf) == Q2_OK) {
                q2_start_pos_list sl;
                if (q2_start_pos_parse(&sl, &cf) == Q2_OK) {
                    u32 k;
                    for (k = 0; k < sl.count; k++) {
                        q2_start_pos sp;
                        if (!q2_start_pos_get(&sl, k, &sp) || sp.zone != zone_index)
                            continue;
                        cam.pos[0] = sp.x;
                        cam.pos[1] = sp.y - 576;   /* Q2PSX_VIEW_STAND */
                        cam.pos[2] = sp.z;
                        cam.yaw    = sp.angle;
                        placed = true;
                        printf("  eye at spawn '%s'\n", sp.name);
                        break;
                    }
                }
                q2_common_close(&cf);
            } else {
                q2_buf_free(&cbuf);
            }
        }
        if (!placed) {
            s32 wmin[3], wmax[3];
            q2_world_bounds(&zone, wmin, wmax);
            cam.pos[0] = (wmin[0] + wmax[0]) / 2;
            cam.pos[1] = (wmin[1] + wmax[1]) / 2;
            cam.pos[2] = (wmin[2] + wmax[2]) / 2;
            printf("  no spawn in this zone; standing at its centre\n");
        }
    }

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    if (vram) {
        q2_vram_section vs;
        if (q2_vram_load(&vs, d, map) == Q2_OK) {
            opts.textures = (q2_vram_upload(&vs, vram) == Q2_OK);
            q2_vram_free(&vs);
        } else {
            opts.textures = false;
        }
    }

    /*
     * The water effect ramps rather than switching on, so one frame submerged
     * looks exactly like one frame dry. Build the frame repeatedly until the
     * amplitude has reached its ceiling — 4096 at 24 per dt unit is fourteen
     * frames of a nominal dt, and twenty is comfortably past it.
     */
    {
        int frames = submerged ? 20 : 1;
        int f;

        s->dt = Q2_SCREEN_DT_NOMINAL;
        for (p = 0; p < s->view_count; p++)
            q2_screen_water_set(s, p, true, submerged);

        for (f = 0; f < frames; f++) {
            /*
             * The damage flash, raised on the LAST frame so it is at full
             * strength in the image — it counts down once per drawn frame, so
             * raising it earlier would compose a nearly-faded one.
             *
             * These are the numbers q2_hud_track would produce for a hit that
             * reached flesh: the health tint at 0x800AE888, mode 2, and the
             * five a solid hit saturates to.
             */
            if (flash && f + 1 == frames)
                for (p = 0; p < s->view_count; p++)
                    q2_screen_flash_set(s, p, q2_hud_flash_health_rgb,
                                        Q2_HUD_FLASH_MAX,
                                        Q2_HUD_FLASH_MODE_SCALE);

            q2_screen_frame_begin(s, &ot);

            s->disp.bg_rgb[0] = 16;
            s->disp.bg_rgb[1] = 16;
            s->disp.bg_rgb[2] = 32;
            s->disp.bg_enable = 1;
            q2_screen_background(s);

            for (p = 0; p < s->view_count; p++) {
                const q2_screen_view *v = &s->view[p];
                q2_screen_view_begin(s, p, &ot, &gte);
                cam.projection = (u16)v->proj;
                cam.far_z      = v->far_z;

                /* The viewport's far distance is also the subdivision
                 * threshold: the same view+264 the original parks at
                 * 0x800B2CCC serves both. */
                render.subdiv_threshold = v->far_z;

                q2_world_build_ot(&zone, &cam, v->w, v->h, &ot, &gte,
                                  &render, &stats);
                q2_screen_view_end(s, &ot);

                if (f + 1 < frames)
                    continue;

                printf("  viewport %d  %4d,%-4d %3dx%-3d  proj %3d  far %4d"
                       "  %u quads, depth %u..%u over %u of %u buckets\n",
                       p, v->x, v->y, v->w, v->h, v->proj, v->far_z,
                       stats.quads_emitted, stats.depth_min, stats.depth_max,
                       stats.buckets_used,
                       Q2_SCREEN_OT_VIEW_STRIDE - (Q2_SCREEN_OT_VIEW_ENV + 1));
                printf("             surfaces: %u semi-transparent,"
                       " %u subdivided, %u nodes hidden, %u deferred\n",
                       stats.quads_semi, stats.quads_subdivided,
                       stats.nodes_hidden, stats.nodes_deferred);
                printf("             culled: %u back-facing, %u sealing nodes\n",
                       stats.quads_rejected_back, stats.nodes_sealing);
                if (submerged)
                    printf("             water: amplitude %d, inset %d,%d,"
                           " %u of %u strip copies spent\n",
                           v->water.amp, v->shake_x, v->shake_y,
                           s->water_moves_used, s->water_moves);
            }

            q2_screen_compose(s, &ot, vram, &opts);
            q2_screen_present(s);
        }
    }

    r = psx_fb_write_ppm(q2_screen_front(s), out);
    if (r == Q2_OK)
        printf("  wrote %s (%d x %d)\n", out,
               s->disp.width, s->disp.height);
    else
        fprintf(stderr, "cannot write %s: %s\n", out, q2_result_str(r));

    free(vram);
    psx_ot_free(&ot);
    q2_world_free_zone(&zone);
    return (r == Q2_OK) ? 0 : 1;
}

static bool layout_by_name(const char *name, q2_screen_layout *out, int *players)
{
    int i;

    *players = 4;
    for (i = 0; i < Q2_SCREEN_LAYOUT_COUNT; i++) {
        if (strcmp(name, q2_screen_layout_name((q2_screen_layout)i)) == 0) {
            *out = (q2_screen_layout)i;
            return true;
        }
    }
    if (strcmp(name, "three") == 0) {
        *out = Q2_SCREEN_LAYOUT_QUAD;
        *players = 3;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
int cmd_screen(disc *d, const char *out, const char *layout_name,
               const char *map, int zone_index)
{
    q2_exe e;
    q2_screen s;
    int failed = 0;
    q2_result r;

    static const check display[] = {
        { 0x800764E0u,   1, "SetVideoMode argument (1 == MODE_PAL)"      },
        { 0x800764F0u, 512, "framebuffer width -> 0x800B2DA0"            },
        { 0x800764FCu, 248, "framebuffer height -> 0x800B2DA2"           },
        { 0x800765E0u, 14128, "double-buffer block base (0x800B3730)"    },
        { 0x8006E190u, 32408, "double-buffer block stride"               },
    };

    static const check envs[] = {
        { 0x800769DCu,   436, "background draw env, offset in the block"  },
        { 0x80076BD8u,   528, "viewport draw envs, first of four"         },
        { 0x8007738Cu,   896, "overlay draw env"                          },
        { 0x800185B0u, 10940, "display env"                               },
        { 0x8001839Cu, 10984, "ordering table"                            },
    };

    static const check table[] = {
        { 0x80018394u,   217, "ordering table entries"                    },
        { 0x80076B10u, 10992, "first viewport slice, offset in the block" },
        { 0x8006E18Cu,    51, "viewport slice length"                     },
        { 0x80077274u, 11808, "overlay slice, offset in the block"        },
    };

    static const check lock[] = {
        { 0x80018974u,  2, "VSync divisor, the frame lock"                },
        { 0x800184B8u, 31, "dt clamp test"                                },
        { 0x800184C0u, 30, "dt clamp value"                               },
        { 0x8006918Cu,  3, "VSync(3): CD bring-up settle, not a frame rate" },
    };

    static const check l_one[] = {
        { 0x80077D2Cu,    1, "viewports"                                  },
        { 0x80077E50u,  160, "projection distance"                        },
        { 0x80077E58u, 6400, "far distance"                               },
        { 0x80077D98u, 8192, "depth scale"                                },
        { 0x80077E60u,   93, "pad a"                                      },
        { 0x80077E6Cu,  201, "pad b"                                      },
    };

    static const check l_full[] = {
        { 0x80077660u,    1, "viewports"                                  },
        { 0x8007766Cu,  320, "projection distance"                        },
        { 0x80077674u, 6400, "far distance"                               },
        { 0x800775C4u, 8192, "depth scale"                                },
        { 0x8007767Cu,   93, "pad a"                                      },
        { 0x80077688u,  201, "pad b"                                      },
    };

    static const check l_two_h[] = {
        { 0x80077954u,    2, "viewports"                                  },
        { 0x80077948u,  160, "projection distance, and the 2D height"     },
        { 0x800779BCu,  320, "2D width"                                   },
        { 0x80077A20u, 6400, "far distance"                               },
        { 0x80077A68u, 6144, "depth scale"                                },
        { 0x80077A70u,   95, "pad b"                                      },
        { 0x80077ACCu,  121, "second viewport y"                          },
    };

    static const check l_two_v[] = {
        { 0x80077B3Cu,    2, "viewports"                                  },
        { 0x80077C18u,  175, "projection distance"                        },
        { 0x80077C20u, 6400, "far distance"                               },
        { 0x80077C64u, 6144, "depth scale"                                },
        { 0x80077C9Cu,  -32, "pad b, as framebuffer height minus 32"      },
        { 0x80077CECu,  256, "second viewport x"                          },
    };

    static const check l_quad[] = {
        { 0x8007776Cu,    4, "viewports"                                  },
        { 0x80077794u,  256, "viewport width"                             },
        { 0x8007779Cu,  123, "viewport height"                            },
        { 0x80077810u,  160, "projection distance"                        },
        { 0x80077818u, 4000, "far distance"                               },
        { 0x8007786Cu, 6144, "depth scale"                                },
        { 0x800778BCu,    1, "the one-pixel inset"                        },
        { 0x800778C8u,  257, "right column x"                             },
        { 0x800778D8u,  124, "bottom row y"                               },
        { 0x8007773Cu, 3920, "the view array, five records of 784"        },
        { 0x800778B0u,  784, "view record stride"                         },
    };

    static const check l_overlay[] = {
        { 0x80077F8Cu,  320, "projection distance"                        },
        { 0x80077EF4u, 8192, "view kick scale"                            },
    };

    /* The per-frame build, 0x800780C0: which globals decide the clear, and
     * which field of a view record each arm of the decision writes. */
    static const check build[] = {
        { 0x80078114u, 11668, "suppress-clear flag (0x800B2D94)"          },
        { 0x8007815Cu, 18712, "background-enable flag (gp+18712)"         },
        { 0x80078140u,   260, "isbg cleared on the suppressed path"       },
        { 0x80078190u,   260, "isbg cleared after the background is armed" },
        { 0x80078150u,   784, "view record stride in the isbg loop"       },
        { 0x800769DCu,   436, "the background env, in the buffer's block"  },
        { 0x800769F4u,   460, "its isbg, at +24 of that env"              },
        { 0x80076A58u, 10988, "and it links at OT[1]"                     },
    };

    /* The per-viewport draw, 0x80076A74, field by field. */
    static const check viewdraw[] = {
        { 0x80076AE4u,   780, "the shake, read unaligned from view+780"    },
        { 0x80076B04u,   274, "viewport width"                            },
        { 0x80076B10u, 10992, "slice base"                                },
        { 0x80076B70u,   266, "geometry offset x"                         },
        { 0x80076B8Cu,   262, "projection distance"                       },
        { 0x80076BA4u,   264, "far distance"                              },
        { 0x80076BB8u,     3, "the rounding bias on far/4"                },
        { 0x80076BD8u,   528, "the viewport's draw env"                   },
        { 0x80076C58u,   552, "its isbg, at +24 of that env"              },
        { 0x80076CE8u,   144, "the flags halfword"                        },
        { 0x80076CF0u,     1, "and bit 0 is the world-draw gate"          },
        { 0x80076D3Cu,     4, "the env links at slice bucket 1"           },
    };

    /* The damage flash, 0x80076764. */
    static const check flash[] = {
        { 0x8007677Cu, 10876, "the flash tile block"                      },
        { 0x80076788u,   676, "strength"                                  },
        { 0x800767C0u,   678, "what it is scaled against"                 },
        { 0x800767C4u,   680, "the mode"                                  },
        { 0x800767A8u,     2, "the semi-transparency bit"                 },
        { 0x80076958u,   274, "the tile takes the viewport's width"       },
        { 0x80076968u,   200, "and links at slice bucket 50"              },
        { 0x80076988u,    -1, "strength counts down once per drawn frame"  },
    };

    /* The overlay camera, 0x80077230. */
    static const check overlay[] = {
        { 0x80077274u, 11808, "its slice"                                 },
        { 0x8007735Cu, 11680, "the framebuffer width it adds on buffer 1"  },
        { 0x8007738Cu,   896, "its draw env"                              },
        { 0x80077414u,   924, "the env packet inside it"                  },
        { 0x800773B0u,   920, "its isbg"                                  },
        { 0x80077438u,     4, "and it links at its slice's bucket 1"      },
    };

    /* The performance meter, 0x80076E88 — nine bars at OT[52]. */
    static const check meter[] = {
        { 0x80076EB0u, 21236, "the bar packets"                           },
        { 0x80076E98u,  1612, "the meter's base y (gp+1612)"              },
        { 0x80076ED0u,     2, "bar width"                                 },
        { 0x80076EC4u,    20, "bar 0 column"                              },
        { 0x80076F1Cu,    23, "bar 1 column"                              },
        { 0x80076F7Cu,    28, "bar 2 column"                              },
        { 0x80076FD8u,    31, "bar 3 column"                              },
        { 0x80077030u,    34, "bar 4 column"                              },
        { 0x80077088u,    37, "bar 5 column"                              },
        { 0x800770ECu,    40, "bar 6 column"                              },
        { 0x80077144u,    43, "bar 7 column"                              },
        { 0x8007719Cu,    46, "bar 8 column"                              },
        { 0x80076F14u, 11192, "all nine link at OT[52]"                   },
    };

    /*
     * The water effect, 0x80062DF0 — the ramp, the two phases, the shake it
     * derives, the strip copies and the tint. The pool it draws out of is named
     * "Water Moves" in the allocator table at 0x800ACF7C, which is how the
     * effect was identified rather than guessed at.
     */
    static const check water[] = {
        { 0x80062E64u, 11180, "its OT bucket: db + 11180, slice bucket 49"   },
        { 0x80062E60u,   288, "the viewport's owner, at view+288"            },
        { 0x80062E70u,   152, "and its flag word, at entity+152"             },
        { 0x80062E7Cu,     1, "one bit of it, after a shift of 8 -> 0x100"   },
        { 0x80062E9Cu,   776, "the amplitude, at view+776"                   },
        { 0x80062EB4u,  4097, "the ramp's clamp test"                        },
        { 0x80062EBCu,  4096, "and the value it clamps to"                   },
        { 0x80062F0Cu,   780, "amplitude zero clears view+780"               },
        { 0x80062F14u,     4, "four bytes of it — both shake halfwords"      },
        { 0x80062F18u,   772, "and both phases: view+772"                    },
        { 0x80062F20u,   774, "view+774"                                     },
        { 0x80062F8Cu,   782, "shake y is written to view+782"               },
        { 0x80062F90u,   780, "shake x to view+780"                          },
        { 0x80062FA8u, 11680, "the framebuffer width it adds on buffer 1"    },
        { 0x8006301Cu, 21552, "the {sin,cos} table at 0x800A5430"            },
        { 0x80063024u,     2, "and it reads +2 of an entry — the COSINE"     },
        { 0x80063100u,   274, "the column pass walks the viewport's width"   },
        { 0x80063108u,     1, "to w INCLUSIVE, which closes the last run"    },
        { 0x800630FCu,    20, "stepping the phase by 20 per column"          },
        { 0x80063224u,   276, "the row pass walks its height"                },
        { 0x8006322Cu,     1, "to h inclusive"                               },
        { 0x80063220u,    13, "stepping by 13 per row"                       },
        { 0x80063114u,  4095, "both phases mask to the 4096-step circle"     },
        { 0x800630E0u,    24, "a strip packet is 24 bytes — a DR_MOVE"       },
        { 0x80063268u,     4, "the tint tile lives at db+4"                  },
        { 0x80063280u,     2, "with the semi-transparency bit set"           },
        { 0x800632E4u,   274, "and covers the whole viewport"                },
        { 0x80062D10u,    48, "the pool is 48 packets in multiplayer"        },
        { 0x80062D14u,    24, "and 24 otherwise"                             },
        { 0x80062D24u, -12420, "its descriptor: \"Water Moves\" at 0x800ACF7C" },
        { 0x80062D3Cu,  1280, "each packet's tag: 0x0500, five code words"   },
        { 0x80062D40u,   256, "code[0] 0x01000000, flush the texture cache"  },
        { 0x80062D44u, -32768, "code[1] 0x80000000, copy VRAM to VRAM"       },
    };

    /* The four shift distances that are the effect's shape. */
    static const check water_shifts[] = {
        { 0x80062E78u,  8, "the flag tested is bit 8 — UNDERWATER, 0x100"   },
        { 0x80062EA0u,  3, "the ramp is 3 << 3 == 24 per dt unit"           },
        { 0x80062F30u,  2, "horizontal amplitude: amp*4095 * 4 >> 12"       },
        { 0x80062F40u,  1, "vertical amplitude:   amp*4095 * 2 >> 12"       },
        { 0x80062F7Cu, 12, "the shake is that amplitude >> 12"              },
        { 0x80063038u, 24, "a strip offset is (cos * amplitude) >> 24"      },
    };

    /* Bring-up state the port had not read: the boot GTE, the blend table, and
     * the average-Z factors InitGeom leaves behind. */
    static const check boot[] = {
        { 0x80076568u,  1608, "boot SetGeomScreen reads gp+1608"          },
        { 0x8007839Cu,     4, "the blend table fills four slots"          },
        { 0x800783ACu,    32, "and then puts 32 back in slot 0"           },
        { 0x8008E4C4u,   341, "InitGeom leaves ZSF3"                      },
        { 0x8008E4D0u,   256, "InitGeom leaves ZSF4"                      },
        { 0x80038374u,   156, "view+156 is read by the view update, not the sort" },
        { 0x800380ACu,     7, "a live view sets flags bits 0-2"           },
    };

    if (q2_screen_init(&s, Q2_VIDEO_PAL) != Q2_OK) {
        fprintf(stderr, "cannot bring the screen up\n");
        return 1;
    }

    /* The rendering mode does not need the executable — it needs a level. */
    if (out) {
        q2_screen_layout l = Q2_SCREEN_LAYOUT_ONE;
        int players = 1;
        bool submerged = false, hit = false;
        char name[32];

        /*
         * `<layout>[+water][+flash]`. The two screen effects a viewport can be
         * wearing are suffixes rather than positional arguments because they
         * are properties of the frame being composed, not separate modes — and
         * they compose, since on the console a player can perfectly well be
         * shot while under water.
         */
        if (layout_name) {
            const char *plus;
            size_t n;

            for (plus = strchr(layout_name, '+'); plus;
                 plus = strchr(plus + 1, '+')) {
                if (strncmp(plus, "+water", 6) == 0)
                    submerged = true;
                else if (strncmp(plus, "+flash", 6) == 0)
                    hit = true;
                else
                    break;
            }

            n = (size_t)(strcspn(layout_name, "+"));
            if (n >= sizeof(name))
                n = sizeof(name) - 1;
            memcpy(name, layout_name, n);
            name[n] = '\0';

            if (plus || !layout_by_name(name, &l, &players)) {
                fprintf(stderr, "unknown layout '%s'; try one, two-horizontal, "
                                "two-vertical, three, quad, full-single, any of "
                                "them with +water and/or +flash\n", layout_name);
                q2_screen_free(&s);
                return 1;
            }
        }

        q2_screen_set_layout(&s, l, players);
        printf("screen: %s, %d viewport%s, %u x %u%s%s\n",
               q2_screen_layout_name(s.layout), s.view_count,
               s.view_count == 1 ? "" : "s", s.disp.width, s.disp.height,
               submerged ? ", submerged" : "", hit ? ", hit" : "");

        failed = render_frame(d, &s, map ? map : "BASE0", zone_index, out,
                              submerged, hit);
        q2_screen_free(&s);
        return failed;
    }

    r = q2_exe_load(&e, d, NULL);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load the boot executable: %s\n", q2_result_str(r));
        q2_screen_free(&s);
        return 1;
    }

    printf("screen — %s\n", e.name);
    printf("  %u x %u, %u Hz fields, VSync(%u) -> %u Hz logic\n",
           s.disp.width, s.disp.height, s.disp.field_hz, s.disp.vsync_divisor,
           s.disp.field_hz / (s.disp.vsync_divisor ? s.disp.vsync_divisor : 1));

    /*
     * The one thing in this report that is NOT read out of the executable, and
     * it is labelled so. It is what the television does with what the executable
     * emits: every horizontal mode spans the same active line, PAL fills the 4:3
     * raster with 256 of them, so a 512-wide buffer has pixels 2:3. Printed here
     * because the field-of-view lines below are meaningless without it.
     */
    {
        int pn = 1, pd = 1, ww = 0, wh = 0;

        q2_screen_pixel_aspect(&s, &pn, &pd);
        q2_screen_window_size(&s, Q2_SCREEN_FIT_TELEVISION, 1, &ww, &wh);
        printf("  pixel %d:%d on the television (NOT from the image), so the "
               "picture is %d x %d -> %.3f:1\n",
               pn, pd, ww, wh, (double)ww / (double)(wh ? wh : 1));
    }

    run_checks(&e, "display", display, sizeof(display) / sizeof(display[0]), &failed);
    run_checks(&e, "environments, as offsets into one buffer's block",
               envs, sizeof(envs) / sizeof(envs[0]), &failed);
    run_checks(&e, "ordering table", table, sizeof(table) / sizeof(table[0]), &failed);
    run_checks(&e, "frame lock and timing", lock, sizeof(lock) / sizeof(lock[0]), &failed);

    /* 2 + 4*51 + 11 == 217. Worth stating as its own check: it is the reason to
     * believe the slicing is real rather than a plausible reading. */
    printf("\nslice arithmetic\n");
    {
        int total = Q2_SCREEN_OT_VIEW_BASE
                  + Q2_SCREEN_MAX_VIEWS * Q2_SCREEN_OT_VIEW_STRIDE
                  + Q2_SCREEN_OT_OVERLAY_LEN;
        printf("  %d root + env, %d x %d viewport, %d overlay = %d, table is %d  %s\n",
               Q2_SCREEN_OT_VIEW_BASE, Q2_SCREEN_MAX_VIEWS,
               Q2_SCREEN_OT_VIEW_STRIDE, Q2_SCREEN_OT_OVERLAY_LEN,
               total, Q2_SCREEN_OT_ENTRIES,
               total == Q2_SCREEN_OT_ENTRIES ? "ok" : "MISMATCH");
        g_total++;
        if (total != Q2_SCREEN_OT_ENTRIES)
            failed++;
    }

    run_checks(&e, "layout: one",         l_one,     sizeof(l_one) / sizeof(l_one[0]), &failed);
    run_checks(&e, "layout: full-single", l_full,    sizeof(l_full) / sizeof(l_full[0]), &failed);
    run_checks(&e, "layout: two-horizontal", l_two_h, sizeof(l_two_h) / sizeof(l_two_h[0]), &failed);
    run_checks(&e, "layout: two-vertical",   l_two_v, sizeof(l_two_v) / sizeof(l_two_v[0]), &failed);
    run_checks(&e, "layout: quad",        l_quad,    sizeof(l_quad) / sizeof(l_quad[0]), &failed);
    run_checks(&e, "layout: overlay",     l_overlay, sizeof(l_overlay) / sizeof(l_overlay[0]), &failed);

    run_checks(&e, "the frame build (0x800780C0)",
               build, sizeof(build) / sizeof(build[0]), &failed);
    run_checks(&e, "the viewport draw (0x80076A74)",
               viewdraw, sizeof(viewdraw) / sizeof(viewdraw[0]), &failed);
    run_checks(&e, "the damage flash (0x80076764)",
               flash, sizeof(flash) / sizeof(flash[0]), &failed);
    run_checks(&e, "the overlay camera (0x80077230)",
               overlay, sizeof(overlay) / sizeof(overlay[0]), &failed);
    run_checks(&e, "the performance meter (0x80076E88)",
               meter, sizeof(meter) / sizeof(meter[0]), &failed);
    run_checks(&e, "the water effect (0x80062DF0)",
               water, sizeof(water) / sizeof(water[0]), &failed);

    printf("\nthe water effect's shift distances\n");
    {
        size_t i;

        for (i = 0; i < sizeof(water_shifts) / sizeof(water_shifts[0]); i++) {
            s32 got;

            g_total++;
            if (!sa_at(&e, water_shifts[i].addr, &got)) {
                printf("  %08X  %-44s  NOT A SHIFT\n",
                       water_shifts[i].addr, water_shifts[i].what);
                failed++;
            } else if (got == water_shifts[i].expect) {
                printf("  %08X  %-44s  %6d  ok\n",
                       water_shifts[i].addr, water_shifts[i].what, got);
            } else {
                printf("  %08X  %-44s  %6d  MISMATCH (port says %d)\n",
                       water_shifts[i].addr, water_shifts[i].what, got,
                       water_shifts[i].expect);
                failed++;
            }
        }
    }

    run_checks(&e, "bring-up", boot, sizeof(boot) / sizeof(boot[0]), &failed);

    /*
     * The order of a frame, as calls rather than as constants. Each of these is
     * a claim src/screen makes about what happens inside what — the kind of
     * thing a table of immediates cannot check.
     */
    printf("\nthe shape of a frame\n");
    {
        static const struct {
            u32 site, target;
            const char *what;
        } calls[] = {
            { 0x8007816Cu, 0x800769A0u, "the build arms the background clear"   },
            { 0x800781B8u, 0x80076A74u, "the build draws each viewport"         },
            { 0x800781D8u, 0x8006FA9Cu, "and finishes with the overlay path"    },
            { 0x80076CC8u, 0x80076764u, "the viewport draws its damage flash"   },
            { 0x80076CFCu, 0x80066858u, "then the world, under the +144 gate"   },
            { 0x80076D44u, 0x800B0EE0u, "then links its env with AddPrim"       },
            { 0x8007696Cu, 0x800B0EE0u, "the flash links with AddPrim too"      },
            { 0x80076E68u, 0x80077230u, "the overlay camera is its own draw"    },
            { 0x8007743Cu, 0x800B0EE0u, "and links its own env last"            },
            { 0x80018588u, 0x80076E88u, "the meter is built inside the lock"    },
            { 0x800185ACu, 0x80083BCCu, "PutDispEnv, then"                      },
            { 0x800185BCu, 0x80083990u, "DrawOTag — in that order"              },
            { 0x80076598u, 0x80077454u, "bring-up sets the display envs"        },
            { 0x8007CACCu, 0x80077454u, "and a session start sets them again"   },
            { 0x800765A0u, 0x80078378u, "bring-up fills the blend table"        },
            { 0x800776D4u, 0x8008AD5Cu, "the boot layout writes its own envs"   },
            /*
             * The water effect is per-VIEW and runs in the game update, not in
             * the frame build — which is why it can write the shake the build
             * then reads. Both of its callers take a view record.
             */
            { 0x80038164u, 0x80062DF0u, "the per-view update runs the water"    },
            { 0x800384C0u, 0x80062DF0u, "and so does the camera install"        },
            { 0x800630D0u, 0x800B0EE0u, "each column strip links with AddPrim"  },
            { 0x800631F4u, 0x800B0EE0u, "each row strip too"                    },
            { 0x800632F8u, 0x800B0EE0u, "and the tint goes in last, so it is"
                                        " drawn first"                          },
            { 0x80063270u, 0x8008A798u, "the tint is a TILE (SetTile)"          },
        };
        size_t i;

        for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
            bool ok = jal_targets(&e, calls[i].site, calls[i].target);
            printf("  %08X -> %08X  %-40s  %s\n",
                   calls[i].site, calls[i].target, calls[i].what,
                   ok ? "ok" : "MISMATCH");
            g_total++;
            if (!ok)
                failed++;
        }
    }

    /* The blend table itself, which the port carries as five halfwords. */
    printf("\ntexture-page blend selectors (0x800B36D8)\n");
    {
        static const char *mode[4] = { "B/2+F/2", "B+F", "B-F", "B+F/4" };
        int i;

        for (i = 0; i < Q2_SCREEN_BLEND_SLOTS; i++)
            printf("  selector %d  bits %3u  %s\n",
                   i, (unsigned)q2_screen_blend_word[i],
                   mode[q2_screen_blend_mode((unsigned)i) & 3]);
    }

    /* The two answered questions, as structure rather than as constants. */
    printf("\nanswered questions\n");
    {
        bool overwritten = jal_targets(&e, 0x8006E0C8u, 0x80077540u);
        s32  w = 0, h = 0;
        bool got = imm_at(&e, 0x8006E004u, &w) && imm_at(&e, 0x8006E008u, &h);

        printf("  #19  secondary env is %d x %d, and 0x8006E0C8 calls the "
               "512 x 248 setup: %s\n",
               got ? w : -1, got ? h : -1, overwritten ? "yes" : "NO");
        g_total++;
        if (!overwritten || !got || w != 512 || h != 256)
            failed++;

        printf("  #20  VSync(3) at 0x80069188 follows CdlSetmode: %s\n",
               jal_targets(&e, 0x80069180u, 0x800890B4u) ? "yes" : "NO");
        g_total++;
        if (!jal_targets(&e, 0x80069180u, 0x800890B4u))
            failed++;
    }

    print_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    print_layout(&s, Q2_SCREEN_LAYOUT_TWO_H, 2);
    print_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    print_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 3);
    print_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    print_layout(&s, Q2_SCREEN_LAYOUT_FULL_SINGLE, 1);
    printf("\noverlay camera\n");
    print_view(&s, "overlay", &s.overlay);

    printf("\n%d of %d checks passed\n", g_total - failed, g_total);

    q2_screen_free(&s);
    q2_exe_free(&e);
    return failed ? 1 : 0;
}
