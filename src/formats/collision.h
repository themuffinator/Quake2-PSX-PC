/*
 * collision.h — the PrimaryColl and SecondaryCol chunks, and the engine's own
 *               collision model read out of SLES_015.34.
 *
 * ---------------------------------------------------------------------------
 * What changed, and why it matters
 * ---------------------------------------------------------------------------
 * An earlier pass inferred this format from the data alone and built a
 * plausible collision system on top of it: convex cells, outward planes, clip
 * a segment against the cell you are in. That guess was right in outline and
 * wrong in every detail that decides behaviour — which cells are searched, how
 * a move leaves one cell and enters the next, what stops it, and what the
 * fields nobody could read actually are.
 *
 * All of it is now transcribed from the original's code. The module lives at
 * 0x80043BDC…0x800456B0 and is eight functions:
 *
 *     0x80044098   point in node                     q2_coll_point_in_node
 *     0x80044294   may a point occupy this node      q2_coll_probe
 *     0x80044420   cross a portal into the next node q2_coll_cross
 *     0x800445A4   clip a move against one node      q2_coll_clip
 *     0x80044C44   the swept move through the graph  q2_coll_move
 *     0x80044F54   locate the node holding a point   q2_coll_find_node
 *     0x80045088   locate + report contents          q2_coll_probe_point
 *     0x80043E20   distance to the nearest plane     q2_coll_ray_span
 *
 * The context every one of them takes is built by 0x80051600, and its field
 * layout is what fixes the chunk layout below beyond argument:
 *
 *     ctx+0x00 = chunk            ctx+0x14 = numPlanes
 *     ctx+0x04 = chunk + 4        ctx+0x16 = numNodes
 *     ctx+0x08 = nodes end        ctx+0x0C = planes end
 *     ctx+0x10 = the remap chunk
 *
 * i.e. `nodes = chunk+4`, `planes = nodes + (numNodes+1)*36`,
 * `links = planes + numPlanes*12`. That is the size equation, read off the
 * loader rather than fitted to the data.
 *
 * ---------------------------------------------------------------------------
 * WHICH HULL THE PLAYER MOVES IN — SecondaryCol, not PrimaryColl
 * ---------------------------------------------------------------------------
 * The zone loader builds two contexts, 344 bytes apart:
 *
 *     0x800C8E90   PrimaryColl  + PrimaryRemap   (0x8007B508, call 0x8007B5E8)
 *     0x800C8FE8   SecondaryCol + SecondaryRem   (0x8007B648, call 0x8007B6A8)
 *
 * and the entity mover at 0x80045144 — the one the player reaches through
 * 0x8003A1C8 → 0x80039AA4 → 0x8004583C → 0x800456B0 — loads 0x800C8FE8
 * (0x800451B4). **Movement runs against SecondaryCol.** PrimaryColl is the
 * context 0x800C8E90, referenced from seventeen other places (AI, line of
 * sight, spawning).
 *
 * This also explains the census result that made no sense before: SecondaryCol
 * has FEWER nodes than PrimaryColl in 9 of 115 zones. It is not a refinement of
 * the primary hull, it is a different hull for a different job.
 *
 * ---------------------------------------------------------------------------
 * The record layout
 * ---------------------------------------------------------------------------
 *     u16            num_nodes
 *     u16            num_planes
 *     q2_coll_node   nodes[num_nodes + 1]    last entry is a totals sentinel
 *     q2_coll_plane  planes[num_planes]
 *     q2_coll_link   links[num_links]
 *
 * A node's plane range and link range are both derived from its successor,
 * which is why the sentinel exists:
 *
 *     planes = [node[i].first_plane, node[i+1].first_plane)
 *     links  = [node[i].first_link,  node[i+1].first_link)
 *
 * ---------------------------------------------------------------------------
 * The three fields that were unknown
 * ---------------------------------------------------------------------------
 * **`first_plane` bit 15 is a SOLID flag.** 0x800440BC tests `bltz` on the
 * halfword before masking with 0x7FFF, and returns "not inside" unconditionally;
 * 0x800442B4 does the same and refuses to let a move enter the node. Every
 * reader masks with 0x7FFF, so the plane index is 15 bits.
 *
 * **`extra[]` is the PORTAL LIST** — renamed `links` here. Each 4-byte entry is
 * two halfwords:
 *
 *     bits 15..11 of the first  = which plane OF THIS NODE the portal is in
 *     bits 10..0  of the first  = the neighbouring node index
 *     the second halfword       = the matching plane index IN THE NEIGHBOUR
 *
 * 0x80044494 masks with 0xF800 and compares against `hitPlane << 11`
 * (0x80044E98); 0x800444C0 masks the same halfword with 0x7FF to get the node;
 * 0x800444BC passes the second halfword as the plane the neighbour test must
 * skip — the shared face, which the entering point sits exactly on.
 *
 * The packing bounds a node to 32 planes and a hull to 2048 nodes. Both hold on
 * the disc; `q2psx-inspect coll` checks them.
 *
 * **`d`'s low byte is the node's CONTENTS id.** 0x80044DB8 reads `lbu +32` and
 * records it in the trace's contact list whenever it changes along the path;
 * 0x8004510C reads it into the point-query result. The other three bytes of `d`
 * are zero on the whole disc.
 *
 * **The halfword at +28 is the SortData byte offset.** The world draw reads it
 * with `lh` at 0x80066AFC after indexing the PrimaryColl node array through
 * the viewport's cell at +146. The other halfword at +30 is the SpaceLights
 * partition offset used from SecondaryCol. The shared record therefore carries
 * two hull-specific render indices alongside its collision data.
 *
 * ---------------------------------------------------------------------------
 * The arithmetic is 16-BIT, and that is load-bearing
 * ---------------------------------------------------------------------------
 * Every test takes the point relative to the node's own minimum corner in
 * SIXTEEN-bit wrapping arithmetic:
 *
 *     rel[i] = (s16)((u16)point[i] - (u16)node.bbox_min[i])
 *
 * (0x8004414C…0x80044184, `lhu`/`lhu`/`subu`/`sh`). A port that subtracts in
 * 32 bits agrees only while the difference fits in a signed halfword. It always
 * does for a point inside the node — the AABB test runs first — but not for the
 * far end of a long move, which is exactly where a trace would silently
 * diverge.
 *
 * The plane's own reference point is stored the same way, as an unsigned
 * halfword offset from `bbox_min`. That reading is now CONFIRMED by the code
 * rather than argued from geometry.
 *
 * One asymmetry worth recording because it looks like a bug and is not:
 * 0x80044098 loads the plane point with `lhu` and truncates the difference to
 * 16 bits, while 0x800445A4, 0x80044294 and 0x80043E20 load it with `lh` and
 * subtract in 32 bits. The two agree for every plane on the disc because no
 * stored component reaches 0x8000 (max 29,439), so this is a difference in the
 * original's source, not in its behaviour.
 *
 * ---------------------------------------------------------------------------
 * Confidence
 * ---------------------------------------------------------------------------
 * The normals are 1.3.12 unit vectors: |n| == 4096 in 120,911 planes, 4095 in
 * 18,321 and 4094 in 8, out of 139,240 measured across every file and both
 * chunks. Diagonals appear as 2896 == round(4096/sqrt 2).
 *
 * The old "95.6% / 99.85% confirmed" figures for the plane point are retired.
 * They were measuring how well a guessed reading matched a geometric
 * expectation; the reading is now read out of four separate functions in the
 * original, and `q2psx-inspect coll` checks the structural invariants the
 * engine depends on (link targets in range, portal planes in range, the
 * 32-plane and 2048-node packing limits, reciprocal portals) across the disc.
 *
 * The 148 nodes that are not strictly convex are not a decode fault and do not
 * need defending against: the engine never assumes convexity. It clips against
 * whichever planes a move is heading toward, ignores planes it is already
 * outside of (0x80044710 `bltz`), and allows a 256-unit slack when deciding
 * whether a point may occupy a node (0x800443CC `slti 256`).
 */
