#include "texture_decoder.h"

#include <stddef.h>
#include <string.h>

#define GX_TF_I4 0u
#define GX_TF_I8 1u
#define GX_TF_IA4 2u
#define GX_TF_IA8 3u
#define GX_TF_RGB565 4u
#define GX_TF_RGB5A3 5u
#define GX_TF_RGBA8 6u
#define GX_TF_CMPR 14u
#define GX_TF_C4 8u
#define GX_TF_C8 9u
#define GX_TF_C14X2 10u
#define GX_TL_IA8 0u
#define GX_TL_RGB565 1u
#define GX_TL_RGB5A3 2u

static uint16_t be16(const unsigned char* p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

static uint32_t be32(const unsigned char* p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 |
           (uint32_t) p[2] << 8 | p[3];
}

static int in_archive(const void* pointer, size_t size,
                      const unsigned char* data, uint32_t data_size)
{
    if (pointer == NULL) return 0;
    const unsigned char* bytes = pointer;
    return bytes >= data && size <= data_size &&
           bytes <= data + data_size - size;
}

static uint32_t rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | g << 8 | b << 16 | a << 24;
}

static uint32_t rgb565(uint16_t value)
{
    const uint32_t r5 = value >> 11;
    const uint32_t g6 = value >> 5 & 0x3fu;
    const uint32_t b5 = value & 0x1fu;
    return rgba8((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4),
                 (b5 << 3) | (b5 >> 2), 255u);
}

static uint32_t expand4(uint32_t value)
{
    return value << 4 | value;
}

static uint32_t expand3(uint32_t value)
{
    return value << 5 | value << 2 | value >> 1;
}

static uint32_t rgb5a3(uint16_t value)
{
    if ((value & 0x8000u) != 0u) {
        const uint32_t r = value >> 10 & 0x1fu;
        const uint32_t g = value >> 5 & 0x1fu;
        const uint32_t b = value & 0x1fu;
        return rgba8(r << 3 | r >> 2, g << 3 | g >> 2,
                     b << 3 | b >> 2, 255u);
    }
    return rgba8(expand4(value >> 8 & 0x0fu),
                 expand4(value >> 4 & 0x0fu), expand4(value & 0x0fu),
                 expand3(value >> 12 & 7u));
}

static void decode_8bit(const unsigned char* source, uint32_t width,
                        uint32_t height, uint32_t* output, uint32_t format)
{
    const uint32_t blocks_x = (width + 7u) / 8u;
    const uint32_t blocks_y = (height + 3u) / 4u;
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const unsigned char* block =
                source + (by * blocks_x + bx) * 32u;
            for (uint32_t y = 0; y < 4u; ++y) {
                for (uint32_t x = 0; x < 8u; ++x) {
                    const uint32_t px = bx * 8u + x;
                    const uint32_t py = by * 4u + y;
                    if (px >= width || py >= height) continue;
                    const uint8_t value = block[y * 8u + x];
                    if (format == GX_TF_I8)
                        output[py * width + px] =
                            rgba8(value, value, value, 255u);
                    else
                        output[py * width + px] =
                            rgba8(expand4(value >> 4),
                                  expand4(value >> 4),
                                  expand4(value >> 4),
                                  expand4(value & 0x0fu));
                }
            }
        }
    }
}

static void decode_16bit(const unsigned char* source, uint32_t width,
                         uint32_t height, uint32_t* output, uint32_t format)
{
    const uint32_t blocks_x = (width + 3u) / 4u;
    const uint32_t blocks_y = (height + 3u) / 4u;
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const unsigned char* block =
                source + (by * blocks_x + bx) * 32u;
            for (uint32_t y = 0; y < 4u; ++y) {
                for (uint32_t x = 0; x < 4u; ++x) {
                    const uint32_t px = bx * 4u + x;
                    const uint32_t py = by * 4u + y;
                    if (px >= width || py >= height) continue;
                    const uint16_t value = be16(block + (y * 4u + x) * 2u);
                    if (format == GX_TF_IA8) {
                        const uint32_t intensity = value >> 8;
                        output[py * width + px] =
                            rgba8(intensity, intensity, intensity,
                                  value & 0xffu);
                    } else if (format == GX_TF_RGB565) {
                        output[py * width + px] = rgb565(value);
                    } else {
                        output[py * width + px] = rgb5a3(value);
                    }
                }
            }
        }
    }
}

