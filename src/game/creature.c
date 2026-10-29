#include "creature.h"

#include <string.h>

const char *const q2_cre_callback_names[13] = {
    "stand", "idle", "search", "walk", "run",
    "dodge", "attack", "melee", "sight", "checkattack", "bigturn",
    "pain", "die"
};

/* ------------------------------------------------------------------------- */
/* Just enough MIPS to follow the chain                                       */
/* ------------------------------------------------------------------------- */
/*
 * The decoder never interprets a program; it walks a function's instructions
 * once, keeping the last value each register was given by a `lui`/`addiu` or
 * `lui`/`ori` pair. That is enough because every pointer a module stores is
 * materialised that way immediately before the store — the compiler has no
 * reason to do otherwise for a constant address, and across all seven modules
 * on the disc it never does.
 */
#define OP(w)     ((w) >> 26)
#define RS(w)     (((w) >> 21) & 0x1F)
#define RT(w)     (((w) >> 16) & 0x1F)
#define IMM(w)    ((s32)(s16)((w) & 0xFFFF))
#define IMMU(w)   ((w) & 0xFFFF)

#define OP_LUI    0x0F
#define OP_ADDIU  0x09
#define OP_ORI    0x0D
#define OP_SW     0x2B
#define OP_SH     0x29
#define OP_SB     0x28
#define OP_JR     0x00
#define OP_JAL    0x03
#define OP_J      0x02

/*
 * A module's method table is not IN the image — export 2 fills it at load time
 * with a run of `sw` instructions into the module's own BSS, which on disc is
 * zeroes. So the decoder keeps a shadow: as it walks, every store to a known
 * in-image address is applied to a writable overlay, and the table is then read
 * back out of that. Reading the image directly finds 32 zeroes and reports a
 * creature with no methods, which is what the first version of this did.
 */
#define DEC_SHADOW_MAX 512

typedef struct dec {
    const u8 *img;
    size_t    size;
    u32       base;
    u32       reg[32];      /* last materialised constant per register */
    bool      known[32];

    /*
     * The last `lui` seen for each register, kept alongside the tracked value.
     *
     * MIPS has a branch DELAY SLOT, and the compiler puts a `lui` in it: when
     * the branch is taken the `lui` still executes and its `addiu` is at the
     * TARGET. A linear walk pairs that `addiu` with whatever the fall-through
     * path last left in the register, which produces an address that is off by
     * the fall-through's own offset — a plausible in-image number that fails
     * validation and is silently dropped. Keeping the `lui` half lets a
     * materialising `addiu` offer BOTH candidates; every one is validated
     * structurally before being accepted, so the wrong one costs nothing.
     */
    u32       lui[32];
    bool      lui_known[32];

    u32       shadow_addr[DEC_SHADOW_MAX];
    u32       shadow_val[DEC_SHADOW_MAX];
    u32       shadow_count;
} dec;

static void shadow_put(dec *d, u32 addr, u32 val)
{
    u32 i;
    for (i = 0; i < d->shadow_count; i++)
        if (d->shadow_addr[i] == addr) { d->shadow_val[i] = val; return; }
    if (d->shadow_count < DEC_SHADOW_MAX) {
        d->shadow_addr[d->shadow_count] = addr;
        d->shadow_val[d->shadow_count]  = val;
        d->shadow_count++;
    }
}

static bool shadow_get(const dec *d, u32 addr, u32 *out)
{
    u32 i;
    for (i = 0; i < d->shadow_count; i++)
        if (d->shadow_addr[i] == addr) { *out = d->shadow_val[i]; return true; }
    return false;
}

static u32 word_at(const dec *d, u32 addr)
{
    u32 off, v;
    if (shadow_get(d, addr, &v))
        return v;
    if (addr < d->base)
        return 0;
    off = addr - d->base;
    if ((size_t)off + 4 > d->size)
        return 0;
    return q2_rd_u32(d->img + off);
}

static bool in_image(const dec *d, u32 addr, u32 len)
{
    if (addr < d->base)
        return false;
    return (size_t)(addr - d->base) + len <= d->size;
}

static void dec_reset(dec *d)
{
    memset(d->reg, 0, sizeof(d->reg));
    memset(d->known, 0, sizeof(d->known));
    memset(d->lui, 0, sizeof(d->lui));
    memset(d->lui_known, 0, sizeof(d->lui_known));
}

/*
 * Track one instruction's effect on the constant map.
 *
 * The register-move case matters more than it looks: the compiler routinely
 * materialises a table address into a saved register and then copies it into
 * `a1` with `addu a1, s0, zero` immediately before the call. Miss that and the
 * class registration reads whatever happened to be in `a1` beforehand, which
 * is how an earlier version of this decoder confidently reported a string
 * constant as a method table.
 */
static void dec_track(dec *d, u32 w)
{
    u32 op = OP(w), rt = RT(w), rs = RS(w);

    if (op == 0) {                          /* SPECIAL: destination is rd */
        u32 funct = w & 0x3F;
        u32 rd    = (w >> 11) & 0x1F;

        if (rd == 0)
            return;

        if (funct == 0x21 || funct == 0x25) {       /* addu / or */
            if (rt == 0 && d->known[rs]) {
                d->reg[rd] = d->reg[rs]; d->known[rd] = true; return;
            }
            if (rs == 0 && d->known[rt]) {
                d->reg[rd] = d->reg[rt]; d->known[rd] = true; return;
            }
            if (d->known[rs] && d->known[rt]) {
                d->reg[rd] = (funct == 0x21) ? d->reg[rs] + d->reg[rt]
                                             : d->reg[rs] | d->reg[rt];
                d->known[rd] = true; return;
            }
        }
        d->known[rd] = false;
        return;
    }

    if (rt == 0)
        return;

    switch (op) {
    case OP_LUI:
        d->reg[rt]       = IMMU(w) << 16;
        d->known[rt]     = true;
        d->lui[rt]       = IMMU(w) << 16;
        d->lui_known[rt] = true;
        break;
    case OP_ADDIU:
        if (rs == 0) { d->reg[rt] = (u32)IMM(w); d->known[rt] = true; }
        else if (d->known[rs]) { d->reg[rt] = d->reg[rs] + (u32)IMM(w);
                                 d->known[rt] = true; }
        else d->known[rt] = false;
        break;
    case OP_ORI:
        if (rs == 0) { d->reg[rt] = IMMU(w); d->known[rt] = true; }
        else if (d->known[rs]) { d->reg[rt] = d->reg[rs] | IMMU(w);
                                 d->known[rt] = true; }
        else d->known[rt] = false;
        break;
    default:
        /* Anything else may clobber rt; forget it rather than believe it. */
        if (op != OP_SW && op != OP_SH && op != OP_SB
            && op != OP_JAL && op != OP_J && op != 0x04 /* beq */
            && op != 0x05 /* bne */)
            d->known[rt] = false;
        break;
    }
}

/* How far to walk a function before giving up. Comfortably past the largest
 * spawn function on the disc, which is the Tank Commander's. */
#define WALK_LIMIT 2048

/*
 * Where a function ends.
 *
 * NOT at the first `jr ra`. These functions routinely have several — the
 * compiler emits one per early return rather than branching to a shared
 * epilogue, and `soldier_run` returns four different moves through four
 * separate `jr ra`s. Stopping at the first finds one move out of five, which
 * is exactly the failure the first version of this decoder had.
 *
 * Instead, a function runs until the next address that is known to start one.
 * The entry set is the callbacks, the method table and every move's end
 * callback — all of them discovered rather than guessed, which is what keeps
 * this deterministic. Over-walking is harmless because every candidate move is
 * validated structurally before it is accepted.
 */