#ifndef Q2PSX_COLLISION_H
#define Q2PSX_COLLISION_H

#include "level.h"
#include "q2psx.h"

#define Q2_COLL_NODE_SIZE  36
#define Q2_COLL_PLANE_SIZE 12
#define Q2_COLL_LINK_SIZE   4

/* A 1.3.12 normal of unit length. */
#define Q2_COLL_NORMAL_ONE 4096

/* first_plane: bit 15 marks the node solid, the rest is the plane index. */
#define Q2_COLL_SOLID       0x8000u
#define Q2_COLL_PLANE_MASK  0x7FFFu

/* link packing: (plane_in_this_node << 11) | neighbour_node */
#define Q2_COLL_LINK_NODE_MASK  0x07FFu
#define Q2_COLL_LINK_PLANE_SHIFT 11

/* Consequences of that packing, both checked on the disc. */
#define Q2_COLL_MAX_PLANES_PER_NODE 32
#define Q2_COLL_MAX_NODES         2048

/*
 * How far outside a node's plane a point may sit and still be allowed to occupy
 * it. 0x800443CC, `slti v0, v0, 256`. This is what lets a move cross a portal
 * whose two hulls do not agree to the unit, and what makes the 148 non-convex
 * nodes harmless.
 */
#define Q2_COLL_PROBE_SLACK 256

