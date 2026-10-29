/*
 * test_model.c — block D's move table, and the rule that ties it to block C.
 *
 * The one thing worth pinning here is a unit. Block D counts TWO units per
 * animation frame, so a move spanning `start..end` is `(end - start) / 2 + 1`
 * frames. That is not a guess: on every model on the disc carrying both a move
 * table and a clip chain — 34 of them, across 25 zones — move `i`'s frame count
 * by this rule equals clip `i`'s length exactly, in list order.
 *
 * The values below are transcribed from BASE1 model 15, whose 31 clips read
 * 108 15 21 54 51 18 159 72 30 90 117 105 135 30 42 30 69 3 ... and whose moves
 * are Death1 0..214, Pain1 216..244, Pain2 246..286 and so on.
 *
 * A disc is not available to the test suite, so this pins the arithmetic rather
 * than the correspondence. If someone later "simplifies" the divisor, or drops
 * the inclusive +1, these fail.
 *
 * Two more sections have since joined it: block A's face draw order, and the
 * LEVEL-LOAD FACE REWRITE that makes the explosion additive — the one thing on
 * this disc whose drawn face flags differ from its stored ones.
 */
#include <stdio.h>
#include <string.h>

#include "model.h"
#include "surface.h"

static int g_failures;
static int g_checks;

static void check_eq(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n", what, (long long)got,
               (long long)want);
        g_failures++;
    }
}

static u32 frames_of(u16 start, u16 end)
{
    q2_model_move mv;
    memset(&mv, 0, sizeof(mv));
    mv.start = start;
    mv.end   = end;
    return q2_model_move_frames(&mv);
}

/* Real records from BASE1 model 15, against that model's real clip lengths. */
static void test_span_matches_clip_length(void)
{
    check_eq(frames_of(0, 214),      108, "Death1  0..214  -> clip 0  (108)");
    check_eq(frames_of(216, 244),     15, "Pain1   216..244 -> clip 1  (15)");
    check_eq(frames_of(246, 286),     21, "Pain2   246..286 -> clip 2  (21)");
    check_eq(frames_of(288, 394),     54, "Pain3   288..394 -> clip 3  (54)");
    check_eq(frames_of(396, 496),     51, "Pain4   396..496 -> clip 4  (51)");
    check_eq(frames_of(498, 532),     18, "Attack4 498..532 -> clip 5  (18)");
    check_eq(frames_of(534, 850),    159, "Death4  534..850 -> clip 6  (159)");
    check_eq(frames_of(1950, 2008),   30, "Walk   1950..2008 -> clip 13 (30)");
    check_eq(frames_of(2292, 2296),    3, "Fire 1 Ready      -> clip 17 (3)");
}

/* A single-frame move is start == end, not a zero-length one. */
static void test_degenerate_spans(void)
{
    check_eq(frames_of(100, 100), 1, "start == end is ONE frame, not zero");
    check_eq(frames_of(100,  98), 0, "end before start is rejected");
    check_eq(q2_model_move_frames(NULL), 0, "NULL is rejected");
}

/*
 * The two-unit gap between consecutive moves is exactly one frame, which is
 * what makes the moves tile the clip chain end to end with nothing left over.
 */
static void test_moves_tile_without_gaps(void)
{
    /* Death1 0..214, Pain1 216..244, Pain2 246..286: 108 + 15 + 21 = 144. */
    u32 total = frames_of(0, 214) + frames_of(216, 244) + frames_of(246, 286);
    check_eq(total, 144, "three consecutive moves total their three clips");
    check_eq(216 - 214, Q2_MODEL_BLOCKD_PER_FRAME, "the gap is one frame");
}


/*
 * The position formula, against the Arachner's real block-D table.
 *
 * Its `Walk` record is 360..418 in block-D units, so `start` is 360 and the
 * animation begins at position 360 * 5 = 1800. Its AI walk is frames 16..24, so
 * frame 16 sits at 1800 and each AI frame after adds 30.
 *
 * The x5 and the 30 are the two units meeting: block D counts 2 per animation
 * frame, the position counts 10, and an AI frame is three animation frames.
 * Getting either wrong is a silent mis-pose rather than a failure, which is why
 * this pins the arithmetic directly rather than through a lookup.
 */
