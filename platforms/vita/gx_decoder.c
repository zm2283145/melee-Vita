#include "gx_decoder.h"

#include <stddef.h>
#include <string.h>

#define GX_VA_POS 9u
#define GX_VA_NRM 10u
#define GX_VA_CLR0 11u
#define GX_VA_CLR1 12u
#define GX_VA_TEX0 13u
#define GX_VA_NBT 25u
#define GX_VA_NULL 0xffu
#define GX_NONE 0u
#define GX_DIRECT 1u
#define GX_INDEX8 2u
#define GX_INDEX16 3u

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
    if (pointer == NULL) return size == 0;
    const unsigned char* bytes = pointer;
    return bytes >= data && size <= data_size &&
           bytes <= data + data_size - size;
}

static uint32_t index_count(uint32_t attr, uint32_t component_count)
{
    return (attr == GX_VA_NRM || attr == GX_VA_NBT) &&
                   component_count == 2u ? 3u : 1u;
}

static uint32_t direct_size(uint32_t attr, uint32_t component_count,
                            uint32_t component_type)
{
    if (attr <= 8u) return 1u;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        if (component_type == 0u || component_type == 3u) return 2u;
        if (component_type == 1u || component_type == 4u) return 3u;
        return 4u;
    }
    uint32_t components;
    if (attr == GX_VA_POS) components = component_count == 0u ? 2u : 3u;
    else if (attr == GX_VA_NRM || attr == GX_VA_NBT)
        components = component_count == 0u ? 3u : 9u;
    else components = component_count == 0u ? 1u : 2u;
    const uint32_t element = component_type <= 1u ? 1u :
                             component_type <= 3u ? 2u : 4u;
    return components * element;
}

int melee_vita_gx_decode(const unsigned char* pobj,
                         const unsigned char* archive_data,
                         uint32_t archive_size,
                         struct melee_vita_gx_stats* stats)
{
    void* descriptors_ptr = NULL;
    void* display_ptr = NULL;
    memcpy(&descriptors_ptr, pobj + 8u, sizeof(descriptors_ptr));
    memcpy(&display_ptr, pobj + 0x10u, sizeof(display_ptr));
    const uint32_t display_size = (uint32_t) be16(pobj + 0x0eu) << 5;
    if (!in_archive(display_ptr, display_size, archive_data, archive_size) ||
        !in_archive(descriptors_ptr, 0x18u, archive_data, archive_size))
        return -1;

    const unsigned char* descriptors = descriptors_ptr;
    uint32_t descriptor_count = 0;
    uint32_t vertex_size = 0;
    for (; descriptor_count < 26u; ++descriptor_count) {
        const unsigned char* desc = descriptors + descriptor_count * 0x18u;
        if (!in_archive(desc, 0x18u, archive_data, archive_size)) return -2;
        const uint32_t attr = be32(desc);
        if (attr == GX_VA_NULL) break;
        const uint32_t type = be32(desc + 4u);
        const uint32_t component_count = be32(desc + 8u);
        const uint32_t component_type = be32(desc + 0x0cu);
        if (type > GX_INDEX16) return -3;
        if (type == GX_DIRECT) {
            vertex_size += direct_size(attr, component_count, component_type);
            ++stats->direct_attributes;
        } else if (type == GX_INDEX8 || type == GX_INDEX16) {
            vertex_size += (type == GX_INDEX8 ? 1u : 2u) *
                           index_count(attr, component_count);
            ++stats->indexed_attributes;
            ++stats->vertex_arrays;
        }
    }
    if (descriptor_count == 26u || vertex_size == 0u) return -4;

    const unsigned char* display = display_ptr;
    uint32_t cursor = 0;
    while (cursor < display_size) {
        const uint8_t command = display[cursor];
        if (command == 0u) break;
        const uint8_t primitive = command & 0xf8u;
        if (primitive < 0x80u || primitive > 0xb8u ||
            cursor + 3u > display_size) return -5;
        const uint32_t count = be16(display + cursor + 1u);
        cursor += 3u;
        if (count > (display_size - cursor) / vertex_size) return -6;
        ++stats->batches;
        stats->vertices += count;
        if (primitive == 0x80u) stats->triangles += count / 4u * 2u;
        else if (primitive == 0x90u) stats->triangles += count / 3u;
        else if (primitive == 0x98u || primitive == 0xa0u)
            stats->triangles += count >= 3u ? count - 2u : 0u;

        for (uint32_t vertex = 0; vertex < count; ++vertex) {
            for (uint32_t i = 0; i < descriptor_count; ++i) {
                const unsigned char* desc = descriptors + i * 0x18u;
                const uint32_t attr = be32(desc);
                const uint32_t type = be32(desc + 4u);
                const uint32_t component_count = be32(desc + 8u);
                const uint32_t component_type = be32(desc + 0x0cu);
                if (type == GX_NONE) continue;
                if (type == GX_DIRECT) {
                    cursor += direct_size(attr, component_count, component_type);
                    continue;
                }
                void* array = NULL;
                memcpy(&array, desc + 0x14u, sizeof(array));
                const uint32_t stride = be16(desc + 0x12u);
                if (array == NULL || stride == 0u) return -7;
                const uint32_t indices = index_count(attr, component_count);
                for (uint32_t j = 0; j < indices; ++j) {
                    const uint32_t index = type == GX_INDEX8
                        ? display[cursor++] : be16(display + cursor);
                    if (type == GX_INDEX16) cursor += 2u;
                    if (index > stats->max_index) stats->max_index = index;
                    const uint64_t needed = (uint64_t) (index + 1u) * stride;
                    if (needed > UINT32_MAX ||
                        !in_archive(array, (uint32_t) needed, archive_data,
                                    archive_size)) return -8;
                }
            }
        }
    }
    return 0;
}

