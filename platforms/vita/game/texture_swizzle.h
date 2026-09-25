/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_TEXTURE_SWIZZLE_H
#define MELEE_VITA_TEXTURE_SWIZZLE_H

#include <stdbool.h>
#include <stdint.h>

/* SceGxm swizzled textures store texels in Morton (Z) order: y occupies the
 * even address bits and x the odd ones (the opposite order shows every
 * texture transposed).  For non-square power-of-two sizes
 * the texture is a row of min(w,h)-sized Morton squares along the longer
 * axis.  Sampling a swizzled texture keeps neighbouring texels in the same
 * cache lines, which linear textures do not; the SGX543 is much faster with
 * them, especially when a texture is minified. */

static inline bool melee_vita_swizzle_supported(uint32_t width, uint32_t height)
{
    return width >= 8u && height >= 8u && width <= 4096u && height <= 4096u &&
           (width & (width - 1u)) == 0u && (height & (height - 1u)) == 0u;
}

static inline uint32_t melee_vita_morton_spread(uint32_t v)
{
    v &= 0x0000ffffu;
    v = (v | (v << 8)) & 0x00ff00ffu;
    v = (v | (v << 4)) & 0x0f0f0f0fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}

static inline uint32_t melee_vita_swizzle_index(
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (width == height) {
        return melee_vita_morton_spread(y) |
               (melee_vita_morton_spread(x) << 1);
    }
    if (width > height) {
        const uint32_t block = x / height;
        const uint32_t bx = x & (height - 1u);
        return block * height * height +
               (melee_vita_morton_spread(y) |
                (melee_vita_morton_spread(bx) << 1));
    }
    {
        const uint32_t block = y / width;
        const uint32_t by = y & (width - 1u);
        return block * width * width +
               (melee_vita_morton_spread(by) |
                (melee_vita_morton_spread(x) << 1));
    }
}

/* Write a linear RGBA8 image into swizzled texture memory. */
static inline void melee_vita_swizzle_rgba8(
    uint32_t* destination, const uint32_t* source,
    uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t* row = source + (size_t) y * width;
        for (uint32_t x = 0; x < width; ++x)
            destination[melee_vita_swizzle_index(x, y, width, height)] = row[x];
    }
}

#endif