/* The trace records a contact each time the contents id changes. The original's
 * list runs from ctx+0x2C to ctx+0xAC, 16 bytes per entry. */
#define Q2_COLL_PATH_MAX 8

typedef struct q2_coll_node {
    s32 bbox_min[3];
    s32 bbox_max[3];
    u16 first_plane;  /* RAW: bit 15 is Q2_COLL_SOLID, bits 0..14 the index */
    u16 first_link;
    s16 sort_offset;  /* +28: SortData byte offset when this is PrimaryColl  */
    u16 first_light;  /* +30: SpaceLights first index for SecondaryCol       */
    u8  contents;     /* byte +32: the node's contents id (0..75)            */
    u8  pad[3];       /* bytes +33..35: zero on the whole disc               */
} q2_coll_node;

typedef struct q2_coll_plane {
    u16 a, b, c;      /* point, as an unsigned offset from the node's bbox_min */
    s16 nx, ny, nz;   /* 1.3.12 unit normal, pointing OUT of the cell          */
} q2_coll_plane;

typedef struct q2_coll_link {
    u16 raw;          /* (plane_in_this_node << 11) | neighbour node          */
    u16 back_plane;   /* the same face's plane index in the neighbour         */
} q2_coll_link;

/* One entry of the trace's contact list: where the contents id changed. */
typedef struct q2_coll_contact {
    s32 pos[3];
    s16 id;
} q2_coll_contact;

typedef struct q2_collision {
    const u8 *nodes;        /* borrowed; (node_count+1) records of 36 bytes */
    const u8 *planes;       /* borrowed; plane_count records of 12 bytes    */
    const u8 *links;        /* borrowed; link_count records of 4 bytes      */
    u32       node_count;   /* excludes the sentinel                        */
    u32       plane_count;
    u32       link_count;

    /*
     * Trace scratch. These are the original's own context fields, kept here for
     * the same reason it kept them: the move writes what it hit and the caller
     * reads it afterwards, rather than threading an out-parameter through five
     * functions.
     */
    s32 hit_plane;        /* ctx+0x18: index WITHIN the node, -1 if none     */
    s32 hit_plane_index;  /* ctx+0x20 as an index rather than a pointer      */
    s32 hit_node;         /* ctx+0x24                                        */
    s32 current_node;     /* ctx+0x1A: where the trace ended up              */

    /*
     * One slot more than the original appends into. The engine's list runs
     * ctx+0x2C..ctx+0xAC and its guard stops appending at the eighth entry, but
     * the terminal write at 0x80044ED0 stores through the unadvanced pointer
     * regardless — which on a full list is sixteen bytes past the array, into
     * the rest of a 344-byte context. Harmless there, undefined here, so the
     * slot is made real instead.
     */
    q2_coll_contact path[Q2_COLL_PATH_MAX + 1];
    u32             path_count;    /* appended transitions; <= Q2_COLL_PATH_MAX */

    /*
     * The original writes these two to 0x800B2AF4 / 0x800B2AF0 when a move runs
     * out of nodes. Nothing in the 632 KB image reads either — they are a
     * dead output — but they are the only record of WHY a move stopped, so the
     * port keeps them where a caller can use them.
     */
    s32 blocked_node;     /* the node the move could not quite enter, or -1  */
    s32 blocked_plane;    /* the plane index that kept it out, or -1         */
} q2_collision;

