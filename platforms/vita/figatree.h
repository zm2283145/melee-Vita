#pragma once

#include <stddef.h>
#include <stdint.h>

#define MELEE_VITA_FIGATREE_MAX_JOINTS 128u

struct melee_vita_joint_pose {
    float rotation[3];
    float translation[3];
    float scale[3];
    uint16_t value_mask;
};

int melee_vita_figatree_sample(
    const void* tree, const unsigned char* data, uint32_t data_size,
    float frame, struct melee_vita_joint_pose* poses, uint32_t pose_capacity,
    uint32_t* joint_count, uint32_t* track_count);
