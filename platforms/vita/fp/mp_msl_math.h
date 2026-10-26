/* Force-included ahead of the MSL maths sources (src/MSL) in an exact-maths
 * build.  Copied from the 3DS Slippi port (melee-3ds slippi-beta-v0.1.0,
 * native-3ds/port/include/mp_msl_math.h).  MSL's own math.h clashes with
 * newlib's, which the rest of the game uses; this provides the few things
 * the MSL .c files take from it.
 *
 * Vita change: MSL_HI/MSL_LO name the high and low words of a double.  On
 * the GameCube (and the 3DS's big-endian engine) the high word comes first
 * in memory, on the little-endian Vita it comes second. */
#ifndef MP_MSL_MATH_H
#define MP_MSL_MATH_H
#define MSL_MATH_H
#include <Runtime/platform.h>
#include <math.h>
#define MSL_HI(x) *(1 + (int*) &x)
#define MSL_LO(x) *(int*) &x
float fabsf__Ff(float);
float sin__Ff(float x);
float cos__Ff(float x);
#endif