static void test_position_formula(void)
{
    /* start * 5 + 30 * (f - first), computed the long way. */
    struct { s32 start, first, f, want; } k[] = {
        { 360, 16, 16, 1800 },          /* the move's own first frame */
        { 360, 16, 17, 1830 },          /* one AI frame on */
        { 360, 16, 24, 2040 },          /* its last, eight frames in */
        {   0,  0,  0,    0 },          /* a move at the timeline's start */
        {   0,  0, 10,  300 },          /* ten AI frames = 30 model frames */
    };
    u32 i;

    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        s32 got = k[i].start * 5
                + (k[i].f - k[i].first) * Q2_MODEL_POS_PER_MOVE_FRAME;
        check_eq(got, k[i].want, "position = start*5 + 30*(f - first)");
    }

    /* The two units, stated so a change to either is caught here. */
    check_eq(Q2_MODEL_POS_PER_MOVE_FRAME, 30, "30 position units per AI frame");
    check_eq(Q2_MODEL_BLOCKD_PER_FRAME,    2, "block D counts 2 per frame");
    check_eq(Q2_MODEL_TICKS_PER_FRAME,    10, "the position counts 10");
}

/* ------------------------------------------------------------------------- */
/* Walking off the end of the timeline                                        */
/* ------------------------------------------------------------------------- */
/*
 * `q2_model_anim_at` is the ENGINE'S loop and nothing more: `position <
 * clip->frames`, subtract, advance, with NO end-of-chain test anywhere
 * (0x8006B924's loop is four instructions and 0x80070188, its advance, is
 * `*cursor += delta`). It relies on the position always landing inside.
 *
 * It does not always land inside, and the reason is not arithmetic. An AI
 * move's length and its animation's extent are different quantities authored
 * independently (#63): the Enforcer's `Duck` is a 30-frame AI move whose
 * animation begins at model frame 1287 of a 1302-frame timeline. Five AI frames
 * of animation, twenty-five frames of nothing after it, and at `into = 5` the
 * position is frame 1302 — one past the last.
 *
 * `q2_model_anim_at_held` holds the last frame there. That is this port's
 * divergence and not the console's, so the two must stay distinguishable —
 * anything that quietly turned the first into the second would make the
 * divergence invisible. Before the hold, 678 of BASE2's 1000 poses fell back to
 * matching a clip by LENGTH, which is something the disc never does.
 *
 * The chain here is synthetic because these tests carry no disc: three clips of
 * 4, 6 and 5 frames, laid out exactly as `anim_read` reads them — u16 frames,
 * u16 flags, u32 delta to the next, then `frames` four-byte keys.
 */
#define TL_CLIPS 3

static u8 g_timeline[512];

static void build_timeline(q2_model *m)
{
    static const u16 k_frames[TL_CLIPS] = { 4, 6, 5 };
    /* The chain starts at 16, not 0: `anim_span` treats ofs_block_c == 0 as
     * "this model has no block C" and refuses. */
    u32 at = 16;
    int i;

    memset(g_timeline, 0, sizeof(g_timeline));
    memset(m, 0, sizeof(*m));

    for (i = 0; i < TL_CLIPS; i++) {
        u32 size = 8u + (u32)k_frames[i] * 4u;
        u32 next = (i + 1 < TL_CLIPS) ? size : 0;

        g_timeline[at + 0] = (u8)(k_frames[i] & 0xFF);
        g_timeline[at + 1] = (u8)(k_frames[i] >> 8);
        g_timeline[at + 4] = (u8)(next & 0xFF);
        g_timeline[at + 5] = (u8)((next >> 8) & 0xFF);
        at += size;
    }

    m->base              = g_timeline;
    m->size              = sizeof(g_timeline);
    m->hdr.ofs_block_c   = 16;
    m->hdr.ofs_block_d   = at;   /* block C ends where block D begins */
}