/* Which hull to open. */
typedef enum q2_coll_which {
    Q2_COLL_PRIMARY = 0,    /* 0x800C8E90 — AI, line of sight, spawning */
    Q2_COLL_SECONDARY       /* 0x800C8FE8 — entity movement             */
} q2_coll_which;

/*
 * Parse a collision chunk, exactly as 0x80051600 does. Validates the size
 * equation and the sentinel, so a success return means the layout is confirmed
 * for this specific file rather than assumed.
 */
q2_result q2_collision_parse(q2_collision *out, const q2_zone_file *zone,
                             q2_coll_which which);

bool q2_collision_get_node(const q2_collision *c, u32 index, q2_coll_node *out);
bool q2_collision_get_plane(const q2_collision *c, u32 index, q2_coll_plane *out);
bool q2_collision_get_link(const q2_collision *c, u32 index, q2_coll_link *out);

/* Number of planes / links belonging to node `index`. Both mask off the solid
 * bit before subtracting, which a raw difference must not forget. */
u32 q2_collision_node_plane_count(const q2_collision *c, u32 index);
u32 q2_collision_node_link_count(const q2_collision *c, u32 index);

/* True when bit 15 of the node's first_plane is set: nothing may enter. */
bool q2_collision_node_is_solid(const q2_collision *c, u32 index);

/* ------------------------------------------------------------------------- */
/* The primitives, in the original's own arithmetic                           */
/* ------------------------------------------------------------------------- */

/*
 * A point relative to a node's minimum corner, in 16-bit wrapping arithmetic.
 * Exposed because every test below is defined in terms of it and a caller
 * comparing against the original needs the same intermediate.
 */
void q2_coll_relative(const q2_coll_node *node, const s32 point[3], s16 out[3]);

/*
 * Signed distance from `point` to a plane of `node_index`, scaled by 4096
 * (the normal is 1.3.12). POSITIVE is outside the cell.
 *
 * This is 0x80044294's `(rel - p)·n`. Note the sign: the clip at 0x800445A4
 * computes the negation of it, so read the sign convention here and nowhere
 * else.
 */
s32 q2_coll_plane_distance(const q2_collision *c, u32 node_index,
                           u32 plane_index, const s32 point[3]);

/* Decode a plane's reference point into world coordinates. Whether it lands
 * inside the node's box is deliberately not checked: a plane is infinite. */
bool q2_coll_plane_point(const q2_collision *c, u32 node_index, u32 plane_index,
                         s32 out_point[3]);

/*
 * 0x80044098 — is `point` inside node `index`?
 *
 * AABB first, then every plane. A solid node always answers false.
 */
bool q2_coll_point_in_node(const q2_collision *c, u32 index, const s32 point[3]);

/*
 * 0x80044F54 — which node holds `point`.
 *
 * Tries `hint` first, then the hint's portal neighbours, then — only if
 * `brute` — every node in the hull. Returns -1 when nothing holds it.
 * Passing hint = -1 skips straight to the brute-force sweep.
 */
s32 q2_coll_find_node(const q2_collision *c, const s32 point[3], s32 hint,
                      bool brute);

/*
 * 0x80044294 — may `point` occupy `node`, ignoring plane `skip_plane`?
 *
 * `*out_inside` is set when the point clears every plane outright.
 * `*out_marginal_plane` receives the plane index it is outside of by less than
 * Q2_COLL_PROBE_SLACK, or -1. Returns false — with both outputs cleared — when
 * the node is solid or the point is outside by the slack or more.
 *
 * Pass skip_plane = -1 to test every plane.
 */
bool q2_coll_probe(const q2_collision *c, u32 node, const s32 point[3],
                   s32 skip_plane, bool *out_inside, s32 *out_marginal_plane);

/*
 * 0x800445A4 — clip the move `point + delta` against node `node`.
 *
 * Returns the fraction as an exact rational `*out_num / *out_den`, which is how
 * the original carries it: no divide happens until the delta is scaled, so the
 * result is bit-identical to the console's. `*out_num == *out_den` means the
 * whole move fits.
 *
 * Writes `c->hit_plane`, `c->hit_plane_index` and `c->hit_node`.
 *
 * Planes the move is not heading toward (n·delta <= 0) and planes the point is
 * already outside of (num < 0) are both skipped, which is why a non-convex node
 * cannot trap anything.
 */
