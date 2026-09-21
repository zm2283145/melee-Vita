#include <assert.h>
#include <stdint.h>

#include "point_sprite.h"

static uint64_t legacy_nonpoint_vertex_hash(
    const void* data, size_t size, uint32_t version)
{
    const uint8_t* bytes = data;
    uint64_t hash = UINT64_C(0x9e3779b97f4a7c15) ^ version;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int main(void)
{
    uint32_t vertices;
    uint32_t indices;
    uint32_t offset;
    uint32_t points;
    uint32_t expanded;
    uint32_t max_indices;
    uint64_t vertex_bytes;
    uint64_t index_bytes;
    const uint8_t key_bytes[] = { 0u, 1u, 2u, 3u };
    const uint8_t zero_key[MELEE_VITA_NONPOINT_VTX_KEY_SIZE] = { 0 };
    uint16_t quad[6];
    float s;
    float t;
    float x;
    float y;

    assert(melee_vita_point_sprite_counts(0u, &vertices, &indices));
    assert(vertices == 0u && indices == 0u);

    assert(melee_vita_point_sprite_counts(1u, &vertices, &indices));
    assert(vertices == 4u && indices == 6u);

    assert(melee_vita_point_sprite_counts(4096u, &vertices, &indices));
    assert(vertices == 16384u && indices == 24576u);

    assert(!melee_vita_point_sprite_counts(UINT16_MAX / 4u + 1u,
                                           &vertices, &indices));
    assert(!melee_vita_point_sprite_counts(1u, NULL, &indices));
    assert(!melee_vita_point_sprite_counts(1u, &vertices, NULL));

    assert(melee_vita_point_sprite_texcoord_offset(0u, 0.5f, &s, &t));
    assert(s == 0.0f && t == 0.0f);
    assert(melee_vita_point_sprite_texcoord_offset(1u, 0.5f, &s, &t));
    assert(s == 0.5f && t == 0.0f);
    assert(melee_vita_point_sprite_texcoord_offset(2u, 0.5f, &s, &t));
    assert(s == 0.5f && t == 0.5f);
    assert(melee_vita_point_sprite_texcoord_offset(3u, 0.5f, &s, &t));
    assert(s == 0.0f && t == 0.5f);
    assert(!melee_vita_point_sprite_texcoord_offset(4u, 0.5f, &s, &t));
    assert(!melee_vita_point_sprite_texcoord_offset(0u, 0.5f, NULL, &t));
    assert(!melee_vita_point_sprite_texcoord_offset(0u, 0.5f, &s, NULL));
    assert(melee_vita_point_sprite_corner(0u, &x, &y));
    assert(x == -1.0f && y == 1.0f);
    assert(melee_vita_point_sprite_corner(2u, &x, &y));
    assert(x == 1.0f && y == -1.0f);
    assert(!melee_vita_point_sprite_corner(4u, &x, &y));
    assert(melee_vita_point_sprite_clip_half(12u, 1.0f, 1.0f, &x, &y));
    assert(x > 0.0020833f && x < 0.0020834f);
    assert(y > 0.0036764f && y < 0.0036765f);
    assert(melee_vita_point_sprite_clip_offset(1u, x, y, 2.0f, &s, &t));
    assert(s > 0.0041666f && s < 0.0041668f);
    assert(t > 0.0073528f && t < 0.0073530f);
    assert(melee_vita_point_sprite_clip_offset(1u, x, y, 4.0f, &s, &t));
    assert(s > 0.0083332f && s < 0.0083336f);
    assert(t > 0.0147056f && t < 0.0147060f);
    assert(!melee_vita_point_sprite_clip_half(12u, 1.0f, 1.0f, NULL, &y));
    assert(!melee_vita_point_sprite_clip_offset(
        0u, x, y, 1.0f, NULL, &t));

    assert(melee_vita_point_sprite_chunk_count(0u) == 0u);
    assert(!melee_vita_point_sprite_chunk(0u, 0u, &offset, &points));

    assert(melee_vita_point_sprite_chunk_count(16383u) == 1u);
    assert(melee_vita_point_sprite_chunk(16383u, 0u, &offset, &points));
    assert(offset == 0u && points == 16383u);
    assert(!melee_vita_point_sprite_chunk(16383u, 1u, &offset, &points));

    assert(melee_vita_point_sprite_chunk_count(16384u) == 2u);
    assert(melee_vita_point_sprite_chunk(16384u, 0u, &offset, &points));
    assert(offset == 0u && points == 16383u);
    assert(melee_vita_point_sprite_chunk(16384u, 1u, &offset, &points));
    assert(offset == 16383u && points == 1u);

    assert(melee_vita_point_sprite_chunk_count(24630u) == 2u);
    assert(melee_vita_point_sprite_chunk(24630u, 0u, &offset, &points));
    assert(offset == 0u && points == 16383u);
    assert(melee_vita_point_sprite_counts(points, &vertices, &indices));
    assert(vertices == 65532u && indices == 98298u);
    assert(melee_vita_point_sprite_chunk(24630u, 1u, &offset, &points));
    assert(offset == 16383u && points == 8247u);
    assert(melee_vita_point_sprite_counts(points, &vertices, &indices));
    assert(vertices == 32988u && indices == 49482u);
    assert(!melee_vita_point_sprite_chunk(24630u, 2u, &offset, &points));

    assert(melee_vita_point_sprite_chunk_count(UINT32_MAX) == 262161u);
    assert(melee_vita_point_sprite_chunk(
        UINT32_MAX, 262160u, &offset, &points));
    assert(offset == 4294967280u && points == 15u);
    assert(points <= MELEE_VITA_POINT_SPRITE_MAX_POINTS);
    assert(!melee_vita_point_sprite_chunk(1u, 0u, NULL, &points));
    assert(!melee_vita_point_sprite_chunk(1u, 0u, &offset, NULL));
    assert(melee_vita_point_sprite_quad_indices(0u, quad));
    assert(quad[0] == 0u && quad[1] == 1u && quad[2] == 2u);
    assert(quad[3] == 0u && quad[4] == 2u && quad[5] == 3u);
    assert(melee_vita_point_sprite_quad_indices(16382u, quad));
    assert(quad[0] == 65528u && quad[5] == 65531u);
    assert(!melee_vita_point_sprite_quad_indices(16383u, quad));
    assert(!melee_vita_point_sprite_quad_indices(0u, NULL));

    assert(melee_vita_point_gpu_payload(
        24630u, MELEE_VITA_GPU_POINT_STRIDE, &expanded, &max_indices,
        &vertex_bytes, &index_bytes));
    assert(expanded == 98520u);
    assert(max_indices == 98298u);
    assert(vertex_bytes == UINT64_C(6699360));
    assert(index_bytes == UINT64_C(196596));
    assert(vertex_bytes + index_bytes == UINT64_C(6895956));
    assert(!melee_vita_point_gpu_payload(
        UINT32_MAX, MELEE_VITA_GPU_POINT_STRIDE, &expanded, &max_indices,
        &vertex_bytes, &index_bytes));
    assert(!melee_vita_point_gpu_payload(
        1u, 0u, &expanded, &max_indices, &vertex_bytes, &index_bytes));
    assert(melee_vita_point_gpu_program_hash(UINT64_C(0x1234), 1u) !=
           UINT64_C(0x1234));
    assert(melee_vita_point_gpu_program_hash(UINT64_C(0x1234), 1u) !=
           melee_vita_point_gpu_program_hash(UINT64_C(0x1234), 2u));
    assert(melee_vita_nonpoint_vertex_program_hash(
               key_bytes, sizeof(key_bytes), 2u) ==
           UINT64_C(0x5381305F3231F2FB));
    assert(melee_vita_nonpoint_vertex_program_hash(
               key_bytes, sizeof(key_bytes), 2u) ==
           legacy_nonpoint_vertex_hash(key_bytes, sizeof(key_bytes), 2u));
    assert(melee_vita_nonpoint_vertex_program_hash(NULL, 1u, 2u) == 0u);
    assert(melee_vita_nonpoint_vertex_program_hash(
               key_bytes, sizeof(key_bytes), 2u) !=
           melee_vita_nonpoint_vertex_program_hash(
               key_bytes, sizeof(key_bytes), 3u));
    assert(melee_vita_nonpoint_vertex_program_hash(
               zero_key, sizeof(zero_key), 2u) ==
           UINT64_C(0xFB3C2DC2839BFAE7));
    assert(melee_vita_point_gpu_path_supported(false, false, true));
    assert(!melee_vita_point_gpu_path_supported(true, false, true));
    assert(!melee_vita_point_gpu_path_supported(false, true, true));
    assert(!melee_vita_point_gpu_path_supported(false, false, false));

    return 0;
}
