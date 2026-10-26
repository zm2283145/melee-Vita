/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Boot-time check of the exact-maths code on the Vita, written to
 * ux0:data/melee/slippi/slippi.log.  Same idea as fp_check.c in the 3DS
 * Slippi port: log the FPSCR the game thread runs with, check the console
 * maths against vectors from an exact Python model (tools/slippi), and time
 * each routine so we know what it costs per frame. */
#include "../slippi/slippi_plat.h"

#include <psp2/kernel/processmgr.h>

#include <stdint.h>

unsigned mp_fp_exact_self_test(unsigned* mismatches);
unsigned mp_ppc_math_self_test(unsigned* mismatches);
float mp_sqrtf(float x);
double mp_fma(double a, double b, double c);
float sinf(float x);

static unsigned read_fpscr(void)
{
    unsigned value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

static void write_fpscr(unsigned value)
{
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value));
}

/* The Vita starts the game thread with flush-to-zero and default NaN on
 * (FPSCR 0x03000000), like the 3DS.  The GameCube keeps subnormals, so with
 * them flushed a value below 2^-126 can come out different from Dolphin.
 * The 3DS can't turn this off: its VFP11 needs support code for subnormals
 * that the 3DS OS doesn't have.  The Vita's Cortex-A9 does them in
 * hardware: on a real Vita with FPSCR 0 all 178 vectors pass (the 6
 * subnormal ones too), whole matches run, and the underflow flag shows
 * subnormals really happen in gameplay.  So exact builds run the game
 * thread with the console's rules: FPSCR 0, round to nearest, nothing
 * flushed. */
#define GAME_THREAD_FPSCR 0x00000000

/* Microseconds for 1000 calls of each routine.  volatile keeps GCC from
 * hoisting the calls out of the loop. */
static void log_costs(void)
{
    volatile float float_sink = 0.0f;
    volatile double double_sink = 0.0;
    volatile float x = 1.2345f;
    volatile double dx = 1.2345;
    uint64_t start;
    uint64_t msl_sqrtf_us;
    uint64_t vfp_sqrtf_us;
    uint64_t msl_sinf_us;
    uint64_t fma_us;
    int i;

    start = sceKernelGetProcessTimeWide();
    for (i = 0; i < 1000; i++) float_sink = mp_sqrtf(x + (float) i);
    msl_sqrtf_us = sceKernelGetProcessTimeWide() - start;

    start = sceKernelGetProcessTimeWide();
    for (i = 0; i < 1000; i++) float_sink = __builtin_sqrtf(x + (float) i);
    vfp_sqrtf_us = sceKernelGetProcessTimeWide() - start;

    start = sceKernelGetProcessTimeWide();
    for (i = 0; i < 1000; i++) float_sink = sinf(x + (float) i);
    msl_sinf_us = sceKernelGetProcessTimeWide() - start;

    start = sceKernelGetProcessTimeWide();
    for (i = 0; i < 1000; i++) double_sink = mp_fma(dx, dx + i, -3.0);
    fma_us = sceKernelGetProcessTimeWide() - start;

    (void) float_sink;
    (void) double_sink;
    sp_log("fp: us per 1000 calls: MSL sqrtf %u, VFP sqrtf %u, MSL sinf %u, mp_fma %u",
           (unsigned) msl_sqrtf_us, (unsigned) vfp_sqrtf_us, (unsigned) msl_sinf_us,
           (unsigned) fma_us);
}

/* How long the game's updates take with exact maths.  gmscene.c calls this
 * after each update (one HSD_PerfSetStartTime to HSD_PerfSetCPUTime body).
 * Lockstep online needs 60 updates a second, so anything over 16.7 ms means
 * the PC opponent would wait for us. */
static unsigned s_update_count;
static unsigned s_update_total_us;
static unsigned s_update_worst_us;
static unsigned s_update_slow;

void melee_vita_fp_note_update(unsigned microseconds)
{
    s_update_count++;
    s_update_total_us += microseconds;
    if (microseconds > s_update_worst_us) s_update_worst_us = microseconds;
    if (microseconds > 16683) s_update_slow++;

    /* Every 600 updates is about 10 s of game. */
    if (s_update_count == 600) {
        /* FPSCR too, to see that nothing changed the mode set at boot. */
        sp_log("fp: 600 updates: average %u us, worst %u us, %u slower than 16.7 ms, FPSCR 0x%08x",
               s_update_total_us / s_update_count, s_update_worst_us, s_update_slow,
               read_fpscr());
        s_update_count = 0;
        s_update_total_us = 0;
        s_update_worst_us = 0;
        s_update_slow = 0;
    }
}

void melee_vita_fp_check(void)
{
    unsigned fpscr;
    unsigned mismatches = 0;
    unsigned exact_vectors;
    unsigned estimate_vectors;

    /* main() runs this on the thread that then runs the game, so the mode
     * stays set for every frame.  The 600-update log line shows FPSCR to
     * prove nothing changes it later. */
    sp_log("fp: game thread FPSCR at boot 0x%08x, switching to IEEE mode (0x%08x)", read_fpscr(),
           (unsigned) GAME_THREAD_FPSCR);
    write_fpscr(GAME_THREAD_FPSCR);
    fpscr = read_fpscr();

    /* FZ (bit 24) flushes subnormals to zero, DN (bit 25) replaces NaN
     * payloads, RMode (bits 22-23) should be 0, round to nearest.  The
     * GameCube runs with all three off. */
    sp_log("fp: game thread FPSCR 0x%08x (flush-to-zero %u, default NaN %u, rounding mode %u)", fpscr,
           (fpscr >> 24) & 1, (fpscr >> 25) & 1, (fpscr >> 22) & 3);

    exact_vectors = mp_fp_exact_self_test(&mismatches);
    estimate_vectors = mp_ppc_math_self_test(&mismatches);
    sp_log("fp: fma/sqrtf vectors checked %u, frsqrte vectors checked %u, mismatches %u (%s)",
           exact_vectors, estimate_vectors, mismatches, mismatches == 0 ? "ok" : "WRONG");

    log_costs();
}