static void test_anim_at_holds_the_last_frame(void)
{
    q2_model m;
    q2_model_anim a;
    u32 within = 0;
    bool held = true;
    const u32 total = 4 + 6 + 5;

    build_timeline(&m);

    /* Inside: both walks agree and neither reports a hold. */
    {
        q2_model_anim plain;
        u32 pw = 0;

        check_eq(q2_model_anim_at(&m, 7, &plain, &pw), 1,
                 "frame 7 resolves on the engine's walk");
        check_eq(q2_model_anim_at_held(&m, 7, &a, &within, &held), 1,
                 "...and on the holding one");
        check_eq(held, 0, "frame 7 is not a hold");
        check_eq(within, pw, "both give the same offset into the clip");
        check_eq(within, 3, "which is 7 - 4, inside the six-frame clip");
    }

    /* The timeline's own last frame is inside it, not a hold. */
    check_eq(q2_model_anim_at_held(&m, total - 1, &a, &within, &held), 1,
             "the last frame of the timeline resolves");
    check_eq(held, 0, "and is the real last frame, not a hold");
    check_eq(within, 4, "at offset 4 of the five-frame clip");

    /* One past: the engine's walk fails, the holding walk holds. */
    check_eq(q2_model_anim_at(&m, total, &a, &within), 0,
             "one past the end fails the engine's own walk");
    check_eq(q2_model_anim_at_held(&m, total, &a, &within, &held), 1,
             "and is held by the port's");
    check_eq(held, 1, "which says so, so the divergence is visible");
    check_eq(a.frames, 5, "on the last clip");
    check_eq(within, 4, "at its last frame, not its first");

    /* Far past: the same place. A hold does not drift. */
    check_eq(q2_model_anim_at_held(&m, total + 5000, &a, &within, &held), 1,
             "far past the end still resolves");
    check_eq(held, 1, "and is held");
    check_eq(within, 4, "at the same frame — a hold does not drift");
}

static void test_clip_transition(void)
{
    q2_model m = {0};
    q2_model_anim old, next;
    q2_model_pose pose = {0};
    memset(g_timeline, 0, sizeof(g_timeline));
    m.base = g_timeline; m.size = sizeof(g_timeline);
    m.hdr.ofs_block_c = 16; m.hdr.ofs_block_d = 128; m.hdr.num_parts = 1;
    /* Two variable-rate clips, each with keys at t=0 and t=30. */
    g_timeline[16] = g_timeline[80] = 4;
    g_timeline[18] = g_timeline[82] = 1;
    g_timeline[20] = 64;
    g_timeline[24] = g_timeline[88] = 4;
    g_timeline[26] = g_timeline[90] = 8;
    g_timeline[28] = g_timeline[92] = 3;
    g_timeline[40] = 50;  /* packed x=100 */
    g_timeline[96] = 100; /* incoming x=200 */
    check_eq(q2_model_anim_at(&m, 0, &old, NULL), 1, "outgoing clip resolves");
    check_eq(q2_model_anim_at(&m, 4, &next, NULL), 1, "incoming clip resolves");
    check_eq(q2_model_pose_at(&m, &old, 15, &pose), Q2_OK, "ordinary key interpolation");
    check_eq(pose.t[0], 50, "ordinary midpoint");
    check_eq(q2_model_pose_at(&m, &old, 35, &pose), Q2_OK, "outgoing last key holds");
    check_eq(pose.t[0], 100, "outgoing endpoint");
    check_eq(q2_model_pose_transition(&m, &old, 35, &next, &pose), Q2_OK, "transition midpoint decodes");
    check_eq(pose.t[0], 150, "outgoing final key blends to incoming first key");
    check_eq(q2_model_pose_transition(&m, &old, 40, &next, &pose), Q2_OK, "transition endpoint decodes");
    check_eq(pose.t[0], 200, "incoming endpoint");
    check_eq(q2_model_pose_transition(&m, &old, 41, &next, &pose), Q2_ERR_BAD_FORMAT, "invalid transition span fails safely");
    g_timeline[28] = 0x30; /* native zero-position fast path ignores the zero nibble */
    check_eq(q2_model_pose_at(&m, &old, 0, &pose), Q2_OK, "zero-duration first key decodes at zero");
    check_eq(pose.t[0], 0, "first key is not skipped");
    next.flags = 0; pose.t[0] = 77;
    check_eq(q2_model_pose_transition(&m, &old, 35, &next, &pose), Q2_OK, "mixed-format transition returns");
    check_eq(pose.t[0], 77, "native mixed-format branch leaves scratch unchanged");
    /* The native decoder reads the successor even when it starts block D.
     * Keeping ka here would silently hold zero instead of reaching x=100. */
    g_timeline[28] = 3;
    m.hdr.ofs_block_d = 40;
    check_eq(q2_model_pose_at(&m, &old, 30, &pose), Q2_OK, "successor may cross block C");
    check_eq(pose.t[0], 100, "the native cross-block successor is decoded");
    m.size = 40;
    check_eq(q2_model_pose_at(&m, &old, 30, &pose), Q2_ERR_BAD_FORMAT, "successor cannot escape the model allocation");
}

