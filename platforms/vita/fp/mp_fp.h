/* GameCube floating-point rounding helpers, force-included ahead of every
 * game source in an exact-maths build (build-game.ps1 -EnableExactMath).
 * Copied from the 3DS Slippi port (melee-3ds slippi-beta-v0.1.0,
 * native-3ds/port/include/mp_fp.h).  The 3DS turned clang's contraction off
 * inside the mp_* helpers with a pragma; on Vita the contraction plugin
 * (tools/gcc_fp_contract) skips every function named mp_* instead.
 *
 * Names follow Melee Unlocked's source port (GPL-3.0-or-later,
 * sourceport/game/include/mu_native.h and math_native.h), whose source
 * annotations tools/slippi_edits/determinism.py carries over. */
#ifndef MP_FP_H
#define MP_FP_H

#ifdef MELEE_VITA_EXACT_MATH
/* One product rounded on its own before the surrounding sum: the plugin only
 * fuses a product that is an unnamed temporary of the same expression, and
 * mu_p_ is a named local. */
#define MU_P(x) ({ __typeof__(x) mu_p_ = (x); mu_p_; })

/* Explicit Gekko single-precision fused operations, as Dolphin rounds them:
 * the float product is exact in double, one rounding in double, then one to
 * single. Contraction is off inside, so nothing is fused twice. */
static inline __attribute__((always_inline)) float mp_fmadds(float a, float c, float b)
{
    return (float) ((double) a * (double) c + (double) b);
}
static inline __attribute__((always_inline)) float mp_fmsubs(float a, float c, float b)
{
    return (float) ((double) a * (double) c - (double) b);
}
/* fnmsubs: -(a*c - b). */
static inline __attribute__((always_inline)) float mp_fnmsubs(float a, float c, float b)
{
    return -(float) ((double) a * (double) c - (double) b);
}
#define mu_fmadds mp_fmadds
#define mu_fmsubs mp_fmsubs
#define mu_fnmsubs mp_fnmsubs
#define __fnmsubs mp_fnmsubs
#define MU_FMADDS(a, b, c) mp_fmadds((float) (a), (float) (b), (float) (c))

/* Functions where the console compiler fused nothing are listed in
 * tools/slippi/no_contract.txt (read by the plugin); the tag is a marker. */
#define MU_NO_CONTRACT

float mp_sqrtf_accurate(float x); /* MSL sqrtf with a fourth Newton step */
double mp_fres(double x);         /* Gekko fres estimate */

#else
/* Without exact maths (no -EnableSlippi or -EnableExactMath) the
 * annotations are plain arithmetic, so the game compiles as it did before
 * the Slippi work. */
#define MU_P(x) (x)
#define mp_fmadds(a, c, b) ((a) * (c) + (b))
#define mp_fmsubs(a, c, b) ((a) * (c) - (b))
#define mp_fnmsubs(a, c, b) (-((a) * (c) - (b)))
#define mu_fmadds mp_fmadds
#define mu_fmsubs mp_fmsubs
#define mu_fnmsubs mp_fnmsubs
#define __fnmsubs mp_fnmsubs
#define MU_FMADDS(a, b, c) mp_fmadds((float) (a), (float) (b), (float) (c))
#define MU_NO_CONTRACT
#endif

#endif
