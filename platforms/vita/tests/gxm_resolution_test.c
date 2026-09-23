#include <assert.h>

#include "gxm_resolution.h"

int main(void)
{
    uint32_t width = 0u;
    uint32_t height = 0u;

    assert(melee_vita_resolution_option_dimensions(
        MELEE_VITA_RESOLUTION_NATIVE, &width, &height));
    assert(width == 960u && height == 544u);
    assert(melee_vita_resolution_option_dimensions(
        MELEE_VITA_RESOLUTION_75, &width, &height));
    assert(width == 720u && height == 408u);
    assert(melee_vita_resolution_option_dimensions(
        MELEE_VITA_RESOLUTION_60, &width, &height));
    assert(width == 576u && height == 328u);
    assert(melee_vita_resolution_option_dimensions(
        MELEE_VITA_RESOLUTION_50, &width, &height));
    assert(width == 480u && height == 272u);
    assert(!melee_vita_resolution_option_dimensions(-1, &width, &height));
    assert(!melee_vita_resolution_option_dimensions(
        MELEE_VITA_RESOLUTION_OPTION_COUNT, &width, &height));
    assert(!melee_vita_resolution_option_dimensions(
        MELEE_VITA_RESOLUTION_75, NULL, &height));
    assert(melee_vita_resolution_dimensions_valid(960u, 544u));
    assert(melee_vita_resolution_dimensions_valid(720u, 408u));
    assert(melee_vita_resolution_dimensions_valid(576u, 328u));
    assert(melee_vita_resolution_dimensions_valid(480u, 272u));
    assert(!melee_vita_resolution_dimensions_valid(719u, 408u));
    assert(!melee_vita_resolution_dimensions_valid(720u, 407u));
    assert(!melee_vita_resolution_dimensions_valid(304u, 240u));
    assert(melee_vita_resolution_use_internal(true, 0u));
    assert(!melee_vita_resolution_use_internal(false, 0u));
    assert(!melee_vita_resolution_use_internal(
        true, MELEE_VITA_NATIVE_REASON_COPY));
    assert(!melee_vita_resolution_use_internal(
        true, MELEE_VITA_NATIVE_REASON_READBACK));
    assert(!melee_vita_resolution_use_internal(
        true, MELEE_VITA_NATIVE_REASON_SHADOW));
    assert(!melee_vita_resolution_use_internal(
        true, MELEE_VITA_NATIVE_REASON_THP));
    assert(melee_vita_resolution_filter_native_reasons(
               MELEE_VITA_NATIVE_REASON_SHADOW, true, false) == 0u);
    assert(melee_vita_resolution_filter_native_reasons(
               MELEE_VITA_NATIVE_REASON_SHADOW, false, false) ==
           MELEE_VITA_NATIVE_REASON_SHADOW);
    assert(melee_vita_resolution_filter_native_reasons(
               MELEE_VITA_NATIVE_REASON_SHADOW |
                   MELEE_VITA_NATIVE_REASON_COPY,
               true, false) == MELEE_VITA_NATIVE_REASON_COPY);
    assert(melee_vita_resolution_filter_native_reasons(
               MELEE_VITA_NATIVE_REASON_SHADOW |
                   MELEE_VITA_NATIVE_REASON_COPY,
               true, true) == 0u);
    assert(melee_vita_resolution_filter_native_reasons(
               MELEE_VITA_NATIVE_REASON_COPY |
                   MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY,
               false, true) ==
           MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY);
    assert(melee_vita_resolution_filter_native_reasons(
               MELEE_VITA_NATIVE_REASON_DEBUG_UI, true, true) ==
           MELEE_VITA_NATIVE_REASON_DEBUG_UI);
    assert(!melee_vita_resolution_is_gameplay_frame(0u));
    assert(melee_vita_resolution_is_gameplay_frame(
        MELEE_VITA_NATIVE_REASON_COPY));
    assert(melee_vita_resolution_is_gameplay_frame(
        MELEE_VITA_NATIVE_REASON_SHADOW));
    assert(!melee_vita_resolution_is_gameplay_frame(
        MELEE_VITA_NATIVE_REASON_THP));
    return 0;
}