static u32 walk_end(const q2_creature *c, const dec *d, u32 entry)
{
    u32 best = entry + WALK_LIMIT * 4;
    u32 i;

    for (i = 0; i < 13; i++)
        if (c->callback[i] > entry && c->callback[i] < best)
            best = c->callback[i];

    for (i = 0; i < Q2_CLASS_METHOD_COUNT; i++)
        if (c->method[i] > entry && c->method[i] < best)
            best = c->method[i];

    for (i = 0; i < c->move_count; i++)
        if (c->move[i].endfunc_addr > entry && c->move[i].endfunc_addr < best)
            best = c->move[i].endfunc_addr;

    /* A return only ends the function after its forward branch targets.
     * This preserves multiple-return installers without walking into the
     * next unregistered function (notably Jorg's embedded Rider spawn). */
    {
        u32 addr, frontier = entry;
        for (addr = entry; addr < best && in_image(d, addr, 4); addr += 4) {
            u32 w = word_at(d, addr), op = OP(w), target = 0;
            if (op == 1 || (op >= 4 && op <= 7))
                target = addr + 4 + (s32)IMM(w) * 4;
            else if (op == 2)
                target = ((addr + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            if (target >= entry && target < best && target > frontier) frontier = target;
            if (w == 0x03E00008u && addr >= frontier) return addr + 8;
        }
    }
    return best;
}

/* ------------------------------------------------------------------------- */
/* Export 2 — the class registration                                          */
/* ------------------------------------------------------------------------- */
/*
 * Looks for `jalr` on the value loaded from the import slot at module+0x118
 * with `a0` a small constant and `a1` a module address. Rather than model the
 * indirect call, it is enough to notice the pattern that always precedes it:
 * `a0` set to a constant under 256 and `a1` set to an in-image address, with a
 * `jalr` before either changes again.
 */
static void decode_classes(q2_creature *c, dec *d, u32 entry, s32 method_class)
{
    u32 addr;
    u32 pending_class = 0xFFFFFFFFu;
    u32 pending_table = 0;

    dec_reset(d);

    for (addr = entry; addr < entry + WALK_LIMIT * 4; addr += 4) {
        u32 w = word_at(d, addr);

        if (w == 0 && !in_image(d, addr, 4))
            break;

        /* jr ra ends the function. */
        if (OP(w) == OP_JR && (w & 0x3F) == 0x08 && RS(w) == 31)
            break;

        /*
         * jalr rX. The instruction AFTER it is the delay slot and runs before
         * the call, so it has to be folded in first — the compiler puts the
         * last argument setup there often enough that ignoring it loses half
         * the registrations.
         */
        /* Record the table stores as they go by. */
        if (OP(w) == OP_SW) {
            u32 rs = RS(w), rt = RT(w);
            if (d->known[rs]) {
                u32 target = d->reg[rs] + (u32)IMM(w);
                if (in_image(d, target, 4))
                    shadow_put(d, target,
                               (rt == 0) ? 0 : (d->known[rt] ? d->reg[rt] : 0));
            }
        }

        if (OP(w) == 0 && (w & 0x3F) == 0x09) {
            dec_track(d, word_at(d, addr + 4));

            if (d->known[4] && d->reg[4] < 256
                && d->known[5] && in_image(d, d->reg[5], 4)) {
                pending_class = d->reg[4];
                pending_table = d->reg[5];

                if (c->class_count < Q2_CRE_MAX_CLASSES) {
                    u32 i;
                    bool dup = false;
                    for (i = 0; i < c->class_count; i++)
                        if (c->class_byte[i] == (u8)pending_class)
                            dup = true;
                    if (!dup) {
                        c->class_method_table[c->class_count] = pending_table;
                        c->class_byte[c->class_count++] = (u8)pending_class;
                    }
                }
                if ((!c->method_table_addr && method_class < 0) ||
                    pending_class == (u32)method_class)
                    c->method_table_addr = pending_table;
            }
        }

        dec_track(d, w);
    }

    (void)pending_class;
    (void)pending_table;

    /* Read the registered table. A slot pointing outside the image is not a
     * method; it is the end of the table. */
    if (c->method_table_addr && in_image(d, c->method_table_addr, 4)) {
        u32 i;
        for (i = 0; i < Q2_CLASS_METHOD_COUNT; i++) {
            u32 v;
            if (!in_image(d, c->method_table_addr + i * 4, 4))
                break;
            v = word_at(d, c->method_table_addr + i * 4);
            if (v && !in_image(d, v, 4))
                break;
            c->method[i] = v;
            if (v)
                c->method_count = i + 1;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Export 0 — the spawn function                                              */
/* ------------------------------------------------------------------------- */
static int callback_slot(s32 imm)
{
    switch (imm) {
    case Q2_ENT_OFS_STAND:   return 0;
    case Q2_ENT_OFS_IDLE:    return 1;
    case Q2_ENT_OFS_SEARCH:  return 2;
    case Q2_ENT_OFS_WALK:    return 3;
    case Q2_ENT_OFS_RUN:     return 4;
    case Q2_ENT_OFS_DODGE:   return 5;
    case Q2_ENT_OFS_ATTACK:  return 6;
    case Q2_ENT_OFS_MELEE:   return 7;
    case Q2_ENT_OFS_SIGHT:   return 8;
    case Q2_ENT_OFS_CHECK:   return 9;
    case Q2_ENT_OFS_BIGTURN: return 10;
    case Q2_ENT_OFS_PAIN:    return 11;
    case Q2_ENT_OFS_DIE:     return 12;
    default:                 return -1;
    }
}

static void decode_spawn(q2_creature *c, dec *d, u32 entry)
{
    u32 addr;

    dec_reset(d);
    c->speed_scale = -1;
    c->mass        = -1;

    for (addr = entry; addr < entry + WALK_LIMIT * 4; addr += 4) {
        u32 w = word_at(d, addr);
        u32 op = OP(w);

        if (w == 0 && !in_image(d, addr, 4))
            break;
        if (op == OP_JR && (w & 0x3F) == 0x08 && RS(w) == 31)
            break;

        if (op == OP_SW) {
            int slot = callback_slot(IMM(w));
            u32 rt = RT(w);
            if (slot >= 0) {
                /* A store of zero is the module explicitly saying it has none,
                 * which is how the Soldier declares no melee. */
                c->callback[slot] = (rt == 0) ? 0
                                  : (d->known[rt] ? d->reg[rt] : 0);
            }
        } else if (op == OP_SB && IMM(w) == Q2_ENT_OFS_SCALE) {
            u32 rt = RT(w);
            if (rt != 0 && d->known[rt])
                c->speed_scale = (s32)(d->reg[rt] & 0xFF);
        } else if (op == OP_SH && IMM(w) == Q2_ENT_OFS_MASS) {
            u32 rt = RT(w);
            if (rt != 0 && d->known[rt])
                c->mass = (s32)(s16)(d->reg[rt] & 0xFFFF);
        }

        dec_track(d, w);
    }
}

/* ------------------------------------------------------------------------- */
/* Following a callback to the moves it installs                              */
/* ------------------------------------------------------------------------- */
static bool add_move(q2_creature *c, dec *d, u32 move_addr, s32 via)
{
    q2_cre_move *m;
    s32 first, last;
    u32 frames_addr, n, i, foff;

    if (!in_image(d, move_addr, 16))
        return false;

    for (i = 0; i < c->move_count; i++)
        if (c->move[i].addr == move_addr) {
            if (c->move[i].via < 0 && via >= 0)
                c->move[i].via = via;
            return true;
        }

    if (c->move_count >= Q2_CRE_MAX_MOVES)
        return false;

    first       = (s32)word_at(d, move_addr + 0);
    last        = (s32)word_at(d, move_addr + 4);
    frames_addr = word_at(d, move_addr + 8);

    if (last < first || last - first > 512)
        return false;

    n = (u32)(last - first + 1);
    if (!in_image(d, frames_addr, n * Q2_MFRAME_SIZE))
        return false;
    if (c->frame_count + n > Q2_CRE_MAX_FRAMES)
        return false;

    /*
     * Every frame's ai byte must select a real verb — one of the six shared
     * slots or one of the creature's own. Walking past the end of a function
     * can materialise an address that happens to be in the image and happens
     * to decode as a plausible frame range; this is what rejects it, and it
     * costs nothing because a genuine move passes trivially.
     */
    foff = frames_addr - d->base;
    for (i = 0; i < n; i++) {
        u8 ai = d->img[foff + i * 3];
        u8 lo = ai & 0x7Fu;
        if (lo >= Q2_AI_VERB_COUNT && !(ai & Q2_AI_LOCAL_FLAG))
            return false;
        if ((ai & Q2_AI_LOCAL_FLAG) && lo >= Q2_CLASS_METHOD_COUNT
                                              - Q2_CLASS_VERB_BASE)
            return false;
        if (d->img[foff + i * 3 + 2] >= Q2_CLASS_VERB_BASE)
            return false;
    }

    m = &c->move[c->move_count];
    memset(m, 0, sizeof(*m));
    m->addr         = move_addr;
    m->first_frame  = first;
    m->last_frame   = last;
    m->frames_addr  = frames_addr;
    m->endfunc_addr = word_at(d, move_addr + 12);

    /*
     * If that endfunc is nothing but "install this other move", record which.
     * The shape is three instructions — `lui`/`addiu` materialising a move
     * record and `sw rX, 0xD8(a0)` — and a function that does more than that is
     * left alone, because guessing at one would install a move the creature was
     * never meant to play.
     */
    m->endfunc_move = 0;
    if (m->endfunc_addr && in_image(d, m->endfunc_addr, 16)) {
        u32 a, stores = 0, target = 0;
        dec probe = *d;

        dec_reset(&probe);
        for (a = m->endfunc_addr; a < m->endfunc_addr + 32; a += 4) {
            u32 w;

            if (!in_image(&probe, a, 4))
                break;
            w = word_at(&probe, a);
            dec_track(&probe, w);

            if (OP(w) == OP_SW && IMM(w) == Q2_ENT_OFS_MOVE) {
                u32 rt = RT(w);

                stores++;
                if (rt < 32 && probe.known[rt])
                    target = probe.reg[rt];
            }
            /*
             * `jr ra` ends it — but its DELAY SLOT is where the compiler puts
             * the store, so the slot has to be examined before breaking rather
             * than merely tracked. Missing that reported every one of these
             * endfuncs as installing nothing.
             */
            if (OP(w) == 0 && (w & 0x3F) == 0x08) {
                if (in_image(&probe, a + 4, 4)) {
                    u32 dw = word_at(&probe, a + 4);

                    dec_track(&probe, dw);
                    if (OP(dw) == OP_SW && IMM(dw) == Q2_ENT_OFS_MOVE) {
                        u32 drt = RT(dw);

                        stores++;
                        if (drt < 32 && probe.known[drt])
                            target = probe.reg[drt];
                    }
                }
                break;
            }
        }

        /* Exactly one install, and it must validate as a move record. */
        if (stores == 1 && target && in_image(d, target, 16))
            m->endfunc_move = target;
    }
    m->frame_index  = c->frame_count;
    m->frame_count  = n;
    m->via          = via;

    foff = frames_addr - d->base;
    for (i = 0; i < n; i++) {
        q2_mframe *f = &c->frames[c->frame_count + i];
        f->ai    = d->img[foff + i * 3 + 0];
        f->dist  = (s8)d->img[foff + i * 3 + 1];
        f->think = d->img[foff + i * 3 + 2];
    }

    c->frame_count += n;
    c->move_count++;
    return true;
}

/* Walk a function, collecting every `sw rX, 0xD8(rS)` whose rX is a known
 * in-image address — i.e. every move it installs. */
static void follow_callback(q2_creature *c, dec *d, u32 entry, s32 via,
                            int depth)
{
    u32 addr, end;

    if (!entry || !in_image(d, entry, 4) || depth > 4)
        return;

    end = walk_end(c, d, entry);

    /*
     * Pass one: does this function install moves at all? A single
     * `sw rX, 0xD8(rS)` anywhere in it is the whole test.
     */
    dec_reset(d);
    {
        bool installs = false;
        for (addr = entry; addr < end; addr += 4) {
            u32 w;
            if (!in_image(d, addr, 4))
                break;
            w = word_at(d, addr);
            if (OP(w) == OP_SW && IMM(w) == Q2_ENT_OFS_MOVE) {
                installs = true;
                break;
            }
        }
        if (!installs)
            return;
    }

    /*
     * Pass two: take every in-image address the function materialises that
     * validates as a move record.
     *
     * Not just the ones at the store site. The compiler shares one epilogue
     * across a function's returns, so `soldier_run` picks between five moves
     * with four `jr ra`s and a backward jump into a single
     * `sw v0, 0xD8(a0)` — a linear walk sees that store once and finds one
     * move out of five. Since a function that stores a move is already known
     * to be about moves, and every candidate is validated structurally before
     * being accepted, taking all of them is both complete and safe.
     */
    dec_reset(d);

    for (addr = entry; addr < end; addr += 4) {
        u32 w, rt;

        if (!in_image(d, addr, 4))
            break;
        w = word_at(d, addr);

        dec_track(d, w);

        /* Only the register a materialising pair just wrote is a candidate. */
        rt = (OP(w) == 0) ? ((w >> 11) & 0x1F) : RT(w);
        if (rt != 0 && rt < 32 && d->known[rt] && in_image(d, d->reg[rt], 16))
            add_move(c, d, d->reg[rt], via);

        /*
         * ...and the same `addiu` paired with the register's last `lui`, which
         * is the value it has when a branch was taken and the `lui` sat in the
         * delay slot. The Arachner's run callback is exactly this shape: the
         * stand-ground branch installs one move and the fall-through installs
         * another, and only the first was ever recorded — so nothing had
         * `via == 4`, the generic run handler found no move to play, and the
         * creature stood still on POWER1 for the whole capture.
         */
        if (OP(w) == OP_ADDIU && rt != 0 && rt < 32 && RS(w) == rt &&
            d->lui_known[rt]) {
            u32 alt = d->lui[rt] + (u32)IMM(w);

            if (alt != d->reg[rt] && in_image(d, alt, 16))
                add_move(c, d, alt, via);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* The module's move names — see the header for the record and the link.      */
/* ------------------------------------------------------------------------- */
#define CRE_MOVE_NAME_STRIDE 20
#define CRE_MOVE_NAME_CHARS  16

/*
 * The text portion of a native 20-byte frame-name record. The first four
 * bytes hold the frame range; the following 16 bytes initially hold the name
 * and zero padding (the engine later caches model positions in that padding).
 *
 * The name is NUL-padded and is NOT terminated when it fills its sixteen
 * bytes, the same rule the level table and the `Strings` dictionary use. The
 * two frames are the move's own range, which is what ties a name to a move
 * without needing the table's order to match the decoder's.
 */
static bool name_text_ok(const u8 *p, size_t avail)
{
    size_t i;
    bool terminated = false;

    if (avail < CRE_MOVE_NAME_CHARS)
        return false;
    if (p[0] < 0x20 || p[0] > 0x7E)
        return false;

    for (i = 0; i < CRE_MOVE_NAME_CHARS; i++) {
        if (p[i] == 0) {
            terminated = true;
            break;
        }
        if (p[i] < 0x20 || p[i] > 0x7E)
            return false;
    }
    if (i < 3)
        return false;

    /*
     * TERMINATED, not merely printable.
     *
     * Without this a run of printable bytes that fills the field passes, and a
     * module has plenty: JAIL4 was naming a move `@.B$L` — a five-character
     * window cut out of code, followed by two halfwords that happened to
     * satisfy `last >= first`. That name then went to the block-D lookup,
     * missed, and 177 poses a run fell back to matching a clip by LENGTH,
     * which is something the disc never does.
     *
     * The padding after the NUL is checked too: a field that goes back to
     * printable is two things next to each other, not a name.
     */
    if (!terminated)
        return false;
    for (; i < CRE_MOVE_NAME_CHARS; i++)
        if (p[i] != 0)
            return false;

    return true;
}

/* A 12-byte slot holding a NUL-terminated printable name of 3 or more bytes. */
static bool slot_is_name(const u8 *image, size_t size, size_t off)
{
    size_t i;

    if (off + 12 > size)
        return false;

    for (i = 0; i < 12; i++) {
        u8 ch = image[off + i];

        if (ch == 0)
            return i >= 3;
        if (ch < 0x20 || ch > 0x7E)
            return false;
    }

    return false;               /* no terminator inside the slot */
}

u32 q2_creature_sound_names(const q2_creature *c, const u8 *image, size_t size,
                            const char **out, u32 out_count)
{
    size_t at, name_off = 0;
    u32 n = 0;

    if (!c || !image || !out || out_count == 0)
        return 0;

    /*
     * The module's sound names are a run of 12-byte slots a little way past its
     * own name, and each slot's INDEX is the sound number — validated against
     * the one creature whose numbering was already read from its code: the
     * Soldier's table gives idle 0, sight 1, pain 2, death 5 and the shotgun
     * cock 9, and its slots are in exactly that order.
     *
     * The gap after the module name is 0x5C on three of the seven and 0x60 on
     * the Arachner, so it is FOUND rather than assumed: the first place where
     * three consecutive slots all hold a name.
     */
    for (at = 0; at + 12 <= size; at += 4)
        if (memcmp(image + at, c->name, strlen(c->name)) == 0) {
            name_off = at;
            break;
        }

    /*
     * Both modules checked — the Berserk at 0x801008A4..0x801008CC and the Tank
     * Commander at 0x80100638..0x80100648 — pass their sound names to an import
     * BY VALUE, packing each 12-byte string into registers a byte at a time
     * because the compiler cannot use aligned loads. So the run this function
     * looks for is real, and it is what the module reads.
     *
     * If the module's own name is not in its image, the anchor is gone but the
     * table need not be. Two of the seven -- Tankcomm and Berserk -- returned
     * zero names under the anchored scan while their sounds are demonstrably on
     * the disc (63 VAG entries begin `tnk_`), so falling back to a whole-image
     * scan is worth more than failing. The run-of-three test below is what
     * actually validates a candidate either way.
     */
    for (at = name_off ? name_off + 12 : 0; at + 36 <= size; at += 4) {
        if (slot_is_name(image, size, at) &&
            slot_is_name(image, size, at + 12) &&
            slot_is_name(image, size, at + 24))
            break;
    }

    if (at + 36 > size)
        return 0;

    while (n < out_count && slot_is_name(image, size, at)) {
        out[n++] = (const char *)(image + at);
        at += 12;
    }

    return n;
}

/* ------------------------------------------------------------------------- */
/* A creature's sounds, decoded from the module's own registrations           */
/* ------------------------------------------------------------------------- */
const char *q2_creature_sound_for_addr(const q2_cre_sound_bind *binds,
                                       u32 count, u32 addr)
{
    u32 i;

    if (!binds)
        return NULL;

    /* Later registrations can be conditional fallbacks (Strider). Keep the
     * primary here; the module-aware resolver handles unconditional stores. */
    for (i = 0; i < count; i++)
        if (binds[i].addr == addr)
            return binds[i].name;

    return NULL;
}

/* ------------------------------------------------------------------------- */
/* The fallback groups — see the block in creature.h for every address        */
/* ------------------------------------------------------------------------- */
/*
 * Keyed by module NAME rather than declared in cre_soldier.c / cre_tankcomm.c,
 * because the resolver is reached from the sound path (creworld.c) and not from
 * the transcription, and a table it can consult without linking against seven
 * creature files is the smaller join. The slot the module plays is unchanged —
 * cre_soldier.c's SOL_SND_PAIN is still +0x32B4, which is what the module
 * passes; the substitution is not the play site's business.
 *
 * The names are the registrations `q2psx-inspect creatures` prints for the two
 * modules — 801032AC=sol_pain1 .. 801032C0=sol_deth3, 80102100=tnk_idle1,
 * 80102110=tnk_step — and the Soldier's six agree with its transcribed name
 * table (cre_soldier.c, indices 2..7).
 */
static const q2_cre_sound_fallback k_sound_fallbacks[] = {
    /* Soldier pain: module+0xE50, preference pain2 -> pain1 -> pain3. */
    { "Soldier",  0x32AC, { 0x32B0, 0x32AC, 0x32B4 },
      "sol_pain1", { "sol_pain2", "sol_pain1", "sol_pain3" } },
    { "Soldier",  0x32B0, { 0x32B0, 0x32AC, 0x32B4 },
      "sol_pain2", { "sol_pain2", "sol_pain1", "sol_pain3" } },
    { "Soldier",  0x32B4, { 0x32B0, 0x32AC, 0x32B4 },
      "sol_pain3", { "sol_pain2", "sol_pain1", "sol_pain3" } },
    /* Soldier death: module+0xEB8, preference deth2 -> deth1 -> deth3. */
    { "Soldier",  0x32B8, { 0x32BC, 0x32B8, 0x32C0 },
      "sol_deth1", { "sol_deth2", "sol_deth1", "sol_deth3" } },
    { "Soldier",  0x32BC, { 0x32BC, 0x32B8, 0x32C0 },
      "sol_deth2", { "sol_deth2", "sol_deth1", "sol_deth3" } },
    { "Soldier",  0x32C0, { 0x32BC, 0x32B8, 0x32C0 },
      "sol_deth3", { "sol_deth2", "sol_deth1", "sol_deth3" } },
    /* Tankcomm idle: module+0x808, tnk_idle1 or else tnk_step. */
    { "Tankcomm", 0x2100, { 0x2100, 0x2110, 0 },
      "tnk_idle1", { "tnk_idle1", "tnk_step", NULL } },
    /* Strider +0x928..0xEF8 registers each alternate only if its slot is
     * empty. Idle and footstep initially share str_step but have distinct
     * fallback handles, so the client must resolve by address. */
    { "Strider", 0x2BC0, { 0x2BC0, 0x2BC0, 0 },
      "str_step", { "str_step", "tnk_idle1", NULL } },
    { "Strider", 0x2BC4, { 0x2BC4, 0x2BC4, 0 },
      "str_pain1", { "str_pain1", "tnk_pain", NULL } },
    { "Strider", 0x2BC8, { 0x2BC8, 0x2BC8, 0 },
      "str_idle", { "str_idle", "tnk_idle1", NULL } },
    { "Strider", 0x2BCC, { 0x2BCC, 0x2BCC, 0 },
      "str_deth", { "str_deth", "tnk_death", NULL } },
    { "Strider", 0x2BD0, { 0x2BD0, 0x2BD0, 0 },
      "str_step", { "str_step", "tnk_step", NULL } },
    { "Strider", 0x2BD4, { 0x2BD4, 0x2BD4, 0 },
      "str_sght", { "str_sght", "tnk_sight1", NULL } },
    { "Strider", 0x2BDC, { 0x2BDC, 0x2BDC, 0 },
      "str_atck2c", { "str_atck2c", "tnk_atck1", NULL } },
    /* Rider+0xD8C..0xEF8; Jorg's embedded copy uses the same names. */
    { "Rider", 0x2A10, { 0x2A14, 0, 0 },
      "mak_pain3", { "mak_pain2", NULL, NULL } },
    { "Rider", 0x2A18, { 0x2A14, 0, 0 },
      "mak_pain1", { "mak_pain2", NULL, NULL } },
    { "Rider", 0x2A3C, { 0x2A38, 0, 0 },
      "mak_voice3", { "mak_voice4", NULL, NULL } },
    { "Rider", 0x2A40, { 0x2A38, 0, 0 },
      "mak_voice", { "mak_voice4", NULL, NULL } },
    { "Rider", 0x2A28, { 0x2A28, 0, 0 },
      "mak_bfg_fire", { "wep_bfg__f1y", NULL, NULL } },
    { "Rider", 0x2A20, { 0x2A24, 0, 0 },
      "mak_step1", { "mak_step2", NULL, NULL } },
    { "Rider", 0x2A24, { 0x2A20, 0, 0 },
      "mak_step2", { "mak_step1", NULL, NULL } },
    /* Jorg+0xEE4: either missing footstep takes the other handle. */
    { "Jorg", 0x4A64, { 0x4A68, 0, 0 },
      "bs3_step1", { "bs3_step2", NULL, NULL } },
    { "Jorg", 0x4A68, { 0x4A64, 0, 0 },
      "bs3_step2", { "bs3_step1", NULL, NULL } }
};

const q2_cre_sound_fallback *q2_creature_sound_fallbacks(u32 *count)
{
    if (count)
        *count = (u32)(sizeof(k_sound_fallbacks) /
                       sizeof(k_sound_fallbacks[0]));
    return k_sound_fallbacks;
}

const char *q2_creature_sound_resolve(const char *module_name,
                                      const q2_cre_sound_bind *binds,
                                      u32 count, u32 load_base, u32 addr,
                                      bool (*bank_has)(const char *name,
                                                       void *user),
                                      void *user)
{
    const char *own = q2_creature_sound_for_addr(binds, count, addr);
    u32 i;

    if (!module_name)
        return own;

    /* Jorg registers its embedded Rider's independent handles at +0x2060
     * relative to the standalone module. Preserve the actual registration
     * above, then share only the verified conditional fill rules. */
    if ((!strcmp(module_name, "Jorg") || !strcmp(module_name, "JorgRider")) &&
        addr >= load_base + 0x4A70 && addr <= load_base + 0x4AA4) {
        module_name = "Rider"; addr -= 0x2060;
    }

    /* Ironmaiden +0xA28 writes a second pain registration to 1B94;
     * +0xAC0 unconditionally replaces that slot with its sight sound. */
    if (!strcmp(module_name, "Ironmaiden") && addr == load_base + 0x1B94) {
        for (i = count; binds && i > 0; i--)
            if (binds[i - 1].addr == addr) return binds[i - 1].name;
    }

    for (i = 0; i < sizeof(k_sound_fallbacks) / sizeof(k_sound_fallbacks[0]);
         i++) {
        const q2_cre_sound_fallback *g = &k_sound_fallbacks[i];
        u32 j;

        if (strcmp(g->module, module_name) != 0)
            continue;
        if (load_base + g->slot_off != addr)
            continue;

        /*
         * THE SLOT'S OWN NAME WINS WHEN THE BANK CARRIES IT, and it wins even
         * over a higher preference. Every store in the fill pass is guarded by
         * a `bne v0, zero` on the DESTINATION — 0x80100EAC for +0x32B4,
         * 0x80100E7C for +0x32AC, 0x80100810 for the Tankcomm's +0x2100 — so a
         * handle that resolved is never overwritten. The preference order only
         * decides what a NULL slot is filled with.
         */
        if (own && own[0] && (!bank_has || bank_has(own, user)))
            return own;

        for (j = 0; j < 3; j++) {
            const char *name;

            if (g->pref_off[j] == 0)
                break;

            name = g->pref_name[j];
            if (!name || !name[0])
                continue;
            if (!bank_has || bank_has(name, user))
                return name;
        }

        /*
         * Nothing in the group resolved: on the console the handle stays zero
         * and playing it does nothing. Return NULL rather than the registered
         * name, so a caller counting misses counts a real silence.
         */
        return NULL;
    }

    return own;
}

const char *q2_creature_sound_fallback(const char *module_name,
                                       const char *registered,
                                       bool (*bank_has)(const char *name,
                                                        void *user),
                                       void *user)
{
    u32 i;

    if (!module_name || !registered || !registered[0])
        return registered;

    for (i = 0; i < sizeof(k_sound_fallbacks) / sizeof(k_sound_fallbacks[0]);
         i++) {
        const q2_cre_sound_fallback *g = &k_sound_fallbacks[i];
        u32 j;

        if (strcmp(g->module, module_name) != 0)
            continue;
        if (strcmp(g->slot_name, registered) != 0)
            continue;

        /* The same two steps as the address form above, for the same reasons:
         * a resolved slot is never overwritten, and a NULL one takes the first
         * handle in the module's order that resolved. */
        if (!bank_has || bank_has(registered, user))
            return registered;

        for (j = 0; j < 3; j++) {
            if (!g->pref_name[j])
                break;
            if (bank_has(g->pref_name[j], user))
                return g->pref_name[j];
        }

        return NULL;
    }

    return registered;
}

/*
 * The whole name table, in order, one entry per record.
 *
 * Export 3 points directly to this table. 0x8007E7F0/0x8007E95C walks it in
 * 20-byte steps; 0x8007EA00 stops at a negative first frame. A reversed range
 * cannot match a frame but does not end the table (Boss2's unused Stand2 is
 * 21..0). Neither the name nor the range belongs to the following record.
 */
u32 q2_creature_frame_names(const q2_creature *c, const u8 *image, size_t size,
                            q2_cre_frame_name *out, u32 out_count)
{
    size_t off;
    u32 address;
    u32    n = 0;

    if (!c || !image || size < 16 || !out || out_count == 0)
        return 0;
    address = q2_rd_u32(image + 12);
    if (!address || address < c->base || (size_t)(address - c->base) >= size)
        return 0;
    for (off = address - c->base; off + CRE_MOVE_NAME_STRIDE <= size && n < out_count;
         off += CRE_MOVE_NAME_STRIDE) {
        s32 first = (s16)q2_rd_u16(image + off);
        s32 last = (s16)q2_rd_u16(image + off + 2);
        if (first < 0)
            break;
        if (last < first)
            continue;
        if (!name_text_ok(image + off + 4, size - off - 4))
            break;
        out[n].first = first;
        out[n].last = last;
        out[n].name = (const char *)(image + off + 4);
        n++;
    }

    return n;
}

const q2_cre_frame_name *q2_creature_frame_name_at(const q2_cre_frame_name *tab,
                                                   u32 count, s32 frame)
{
    u32 i;

    if (!tab)
        return NULL;

    for (i = 0; i < count; i++)
        if (frame >= tab[i].first && frame <= tab[i].last)
            return &tab[i];

    return NULL;
}

u32 q2_creature_move_names(const q2_creature *c, const u8 *image, size_t size,
                           const char **out, u32 out_count)
{
    q2_cre_frame_name tab[Q2_CRE_MAX_MOVES];
    u32 n, found = 0, i, k;

    if (!c || !image || !out || c->move_count == 0)
        return 0;

    for (i = 0; i < out_count; i++)
        out[i] = NULL;

    /*
     * Built on the ANCHORED table rather than on its own scan.
     *
     * This used to walk the image four bytes at a time taking anything that
     * validated, and a 20-byte record read at a 4-byte offset can validate
     * against the tail of its predecessor — which is how six of the Soldier's
     * records came back paired with the wrong name (0-0 "Run" is really
     * "Fire 1 Ready", 30-31 "Death6" is "Fire 3 Aim", and so on). One reader,
     * one table, one answer.
     *
     * A record names EVERY move whose range it matches, not just the first: a
     * module may list the same range in more than one callback slot — the
     * Arachner has 16-24 twice — and stopping at the first left the rest
     * unnamed.
     */
    n = q2_creature_frame_names(c, image, size, tab, Q2_CRE_MAX_MOVES);

    for (k = 0; k < n; k++) {
        for (i = 0; i < c->move_count && i < out_count; i++) {
            if (out[i])
                continue;
            if (c->move[i].first_frame == tab[k].first &&
                c->move[i].last_frame  == tab[k].last) {
                out[i] = tab[k].name;
                found++;
            }
        }
    }

    return found;
}

bool q2_creature_think_is_empty(const q2_creature *c, const u8 *image,
                                size_t size, u32 base, u32 index)
{
    u32 addr, off, w;

    if (!c || !image || index >= Q2_CLASS_METHOD_COUNT)
        return false;

    addr = c->method[index];
    if (!addr || addr < base)
        return false;

    off = addr - base;
    if ((size_t)off + 8 > size)
        return false;

    /*
     * `jr ra` as the very first instruction. The Berserk's think 7 (0x80101008)
     * is exactly that, and a frame naming it is asking for nothing to happen --
     * a decoded answer, not a failure to decode. Without this the census reports
     * it as a think that "does not decode to an action", which reads as a gap in
     * the port when it is a property of the module.
     */
    w = q2_rd_u32(image + off);
    return w == 0x03E00008u;
}

u32 q2_creature_unclaimed_names(const q2_creature *c, const u8 *image,
                                size_t size, const char **out, u32 out_count)
{
    q2_cre_frame_name names[Q2_CRE_MAX_MOVES];
    u32 count, index;
    u32 unclaimed = 0;

    if (!c || !image)
        return 0;

    /*
     * Name records the decoder's move list does not account for. A non-zero
     * count means the module names behaviour we never reached -- a gap in the
     * decode, not in the disc.
     */
    count = q2_creature_frame_names(c, image, size, names, Q2_CRE_MAX_MOVES);
    for (index = 0; index < count; index++) {
        s32 first, last;
        u32 i;
        bool claimed = false;

        first = names[index].first;
        last = names[index].last;

        for (i = 0; i < c->move_count; i++) {
            if (c->move[i].first_frame == first &&
                c->move[i].last_frame  == last) {
                claimed = true;
                break;
            }
        }
        if (!claimed) {
            unclaimed++;
            if (out && unclaimed <= out_count)
                out[unclaimed - 1] = names[index].name;
        }
    }

    return unclaimed;
}

/*
 * Install moves the module NAMES but no callback path reached.
 *
 * The decoder finds moves by following callbacks and endfuncs, so a move only
 * a branch-taken path installs is invisible to it — the Arachner names
 * "Pain 2" (78-93) and "Stand" (65-77) and this port had neither, while those
 * two spans are exactly the two clips in its model that nothing else claimed.
 *
 * The name record gives the frame RANGE but not the move record's address, so
 * the range is what we search for: a move record is
 * {u32 first; u32 last; u32 frames; u32 endfunc}, and scanning word-aligned for
 * a first/last pair matching a named range finds it. `add_move` then validates
 * it exactly as it validates one reached by a callback — the frames must be in
 * the image and every ai byte must select a real verb — so a coincidental pair
 * of words is rejected on the same terms as any other candidate.
 *
 * These come in with via == -2, distinct from -1 ("reached only through another
 * move's endfunc"), so nothing mistakes them for callback-installed behaviour.
 */
static void add_named_but_unreached(q2_creature *c, dec *d, const u8 *image,
                                    size_t size)
{
    q2_cre_frame_name names[Q2_CRE_MAX_MOVES];
    u32 count, index;

    count = q2_creature_frame_names(c, image, size, names, Q2_CRE_MAX_MOVES);
    for (index = 0; index < count; index++) {
        s32 first, last;
        u32 i, scan;
        bool claimed = false;

        first = names[index].first;
        last = names[index].last;

        for (i = 0; i < c->move_count; i++)
            if (c->move[i].first_frame == first &&
                c->move[i].last_frame  == last) {
                claimed = true;
                break;
            }
        if (claimed)
            continue;

        /* Find the move record carrying this range and let add_move judge it. */
        for (scan = 0; (size_t)scan + 16 <= size; scan += 4) {
            if ((s32)q2_rd_u32(image + scan) != first)
                continue;
            if ((s32)q2_rd_u32(image + scan + 4) != last)
                continue;
            if (add_move(c, d, d->base + scan, -2))
                break;
        }
    }
}

/*
 * Split a decoded move wherever the module's OWN name table says there is a
 * boundary inside it.
 *
 * The decoder finds a move by following a callback to a frame record and
 * walking forward, so two moves laid end to end in the image come back as one.
 * The Soldier's 0-11 is really Run(0-0), Fire 1 Ready(1-5), Fire 1 Aim(6-8) and
 * Fire 1 Shoot(9-11); its 215-247 is Fire 2 Done(215-224) and Walk 1 Loop
 * (225-247). A merged move's length is a sum, so three times it matches no clip
 * -- which is how these surfaced at all, as the only exceptions to the 3:1 rule
 * (openquestions #51h, #58).
 *
 * The boundaries are not inferred: the module ships a {s16 first, s16 last,
 * char[16]} record per move, and this splits at exactly those. A move the table
 * does not subdivide is left alone.
 */
/*
 * Shrink a move that CONTAINS another decoded move.
 *
 * `split_merged_moves` splits on the module's name records, which catches the
 * Soldier. It does not catch the Tank Commander, whose 55-70 contains its own
 * 65-70 as a separate decoded move — no name record marks that boundary, but
 * another move does, and a move cannot span frames another move owns.
 *
 * The evidence this is real is arithmetic rather than structural: summing a
 * creature's distinct move lengths and tripling gives its model's total clip
 * frames exactly on four of seven creatures. The Tank Commander was -18, and
 * 55-70 overlapping 65-70 double-counts exactly 6 AI frames. It is the only
 * overlap on the disc.
 */
static void split_overlapping_moves(q2_creature *c)
{
    u32 i, j;

    if (!c)
        return;

    for (i = 0; i < c->move_count; i++) {
        s32 cut = -1;

        for (j = 0; j < c->move_count; j++) {
            const q2_cre_move *o = &c->move[j];

            if (i == j)
                continue;
            if (o->first_frame <= c->move[i].first_frame ||
                o->last_frame  >  c->move[i].last_frame)
                continue;               /* not strictly inside */
            if (cut < 0 || o->first_frame < cut)
                cut = o->first_frame;
        }

        /*
         * AND THIS ONE DOES NOT TRUNCATE EITHER, for the same reason.
         *
         * It used to cut a move short wherever another move started strictly
         * inside it — the Tank Commander's 55..70 against its 65..70, which
         * became 55..64. Both are module records with their own endfuncs, and
         * an installer that asks for 55 wants the range the module gave it.
         * The overlap is a property of the clip table, not an error in the
         * mmove list.
         */
        (void)cut;
    }
}

static void split_merged_moves(q2_creature *c, const u8 *image, size_t size)
{
    q2_cre_frame_name names[Q2_CRE_MAX_MOVES];
    u32 i, count;

    if (!c || !image)
        return;

    count = q2_creature_frame_names(c, image, size, names, Q2_CRE_MAX_MOVES);
    for (i = 0; i < c->move_count; i++) {
        u32 index;
        u32 pieces = 0;

        for (index = 0; index < count; index++) {
            s32 first, last;
            u32 fi;

            first = names[index].first;
            last = names[index].last;

            /* A STRICT interior piece: inside the move, and not the move. */
            if (first < c->move[i].first_frame || last > c->move[i].last_frame)
                continue;
            if (first == c->move[i].first_frame &&
                last  == c->move[i].last_frame)
                continue;
            /*
             * The piece that STARTS where the parent does is the parent, once
             * the parent is shrunk below. Creating it as well would leave two
             * moves with the same range.
             */
            if (first == c->move[i].first_frame)
                continue;
            if (c->move_count >= Q2_CRE_MAX_MOVES)
                break;

            /* Does a piece with this range already exist? */
            for (fi = 0; fi < c->move_count; fi++)
                if (c->move[fi].first_frame == first &&
                    c->move[fi].last_frame  == last)
                    break;
            if (fi < c->move_count)
                continue;

            c->move[c->move_count] = c->move[i];
            c->move[c->move_count].clip_piece  = true;
            c->move[c->move_count].first_frame = first;
            c->move[c->move_count].last_frame  = last;
            c->move[c->move_count].frame_index =
                c->move[i].frame_index +
                (u32)(first - c->move[i].first_frame);
            c->move[c->move_count].frame_count = (u32)(last - first + 1);
            /* Only the piece that ends where the parent ended keeps the
             * parent's endfunc; the others run on into the next piece. */
            if (last != c->move[i].last_frame) {
                c->move[c->move_count].endfunc_addr = 0;
                c->move[c->move_count].endfunc_move = 0;
            }
            c->move_count++;
            pieces++;
        }

        /*
         * THE PARENT IS LEFT ALONE. It used to be shrunk to its first piece
         * with its endfunc cleared, on the reading that "the merged span is not
         * a move". It is: the module declares it, with a first frame, a last
         * frame and an endfunc, and it is what the AI installs.
         *
         * The pieces are the MODEL's clip boundaries, which is a different
         * fact about the same frames and is what the animation census needs.
         * Both are kept; the pieces are flagged so q2_cre_find_move will not
         * hand one to a creature. See q2_cre_move in creature.h for what
         * shrinking the parents cost.
         */
        (void)pieces;
    }
}

bool q2_creature_decode(q2_creature *out, const u8 *image, size_t size,
                        u32 base, const char *name)
{
    return q2_creature_decode_entry(out, image, size, base, name, 0, -1);
}

bool q2_creature_decode_entry(q2_creature *out, const u8 *image, size_t size,
                              u32 base, const char *name, u32 spawn_entry,
                              s32 method_class)
{
    dec d;
    u32 export0, export2;
    u32 i, pass;

    if (!out || !image || size < 64)
        return false;

    memset(out, 0, sizeof(*out));
    out->base = base;
    if (name) {
        size_t n = strlen(name);
        if (n > sizeof(out->name) - 1) n = sizeof(out->name) - 1;
        memcpy(out->name, name, n);
        out->name[n] = 0;
    }

    memset(&d, 0, sizeof(d));
    d.img  = image;
    d.size = size;
    d.base = base;

    export0 = spawn_entry ? spawn_entry : q2_rd_u32(image + 0);
    export2 = q2_rd_u32(image + 8);

    if (in_image(&d, export2, 4))
        decode_classes(out, &d, export2, method_class);
    if (in_image(&d, export0, 4))
        decode_spawn(out, &d, export0);

    /* The callbacks the spawn function installed. */
    for (i = 0; i < 13; i++)
        if (out->callback[i])
            follow_callback(out, &d, out->callback[i], (s32)i, 0);

    /*
     * Then everything reachable from a method-table entry, because a move is
     * often installed by a think handler rather than by a callback — the
     * attack sequence chains through several. Repeat until it stops growing,
     * which is what makes this a closure rather than one hop.
     */
    for (pass = 0; pass < 4; pass++) {
        u32 before = out->move_count;

        for (i = 0; i < out->method_count; i++)
            if (out->method[i])
                follow_callback(out, &d, out->method[i], -1, 1);

        for (i = 0; i < out->move_count; i++)
            if (out->move[i].endfunc_addr)
                follow_callback(out, &d, out->move[i].endfunc_addr, -1, 1);

        if (out->move_count == before)
            break;
    }

    add_named_but_unreached(out, &d, image, size);
    split_merged_moves(out, image, size);
    split_overlapping_moves(out);

    return true;
}

const q2_cre_move *q2_creature_move_via(const q2_creature *c, u32 callback_slot)
{
    u32 i;
    if (!c)
        return NULL;
    for (i = 0; i < c->move_count; i++)
        if (c->move[i].via == (s32)callback_slot)
            return &c->move[i];
    return NULL;
}

const q2_cre_move *q2_creature_move_at(const q2_creature *c, u32 addr)
{
    u32 i;
    if (!c)
        return NULL;
    for (i = 0; i < c->move_count; i++)
        if (c->move[i].addr == addr)
            return &c->move[i];
    return NULL;
}

void q2_creature_mmove(const q2_creature *c, const q2_cre_move *m,
                       q2_endfunc endfunc, q2_mmove *out)
{
    if (!c || !m || !out)
        return;

    memset(out, 0, sizeof(*out));
    out->first_frame    = m->first_frame;
    out->last_frame     = m->last_frame;
    out->frames         = &c->frames[m->frame_index];
    out->endfunc        = endfunc;
    out->image_offset   = m->addr - c->base;
    out->frames_offset  = m->frames_addr - c->base;
    out->endfunc_offset = m->endfunc_addr ? (m->endfunc_addr - c->base) : 0;
}

static u32 collect(const q2_creature *c, u8 *out, u32 cap, bool verbs)
{
    bool seen[256];
    u32 i, n = 0;

    memset(seen, 0, sizeof(seen));

    if (!c || !out)
        return 0;

    for (i = 0; i < c->frame_count; i++) {
        u32 v;
        if (verbs) {
            if (!(c->frames[i].ai & Q2_AI_LOCAL_FLAG))
                continue;
            v = c->frames[i].ai & 0x7Fu;
        } else {
            v = c->frames[i].think;
            if (!v)
                continue;
        }
        seen[v] = true;
    }

    for (i = 0; i < 256 && n < cap; i++)
        if (seen[i])
            out[n++] = (u8)i;

    return n;
}

u32 q2_creature_think_indices(const q2_creature *c, u8 *out, u32 cap)
{
    return collect(c, out, cap, false);
}

u32 q2_creature_verb_indices(const q2_creature *c, u8 *out, u32 cap)
{
    return collect(c, out, cap, true);
}

/* ------------------------------------------------------------------------- */
/* Decoding what a think function does                                        */
/* ------------------------------------------------------------------------- */
/*
 * The same walk as everything else here, with two additions: a small shadow of
 * the stack frame, because `fire_hit`'s damage, kick and aim are pushed there
 * as constants, and a note of which register holds which import slot, because
 * that is what turns an indirect call into a named action.
 *
 * `gated` marks a step reached only through a branch. It matters: a creature
 * whose sound plays one time in five must not play it every time, and the
 * executor uses the flag to reproduce that rather than pretending the branch
 * is not there.
 */
#define STK_SLOTS 48

typedef struct think_dec {
    dec  d;
    s32  stk[STK_SLOTS];        /* sp + i*4 */
    bool stk_known[STK_SLOTS];
    u32  imp_slot[32];          /* per register: the import offset it holds */
    bool imp_known[32];
    /* The last small constant added into a register, which is how the damage
     * base survives the modulo arithmetic. */
    s32  last_add[32];
    bool last_add_known[32];
    /*
     * The modulo's divisor, recovered from the multiply-back rather than from
     * the divide's magic constant. After a magic divide the compiler
     * reconstructs `q * d` to subtract it, and for a small `d` it does that
     * with a shift and an add — `sll rX, q, 2; addu rX, rX, q` is times five.
     * Reading the reconstruction is exact where keying on the magic is not:
     * 0x66666667 serves both five and ten, and an earlier version of this
     * reported the Arachner's `rand() % 5` as `% 10` for exactly that reason.
     */
    /*
     * AND THE RECONSTRUCTION DOES NOT ALWAYS STOP AT THE ADD.
     *
     * For a divisor with a factor of two the compiler builds the odd part with
     * the shift-and-add and then doubles it, so a divide by SIX comes back as
     *
     *     sll  v1, a0, 1      ; 2q
     *     addu v1, v1, a0     ; 3q      <- the pair this used to stop on
     *     sll  v1, v1, 1      ; 6q
     *     subu v0, v0, v1     ; x - 6q
     *
     * Reading only the pair reported the Berserk's two attacks as `rand() % 3`
     * where the module says `% 6` — and id's berserker uses `rand() % 6` in
     * both, so the short read disagreed with the one independent check there
     * is. `mb_dest` is the register the add left the product in; a shift that
     * immediately consumes it is part of the same multiply.
     */
    s32  mul_back;
    u32  mb_dest;
    u32  mb_base;               /* quotient register for chained products */
    bool mb_dest_known;
    u32  pend_shift_reg;
    s32  pend_shift_k;
    bool pend_shift;

    /*
     * Where a register was loaded FROM. A sound handle is a runtime value the
     * loader wrote into module data, so the image holds nothing useful at it —
     * what identifies the sound is the ADDRESS it came from, and that is a
     * relocated module address which is stable.
     */
    u32  load_src[32];
    bool load_known[32];

    /* How a register's value relates to the aiflags word it was loaded from,
     * which is what makes a duck helper readable. */
    u32  flag_set[32];
    u32  flag_clear[32];
    bool flag_known[32];
} think_dec;

static void think_note_stack(think_dec *t, u32 w)
{
    u32 rt;
    s32 off;

    if (OP(w) != OP_SW || RS(w) != 29)      /* sp is register 29 */
        return;

    off = IMM(w);
    if (off < 0 || off >= STK_SLOTS * 4 || (off & 3))
        return;

    rt = RT(w);
    if (rt == 0) {
        t->stk[off / 4] = 0;
        t->stk_known[off / 4] = true;
    } else if (t->d.known[rt]) {
        t->stk[off / 4] = (s32)t->d.reg[rt];
        t->stk_known[off / 4] = true;
    } else {
        t->stk_known[off / 4] = false;
    }
}

static bool stk_get(const think_dec *t, s32 off, s32 *out)
{
    if (off < 0 || off >= STK_SLOTS * 4 || (off & 3))
        return false;
    if (!t->stk_known[off / 4])
        return false;
    *out = t->stk[off / 4];
    return true;
}

static q2_cre_step *push_step(q2_cre_think *th)
{
    q2_cre_step *s;
    if (th->step_count >= Q2_CRE_MAX_STEPS)
        return NULL;
    s = &th->step[th->step_count++];
    memset(s, 0, sizeof(*s));
    return s;
}

static void decode_body(const q2_creature *c, think_dec *t, u32 entry,
                        q2_cre_think *th, int inline_depth);

static void decode_one_think(const q2_creature *c, think_dec *t, u32 entry,
                             q2_cre_think *th)
{
    memset(th, 0, sizeof(*th));

    if (!entry || !in_image(&t->d, entry, 4))
        return;

    dec_reset(&t->d);
    memset(t->stk_known, 0, sizeof(t->stk_known));
    memset(t->imp_known, 0, sizeof(t->imp_known));
    memset(t->load_known, 0, sizeof(t->load_known));
    memset(t->last_add_known, 0, sizeof(t->last_add_known));
    memset(t->flag_known, 0, sizeof(t->flag_known));
    t->mul_back = 0;
    t->mb_dest_known = false;
    t->pend_shift = false;

    decode_body(c, t, entry, th, 0);

    if (th->step_count)
        th->decoded = true;
}

/*
 * One function body.
 *
 * A direct `jal` into the module is followed rather than ignored, because the
 * six Soldier firing frames are one-line wrappers around a shared
 * `soldier_fire` and every other creature does the same thing — a decoder that
 * stops at the wrapper sees six think functions that do nothing at all. One
 * level of inlining is enough for every module on the disc; the argument the
 * wrapper passes is already in the register map when the callee is entered,
 * which is what makes the shared function's behaviour attributable to the
 * frame that called it.
 */
static void decode_body(const q2_creature *c, think_dec *t, u32 entry,
                        q2_cre_think *th, int inline_depth)
{
    u32 addr, end;
    int depth = 0;              /* how many branches we are past */

    end = walk_end(c, &t->d, entry);

    for (addr = entry; addr < end; addr += 4) {
        u32 w, op, rt, rs;

        if (!in_image(&t->d, addr, 4))
            break;
        w  = word_at(&t->d, addr);
        op = OP(w);
        rt = RT(w);
        rs = RS(w);

        /*
         * A RETURN FOLLOWED BY A PROLOGUE IS THE NEXT FUNCTION.
         *
         * `walk_end` deliberately does not stop at the first `jr ra` — these
         * functions have several, one per early return, and stopping at the
         * first found one move out of five when the decoder was young. What it
         * stops at instead is the next address KNOWN to start a function, and
         * that set is the callbacks, the methods and the endfuncs. A function
         * in none of those three is invisible to it.
         *
         * The Infantry's spawn function is exactly that. It sits at
         * module+0x13F0, immediately after think 11's `jr ra` at +0x13EC, and
         * is reachable only as export 0 — so the walk ran straight through the
         * return and attributed four of the SPAWN function's steps to
         * `infantry_smack`. That is why the census printed a melee ending in
         * `walkstart(+FC)`: a creature was credited with waking itself up in
         * the middle of a punch.
         *
         * `jr ra` in a delay-slot pair followed by `addiu sp, sp, -N` is a
         * prologue and nothing else — a mid-function epilogue is followed by
         * more body, never by a stack frame being opened. So this cannot
         * truncate the multi-return case the comment above protects.
         */
        /*
         * And the DELAY SLOT is still part of this function, which is where
         * the first version of this test went wrong: `soldier_duck_up`
         * (module+0x1EC0) puts the whole point of the function — the store
         * that clears AI_DUCKED — in the delay slot of its own `jr ra`.
         * Breaking on sight of the return dropped it, and the census lost
         * three of the disc's duck helpers to a fix meant to remove one
         * over-read. So the end is moved to just past the pair rather than
         * taken immediately.
         */
        if (w == 0x03E00008u && in_image(&t->d, addr + 8, 4)) {
            u32 next = word_at(&t->d, addr + 8);

            if ((next & 0xFFFF8000u) == 0x27BD8000u)    /* addiu sp, sp, -N */
                end = addr + 8;
        }

        /* A branch means everything after it may be conditional. */
        if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07
            || op == 0x01)
            depth++;

        /*
         * `lw rX, imm(sp)` — put the shadowed constant back in the register.
         * The aim triple is pushed as constants and loaded into a1..a3 right
         * before the call, and the stack offsets differ per creature because
         * the frame sizes do, so reading fixed offsets does not generalise.
         * Propagating the shadow does.
         */
        if (op == 0x23 && rt != 0 && rs == 29) {
            s32 v;
            if (stk_get(t, IMM(w), &v)) {
                t->d.reg[rt]   = (u32)v;
                t->d.known[rt] = true;
            } else {
                t->d.known[rt] = false;
            }
            t->imp_known[rt]  = false;
            t->load_known[rt] = false;
            continue;
        }

        /* `lw rX, imm(module_base)` — rX now holds an import. */
        if (op == 0x23 && rt != 0) {
            if (t->d.known[rs] && t->d.reg[rs] == t->d.base) {
                t->imp_slot[rt] = (u32)IMM(w);
                t->imp_known[rt] = true;
            } else {
                t->imp_known[rt] = false;
            }
            if (t->d.known[rs]) {
                t->load_src[rt]   = t->d.reg[rs] + (u32)IMM(w);
                t->load_known[rt] = true;
            } else {
                t->load_known[rt] = false;
            }
            t->d.known[rt] = false;
        }

        /*
         * `sll rd, rs, k` then `addu/subu rd2, rd, rs` is a multiply by
         * (1<<k)+1 or (1<<k)-1 — the multiply-back after a magic divide.
         */
        if (op == 0 && (w & 0x3F) == 0x00 && rt != 0) {      /* sll */
            s32 k = (s32)((w >> 6) & 0x1F);

            /* The doubling that finishes an even divisor — see mb_dest. */
            if (t->mb_dest_known && rt == t->mb_dest && t->mul_back >= 2
                && k > 0 && k < 8) {
                t->mul_back <<= k;
                t->mb_dest    = (w >> 11) & 0x1F;
            } else {
                t->mb_dest_known = false;
            }

            t->pend_shift_reg = rt;
            t->pend_shift_k   = k;
            t->pend_shift     = k > 0 && k < 12;
        } else if (op == 0 && ((w & 0x3F) == 0x21 || (w & 0x3F) == 0x23)
                   && t->pend_shift) {
            /* Strider's %50 builds (((q*2)+q)*8+q)*2. Retain the
             * quotient across the second add instead of restarting at 9q. */
            if (t->mb_dest_known &&
                ((rs == t->mb_dest && rt == t->mb_base) ||
                 ((w & 0x3F) == 0x21 && rt == t->mb_dest && rs == t->mb_base))) {
                t->mul_back += (w & 0x3F) == 0x21 ? 1 : -1;
                t->mb_dest = (w >> 11) & 0x1F;
                t->pend_shift = false;
            }
            /* addu/subu rd, rs, rt with one operand the shifted value */
            if (t->pend_shift && (rs == t->pend_shift_reg || rt == t->pend_shift_reg)) {
                s32 f = (s32)1 << t->pend_shift_k;
                s32 got = ((w & 0x3F) == 0x21) ? f + 1 : f - 1;
                /* A multiply-back of one is not a modulo; it is a shift pair
                 * that happened to look like one. */
                if (got >= 2) {
                    t->mul_back      = got;
                    t->mb_dest       = (w >> 11) & 0x1F;
                    t->mb_base       = t->pend_shift_reg;
                    t->mb_dest_known = true;
                }
            }
            t->pend_shift = false;
        } else if (op != 0 || (w & 0x3F) != 0x00) {
            /* any other instruction breaks the pair */
            if (op != 0x00)
                t->pend_shift = false;
        }

        /* An explicit multiply by a small constant is the other form. */
        if (op == 0 && (w & 0x3F) == 0x18) {                 /* mult */
            if (t->d.known[rt] && t->d.reg[rt] > 1 && t->d.reg[rt] < 64)
                t->mul_back = (s32)t->d.reg[rt];
            else if (t->d.known[rs] && t->d.reg[rs] > 1 && t->d.reg[rs] < 64)
                t->mul_back = (s32)t->d.reg[rs];
        }

        /* Remember the last small constant added into a register. */
        if (op == OP_ADDIU && rt != 0) {
            s32 k = IMM(w);
            if (k > 0 && k <= 400) {
                t->last_add[rt]       = k;
                t->last_add_known[rt] = true;
            }
        }

        /*
         * An and/or against a constant, applied to a value that came out of
         * the aiflags word. Tracking the mask rather than the value is what
         * lets a store back to +0xDC be read as "set these bits, clear those".
         */
        if (op == 0 && (w & 0x3F) == 0x24 && rt != 0) {          /* and */
            u32 rd = (w >> 11) & 0x1F;
            if (rd && t->d.known[rt] && !t->d.known[rs]) {
                t->flag_set[rd]   = t->flag_known[rs] ? t->flag_set[rs] : 0;
                t->flag_clear[rd] = (t->flag_known[rs] ? t->flag_clear[rs] : 0)
                                  | ~t->d.reg[rt];
                t->flag_known[rd] = true;
            } else if (rd) {
                t->flag_known[rd] = false;
            }
        } else if (op == OP_ORI && rt != 0) {
            if (!t->d.known[rs]) {
                t->flag_set[rt]   = (t->flag_known[rs] ? t->flag_set[rs] : 0)
                                  | IMMU(w);
                t->flag_clear[rt] = t->flag_known[rs] ? t->flag_clear[rs] : 0;
                t->flag_known[rt] = true;
            } else {
                t->flag_known[rt] = false;
            }
        } else if (op == 0x0C && rt != 0) {                      /* andi */
            if (!t->d.known[rs]) {
                t->flag_set[rt]   = t->flag_known[rs] ? t->flag_set[rs] : 0;
                t->flag_clear[rt] = (t->flag_known[rs] ? t->flag_clear[rs] : 0)
                                  | ~IMMU(w);
                t->flag_known[rt] = true;
            } else {
                t->flag_known[rt] = false;
            }
        }

        /* a store back to the aiflags word */
        if (op == OP_SW && IMM(w) == 0xDC && rt != 0 && t->flag_known[rt]
            && (t->flag_set[rt] || t->flag_clear[rt])) {
            q2_cre_step *s = push_step(th);
            if (s) {
                s->op         = Q2_CRE_OP_AIFLAG;
                s->flag_set   = t->flag_set[rt];
                s->flag_clear = t->flag_clear[rt];
                s->gated      = depth > 0;
            }
        }

        think_note_stack(t, w);

        /* nextframe */
        if (op == OP_SH && IMM(w) == 0x138 && rt != 0 && t->d.known[rt]) {
            q2_cre_step *s = push_step(th);
            if (s) {
                s->op    = Q2_CRE_OP_NEXTFRAME;
                s->frame = (s32)t->d.reg[rt];
                s->gated = depth > 0;
            }
        }

        /* installing a move */
        if (op == OP_SW && IMM(w) == Q2_ENT_OFS_MOVE && rt != 0
            && t->d.known[rt] && in_image(&t->d, t->d.reg[rt], 16)) {
            q2_cre_step *s = push_step(th);
            if (s) {
                s->op    = Q2_CRE_OP_MOVE;
                s->addr  = t->d.reg[rt];
                s->gated = depth > 0;
            }
        }

        /* an indirect call through an import */
        if (op == 0 && (w & 0x3F) == 0x09) {
            u32 callee = RS(w);

            /* fold the delay slot in first */
            dec_track(&t->d, word_at(&t->d, addr + 4));
            think_note_stack(t, word_at(&t->d, addr + 4));

            if (t->imp_known[callee]) {
                u32 slot = t->imp_slot[callee];
                q2_cre_step *s;

                if (slot == Q2_IMP_RAND) {
                    /* not an action; it feeds the ones that follow */
                } else if ((s = push_step(th)) != NULL) {
                    s->import_ofs = slot;
                    s->gated      = depth > 0;

                    if (slot == Q2_IMP_SOUND) {
                        s->op   = Q2_CRE_OP_SOUND;
                        s->addr = t->load_known[4] ? t->load_src[4]
                                : (t->d.known[4] ? t->d.reg[4] : 0);
                    } else if (slot == Q2_IMP_FIRE_HIT) {
                        s32 v;
                        s->op = Q2_CRE_OP_MELEE;
                        /*
                         * The aim triple is pushed as three stack constants
                         * and then loaded into a1..a3 immediately before the
                         * call, so the stack shadow is where it is readable —
                         * the registers themselves hold the result of a load
                         * and are rightly marked unknown.
                         */
                        if (t->d.known[5]) s->aim[0] = (s32)t->d.reg[5];
                        if (t->d.known[6]) s->aim[1] = (s32)t->d.reg[6];
                        if (t->d.known[7]) s->aim[2] = (s32)t->d.reg[7];
                        if (stk_get(t, 16, &v)) s->damage_base = v;
                        else s->damage_base = t->last_add_known[2]
                                            ? t->last_add[2] : 0;
                        if (stk_get(t, 20, &v)) s->kick = v;
                        s->damage_rand = t->mul_back;
                    } else {
                        s->op = Q2_CRE_OP_CALL;
                    }
                }
            }

            addr += 4;          /* the delay slot is consumed */
            continue;
        }

        /* a direct call inside the module */
        if (op == OP_JAL && inline_depth < 1) {
            u32 target = (addr & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            if (in_image(&t->d, target, 4) && target != entry) {
                dec_track(&t->d, word_at(&t->d, addr + 4));
                think_note_stack(t, word_at(&t->d, addr + 4));
                decode_body(c, t, target, th, inline_depth + 1);
                addr += 4;
                continue;
            }
        }

        dec_track(&t->d, w);
    }
}

u32 q2_creature_decode_thinks(const q2_creature *c, const u8 *image,
                              size_t size, u32 base,
                              q2_cre_think *out, u32 out_count)
{
    think_dec t;
    u8  idx[64];
    u32 n, i, done = 0;

    if (!c || !image || !out)
        return 0;

    memset(out, 0, sizeof(*out) * out_count);
    memset(&t, 0, sizeof(t));
    t.d.img  = image;
    t.d.size = size;
    t.d.base = base;

    n = q2_creature_think_indices(c, idx, sizeof(idx));

    for (i = 0; i < n; i++) {
        u32 k = idx[i];
        if (k >= out_count || k >= Q2_CLASS_METHOD_COUNT)
            continue;
        decode_one_think(c, &t, c->method[k], &out[k]);
        if (out[k].step_count)
            done++;
    }

    return done;
}
