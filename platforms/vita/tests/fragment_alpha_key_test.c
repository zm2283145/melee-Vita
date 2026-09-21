#include <assert.h>

#include "fragment_alpha_key.h"

int main(void)
{
    u8 animated_a[2] = { 6u, 200u };
    u8 animated_b[2] = { 76u, 191u };
    u8 unused[2] = { 31u, 63u };

    melee_vita_normalize_alpha_shader_refs(animated_a);
    melee_vita_normalize_alpha_shader_refs(animated_b);
    assert(animated_a[0] == 0u && animated_a[1] == 0u);
    assert(animated_b[0] == 0u && animated_b[1] == 0u);

    melee_vita_normalize_alpha_shader_refs(unused);
    assert(unused[0] == 0u && unused[1] == 0u);
    return 0;
}