static void test_subframe_cursor(void)
{
    q2_model_cursor c;
    q2_model m;
    q2_model_anim a;
    u32 within = 0;
    bool held = true;

    memset(&c, 0, sizeof(c));
    check_eq(q2_model_cursor_phase(&c, 300, 10), 300,
             "first AI base is installed at phase zero");
    check_eq(q2_model_cursor_phase(&c, 300, 10), 310,
             "first render phase is one third of an AI interval");
    check_eq(q2_model_cursor_phase(&c, 300, 10), 320,
             "second render phase retains the cursor");
    check_eq(q2_model_cursor_phase(&c, 330, 10), 330,
             "the next AI base resets phase instead of being chased");
    check_eq(q2_model_cursor_phase(&c, 330, 50), 359,
             "a long render step clamps inside the 0..29 retail phase");
    check_eq(q2_model_cursor_phase(&c, 30, 10), 30,
             "an animation loop also installs its new base directly");

    build_timeline(&m);
    check_eq(q2_model_anim_at_position_held(&m, 47, &a, &within, &held), 1,
             "a position with a remainder resolves");
    check_eq(held, 0, "the in-range position is not held");
    check_eq(within, 7, "the seven-tenths remainder reaches pose interpolation");

    check_eq(q2_model_anim_at_position_held(&m, 151, &a, &within, &held), 1,
             "a position beyond the synthetic timeline is held");
    check_eq(held, 1, "the held position reports the divergence");
    check_eq(within, 40, "a held final key discards the unusable remainder");
}

/* ------------------------------------------------------------------------- */
/* Block A: the face draw order                                               */
/* ------------------------------------------------------------------------- */
/*
 * The two rules a reader will get wrong, pinned on a hand-built block.
 *
 *   - the bytes of each 32-bit word are consumed MOST SIGNIFICANT FIRST, which
 *     on this little-endian machine means byte 3 of the four comes first;
 *   - a ZERO byte is an escape, not a step of nothing: it makes the NEXT delta
 *     land on top of +256.
 *
 * Both come from 0x800B25E0; see the block-A note in model.h. What makes them
 * checkable without a disc is that a correct decode of retail data always lands
 * on a PERMUTATION of the model's faces, so the test builds a stream whose
 * answer is known and asserts the sequence exactly.
 */
#define BLOCK_A 64        /* where the fixture parks the block */
static u8 g_blocka[512];

static void build_block_a(q2_model *m, u32 faces, u16 start,
                          const s8 *deltas, u32 n)
{
    u32 i;

    memset(g_blocka, 0, sizeof(g_blocka));
    memset(m, 0, sizeof(*m));

    /* One record is { u16 start; u16 offset; u32 pad }; entry 0's stream sits
     * straight after the 64-byte table, which is where a real one starts. */
    /* Block A never sits at offset 0 on real data — the header occupies the
     * first 64 bytes — and a zero offset is how the parser spells "absent", so
     * the fixture puts it somewhere a real one could be. */
    g_blocka[BLOCK_A + 0] = (u8)(start & 0xFF);
    g_blocka[BLOCK_A + 1] = (u8)(start >> 8);
    g_blocka[BLOCK_A + 2] = 64;
    g_blocka[BLOCK_A + 3] = 0;

    /* Written most-significant-byte-first within each word, which is the order
     * the reader takes them in. */
    for (i = 0; i < n; i++) {
        u32 word = (i >> 2) << 2;
        u32 lane = 3u - (i & 3u);
        g_blocka[BLOCK_A + 64 + word + lane] = (u8)deltas[i];
    }

    m->base             = g_blocka;
    m->size             = sizeof(g_blocka);
    m->hdr.num_faces    = (u16)faces;
    m->hdr.ofs_block_a  = BLOCK_A;
}

static void test_draw_order_walks_the_stream(void)
{
    q2_model m;
    q2_model_draw_order ord;
    u16 out[16];
    /* from 3: +2 -> 5, -4 -> 1, +6 -> 7, wrap +3 -> 0, -1 -> 7 */
    static const s8 d[5] = { 2, -4, 6, 3, -1 };
    u32 n;

    build_block_a(&m, 8, 3, d, 5);

    check_eq(q2_model_get_draw_order(&m, 0, &ord), 1, "entry 0 reads");
    check_eq(ord.start, 3, "its start face");
    check_eq(ord.offset, 64, "and its stream offset");

    n = q2_model_draw_sequence(&m, 0, out, 16);
    check_eq(n, 6, "six faces walked: the start plus five deltas");
    check_eq(out[0], 3, "starts on the start face");
    check_eq(out[1], 5, "+2");
    check_eq(out[2], 1, "-4");
    check_eq(out[3], 7, "+6");
    check_eq(out[4], 2, "+3 wraps 10 back to 2");
    check_eq(out[5], 1, "-1");
}

