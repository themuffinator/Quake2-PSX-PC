/*
 * levelbin.c — see levelbin.h for the derivation.
 */
#include "levelbin.h"

#include <stdio.h>
#include <string.h>

bool q2_levelbin_names_group(const u8 *module, u32 size, const q2_pop_group *g)
{
    u32 i;
    size_t n;

    if (!module || !g || !g->name[0])
        return false;

    n = strlen(g->name);
    if (n == 0 || n > 12 || size < 12)
        return false;

    /*
     * A whole twelve-byte field, NUL-padded. Anchoring on the padding is what
     * stops `Zone1` matching the front of `Zone1Key` — which matters, because a
     * map that ships both would otherwise have the wrong one selected and the
     * error would look like a spawn bug rather than a string bug.
     *
     * A name that fills the field has no padding to check, which is the same
     * rule the Strings chunk's own reader carries: `FoundASecret` is exactly
     * twelve characters and is not terminated there either.
     */
    for (i = 0; i + 12 <= size; i++) {
        if (memcmp(module + i, g->name, n) != 0)
            continue;
        if (n < 12 && module[i + n] != 0)
            continue;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* The call decode                                                            */
/* ------------------------------------------------------------------------- */
#define OP(w)      ((w) >> 26)
#define RS(w)      (((w) >> 21) & 0x1F)
#define RT(w)      (((w) >> 16) & 0x1F)
#define IMM(w)     ((s16)((w) & 0xFFFF))
#define FUNCT(w)   ((w) & 0x3F)

#define OP_LW      0x23u
#define OP_ADDIU   0x09u
#define FN_JALR    0x09u

/* How far back to look for the instruction that set a register. The scheduler
 * moves loads a long way from their use, but not arbitrarily: BASE1's widest
 * gap between the `addiu` that forms the name pointer and the `jalr` that
 * consumes it is three instructions, and glint_scan's worst case was five. */
#define LOOKBACK   200

u32 q2_levelbin_selected_slot(const u8 *module, u32 size, s32 slot_off,
                              u32 *out, u32 max)
{
    u32 i, n = 0;

    if (!module || !out || size < 12)
        return 0;

    for (i = 0; i + 4 <= size; i += 4) {
        u32 w = q2_rd_u32(module + i);
        u32 target, j;
        bool found = false;
        u32 name_at = 0;

        /* `jalr rD` — the call. */
        if (OP(w) != 0 || FUNCT(w) != FN_JALR)
            continue;
        target = RS(w);

        /* Walk back for `lw rD, slot_off(rB)`, the slot fetch that produced it. A
         * different offset is a different service and not ours. */
        for (j = 4; j <= LOOKBACK && j <= i; j += 4) {
            u32 p = q2_rd_u32(module + i - j);

            if (OP(p) == OP_LW && RT(p) == target) {
                found = (IMM(p) == slot_off);
                break;              /* the nearest load of rD decides */
            }
        }
        if (!found)
            continue;

        /*
         * And back for the `addiu rX, rY, imm` that formed the NAME POINTER.
         *
         * Not `a0`, and that was the first attempt's mistake. BASE1's zone
         * handler at module+0x94 does
         *
         *     801000A0  addiu a3, v0, 12       ; a3 = module + 0xC = "Zone0"
         *     801000A4  lbu   v1, 1(a3)        ; ...and twenty more
         *     801000C4  or    a0, a0, v1       ; assembled into a0/a1/a2
         *
         * so the twelve bytes arrive BY VALUE in three registers and the
         * pointer that produced them is whatever the compiler picked. Taking
         * the nearest `addiu` whose immediate lands on a printable twelve-byte
         * field is what survives that, and it is checkable: the name must be a
         * group the map ships.
         */
        found = false;
        for (j = 4; j <= LOOKBACK && j <= i; j += 4) {
            u32 p = q2_rd_u32(module + i - j);
            s16 off;
            u32 c;
            bool printable = true;

            if (OP(p) != OP_ADDIU)
                continue;

            off = IMM(p);
            if (off <= 0 || (u32)off + 12 > size)
                continue;

            /* A name field: at least one character, then printable or NUL. */
            if (module[off] < 32 || module[off] > 126)
                continue;
            for (c = 0; c < 12; c++) {
                u8 ch = module[off + c];
                if (ch != 0 && (ch < 32 || ch > 126)) {
                    printable = false;
                    break;
                }
            }
            if (!printable)
                continue;

            name_at = (u32)off;
            found   = true;
            break;
        }
        if (!found)
            continue;

        if (n < max)
            out[n++] = name_at;
    }

    return n;
}

u32 q2_levelbin_selected(const u8 *module, u32 size, u32 *out, u32 max)
{
    return q2_levelbin_selected_slot(module, size,
                                     Q2_LEVELBIN_SLOT_SELECT, out, max);
}

/* ------------------------------------------------------------------------- */
/* The map's own MISEVENT table                                               */
/* ------------------------------------------------------------------------- */
static bool misevent_name(const u8 *p, char *out, bool *out_padded)
{
    u32 i;
    bool ended = false;
    u32 n = 0;

    /*
     * Printable, then NUL padding, and the padding is checked: a field that
     * goes back to printable after a NUL is not a name, it is two things that
     * happen to sit next to each other.
     */
    for (i = 0; i < 12; i++) {
        u8 ch = p[i];

        if (ch == 0) {
            ended = true;
            continue;
        }
        if (ended)
            return false;
        if (ch < 32 || ch > 126)
            return false;
        n++;
    }

    if (n < 2)
        return false;

    if (out) {
        memcpy(out, p, 12);
        out[12] = '\0';
    }
    if (out_padded)
        *out_padded = ended;

    return true;
}

/*
 * A record cannot BEGIN in the middle of a printable RUN.
 *
 * This is what separates a field from a substring, and it took two goes.
 * BIGGUN needed the check at all: the backward walk from its real table ran on
 * into `TeleportDeat` and `royGlassZone`, twelve-character windows cut out of
 * longer strings, each followed by something that passed for a handler. But
 * "the preceding byte is not printable" was too blunt — three real tables sit
 * immediately after a function epilogue, and `addiu sp, sp, 32` ends in 0x27,
 * which is an apostrophe. BASE0's `DOCRATES`, COMMAND's `Comp1` and JAIL3's
 * `Bridge` all vanished on that.
 *
 * So it is a RUN, not a byte: text that reaches the field is text, and one or
 * two stray printable bytes out of an instruction word are not.
 */
#define Q2_MISEVENT_TEXT_RUN 3

static bool misevent_starts_field(const u8 *module, u32 at)
{
    u32 run = 0;

    while (run < Q2_MISEVENT_TEXT_RUN && run < at) {
        u8 ch = module[at - 1 - run];

        if (ch < 32 || ch > 126)
            break;
        run++;
    }

    return run < Q2_MISEVENT_TEXT_RUN;
}

static bool misevent_handler(const u8 *p, u32 size, u32 load_base, u32 *out)
{
    u32 h = q2_rd_u32(p + 12);

    /* A handler is code: non-zero, word-aligned, and inside this module. */
    if (h == 0 || (h & 3u) != 0)
        return false;
    if (h < load_base || h >= load_base + size)
        return false;

    if (out)
        *out = h;
    return true;
}

u32 q2_levelbin_misevents(const u8 *module, u32 size, u32 load_base,
                          q2_levelbin_misevent *out, u32 max)
{
    u32 i, n = 0;

    if (!module || size < 20)
        return 0;

    /*
     * Anchored on the TERMINATOR, not on a name.
     *
     * Two earlier anchors both failed, and both failures are worth keeping.
     * Anchoring on "any name-shaped record" walks straight into a module's
     * text: QENDMIS5's is thirty kilobytes of strings, and a twelve-byte window
     * inside `Initialise %` or `MDEC_out_sync` is printable and is followed by
     * something small, so twenty screens were reported that do not exist.
     * Anchoring on "a NUL-PADDED record" fixed that and then missed any table
     * whose names fill the field — BOSS1's begins `LaserButton0`,
     * `LaserButton1` and BIGGUN's is `STOPPLATFORM`, `Destroy Grav`, all four
     * exactly twelve characters, and all four came back as script naming
     * events that do not exist.
     *
     * The zero word is the engine's own marker: 0x8006DB10 stops its search
     * there. So find one, walk BACKWARD while the record shape holds, and take
     * the maximal run. It anchors on structure rather than on how long the
     * level designer's names happen to be.
     */
    for (i = 16; i + 4 <= size; i += 4) {
        u32 head, run = 0, j;

        if (q2_rd_u32(module + i) != 0)
            continue;
        if (!misevent_name(module + i - 16, NULL, NULL))
            continue;
        if (!misevent_handler(module + i - 16, size, load_base, NULL))
            continue;
        if (!misevent_starts_field(module, i - 16))
            continue;

        head = i;
        while (head >= 16 &&
               misevent_name(module + head - 16, NULL, NULL) &&
               misevent_handler(module + head - 16, size, load_base, NULL) &&
               misevent_starts_field(module, head - 16)) {
            head -= 16;
            run++;
        }

        for (j = 0; j < run; j++) {
            const u8 *rec = module + head + j * 16;

            if (n < max && out) {
                misevent_name(rec, out[n].name, NULL);
                misevent_handler(rec, size, load_base, &out[n].handler);
                out[n].offset = head + j * 16;
            }
            n++;
        }
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/* The front end's own menu pages                                             */
/* ------------------------------------------------------------------------- */
#define MENU_REC_SIZE 24u
#define MENU_ROW_MAX   8u

/* A row's text: printable, NUL-terminated, and inside the module. */
static bool menu_text_at(const u8 *module, u32 size, u32 off, char *out)
{
    u32 i;

    if (off >= size)
        return false;

    for (i = 0; i < 31 && off + i < size; i++) {
        u8 ch = module[off + i];

        if (ch == 0)
            break;
        if (ch < 0x20 || ch > 0x7E)
            return false;
    }

    if (i < 3 || off + i >= size || module[off + i] != 0)
        return false;

    memcpy(out, module + off, i);
    out[i] = '\0';
    return true;
}

/* Does a 24-byte record look like a menu row? */
static bool menu_row_at(const u8 *module, u32 size, u32 load_base, u32 off,
                        q2_lb_menu_row *out)
{
    u32 text = q2_rd_u32(module + off);
    s16 x    = q2_rd_s16(module + off + 4);
    s16 y    = q2_rd_s16(module + off + 6);
    u32 act  = q2_rd_u32(module + off + 8);
    char name[32];

    if (text < load_base || text - load_base >= size)
        return false;
    if (!menu_text_at(module, size, text - load_base, name))
        return false;

    /* Every front-end row is centred at 256 and sits on a 480-line screen. */
    if (x != 256 || y < 0 || y > 480)
        return false;

    /* An action, when there is one, is code in this module. A NULL action is
     * legal — the deathmatch setup page's rows have none. */
    if (act != 0 && (act < load_base || act - load_base >= size))
        return false;

    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", name);
        out->x      = x;
        out->y      = y;
        out->action = act;
        out->offset = off;
    }
    return true;
}

u32 q2_levelbin_menu_pages(const u8 *module, u32 size, u32 load_base,
                           q2_lb_menu_page *out, u32 max)
{
    u32 off, n = 0;

    if (!module || size < MENU_REC_SIZE)
        return 0;

    for (off = 0; off + MENU_REC_SIZE <= size; off += 4) {
        u32 run = 0, at;

        if (!menu_row_at(module, size, load_base, off, NULL))
            continue;

        for (at = off;
             at + MENU_REC_SIZE <= size && run < MENU_ROW_MAX;
             at += MENU_REC_SIZE) {
            if (!menu_row_at(module, size, load_base, at, NULL))
                break;
            run++;
        }

        /*
         * A page has at least two rows. One row on its own is far more likely
         * to be a pointer that happens to sit next to a plausible pair of
         * halfwords than a menu, and the front end has no one-row page.
         */
        if (run < 2) {
            continue;
        }

        if (n < max && out) {
            u32 k;

            memset(&out[n], 0, sizeof(out[n]));
            out[n].offset = off;
            out[n].count  = run;
            for (k = 0; k < run; k++)
                menu_row_at(module, size, load_base, off + k * MENU_REC_SIZE,
                            &out[n].row[k]);
        }
        n++;

        off += (run - 1) * MENU_REC_SIZE;   /* the loop's += 4 finishes it */
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/* QENDMIS — the movie table                                                  */
/* ------------------------------------------------------------------------- */
static bool movie_field(const u8 *p, char *out)
{
    u32 i;
    bool ended = false;
    u32 n = 0;

    for (i = 0; i < 12; i++) {
        u8 ch = p[i];

        if (ch == 0) {
            ended = true;
            continue;
        }
        if (ended)
            return false;
        if (ch < 32 || ch > 126) {
            /* The trace labels end in a newline before their padding —
             * `Do Intro\n` — so that one control character is a field
             * character here and nothing else is. */
            if (ch != '\n')
                return false;
        }
        n++;
    }

    if (n < 2)
        return false;

    memcpy(out, p, 12);
    out[12] = '\0';
    return true;
}

/* Does this field end in `.STX`? That is what makes a record a movie rather
 * than three names that happen to sit in a row. */
static bool movie_is_stx(const char *s)
{
    size_t n = strlen(s);

    return n > 4 && memcmp(s + n - 4, ".STX", 4) == 0;
}

u32 q2_levelbin_movies(const u8 *module, u32 size,
                       q2_levelbin_movie *out, u32 max)
{
    u32 i, n = 0;

    if (!module || size < 36)
        return 0;

    for (i = 0; i + 36 <= size; i += 4) {
        char a[13], b[13], f[13];

        if (!movie_field(module + i, a))
            continue;
        if (!movie_field(module + i + 12, b))
            continue;
        if (!movie_field(module + i + 24, f))
            continue;

        /*
         * Anchored on the FILENAME, which is the one field with a shape a
         * coincidence does not have. Three printable twelve-byte fields in a
         * row are common in a module; three of them where the third ends in
         * `.STX` are not.
         */
        if (!movie_is_stx(f))
            continue;

        if (n < max && out) {
            memcpy(out[n].screen, a, sizeof(out[n].screen));
            memcpy(out[n].label,  b, sizeof(out[n].label));
            memcpy(out[n].file,   f, sizeof(out[n].file));
            out[n].offset = i;
        }
        n++;
        i += 32;        /* the loop's own += 4 completes the record */
    }

    return n;
}

/* ------------------------------------------------------------------------- */
u32 q2_laserbeams_build(q2_laserbeam_set *out, const q2_events *events,
                        const q2_userfuncs *uf, const q2_uf_operands *ops,
                        const q2_collision *coll)
{
    q2_event_record rec;

    if (!out || !events || !uf)
        return 0;

    memset(out, 0, sizeof(*out));

    if (events->record_count == 0 || !q2_events_first_record(events, &rec))
        return 0;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call    call;
            const u8     *p;
            q2_laserbeam *b;
            s32           node;
            int           k;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;
            if ((item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                continue;
            if (q2_uf_decode_call(&call, uf, &item) != Q2_OK)
                continue;

            /* `lbu v1, 1(s0)` against 36, in both halves. */
            if (call.prim != Q2_UF_LASERBEAM || item.len != 36)
                continue;

            /* Check before taking a writable slot: a zone may declare more
             * beams than the retail pool can hold. */
            if (out->count >= Q2_LASERBEAM_MAX)
                return out->count;

            /* The constructor's own restore: both endpoints come from the
             * pristine buffer, not the working one (0x8002E744). */
            p = q2_uf_operand_at(ops, item.payload - 2, 36);

            b = &out->beam[out->count];
            memset(b, 0, sizeof(*b));

            for (k = 0; k < 3; k++) {
                b->from[k] = q2_rd_s32(p + 4  + 4 * k);
                b->to[k]   = q2_rd_s32(p + 20 + 4 * k);
            }
            b->kind   = q2_rd_s16(p + 34);
            b->area   = q2_rd_s16(p + 18);
            b->raiser = rec.offset;

            /* The load-time clamp, so an out-of-range kind is kind 0 here too
             * and not a beam the dispatcher silently drops. No disc beam needs
             * it — all 72 are already in range, which is itself the evidence
             * that +34 is a kind and not the counter this table once called
             * it. */
            if (b->kind >= 6 || b->kind < 0)
                b->kind = 0;

            /*
             * And the area, which the constructor resolves rather than reads:
             * hint -1 forces the brute-force sweep, so this is the node holding
             * the near end wherever in the hull it happens to be.
             */
            if (coll) {
                q2_coll_node cell;
                node = q2_coll_find_node(coll, b->from, -1, true);
                /* Retail stores the node here, then dereferences its +32
                 * byte in the beam renderer. Our effect queue stores that
                 * byte directly, like the particle and timed-beam queues. */
                b->area = node >= 0 &&
                          q2_collision_get_node(coll, (u32)node, &cell)
                        ? (s16)(cell.contents & 0x7F) : 0;
            }

            /*
             * `andi v0, v0, 1` at 0x8002E6C0, against the word the constructor
             * has just copied out of the ZONE's chunk. Dark in this room.
             */
            if ((b->from[0] & 1) == 0) {
                out->declined++;
                continue;
            }

            out->count++;
        }
    } while (q2_events_next_record(events, &rec, &rec));

    return out->count;
}

u32 q2_laserbeams_draw(const q2_laserbeam_set *set, const q2_event_rt *rt,
                       q2_fx_world *w, q2_rng *rng)
{
    u32 i, n = 0;

    if (!set || !w || !rng)
        return 0;

    for (i = 0; i < set->count; i++) {
        const q2_laserbeam *b = &set->beam[i];
        q2_fx_laser_result  r;

        /*
         * THE RAISER'S DEAD BIT, 0x8002EE78..0x8002EE94. The walk takes the
         * entry's raiser out of the 32-entry list at 0x800C7014, rebases it
         * onto the Events chunk at gp+372 and skips the beam when byte +3 of
         * that record has 0x80 set. Nothing else ever clears the list — gp+0x420C
         * only counts up — so this is the only way a beam a script switched off
         * goes dark before the zone changes, and the port stored `raiser` for
         * this test without ever making it.
         */
        if (rt && (q2_event_rt_flags(rt, b->raiser) & Q2_EVREC_DISABLED))
            continue;

        /* ends = 0 — the fifth argument the walk zeroes on the stack at
         * 0x8002EEBC. A level's beams are tube only. */
        if (q2_fx_laser(w, rng, (u32)b->kind, b->from, b->to, b->area, 0, &r)
            && r.queued)
            n++;
    }

    return n;
}

u32 q2_laserwalls_build(q2_laserwall_set *out, const q2_events *events,
                       const q2_userfuncs *uf, const q2_uf_operands *ops)
{
    q2_event_record rec;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!events || !uf || !q2_events_first_record(events, &rec)) return 0;
    do {
        u32 i;
        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call call;
            const u8 *p;
            q2_laserwall *wall;
            int k;
            if (!q2_events_get_item(events, &rec, i, &item)) break;
            if (!item.payload || (item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL ||
                item.len != 24 || q2_uf_decode_call(&call, uf, &item) != Q2_OK ||
                call.prim != Q2_UF_LASERWALL) continue;
            p = q2_uf_operand_at(ops, item.payload - 2, 24);
            if (q2_rd_s16(p + 18) < 0 || p[4] > 2) {
                out->declined++;
                continue;
            }
            if (out->count >= Q2_LASERWALL_MAX) return out->count;
            wall = &out->wall[out->count++];
            wall->raiser = rec.offset;
            wall->scene_node = q2_rd_s16(p + 18);
            wall->axis = p[4]; wall->rows = p[5];
            wall->rise = q2_rd_s16(p + 6);
            wall->length = q2_rd_s16(p + 8);
            wall->spread = q2_rd_s16(p + 10);
            for (k = 0; k < 3; k++) wall->offset[k] = q2_rd_s16(p + 12 + 2*k);
        }
    } while (q2_events_next_record(events, &rec, &rec));
    return out->count;
}

bool q2_laserwall_segment(const q2_laserwall *wall, const q2_scene *scene,
                         u32 row, s32 from[3], s32 to[3])
{
    q2_scene_node node;
    u32 spread_axis;
    int k;
    if (!wall || !scene || !from || !to || wall->axis > 2 || row >= wall->rows ||
        wall->scene_node < 0 || !q2_scene_get_node(scene, (u32)wall->scene_node, &node))
        return false;
    /* Native signed division truncates toward zero, including negative odd
     * sums. Read live Scene bounds: a moving emitter moves its whole wall. */
    for (k = 0; k < 3; k++)
        from[k] = to[k] = (s32)(((s64)node.bbox_min[k] + node.bbox_max[k]) / 2) +
                         (k == 1 ? -wall->offset[k] : wall->offset[k]);
    to[wall->axis] += wall->length;
    spread_axis = wall->axis == 0 ? 0 : 2;
    from[spread_axis] -= (s32)row * wall->spread;
    to[spread_axis] += (s32)row * wall->spread;
    from[1] -= (s32)row * wall->rise;
    to[1] -= (s32)row * wall->rise;
    return true;
}

/* ------------------------------------------------------------------------- */
/* TELEPORT — PSXTELEPORT_T (0x8002E3F0) and DrawTelePorts (0x80028BDC)       */
/* ------------------------------------------------------------------------- */
/*
 * `(a + b) / 2`, rounding toward zero.
 *
 * The idiom is `addu; srl 31; addu; sra 1` — 0x80028D04..0x80028D10 and its
 * two siblings — which is C's own division and not the floor an arithmetic
 * shift alone would give. One unit on a negative coordinate, and most of this
 * game's world is at negative coordinates.
 */
static s32 mid2(s32 a, s32 b)
{
    s32 sum = a + b;

    return (sum + (s32)((u32)sum >> 31)) >> 1;
}

u32 q2_teleports_build(q2_teleport_set *out, const q2_events *events,
                       const q2_userfuncs *uf, const q2_triggers *trigs)
{
    q2_event_record rec;

    if (!out)
        return 0;

    memset(out, 0, sizeof(*out));

    if (!events || !uf || !trigs || trigs->count == 0)
        return 0;
    if (events->record_count == 0 || !q2_events_first_record(events, &rec))
        return 0;

    do {
        u32 i;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_uf_call    call;
            u32           t;

            if (!q2_events_get_item(events, &rec, i, &item))
                break;
            if ((item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                continue;
            if (q2_uf_decode_call(&call, uf, &item) != Q2_OK)
                continue;
            if (call.prim != Q2_UF_TELEPORT)
                continue;

            /*
             * The scan, 0x8002E424..0x8002E490. From the top of the table, not
             * from wherever the item pointer's low halfword happened to point
             * — see levelbin.h. First match wins and the constructor returns
             * (0x8002E46C), so an item whose record several volumes name gets
             * the lowest-numbered of them, and two TELEPORT items in one
             * record register that same volume twice. Both are kept: they are
             * what the console's list holds.
             */
            for (t = 0; t <= trigs->count; t++) {
                q2_trigger trig;

                if (t == trigs->count) {       /* the scan ran out */
                    out->unmarked++;
                    break;
                }
                if (!q2_trigger_get(trigs, t, &trig))
                    break;
                /* `lb v0, 32(rec)` against -1 — the ID byte, which is the
                 * volume's AREA (the dispatcher at 0x80027EF4 compares it with
                 * entity+0x9E). 255 means it has none. */
                if ((trig.id & 0xFFu) == Q2_TRIGGER_NO_ID)
                    continue;
                if (trig.event_offset != rec.offset)
                    continue;

                if (out->count < Q2_TELEPORT_MAX)
                    out->trigger[out->count++] = (u16)t;
                break;
            }
        }
    } while (q2_events_next_record(events, &rec, &rec));

    return out->count;
}

u32 q2_teleports_draw(const q2_teleport_set *set, const q2_triggers *trigs,
                      q2_fx_world *w, q2_rng *rng, s32 level_time,
                      q2_teleport_frame *out)
{
    s16 vel[Q2_FX_GROUP_QUADS][3];
    s32 sign, radius;
    u32 i, n = 0;

    if (out)
        memset(out, 0, sizeof(*out));

    if (!w || !rng)
        return 0;

    /*
     * BEFORE THE COUNT IS TESTED. 0x80028C34's fifteen triples are drawn at
     * the top of the function and 0x80028CA8's `blez` on NumTelePorts comes
     * after them, so a map with no pads still spends 45 draws a frame. Moving
     * them inside the walk would be tidier and would put every seeded replay
     * one generator state out per frame.
     */
    sign = q2_fx_teleport_sign(level_time);
    q2_fx_teleport_velocities(rng, sign, vel);

    if (!set || set->count == 0 || !trigs)
        return 0;

    radius = q2_fx_teleport_light_radius(level_time);

    for (i = 0; i < set->count; i++) {
        q2_trigger trig;
        s32 centre[3];
        s32 y_face, y_centre;

        if (!q2_trigger_get(trigs, set->trigger[i], &trig))
            continue;

        /* 0x80028CF8..0x80028D34: the bbox centre in X and Z. */
        centre[0] = mid2(trig.min[0], trig.max[0]);
        centre[2] = mid2(trig.min[2], trig.max[2]);

        /*
         * And Y is a FACE, not a centre: 0x80028D38 branches on the sign of
         * the same up/down saw the velocities use, taking mins[1] while the
         * column streams up and maxs[1] while it falls. So the quads always
         * start at the end of the volume they are travelling away from and the
         * column fills the pad rather than hanging in its middle.
         */
        y_face = (sign > 0) ? trig.min[1] : trig.max[1];
        centre[1] = y_face;

        if (q2_fx_teleport_column(w, rng, vel, centre,
                                  (u8)(trig.id & 0xFFu)) >= 0)
            n++;

        if (!out || out->light_count >= Q2_TELEPORT_MAX)
            continue;

        /*
         * The light, 0x80028DD8..0x80028E6C. It does NOT sit on the face: the
         * Y at sp+132 is overwritten with the bbox centre at 0x80028DF8, after
         * the particle spawn and before AddLight, while the jittered X and Z
         * are carried over from it. So the column comes out of one end of the
         * volume and the glow sits in the middle of it.
         *
         * The jitter is the PARTICLES' — q2_fx_teleport_column drew it — and
         * this port cannot see it from here, so the light takes the unjittered
         * centre. 127 units either side of a source whose outer radius is at
         * least 800 is not a difference anything can see, and inventing two
         * more generator draws to recover it would be.
         */
        {
            q2_lb_light *l = &out->light[out->light_count++];

            y_centre = mid2(trig.min[1], trig.max[1]);

            l->pos[0] = centre[0];
            l->pos[1] = y_centre;
            l->pos[2] = centre[2];
            l->rgb[0] = Q2_FX_TELEPORT_LIGHT_R;
            l->rgb[1] = Q2_FX_TELEPORT_LIGHT_G;
            l->rgb[2] = Q2_FX_TELEPORT_LIGHT_B;
            l->inner  = Q2_FX_TELEPORT_LIGHT_INNER;
            l->outer  = radius;
        }
    }

    if (out)
        out->groups = n;

    return n;
}

/* ------------------------------------------------------------------------- */
/* VIEW CREDITS — see levelbin.h                                              */
/* ------------------------------------------------------------------------- */
static bool credit_row_at(const u8 *module, u32 size, u32 base, u32 off,
                            q2_credit_row *out)
{
    u32 text, i;
    if (off > size || size - off < 24) return false;
    text = q2_rd_u32(module + off);
    if (!text || text < base || text - base >= size) return false;
    text -= base;
    for (i = 0; i < 64 && i < size - text; i++) {
        u8 ch = module[text + i];
        if (!ch) break;
        if (ch < 0x20 || ch > 0x7E) return false;
    }
    if (i == 64 || i == size - text) return false;
    if (q2_rd_s16(module + off + 4) != 256 ||
        q2_rd_s16(module + off + 6) < 0 ||
        q2_rd_s16(module + off + 6) > 480) return false;
    /* The last blank is an ordinary menu record with y=223 and a Back
     * callback. The stream uses only text/x, replacing y with 210. */
    i = q2_rd_u32(module + off + 8);
    if (i && (i < base || i - base >= size)) return false;
    for (i = 12; i < 24; i += 4)
        if (q2_rd_u32(module + off + i)) return false;
    if (out) {
        out->text = (const char *)module + text;
        out->x = q2_rd_s16(module + off + 4);
        out->y = q2_rd_s16(module + off + 6);
    }
    return true;
}

static bool credit_end_at(const u8 *module, u32 size, u32 off)
{
    u32 i;
    if (off > size || size - off < 24) return false;
    for (i = 0; i < 24; i += 4)
        if (q2_rd_u32(module + off + i)) return false;
    return true;
}

u32 q2_levelbin_credits(const u8 *module, u32 size, u32 load_base,
                        q2_credit_row *out, u32 max)
{
    u32 off;
    if (!module || size < 120) return 0;
    for (off = 0; off <= size - 120; off += 4) {
        q2_credit_row first, sentinel;
        u32 at, n = 1, i;
        if (!credit_row_at(module, size, load_base, off, &first) ||
            strcmp(first.text, "DEVELOPED BY") || first.y != 210 ||
            !credit_end_at(module, size, off + 24) ||
            !credit_row_at(module, size, load_base, off + 48, &sentinel) ||
            sentinel.text[0]) continue;
        for (at = off + 72; n < Q2_CREDITS_MAX; at += 24, n++)
            if (!credit_row_at(module, size, load_base, at, NULL)) break;
        if (n < 3 || !credit_end_at(module, size, at)) continue;
        if (out && max) {
            out[0] = first;
            for (i = 1; i < n && i < max; i++)
                credit_row_at(module, size, load_base, off + 48 + i * 24,
                                &out[i]);
        }
        return n;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* The title screen's scene — see the derivation in levelbin.h                */
/* ------------------------------------------------------------------------- */

/*
 * Does the module materialise `addr` with a `lui`/`addiu` pair?
 *
 * The two halves are almost never adjacent — the scheduler fills the load
 * delay — so this is the same bounded look-back `q2_levelbin_misevents` uses
 * for its own constants: find the `addiu`, then walk up to eight instructions
 * back for a `lui` into the register it reads.
 *
 * A `lui` whose low half is negative carries a borrow, which is why the sum is
 * taken over sign-extended halves rather than concatenated.
 */
static bool module_materialises(const u8 *module, u32 size, u32 addr)
{
    u32 i;

    for (i = 0; i + 4 <= size; i += 4) {
        u32 w = q2_rd_u32(module + i);
        u32 rs, j;
        s32 lo;

        if ((w >> 26) != 0x09)                 /* addiu rt, rs, imm */
            continue;
        rs = (w >> 21) & 0x1F;
        lo = (s32)(s16)(u16)(w & 0xFFFF);

        for (j = 4; j <= 32 && j <= i; j += 4) {
            u32 p = q2_rd_u32(module + i - j);

            if ((p >> 26) != 0x0F)             /* lui rt, imm */
                continue;
            if (((p >> 16) & 0x1F) != rs)
                continue;
            if ((u32)(((p & 0xFFFF) << 16) + lo) == addr)
                return true;
            break;
        }
    }
    return false;
}

bool q2_levelbin_scene(const u8 *module, u32 size, u32 load_base,
                       q2_lb_scene *out)
{
    u32 off;

    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!module || size < 8)
        return false;

    for (off = 0; off + 8 <= size; off += 4) {
        u32 run = 0, at;

        for (at = off; at + 4 <= size; at += 4) {
            u32 w = q2_rd_u32(module + at);

            if (w == 0xFFFFFFFFu)
                break;
            /*
             * The spawner reads the id with `lh`, so a word is only an id when
             * its upper half is clear as well — which is what stops a run of
             * relocated pointers or small negative constants reading as a
             * short list of low ids.
             */
            if (w >= Q2_ITEM_COUNT)
                break;
            if (++run > Q2_LB_SCENE_MAX)
                break;
        }

        /*
         * Two independent things, as `q2_levelbin_menu_pages` needs two: a
         * terminated run of plausible ids, and an address the module actually
         * names. QFRONT has exactly one such list; a module with none — every
         * other map's — finds nothing rather than the first pair of small
         * words that happens to sit next to a -1.
         */
        if (run < 2 || run > Q2_LB_SCENE_MAX)
            continue;
        if (at + 4 > size || q2_rd_u32(module + at) != 0xFFFFFFFFu)
            continue;
        if (!module_materialises(module, size, load_base + off))
            continue;

        out->offset = off;
        out->count  = run;
        for (at = 0; at < run; at++)
            out->id[at] = (u16)q2_rd_u32(module + off + at * 4);
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* The title screen's lights — module+0x2BD8, see levelbin.h                   */
/* ------------------------------------------------------------------------- */

/*
 * 0x80102DB4..0x80102DC8, and again at 0x80102E84..0x80102E9C. The compiler's
 * signed divide-by-four: `d >= 0 ? d >> 2 : (d + 3) >> 2`, which rounds toward
 * zero. Written out rather than left as `/ 4` because the two arms are visible
 * in the disassembly and this is the only place the port needs to agree with
 * them on a negative delta — which the LEFT light spends its whole life on.
 */
static s32 ease_quarter(s32 cur, s32 target)
{
    s32 d = target - cur;
    return cur + ((d >= 0) ? (d >> 2) : ((d + 3) >> 2));
}

u32 q2_levelbin_scene_lights(q2_lb_light out[Q2_LB_LIGHT_MAX],
                             s32 wander[2], q2_rng *rng)
{
    /* The three small ones are constants on the frame: 0x80102BFC packs their
     * radii as 0x00C80064 and 0x80102C9C packs their colour as 0x0040FF10. */
    static const s32 k_small_y[3] = { -200, 0, 200 };
    u32 i;

    if (!out || !wander || !rng)
        return 0;

    for (i = 0; i < 3; i++) {
        out[i].pos[0] = 0;
        out[i].pos[1] = k_small_y[i];
        out[i].pos[2] = 500;
        out[i].rgb[0] = 16;
        out[i].rgb[1] = 255;
        out[i].rgb[2] = 64;
        out[i].inner  = 100;
        out[i].outer  = 200;
    }

    /*
     * The two big ones, in the module's own draw order — right x, right green,
     * left x, left green. The order is kept because the generator is shared:
     * reordering the four draws would give every light a different value from
     * the frame after the first, and the flicker is visible.
     */
    for (i = 0; i < 2; i++) {
        s32 target = (q2_rng_next(rng) & 0x1FF) + 200;
        s32 green;

        if (i == 1)
            target = -target;              /* 80102E80: subu v0, zero, v0 */

        wander[i]        = ease_quarter(wander[i], target);
        green            = (q2_rng_next(rng) & 0x3F) + 64;

        out[3 + i].pos[0] = wander[i];
        out[3 + i].pos[1] = 600;
        out[3 + i].pos[2] = 900;
        out[3 + i].rgb[0] = 64;
        out[3 + i].rgb[1] = (u8)green;
        out[3 + i].rgb[2] = 127;
        out[3 + i].inner  = 500;
        out[3 + i].outer  = 1500;
    }

    return Q2_LB_LIGHT_MAX;
}
