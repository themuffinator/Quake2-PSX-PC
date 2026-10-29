#ifndef Q2PSX_SDKMATH_H
#define Q2PSX_SDKMATH_H

#include "q2psx.h"

/* The retail libgte integer routines: quantized lookup, not exact Euclidean
 * division. Tables are reconstructed mathematically and checked against both
 * supported executables. Native table indices require a positive signed sum;
 * the host defines extreme out-of-domain inputs with an integer fallback. */
u32 q2_sdk_sqrt0(u32 squared);
/* Engine 0x80055CBC: SDK seed followed by two signed Newton corrections. */
u32 q2_sdk_sqrt_refined(u32 squared);
void q2_sdk_vector_normal(const s32 input[3], s32 output[3]);

#endif