static void decode_rgba8(const unsigned char* source, uint32_t width,
                         uint32_t height, uint32_t* output)
{
    const uint32_t blocks_x = (width + 3u) / 4u;
    const uint32_t blocks_y = (height + 3u) / 4u;
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const unsigned char* block =
                source + (by * blocks_x + bx) * 64u;
            for (uint32_t y = 0; y < 4u; ++y) {
                for (uint32_t x = 0; x < 4u; ++x) {
                    const uint32_t px = bx * 4u + x;
                    const uint32_t py = by * 4u + y;
                    if (px >= width || py >= height) continue;
                    const uint32_t pixel = y * 4u + x;
                    const uint32_t a = block[pixel * 2u];
                    const uint32_t r = block[pixel * 2u + 1u];
                    const uint32_t g = block[32u + pixel * 2u];
                    const uint32_t b = block[32u + pixel * 2u + 1u];
                    output[py * width + px] = rgba8(r, g, b, a);
                }
            }
        }
    }
}

static int texture_storage_size(uint32_t width, uint32_t height,
                                uint32_t format, uint32_t* size)
{
    uint32_t block_width, block_height, block_bytes;
    if (format == GX_TF_I4 || format == GX_TF_C4 || format == GX_TF_CMPR) {
        block_width = 8u;
        block_height = 8u;
        block_bytes = 32u;
    } else if (format == GX_TF_I8 || format == GX_TF_IA4 || format == GX_TF_C8) {
        block_width = 8u;
        block_height = 4u;
        block_bytes = 32u;
    } else if ((format >= GX_TF_IA8 && format <= GX_TF_RGB5A3) ||
               format == GX_TF_C14X2) {
        block_width = block_height = 4u;
        block_bytes = 32u;
    } else if (format == GX_TF_RGBA8) {
        block_width = block_height = 4u;
        block_bytes = 64u;
    } else {
        return 0;
    }
    const uint64_t bytes = (uint64_t) ((width + block_width - 1u) /
                                       block_width) *
                           ((height + block_height - 1u) / block_height) *
                           block_bytes;
    if (bytes > UINT32_MAX) return 0;
    *size = (uint32_t) bytes;
    return 1;
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t aw, uint32_t bw,
                    uint32_t divisor)
{
    return rgba8((((a & 0xffu) * aw + (b & 0xffu) * bw) / divisor),
                 ((((a >> 8) & 0xffu) * aw +
                    ((b >> 8) & 0xffu) * bw) / divisor),
                 ((((a >> 16) & 0xffu) * aw +
                    ((b >> 16) & 0xffu) * bw) / divisor),
                 255u);
}

static void decode_i4(const unsigned char* source, uint32_t width,
                      uint32_t height, uint32_t* output)
{
    const uint32_t blocks_x = (width + 7u) / 8u;
    const uint32_t blocks_y = (height + 7u) / 8u;
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const unsigned char* block =
                source + (by * blocks_x + bx) * 32u;
            for (uint32_t y = 0; y < 8u; ++y) {
                for (uint32_t x = 0; x < 8u; ++x) {
                    const uint8_t pair = block[y * 4u + x / 2u];
                    const uint32_t i4 =
                        (x & 1u) == 0u ? pair >> 4 : pair & 0x0fu;
                    const uint32_t intensity = i4 * 17u;
                    const uint32_t px = bx * 8u + x;
                    const uint32_t py = by * 8u + y;
                    if (px < width && py < height)
                        output[py * width + px] =
                            rgba8(intensity, intensity, intensity, 255u);
                }
            }
        }
    }
}

static void decode_cmpr_subblock(const unsigned char* source, uint32_t x0,
                                 uint32_t y0, uint32_t width,
                                 uint32_t height, uint32_t* output)
{
    const uint16_t c0_raw = be16(source);
    const uint16_t c1_raw = be16(source + 2u);
    const uint32_t c0 = rgb565(c0_raw);
    const uint32_t c1 = rgb565(c1_raw);
    uint32_t palette[4] = { c0, c1, 0u, 0u };
    if (c0_raw > c1_raw) {
        palette[2] = mix(c0, c1, 2u, 1u, 3u);
        palette[3] = mix(c0, c1, 1u, 2u, 3u);
    } else {
        palette[2] = mix(c0, c1, 1u, 1u, 2u);
        palette[3] = 0u;
    }
    const uint32_t selectors = be32(source + 4u);
    for (uint32_t y = 0; y < 4u; ++y) {
        for (uint32_t x = 0; x < 4u; ++x) {
            const uint32_t shift = 30u - (y * 4u + x) * 2u;
            const uint32_t px = x0 + x;
            const uint32_t py = y0 + y;
            if (px < width && py < height)
                output[py * width + px] =
                    palette[(selectors >> shift) & 3u];
        }
    }
}

static void decode_cmpr(const unsigned char* source, uint32_t width,
                        uint32_t height, uint32_t* output)
{
    const uint32_t blocks_x = (width + 7u) / 8u;
    const uint32_t blocks_y = (height + 7u) / 8u;
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const unsigned char* block =
                source + (by * blocks_x + bx) * 32u;
            decode_cmpr_subblock(block, bx * 8u, by * 8u, width, height,
                                 output);
            decode_cmpr_subblock(block + 8u, bx * 8u + 4u, by * 8u,
                                 width, height, output);
            decode_cmpr_subblock(block + 16u, bx * 8u, by * 8u + 4u,
                                 width, height, output);
            decode_cmpr_subblock(block + 24u, bx * 8u + 4u,
                                 by * 8u + 4u, width, height, output);
        }
    }
}

