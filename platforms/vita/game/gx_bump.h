#ifndef MELEE_VITA_GX_BUMP_H
#define MELEE_VITA_GX_BUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MELEE_VITA_BUMP_TEXGEN_MAX 8u
#define MELEE_VITA_GPU_VERTEX_BYTES 68u
#define MELEE_VITA_GPU_BUMP_VERTEX_BYTES 92u
#define MELEE_VITA_GPU_BUMP_BINORMAL_OFFSET 68u
#define MELEE_VITA_GPU_BUMP_TANGENT_OFFSET 80u
#define MELEE_VITA_GXR_LEGACY_KEY_BYTES 76u
#define MELEE_VITA_GXR_LEGACY_KEY_VERSION 2u
#define MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES 16u

enum melee_vita_bump_plan_result {
    MELEE_VITA_BUMP_PLAN_OK = 0,
    MELEE_VITA_BUMP_PLAN_NONE = 1,
    MELEE_VITA_BUMP_PLAN_BAD_COUNT = -1,
    MELEE_VITA_BUMP_PLAN_BAD_SOURCE = -2,
};

struct melee_vita_bump_plan {
    uint8_t count;
    uint8_t mask;
    uint8_t type[MELEE_VITA_BUMP_TEXGEN_MAX];
    uint8_t source[MELEE_VITA_BUMP_TEXGEN_MAX];
};

int melee_vita_bump_plan_build(
    const uint8_t* type, const uint8_t* source, uint8_t count,
    uint8_t bump0, uint8_t bump7, uint8_t texcoord0,
    struct melee_vita_bump_plan* plan);

bool melee_vita_bump_stage(
    const struct melee_vita_bump_plan* plan, uint8_t stage,
    uint8_t bump0, uint8_t texcoord0, uint8_t* source_stage,
    uint8_t* light);

void melee_vita_bump_scope_begin(void);
bool melee_vita_bump_scope_register(const void* identity);
bool melee_vita_bump_scope_register_or_begin(const void* identity);
void melee_vita_bump_scope_unregister(const void* identity);
uint32_t melee_vita_bump_scope_generation(const void* identity);

uint64_t melee_vita_gxr_legacy_vertex_hash(
    const void* key, size_t key_size);
uint64_t melee_vita_gxr_bump_vertex_hash(
    const void* legacy_key, size_t legacy_key_size,
    const struct melee_vita_bump_plan* plan);

void melee_vita_bump_texcoord(
    const float source_uv[2], const float eye[3], const float light[3],
    const float tangent[3], const float binormal[3], bool has_nbt,
    float output_uv[2]);

#endif
