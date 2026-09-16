#pragma once

#include <stdint.h>

struct melee_vita_render_character {
    const float* positions;
    const float* texture_coordinates;
    const uint8_t* cull_modes;
    unsigned int vertex_count;
    unsigned int frame_count;
    const uint32_t* atlas_pixels;
    unsigned int atlas_width;
    unsigned int atlas_height;
};

int melee_vita_gxm_probe_run(
    const struct melee_vita_render_character* characters,
    unsigned int character_count);