/*
 * The escape only means anything on a model with more faces than a signed byte
 * can step over, so this one is 300 faces: 0 then 5 is +261, not +5 and not a
 * step of nothing. The rest of the stream is filled with +1 so the walk still
 * terminates on a full-length sequence rather than running off the fixture.
 */
static void test_zero_byte_is_an_escape(void)
{
    q2_model m;
    u16 out[300];
    s8  d[299];
    u32 n, i;

    d[0] = 0;
    d[1] = 5;
    d[2] = -6;
    for (i = 3; i < 299; i++)
        d[i] = 1;

    build_block_a(&m, 300, 0, d, 299);

    n = q2_model_draw_sequence(&m, 0, out, 300);
    check_eq(n, 300, "a full-length sequence, one entry per face");
    check_eq(out[0], 0, "the start");
    check_eq(out[1], 261, "0 then 5 is +256+5, not +5 and not a no-op");
    check_eq(out[2], 255, "and the step after it is an ordinary one");
    check_eq(out[3], 256, "and the one after that");
}

/*
 * CalcSortList (0x8006A020): the octant of the camera-to-model vector in the
 * model's own axes picks block A's entry, unless block B carries a node for
 * that octant whose frame range holds the animation position. The fixture
 * gives every block-A entry its own index as its start face, so the start
 * says which entry was taken, and parks a block-B node on octant 5.
 */
#define BLOCK_B 256

static void test_draw_order_select(void)
{
    q2_model m;
    q2_model_draw_order ord;
    u16 out[16];
    static const s8 d[7] = { 1, 1, 1, 1, 1, 1, 1 };
    static const s32 o5[3] = { 7, -2, 1 }, o2[3] = { -7, 2, -1 };
    static const s32 none[3] = { 0, 0, 0 };
    u32 e, node = BLOCK_B + 16, n;

    build_block_a(&m, 8, 0, d, 7);
    for (e = 1; e < 8; e++) {
        g_blocka[BLOCK_A + 8 * e + 0] = (u8)e;
        g_blocka[BLOCK_A + 8 * e + 2] = 64;
    }

    check_eq(q2_model_draw_octant(o5), 5, "x > 0 and z > 0 is octant 5");
    check_eq(q2_model_draw_octant(o2), 2, "y > 0 alone is octant 2");
    check_eq(q2_model_draw_octant(none), 0,
             "a zero component counts as not greater than zero");

    check_eq(q2_model_draw_order_select(&m, o2, 0, &ord), 1, "selects");
    check_eq(ord.start, 2, "an empty block B leaves the octant's own entry");

    /* Block B: octant 5's chain head, then one node — count 1, start face 6
     * in the upper nine bits — holding frames 2..4, its stream after it. */
    m.hdr.ofs_block_b = BLOCK_B;
    g_blocka[BLOCK_B + 2 * 5] = (u8)(node - BLOCK_B);
    g_blocka[node + 0] = (u8)((1u | (6u << 7)) & 0xFF);
    g_blocka[node + 1] = (u8)((1u | (6u << 7)) >> 8);
    g_blocka[node + 4] = 2;
    g_blocka[node + 6] = 4;
    g_blocka[node + 8 + 3] = 1;     /* first delta, most significant first */

    check_eq(q2_model_draw_order_select(&m, o5, 0, &ord), 1, "selects");
    check_eq(ord.start, 5, "frame 0 is outside 2..4: block A's entry 5");

    check_eq(q2_model_draw_order_select(&m, o5, 30, &ord), 1, "selects");
    check_eq(ord.start, 6, "position 30 is inside 20..40: block B's node");
    check_eq(ord.offset, node + 8 - BLOCK_A,
             "and its stream is the one after its pairs, from block A");
    n = q2_model_draw_sequence_from(&m, &ord, out, 16);
    check_eq(out[0], 6, "the walk starts on the node's face");
    check_eq(n >= 2 && out[1] == 7, 1, "and steps by the node's own stream");

    check_eq(q2_model_draw_order_select(&m, o5, 41, &ord), 1, "selects");
    check_eq(ord.start, 5, "41 is past 4 * 10: block A again");
    check_eq(q2_model_draw_order_select(&m, o2, 30, &ord), 1, "selects");
    check_eq(ord.start, 2, "and another octant never sees octant 5's node");
}