static float position_component(const unsigned char* p, uint32_t type,
                                uint32_t fraction)
{
    float value;
    if (type == 0u) value = p[0];
    else if (type == 1u) value = (int8_t) p[0];
    else if (type == 2u) value = be16(p);
    else if (type == 3u) value = (int16_t) be16(p);
    else {
        const uint32_t bits = be32(p);
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return value / (float) (UINT32_C(1) << fraction);
}

int melee_vita_gx_extract_positions(const unsigned char* pobj,
                                    const unsigned char* archive_data,
                                    uint32_t archive_size, float* positions,
                                    float* texture_coordinates,
                                    uint8_t* position_matrix_indices,
                                    uint32_t capacity, uint32_t* count)
{
    void* descriptors_ptr = NULL;
    void* display_ptr = NULL;
    memcpy(&descriptors_ptr, pobj + 8u, sizeof(descriptors_ptr));
    memcpy(&display_ptr, pobj + 0x10u, sizeof(display_ptr));
    const uint32_t display_size = (uint32_t) be16(pobj + 0x0eu) << 5;
    if (!in_archive(display_ptr, display_size, archive_data, archive_size))
        return -1;
    const unsigned char* descriptors = descriptors_ptr;
    uint32_t vertex_size = 0, position_offset = 0, position_type = 0;
    uint32_t position_components = 0, position_component_type = 0;
    uint32_t position_fraction = 0, position_stride = 0;
    uint32_t matrix_offset = 0, matrix_type = GX_NONE;
    uint32_t texture_offset = 0, texture_type = GX_NONE;
    uint32_t texture_components = 0, texture_component_type = 0;
    uint32_t texture_fraction = 0, texture_stride = 0;
    void* position_array = NULL;
    void* texture_array = NULL;
    int found_position = 0;
    for (uint32_t i = 0; i < 26u; ++i) {
        const unsigned char* d = descriptors + i * 0x18u;
        if (!in_archive(d, 0x18u, archive_data, archive_size)) return -2;
        const uint32_t attr = be32(d);
        if (attr == GX_VA_NULL) break;
        const uint32_t type = be32(d + 4u);
        const uint32_t comp = be32(d + 8u);
        const uint32_t ctype = be32(d + 0x0cu);
        uint32_t size = 0;
        if (type == GX_DIRECT) size = direct_size(attr, comp, ctype);
        else if (type == GX_INDEX8 || type == GX_INDEX16)
            size = (type == GX_INDEX8 ? 1u : 2u) * index_count(attr, comp);
        if (attr == GX_VA_POS) {
            found_position = 1;
            position_offset = vertex_size;
            position_type = type;
            position_components = comp == 0u ? 2u : 3u;
            position_component_type = ctype;
            position_fraction = d[0x10u];
            position_stride = be16(d + 0x12u);
            memcpy(&position_array, d + 0x14u, sizeof(position_array));
        }
        if (attr == 0u) {
            matrix_offset = vertex_size;
            matrix_type = type;
        }
        if (attr == GX_VA_TEX0) {
            texture_offset = vertex_size;
            texture_type = type;
            texture_components = comp == 0u ? 1u : 2u;
            texture_component_type = ctype;
            texture_fraction = d[0x10u];
            texture_stride = be16(d + 0x12u);
            memcpy(&texture_array, d + 0x14u, sizeof(texture_array));
        }
        vertex_size += size;
    }
    if (!found_position || position_type == GX_NONE || vertex_size == 0u)
        return -3;
    const unsigned char* dl = display_ptr;
    uint32_t cursor = 0;
    *count = 0;
    while (cursor + 3u <= display_size) {
        const uint32_t primitive = dl[cursor] & 0xf8u;
        if (dl[cursor] == 0u) break;
        if (primitive < 0x80u || primitive > 0xa0u) return -4;
        const uint32_t n = be16(dl + cursor + 1u);
        cursor += 3u;
        if (n > 512u || cursor + n * vertex_size > display_size) return -5;
        float batch[512][3];
        float batch_texture[512][2];
        uint8_t batch_matrix[512];
        for (uint32_t v = 0; v < n; ++v) {
            const unsigned char* vertex_data =
                dl + cursor + v * vertex_size;
            const unsigned char* encoded = dl + cursor + v * vertex_size +
                                           position_offset;
            const unsigned char* source = encoded;
            if (position_type == GX_INDEX8 || position_type == GX_INDEX16) {
                const uint32_t index = position_type == GX_INDEX8
                    ? encoded[0] : be16(encoded);
                if (position_array == NULL || position_stride == 0u ||
                    !in_archive(position_array,
                                (index + 1u) * position_stride,
                                archive_data, archive_size)) return -6;
                source = (const unsigned char*) position_array +
                         index * position_stride;
            }
            const uint32_t element = position_component_type <= 1u ? 1u :
                                     position_component_type <= 3u ? 2u : 4u;
            for (uint32_t c = 0; c < 3u; ++c) {
                batch[v][c] = c < position_components
                    ? position_component(source + c * element,
                                         position_component_type,
                                         position_fraction) : 0.0f;
            }
            batch_matrix[v] = matrix_type == GX_DIRECT
                ? vertex_data[matrix_offset] : 0u;
            batch_texture[v][0] = batch_texture[v][1] = 0.0f;
            if (texture_type != GX_NONE) {
                const unsigned char* encoded_texture =
                    vertex_data + texture_offset;
                const unsigned char* texture_source = encoded_texture;
                if (texture_type == GX_INDEX8 ||
                    texture_type == GX_INDEX16) {
                    const uint32_t index = texture_type == GX_INDEX8
                        ? encoded_texture[0] : be16(encoded_texture);
                    if (texture_array == NULL || texture_stride == 0u ||
                        !in_archive(texture_array,
                                    (index + 1u) * texture_stride,
                                    archive_data, archive_size)) return -9;
                    texture_source =
                        (const unsigned char*) texture_array +
                        index * texture_stride;
                }
                const uint32_t texture_element =
                    texture_component_type <= 1u ? 1u :
                    texture_component_type <= 3u ? 2u : 4u;
                for (uint32_t c = 0; c < 2u; ++c) {
                    batch_texture[v][c] = c < texture_components
                        ? position_component(texture_source +
                                                 c * texture_element,
                                             texture_component_type,
                                             texture_fraction)
                        : 0.0f;
                }
            }
        }
        cursor += n * vertex_size;
#define EMIT_VERTEX(V) do { \
            if (*count >= capacity) return -7; \
            memcpy(positions + (*count) * 3u, batch[(V)], 3u * sizeof(float)); \
            if (texture_coordinates != NULL) \
                memcpy(texture_coordinates + (*count) * 2u, \
                       batch_texture[(V)], 2u * sizeof(float)); \
            if (position_matrix_indices != NULL) \
                position_matrix_indices[*count] = batch_matrix[(V)]; \
            ++*count; \
        } while (0)
        if (primitive == 0x80u) {
            for (uint32_t v = 0; v + 3u < n; v += 4u) {
                EMIT_VERTEX(v); EMIT_VERTEX(v + 1u); EMIT_VERTEX(v + 2u);
                EMIT_VERTEX(v); EMIT_VERTEX(v + 2u); EMIT_VERTEX(v + 3u);
            }
        } else if (primitive == 0x90u) {
            for (uint32_t v = 0; v + 2u < n; v += 3u) {
                EMIT_VERTEX(v); EMIT_VERTEX(v + 1u); EMIT_VERTEX(v + 2u);
            }
        } else if (primitive == 0x98u) {
            for (uint32_t v = 0; v + 2u < n; ++v) {
                if ((v & 1u) == 0u) {
                    EMIT_VERTEX(v); EMIT_VERTEX(v + 1u); EMIT_VERTEX(v + 2u);
                } else {
                    EMIT_VERTEX(v + 1u); EMIT_VERTEX(v); EMIT_VERTEX(v + 2u);
                }
            }
        } else if (primitive == 0xa0u) {
            for (uint32_t v = 1; v + 1u < n; ++v) {
                EMIT_VERTEX(0); EMIT_VERTEX(v); EMIT_VERTEX(v + 1u);
            }
        }
#undef EMIT_VERTEX
    }
    return *count != 0 ? 0 : -8;
}
