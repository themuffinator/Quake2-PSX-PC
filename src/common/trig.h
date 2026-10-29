/*
 * trig.h — fixed-point sine and cosine on the PlayStation's angle convention.
 *
 * Angles are a full circle in 4096 steps, not radians and not degrees. That is
 * what the console's libraries used, what the game's stored angles are in, and
 * what keeps rotation matrices in the 1.3.12 format the GTE expects. Converting
 * to radians and back would introduce rounding the original never had.
 *
 * Results are 1.3.12: +4096 is +1.0, -4096 is -1.0.
 *
 * The table is built once at first use from the host's libm. That is a
 * deliberate trade: the *table* is computed in floating point, but every
 * subsequent use is exact integer arithmetic, so results are reproducible across
 * platforms as long as the table is. A future change could hard-code it if a
 * platform's libm ever disagreed at the ulp level.
 */
#ifndef Q2PSX_TRIG_H
#define Q2PSX_TRIG_H

#include "fixed.h"
#include "q2psx.h"

#define Q2_ANGLE_360 4096
#define Q2_ANGLE_180 2048
#define Q2_ANGLE_90  1024

/*
 * The executable's own table, for tools that want to measure this one against
 * it: 4096 entries of two halfwords, {sine, cosine}, at 0x800A5430 in the PAL
 * build. The flare ring generators index it directly — `lh 0(e)` for the sine
 * and `lh 2(e)` for the cosine of the same entry, stepping by whole entries —
 * which is why the pair layout matters and not just the sine column.
 *
 * `q2psx-inspect lights` compares all 4096 of both columns against q2_sin12 and
 * q2_cos12 and reports 4096 of 4096 on each, so the table this file builds from
 * libm is the console's table and not an approximation of it.
 */
#define Q2_TRIG_TABLE_ADDR    0x800A5430u
#define Q2_TRIG_TABLE_ENTRIES Q2_ANGLE_360
#define Q2_TRIG_TABLE_STRIDE  4

/* Angle may be any value; it is reduced modulo a full circle. */
s32 q2_sin12(s32 angle);
s32 q2_cos12(s32 angle);

/*
 * Native inverse cosine (PAL 0x8009FC44): quantize cos12 down to an even
 * value and clamp to [-4096,4094]. The endpoint results are 2047 and 20;
 * zero maps to 1023. Computed rather than shipping the executable's table,
 * with every entry checked against the disc by test_cremotion --disc.
 */
s32 q2_acos12(s32 cos12);

/* PAL 0x80069C64, including its equal-input/endpoint copies and separate
 * signed product shifts. t is a 12-bit interpolation fraction, 0..4096. */
void q2_quat_slerp(const s16 a[4], const s16 b[4], s32 t, s16 out[4]);

/*
 * ratan2 (0x8008A358): the Psy-Q library's table-driven atan2, an angle on the
 * 4096-step circle in -2048..2048, and 0 for (0, 0) (0x8008A384). The console
 * looks the ratio up in a 1025-entry table at 0x800A9430 after folding the
 * octant. The reconstructed table matches all 1025 PAL entries; ratio
 * quantization and the large-magnitude division follow the native instructions.
 */
s32 q2_ratan2(s32 y, s32 x);

/* Build a 1.3.12 rotation matrix for yaw then pitch, in the order the camera
 * wants: yaw about Y, then pitch about X. `m` is row-major m[row][col]. */
void q2_rotation_yaw_pitch(s16 m[3][3], s32 yaw, s32 pitch);