static void test_draw_order_rejects_a_bad_block(void)
{
    q2_model m;
    u16 out[16];
    static const s8 d[3] = { 1, 1, 1 };

    build_block_a(&m, 8, 99, d, 3);      /* start past the last face */
    check_eq(q2_model_draw_sequence(&m, 0, out, 16), 0,
             "a start outside the model is refused, not clamped");

    build_block_a(&m, 8, 0, d, 3);
    check_eq(q2_model_draw_sequence(&m, 8, out, 16), 0,
             "and so is an entry past the eighth");
}

/* ------------------------------------------------------------------------- */
/* The level-load face rewrite                                                */
/* ------------------------------------------------------------------------- */
/*
 * `PreInitExplosion` (0x8005AAA4), reached once a level from the class table's
 * +0x14 column by `RemapModels` (0x8007E6E4), runs
 * `BodgeModelDataTransparent` (0x8005B938) over every face of the `Explosion`
 * model. Four instructions:
 *
 *     lbu v0, 12(a0) ; andi v0, v0, 0x1F ; ori v0, v0, 0x40 ; sb v0, 12(a0)
 *
 * so the texture page survives and the blend selector becomes 2 — which is
 * codeTable[2] = ABE and blendTable[2] = 32, i.e. B+F, additive.
 *
 * The AND BEFORE THE OR is the load-bearing part and the reason this is pinned
 * rather than left to the one-liner: a reader who writes `flags | 0x40` gets the
 * same answer on all 36 of BASE1's faces (they are all 0x05) and a different one
 * the moment a face arrives with any other selector already set.
 */
#define FACE_STRIDE 16
static u8 g_faces[4 * FACE_STRIDE];

static void build_faces(q2_model *m, const u8 *flags, const u8 *texture, u32 n)
{
    u32 i;

    memset(g_faces, 0, sizeof(g_faces));
    memset(m, 0, sizeof(*m));

    for (i = 0; i < n; i++) {
        g_faces[i * FACE_STRIDE + 12] = flags[i];
        g_faces[i * FACE_STRIDE + 13] = texture[i];
    }

    m->base           = g_faces;
    m->size           = sizeof(g_faces);
    m->hdr.num_faces  = (u16)n;
    m->hdr.ofs_faces  = 0;
}

static void test_bodge_is_keyed_on_the_class_name(void)
{
    check_eq(q2_model_class_bodges_transparent("Explosion"), 1,
             "0x800A3CF8's name is the one that carries 0x8005AAA4");
    check_eq(q2_model_class_bodges_transparent("EXPLOSION"), 1,
             "GetModelHdr's compare ignores case, so this does too");
    /* The other three -1 records in the same run of the table, whose PreInit is
     * PreInitDebris (0x80064F70) and touches no face data. */
    check_eq(q2_model_class_bodges_transparent("Debris1"), 0,
             "Debris1 registers a model, it does not rewrite one");
    /* 0x800ACE00, which 0 of the disc's 164 CastLists carry and which has no
     * class record at all — a spawn name, not a class name. */
    check_eq(q2_model_class_bodges_transparent("Hexplosion"), 0,
             "Hexplosion is not in the class table");
    check_eq(q2_model_class_bodges_transparent("Explosions"), 0,
             "and the match is the whole name, not a prefix");
    check_eq(q2_model_class_bodges_transparent(NULL), 0, "NULL is rejected");
    check_eq(q2_model_class_bodges_transparent(""), 0, "and so is empty");
}

static void test_faces_are_untouched_without_the_bodge(void)
{
    /* BASE1 model 14 `Explosion`: 36 faces, every one of them flags 0x05 and
     * texture 59 on the disc. */
    static const u8 flags[2]   = { 0x05, 0x05 };
    static const u8 texture[2] = { 59, 59 };
    q2_model m;
    q2_model_face f;

    build_faces(&m, flags, texture, 2);

    check_eq(q2_model_get_face(&m, 0, &f), 1, "face 0 reads");
    check_eq(f.flags, 0x05, "the file's own byte, on an unbodged model");
    check_eq(q2_model_face_page(&f), 5, "page 5");
    check_eq(q2_model_face_blend(&f), 0, "selector 0 — opaque, as on the disc");
    check_eq(q2_surf_selector_semi(q2_model_face_blend(&f)), 0,
             "and the rasteriser is told not to blend it");
}

