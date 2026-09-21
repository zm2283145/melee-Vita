#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pc/gx_texture_compat.h"
#include "texture_decoder.h"

#define GX_TF_RGB565 4U
#define RGBA8(r, g, b, a)                                                     \
    ((uint32_t) (r) | (uint32_t) (g) << 8 | (uint32_t) (b) << 16 |            \
     (uint32_t) (a) << 24)

static void test_poke_floats_toon_texture_byte_order(void)
{
    static const uint16_t values[] = {
        0xFFFF, 0xCE79, 0xA534, 0x8410, 0x738E,
        0xFFFF, 0xCE79, 0xA534, 0x8410, 0x738E,
        0xFFFF, 0xCE79, 0xA534, 0x8410, 0x738E, 0xFFFF,
    };
    static const uint32_t expected[] = {
        RGBA8(255, 255, 255, 255),
        RGBA8(206, 207, 206, 255),
        RGBA8(165, 166, 165, 255),
        RGBA8(132, 130, 132, 255),
        RGBA8(115, 113, 115, 255),
    };
    static const uint32_t artifacts[] = {
        RGBA8(123, 56, 115, 255),
        RGBA8(49, 150, 41, 255),
        RGBA8(16, 16, 33, 255),
    };
    uint16_t pixels[16];
    uint8_t normalized_bytes[sizeof(pixels)];
    uint32_t decoded[16];
    bool normalized = false;
    size_t i;
    size_t j;

    memcpy(pixels, values, sizeof(pixels));
    assert(pc_normalize_gx_u16_texture(
        pixels, sizeof(pixels) / sizeof(pixels[0]), &normalized));
    assert(normalized);

    for (i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) {
        const uint8_t* bytes = (const uint8_t*) pixels;
        assert(bytes[i * 2] == (uint8_t) (values[i] >> 8));
        assert(bytes[i * 2 + 1] == (uint8_t) values[i]);
    }

    memcpy(normalized_bytes, pixels, sizeof(pixels));
    assert(!pc_normalize_gx_u16_texture(
        pixels, sizeof(pixels) / sizeof(pixels[0]), &normalized));
    assert(memcmp(normalized_bytes, pixels, sizeof(pixels)) == 0);

    assert(melee_vita_decode_texture_raw(
               (const unsigned char*) pixels, 4, 4, GX_TF_RGB565,
               NULL, 0, 0, decoded,
               sizeof(decoded) / sizeof(decoded[0])) == 0);
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        assert(decoded[i] == expected[i]);
        for (j = 0; j < sizeof(artifacts) / sizeof(artifacts[0]); j++) {
            assert(decoded[i] != artifacts[j]);
        }
    }
}

int main(void)
{
    test_poke_floats_toon_texture_byte_order();
    return 0;
}
