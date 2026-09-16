#pragma once

#include <stdint.h>

struct melee_vita_bringup_info;

struct melee_vita_texture_atlas {
    uint32_t width;
    uint32_t height;
    uint32_t image_count;
    uint32_t* pixels;
};

int melee_vita_build_texture_atlas(
    const char* disc_path, struct melee_vita_bringup_info* mesh,
    struct melee_vita_texture_atlas* atlas);
void melee_vita_free_texture_atlas(struct melee_vita_texture_atlas* atlas);