static void test_bodge_forces_the_additive_selector(void)
{
    static const u8 flags[4]   = { 0x05, 0x1F, 0xE5, 0x00 };
    static const u8 texture[4] = { 59, 180, 7, 0 };
    q2_model m;
    q2_model_face f;

    build_faces(&m, flags, texture, 4);
    m.bodge_transparent = true;

    check_eq(q2_model_get_face(&m, 0, &f), 1, "face 0 reads");
    check_eq(f.flags, 0x45, "0x05 -> 0x45: page 5 kept, selector 2 forced");
    check_eq(q2_model_face_page(&f), 5, "the texture page survives");
    check_eq(q2_model_face_blend(&f), 2, "selector 2");
    check_eq(q2_surf_selector_semi(q2_model_face_blend(&f)), 1,
             "codeTable[2] sets ABE, so the face is drawn blended");
    check_eq(q2_surf_selector_blend(q2_model_face_blend(&f)), PSX_BLEND_ADD,
             "blendTable[2] is 32, which is ABR 1: B+F, additive");
    check_eq(f.texture, 59, "and the CLUT byte at +13 is not touched at all");

    /* The highest page a model face can name. */
    check_eq(q2_model_get_face(&m, 1, &f), 1, "face 1 reads");
    check_eq(f.flags, 0x5F, "0x1F -> 0x5F: page 31 kept");
    check_eq(f.texture, 180, "the highest `texture` on the disc, untouched");

    /* The andi before the ori: a selector that is already set is REPLACED, not
     * merged. 0xE5 is selector 7 on page 5; `flags | 0x40` would leave 0xE5. */
    check_eq(q2_model_get_face(&m, 2, &f), 1, "face 2 reads");
    check_eq(f.flags, 0x45, "0xE5 -> 0x45, not 0xE5: the AND runs first");
    check_eq(q2_model_face_blend(&f), 2, "so the selector is 2 and not 7");

    check_eq(q2_model_get_face(&m, 3, &f), 1, "face 3 reads");
    check_eq(f.flags, 0x40, "0x00 -> 0x40: page 0 is still page 0");
}

static void test_pose_bounds(void)
{
    u8 data[64]={0}; q2_model m={0}; q2_model_pose pose[2]={0}; s32 low=999;
    m.base=data; m.size=sizeof(data); m.hdr.ofs_parts=16; m.hdr.ofs_verts=32;
    m.hdr.num_parts=2; m.hdr.num_verts=2;
    data[19]=1; data[27]=1;
    /* Vertex stride is twelve: signed 16-bit x,y,z and normal. */
    data[34]=20; data[46]=40;
    pose[0].q[3]=pose[1].q[3]=4096; pose[0].t[1]=100; pose[1].t[1]=-10;
    check_eq(q2_model_pose_low_y(&m,pose,2,&low),1,"valid parts produce pose bounds");
    check_eq(low,120,"lowest posed vertex includes part translation");
    check_eq(q2_model_pose_low_y(&m,NULL,0,&low),1,"unposed bounds decode");
    check_eq(low,40,"unposed scan includes the second part");
    m.hdr.num_parts=0; low=999;
    check_eq(q2_model_pose_low_y(&m,pose,2,&low),0,"empty model has no bounds");
    check_eq(low,999,"failed bounds leave output untouched");
}

int main(void)
{
    puts("block D: the move table");
    puts("=======================");

    test_span_matches_clip_length();
    test_degenerate_spans();
    test_moves_tile_without_gaps();
    test_position_formula();
    test_anim_at_holds_the_last_frame();
    test_subframe_cursor();
    test_clip_transition();
    test_pose_bounds();

    puts("");
    puts("block A: the face draw order");
    puts("============================");

    test_draw_order_walks_the_stream();
    test_zero_byte_is_an_escape();
    test_draw_order_rejects_a_bad_block();
    test_draw_order_select();

    puts("");
    puts("the level-load face rewrite");
    puts("===========================");

    test_bodge_is_keyed_on_the_class_name();
    test_faces_are_untouched_without_the_bodge();
    test_bodge_forces_the_additive_selector();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
