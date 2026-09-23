/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_GXM_RESOLUTION_H
#define MELEE_VITA_GXM_RESOLUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MELEE_VITA_DISPLAY_WIDTH 960u
#define MELEE_VITA_DISPLAY_HEIGHT 544u

typedef enum MeleeVitaResolutionOption {
    MELEE_VITA_RESOLUTION_NATIVE = 0,
    MELEE_VITA_RESOLUTION_75 = 1,
    MELEE_VITA_RESOLUTION_60 = 2,
    MELEE_VITA_RESOLUTION_50 = 3,
    MELEE_VITA_RESOLUTION_OPTION_COUNT = 4,
} MeleeVitaResolutionOption;

enum {
    MELEE_VITA_NATIVE_REASON_COPY = 1u << 0,
    MELEE_VITA_NATIVE_REASON_READBACK = 1u << 1,
    MELEE_VITA_NATIVE_REASON_SHADOW = 1u << 2,
    MELEE_VITA_NATIVE_REASON_THP = 1u << 3,
    MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY = 1u << 4,
    MELEE_VITA_NATIVE_REASON_DEBUG_UI = 1u << 5,
};

static inline bool melee_vita_resolution_option_dimensions(
    int option, uint32_t* width, uint32_t* height)
{
    static const uint16_t widths[MELEE_VITA_RESOLUTION_OPTION_COUNT] = {
        960u, 720u, 576u, 480u
    };
    static const uint16_t heights[MELEE_VITA_RESOLUTION_OPTION_COUNT] = {
        544u, 408u, 328u, 272u
    };
    if (option < 0 || option >= MELEE_VITA_RESOLUTION_OPTION_COUNT ||
        width == NULL || height == NULL)
        return false;
    *width = widths[option];
    *height = heights[option];
    return true;
}

static inline bool melee_vita_resolution_dimensions_valid(
    uint32_t width, uint32_t height)
{
    return width >= 320u && width <= MELEE_VITA_DISPLAY_WIDTH &&
           height >= 240u && height <= MELEE_VITA_DISPLAY_HEIGHT &&
           (width % 16u) == 0u && (height % 8u) == 0u;
}

static inline bool melee_vita_resolution_use_internal(
    bool target_available, uint32_t native_reasons)
{
    return target_available && native_reasons == 0u;
}

static inline uint32_t melee_vita_resolution_filter_native_reasons(
    uint32_t native_reasons, bool scaled_shadow_frames,
    bool scaled_gx_copy_frames)
{
    if (scaled_shadow_frames)
        native_reasons &= ~MELEE_VITA_NATIVE_REASON_SHADOW;
    if (scaled_gx_copy_frames)
        native_reasons &= ~MELEE_VITA_NATIVE_REASON_COPY;
    return native_reasons;
}

static inline bool melee_vita_resolution_is_gameplay_frame(
    uint32_t native_reasons)
{
    return (native_reasons &
            (MELEE_VITA_NATIVE_REASON_COPY |
             MELEE_VITA_NATIVE_REASON_SHADOW)) != 0u;
}

#endif
