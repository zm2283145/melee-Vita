#pragma once

#include <stdint.h>

struct melee_vita_gx_stats {
    uint32_t batches;
    uint32_t vertices;
    uint32_t triangles;
    uint32_t indexed_attributes;
    uint32_t direct_attributes;
    uint32_t vertex_arrays;
    uint32_t max_index;
};

int melee_vita_gx_decode(const unsigned char* pobj,
                         const unsigned char* archive_data,
                         uint32_t archive_size,
                         struct melee_vita_gx_stats* stats);

int melee_vita_gx_extract_positions(const unsigned char* pobj,
                                    const unsigned char* archive_data,
                                    uint32_t archive_size, float* positions,
                                    float* texture_coordinates,
                                    uint8_t* position_matrix_indices,
                                    uint32_t capacity, uint32_t* count);