static uint32_t palette_color(const unsigned char* palette, uint32_t index,
                              uint32_t format, uint32_t entries)
{
    uint16_t value;
    if (palette == NULL || index >= entries) return 0xffff00ffu;
    value = be16(palette + index * 2u);
    if (format == GX_TL_IA8) {
        const uint32_t intensity = value & 0xffu;
        return rgba8(intensity, intensity, intensity, value >> 8);
    }
    if (format == GX_TL_RGB565) return rgb565(value);
    if (format == GX_TL_RGB5A3) return rgb5a3(value);
    return 0xffff00ffu;
}

static void decode_ci(const unsigned char* source, uint32_t width,
                      uint32_t height, uint32_t format,
                      const unsigned char* palette, uint32_t palette_format,
                      uint32_t palette_entries, uint32_t* output)
{
    const uint32_t block_width = format == GX_TF_C4 ? 8u :
                                 format == GX_TF_C8 ? 8u : 4u;
    const uint32_t block_height = format == GX_TF_C4 ? 8u : 4u;
    const uint32_t blocks_x = (width + block_width - 1u) / block_width;
    const uint32_t blocks_y = (height + block_height - 1u) / block_height;
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const unsigned char* block = source + (by * blocks_x + bx) * 32u;
            for (uint32_t y = 0; y < block_height; ++y) {
                for (uint32_t x = 0; x < block_width; ++x) {
                    const uint32_t pixel = y * block_width + x;
                    uint32_t index;
                    if (format == GX_TF_C4) {
                        const uint8_t pair = block[pixel / 2u];
                        index = (pixel & 1u) == 0u ? pair >> 4 : pair & 15u;
                    } else if (format == GX_TF_C8) {
                        index = block[pixel];
                    } else {
                        index = be16(block + pixel * 2u) & 0x3fffu;
                    }
                    const uint32_t px = bx * block_width + x;
                    const uint32_t py = by * block_height + y;
                    if (px < width && py < height)
                        output[py * width + px] = palette_color(
                            palette, index, palette_format, palette_entries);
                }
            }
        }
    }
}

int melee_vita_decode_texture_raw(const unsigned char* source,
                                  uint32_t width, uint32_t height,
                                  uint32_t format,
                                  const unsigned char* palette,
                                  uint32_t palette_format,
                                  uint32_t palette_entries,
                                  uint32_t* rgba,
                                  uint32_t pixel_capacity)
{
    if (source == NULL || rgba == NULL || width == 0 || height == 0 ||
        (uint64_t) width * height > pixel_capacity) return -1;
    if (format == GX_TF_I4) decode_i4(source, width, height, rgba);
    else if (format == GX_TF_I8 || format == GX_TF_IA4)
        decode_8bit(source, width, height, rgba, format);
    else if (format >= GX_TF_IA8 && format <= GX_TF_RGB5A3)
        decode_16bit(source, width, height, rgba, format);
    else if (format == GX_TF_RGBA8) decode_rgba8(source, width, height, rgba);
    else if (format == GX_TF_CMPR) decode_cmpr(source, width, height, rgba);
    else if (format == GX_TF_C4 || format == GX_TF_C8 || format == GX_TF_C14X2)
        decode_ci(source, width, height, format, palette, palette_format,
                  palette_entries, rgba);
    else return -2;
    return 0;
}

int melee_vita_decode_texture(const unsigned char* image_desc,
                              const unsigned char* archive_data,
                              uint32_t archive_size, uint32_t* rgba,
                              uint32_t pixel_capacity, uint32_t* width,
                              uint32_t* height, uint32_t* format)
{
    if (!in_archive(image_desc, 0x18u, archive_data, archive_size)) return -1;
    void* image_pointer = NULL;
    memcpy(&image_pointer, image_desc, sizeof(image_pointer));
    *width = be16(image_desc + 4u);
    *height = be16(image_desc + 6u);
    *format = be32(image_desc + 8u);
    if (*width == 0u || *height == 0u ||
        (uint64_t) *width * *height > pixel_capacity)
        return -2;
    uint32_t storage_size;
    if (!texture_storage_size(*width, *height, *format, &storage_size))
        return -4;
    if (!in_archive(image_pointer, storage_size, archive_data, archive_size))
        return -3;
    return melee_vita_decode_texture_raw(image_pointer, *width, *height,
                                         *format, NULL, 0, 0, rgba,
                                         pixel_capacity);
}
