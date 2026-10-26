#ifndef PLACEHOLDER_H
#define PLACEHOLDER_H

#include <m2c_macros.h> // IWYU pragma: export

/// A label in a jump table
typedef void (*jmp_t)(void);

/// A jump table
typedef jmp_t jtbl_t[];

#if !defined(MWERKS_GEKKO) && defined(MELEE_VITA_EXACT_MATH)
/* Slippi Direct play: the console's own results, from
 * platforms/vita/fp (the 3DS Slippi port does the same).  frsqrte is
 * Dolphin's estimate table, sqrtf_accurate is MSL's sqrtf with a fourth
 * Newton step, and __fabs is the double fabs instruction.  sqrtf itself
 * is mp_sqrtf through -Dsqrtf in build-game.ps1. */
double mp_frsqrte(double);
float mp_sqrtf_accurate(float);
#define __frsqrte(x) mp_frsqrte(x)
#define sqrtf__Ff(x) sqrtf(x)
#define sqrtf_accurate(x) mp_sqrtf_accurate(x)
#define __fabs(f) __builtin_fabs(f)
#elif !defined(MWERKS_GEKKO)
#define __frsqrte(x) (1.0 / sqrt(x))
#define sqrtf__Ff(x) sqrtf(x)
#define sqrtf_accurate(x) sqrtf(x)
#define __fabs(f) fabsf(f)
#endif

#ifndef UNK_T
#ifdef M2CTX
#define UNK_T M2C_UNK
#else
#define UNK_T void*
#endif
#endif

#define U32_TO_F32 4503599627370496.0
#define S32_TO_F32 4503601774854144.0

#define PAD_STACK(bytes)                                                      \
    do {                                                                      \
        UNUSED unsigned char _[(bytes)];                                      \
    } while (0)

/// @warning Creates fake @c .rodata entries!
#define FORCE_PAD_STACK(bytes)                                                \
    do {                                                                      \
        UNUSED unsigned char _[(bytes)] = { 0 };                              \
    } while (0)

#define FORCE_PAD_STACK_4                                                     \
    do {                                                                      \
        UNUSED u32 _0 = 0;                                                    \
    } while (0)

#define FORCE_PAD_STACK_8                                                     \
    do {                                                                      \
        UNUSED u64 _0 = 0;                                                    \
    } while (0)

#define FORCE_PAD_STACK_16                                                    \
    do {                                                                      \
        UNUSED u64 _0 = 0, _1 = 0;                                            \
    } while (0)

#define FORCE_PAD_STACK_32                                                    \
    do {                                                                      \
        UNUSED u64 _0 = 0, _1 = 0, _2 = 0, _3 = 0;                            \
    } while (0)

#endif
