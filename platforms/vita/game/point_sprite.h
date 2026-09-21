#ifndef MELEE_VITA_POINT_SPRITE_H
#define MELEE_VITA_POINT_SPRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MELEE_VITA_POINT_SPRITE_MAX_POINTS (UINT16_MAX / 4u)
#define MELEE_VITA_GPU_POINT_STRIDE 68u
#define MELEE_VITA_NONPOINT_VTX_KEY_SIZE 76u
#define MELEE_VITA_POINT_GPU_HASH_TAG UINT64_C(0x706f696e74535052)

static inline bool melee_vita_point_sprite_counts(
    uint32_t points, uint32_t* vertices, uint32_t* indices)
{
    if (vertices == NULL || indices == NULL ||
        points > MELEE_VITA_POINT_SPRITE_MAX_POINTS)
        return false;
    *vertices = points * 4u;
    *indices = points * 6u;
    return true;
}

static inline uint32_t melee_vita_point_sprite_chunk_count(uint32_t points)
{
    return points / MELEE_VITA_POINT_SPRITE_MAX_POINTS +
           (points % MELEE_VITA_POINT_SPRITE_MAX_POINTS != 0u);
}

static inline bool melee_vita_point_sprite_chunk(
    uint32_t points, uint32_t chunk_index, uint32_t* offset,
    uint32_t* chunk_points)
{
    const uint32_t chunks = melee_vita_point_sprite_chunk_count(points);
    uint32_t start;
    uint32_t remaining;
    if (offset == NULL || chunk_points == NULL || chunk_index >= chunks)
        return false;
    start = chunk_index * MELEE_VITA_POINT_SPRITE_MAX_POINTS;
    remaining = points - start;
    *offset = start;
    *chunk_points = remaining < MELEE_VITA_POINT_SPRITE_MAX_POINTS
        ? remaining : MELEE_VITA_POINT_SPRITE_MAX_POINTS;
    return true;
}

static inline bool melee_vita_point_sprite_quad_indices(
    uint32_t point_index, uint16_t indices[6])
{
    uint32_t base;
    if (indices == NULL ||
        point_index >= MELEE_VITA_POINT_SPRITE_MAX_POINTS)
        return false;
    base = point_index * 4u;
    indices[0] = (uint16_t) base;
    indices[1] = (uint16_t) (base + 1u);
    indices[2] = (uint16_t) (base + 2u);
    indices[3] = (uint16_t) base;
    indices[4] = (uint16_t) (base + 2u);
    indices[5] = (uint16_t) (base + 3u);
    return true;
}

static inline bool melee_vita_point_sprite_texcoord_offset(
    uint32_t corner, float span, float* s, float* t)
{
    if (s == NULL || t == NULL || corner >= 4u)
        return false;
    *s = (corner == 1u || corner == 2u) ? span : 0.0f;
    *t = corner >= 2u ? span : 0.0f;
    return true;
}

static inline bool melee_vita_point_sprite_corner(
    uint32_t corner, float* x, float* y)
{
    if (x == NULL || y == NULL || corner >= 4u)
        return false;
    *x = (corner == 0u || corner == 3u) ? -1.0f : 1.0f;
    *y = corner < 2u ? 1.0f : -1.0f;
    return true;
}

static inline bool melee_vita_point_sprite_clip_half(
    uint32_t point_size_sixths, float scale_x, float scale_y,
    float* half_x, float* half_y)
{
    if (half_x == NULL || half_y == NULL)
        return false;
    *half_x = 0.5f * (float) point_size_sixths / 6.0f * scale_x / 480.0f;
    *half_y = 0.5f * (float) point_size_sixths / 6.0f * scale_y / 272.0f;
    return true;
}

static inline bool melee_vita_point_sprite_clip_offset(
    uint32_t corner, float half_x, float half_y, float clip_w,
    float* clip_x, float* clip_y)
{
    float x;
    float y;
    if (clip_x == NULL || clip_y == NULL ||
        !melee_vita_point_sprite_corner(corner, &x, &y))
        return false;
    *clip_x = x * half_x * clip_w;
    *clip_y = y * half_y * clip_w;
    return true;
}

static inline bool melee_vita_point_gpu_payload(
    uint32_t points, uint32_t stride, uint32_t* expanded_vertices,
    uint32_t* max_chunk_indices, uint64_t* vertex_bytes,
    uint64_t* index_bytes)
{
    const uint32_t chunk_points =
        points < MELEE_VITA_POINT_SPRITE_MAX_POINTS
            ? points : MELEE_VITA_POINT_SPRITE_MAX_POINTS;
    if (stride == 0u || expanded_vertices == NULL ||
        max_chunk_indices == NULL || vertex_bytes == NULL ||
        index_bytes == NULL || points > UINT32_MAX / 4u)
        return false;
    *expanded_vertices = points * 4u;
    *max_chunk_indices = chunk_points * 6u;
    *vertex_bytes = (uint64_t) *expanded_vertices * stride;
    *index_bytes = (uint64_t) *max_chunk_indices * sizeof(uint16_t);
    return true;
}

static inline uint64_t melee_vita_point_gpu_program_hash(
    uint64_t nonpoint_hash, uint8_t tex_offset_mask)
{
    uint64_t h = nonpoint_hash ^ MELEE_VITA_POINT_GPU_HASH_TAG;
    h = (h ^ tex_offset_mask) * UINT64_C(1099511628211);
    return h;
}

static inline uint64_t melee_vita_nonpoint_vertex_program_hash(
    const void* data, size_t size, uint32_t version)
{
    const uint8_t* bytes = (const uint8_t*) data;
    uint64_t h = UINT64_C(0x9e3779b97f4a7c15) ^ version;
    if (bytes == NULL && size != 0u)
        return 0u;
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static inline bool melee_vita_point_gpu_path_supported(
    bool has_matrix_index, bool has_unsupported_texcoord_source,
    bool buffers_ready)
{
    return !has_matrix_index && !has_unsupported_texcoord_source &&
           buffers_ready;
}

#endif
