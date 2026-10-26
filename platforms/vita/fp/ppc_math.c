/* Gekko frsqrte for the Vita, copied from the 3DS Slippi port (melee-3ds
 * slippi-beta-v0.1.0, native-3ds/port/engine/ppc_math.c).  Vita changes: the
 * self-test counts mismatches instead of panicking, and calls mp_frsqrte
 * directly instead of going through placeholder.h's __frsqrte. */
#include "ppc_math.h"
#include "ppc_math_vectors.h"

/* Keep integer classification opaque to floating-point optimization. ARM's
 * flush-to-zero mode must not turn a subnormal bit pattern into zero when
 * the compiler recognizes a double-to-integer bitcast and its zero test. */
__attribute__((noinline)) static uint64_t estimate_word(uint64_t bits){
    return mp_frsqrte_bits(bits);
}
double mp_frsqrte(double value){
    union {double d;uint64_t u;} result={.d=value};
    result.u=estimate_word(result.u);
    return result.d;
}

/* Returns the number of vectors checked and adds wrong results to
 * *mismatches. */
unsigned mp_ppc_math_self_test(unsigned* mismatches){
    unsigned i;
    for(i=0;i<sizeof(mp_ppc_vectors)/sizeof(*mp_ppc_vectors);++i){
        union {uint64_t u;double d;} input={.u=mp_ppc_vectors[i][0]},output;
        output.d=mp_frsqrte(input.d);
        if(output.u!=mp_ppc_vectors[i][1])++*mismatches;
    }
    return i;
}