void q2_coll_clip(q2_collision *c, u32 node, const s32 point[3],
                  const s16 delta[3], s32 *out_num, s32 *out_den);

/*
 * 0x80044420 — leave `node` through the plane the clip stopped at.
 *
 * Walks the node's links, considers only those whose portal is in plane
 * `hit_plane`, and asks each neighbour whether `point` may occupy it. Returns
 * the first neighbour that says yes outright, or -1. A neighbour that is only
 * marginally outside is reported through `*out_near_node` / `*out_near_plane`
 * instead, which is what the original stores when a move dead-ends.
 */
s32 q2_coll_cross(const q2_collision *c, u32 node, const s32 point[3],
                  s32 hit_plane, s32 *out_near_node, s32 *out_near_plane);

/*
 * 0x80044C44 — the swept move.
 *
 * Walks `start` toward `end` through the portal graph, clipping in each cell it
 * passes through. Returns true when the move completed, false when it was
 * stopped; `out_pos` always receives where it actually ended up and `out_node`
 * the cell it ended in (-1 when it never found one).
 *
 * `node_hint` is the caller's cached cell — entities keep it at +0x4E and feed
 * it back every frame. -1 makes the trace find the starting cell by brute
 * force, which is what a fresh spawn does.
 *
 * On a completed move the end position is `end` EXACTLY, not the accumulated
 * sum of clipped steps (0x80044D50 copies it wholesale). That is deliberate in
 * the original and removes any drift from repeated scaling.
 *
 * Afterwards `c->hit_plane_index` names the plane that stopped it (valid only
 * when this returned false), `c->path` holds the contents transitions along the
 * way, and `c->blocked_node` / `c->blocked_plane` say what it could not enter.
 */
bool q2_coll_move(q2_collision *c, const s32 start[3], const s32 end[3],
                  s32 node_hint, s32 out_pos[3], s32 *out_node);

/*
 * 0x80045088 — locate `point` and report what is there.
 *
 * The original packs its answer into one 16-byte record whose `dir` and `scale`
 * are inputs and whose `node`/`plane` share a halfword under the same
 * (plane << 11) | node packing the links use. Fill `dir` and `scale` before
 * calling; everything else is written. Returns false when no node holds the
 * point, with the outputs cleared.
 */
typedef struct q2_coll_point_query {
    s16 dir[3];    /* IN  (+0x06): the direction to measure along           */
    s32 scale;     /* IN  (+0x0E): per-axis divisor, see q2_coll_ray_span   */
    s32 span;      /* OUT (+0x00): distance * 64 to the nearest plane       */
    s32 node;      /* OUT (+0x04, bits 0..10)                               */
    s32 plane;     /* OUT (+0x04, bits 11..15): index within the node       */
    s32 contents;  /* OUT (+0x0C): the node's contents id                   */
} q2_coll_point_query;

bool q2_coll_probe_point(q2_collision *c, const s32 point[3], s32 hint,
                         q2_coll_point_query *q);

/*
 * 0x80043E20 — how far `point` may travel along `dir` before leaving `node`.
 *
 * `scale` divides each per-axis product before they are summed, exactly as the
 * original does, which keeps the sum inside 32 bits for long directions. The
 * result is `(distance * 64)`; the original returns it pre-shifted by 2 after
 * an internal factor of 16. Returns 0x3FFFFFFC when no plane is in the way.
 *
 * Also writes `c->hit_plane`, which is how 0x80045088 gets its plane index.
 */
s32 q2_coll_ray_span(q2_collision *c, u32 node, const s32 point[3],
                     const s16 dir[3], s32 scale);

/* Squared length of a normal, for validating that it really is unit length. */
s32 q2_coll_normal_len_sq(const q2_coll_plane *p);

/* Bit 15 of a raw first_plane; the index is the rest. */
Q2PSX_INLINE u32 q2_coll_plane_base(u16 raw) { return raw & Q2_COLL_PLANE_MASK; }

#endif /* Q2PSX_COLLISION_H */