/*
 * Three Euler angles to a 1.3.12 rotation matrix, in libgte `RotMatrix`'s form:
 * **R = Ry(ry) * Rx(rx) * Rz(rz)**, angles on the 4096-step circle.
 *
 * This is what the zone draw builds from a rotating node's runtime object
 * (0x800678B8 calls RotMatrix at 0x80089E38 on the object's +0x0C triple), and
 * what the VIEW WEAPON builds its clip rotation with.
 *
 * THE ORDER HERE WAS WRONG, and the reason it survived is worth keeping. This
 * said `Rz * Ry * Rx` and justified not checking: "the composition order is
 * unobservable on this disc — the rotation integrator at 0x8002F1A8 stores to
 * obj[0x0C + 2*axis] and nothing ever clears the other two slots, so exactly
 * one angle is non-zero and the three orders agree." That is true, for a
 * ROTATING BRUSH. It stopped being true when the view weapon became a caller:
 * a clip's rotation carries all three angles at once — the blaster's idle is
 * (2078, 2110, 1985) — and the two orders agree only when the roll is zero.
 * An argument that a difference cannot be seen is only as good as the list of
 * things looking, and that list grew.
 *
 * 0x80089E38 is now transcribed element by element rather than inferred from
 * two of its nine; trig.c names the store address of each.
 */
void q2_rotation_euler(s16 m[3][3], s32 rx, s32 ry, s32 rz);

/*
 * The view basis, with the strafe roll.
 *
 * `RotMatrix` (0x80089E38) composes Rz * Ry * Rx, and the camera at 0x8004F40C
 * hands it (pitch, yaw, roll) — so the roll is the OUTERMOST rotation, applied
 * in screen space after the view direction is decided. That is what makes it a
 * lean rather than a change of heading.
 *
 * Identical to q2_rotation_yaw_pitch when `roll` is zero, which is what keeps it
 * safe to use on every camera rather than only the player's.
 */
void q2_rotation_view(s16 m[3][3], s32 yaw, s32 pitch, s32 roll);

/*
 * The camera, with the console's ANAMORPHIC term in it — 0x80055DE4.
 *
 * The world draw loads its matrix from view+160 (0x800313DC), and 0x80037F44
 * builds that by scaling a basis ROW BY ROW: row 0 by `vw / 320` and row 1 by
 * `vh / 240`, the divides at 0x80055DFC's 0x66666667 and 0x80055E8C's
 * 0x88888889. `vw` is the immediate 320 at 0x800779BC and `vh` the immediate
 * 160 at 0x80077948, so the console's camera is `diag(1, 2/3, 1) * basis` and
 * its effective projection is (H, 2H/3).
 *
 * The port had `diag(1, 1, 1)` and H = 160, so it rendered (160, 160): the
 * vertical right and **the horizontal one and a half times too wide**. Measured
 * against a native-resolution capture of the BASE0 spawn, with the 2D briefing
 * panel proving both frames share a framebuffer origin and width:
 *
 *     sky wedge above the panel   console 136 px   before 90 px   after 135 px
 *     weapon emitter, x           console 325..337  before 301..307  after 322..333
 *     weapon emitter, y           console 157..166  before 157..166  after 157..167
 *
 * The vertical was already exact and stays exact; the horizontal closes from
 * 27 pixels of error to 3.
 *
 * WHICH HALF CARRIES IT IS AMBIGUOUS and the ambiguity is worth stating. The
 * console reaches (240, 160) as H = 240 with row 1 at two thirds; this reaches
 * the same place as H = 160 with row 0 at three halves. The pictures are
 * identical. This form is used because it leaves every viewport's VERTICAL
 * untouched — the split-screen layouts carry their own projection distances and
 * none of them has been measured, so scaling their verticals on the strength of
 * one full-screen capture would be the riskier half of the same guess.
 * 0x800781F0, which would settle it by naming the distance, is reached only as
 * a function pointer (0x80079CB4) and can be called by a relocated module.
 */
void q2_rotation_view_anamorphic(s16 m[3][3], s32 yaw, s32 pitch, s32 roll);

/*
 * Rotation matrix for a quaternion, both in 1.3.12. `q` is x, y, z, w, which is
 * the order the model animation keys decode to. Row-major, and in the form the
 * GTE's rotation registers want.
 */
void q2_quat_to_matrix(s16 m[3][3], const s16 q[4]);

#endif /* Q2PSX_TRIG_H */
