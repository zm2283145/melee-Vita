#ifndef MELEE_PC_GX_TEXTURE_COMPAT_H
#define MELEE_PC_GX_TEXTURE_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool pc_normalize_gx_u16_texture(uint16_t* pixels, size_t count,
                                               bool* normalized)
{
    const uint16_t one = 1;
    size_t i;

    if (pixels == NULL || normalized == NULL || *normalized) {
        return false;
    }
    if (*(const uint8_t*) &one != 0) {
        for (i = 0; i < count; i++) {
            pixels[i] =
                (uint16_t) ((pixels[i] << 8) | (pixels[i] >> 8));
        }
    }
    *normalized = true;
    return true;
}

#endif
