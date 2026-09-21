#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gx_bump.h"

struct legacy_texgen {
    uint8_t type, source, has_matrix, normalize, has_post, reserved;
};

struct legacy_channel {
    uint8_t enabled, amb_src, mat_src, lights, diffuse, atten;
};

struct legacy_key {
    uint8_t has_mtxidx;
    uint8_t perspective;
    uint8_t channel_count;
    uint8_t texgen_count;
    struct legacy_channel channel[4];
    struct legacy_texgen texgen[8];
};

struct gpu_vertex {
    float pos[3];
    float mtx;
    float normal[3];
    uint8_t color0[4];
    uint8_t color1[4];
    float tex[4][2];
};

struct bump_vertex {
    struct gpu_vertex base;
    float binormal[3];
    float tangent[3];
};

static uint64_t reference_legacy_hash(const void* data, size_t size)
{
    const uint8_t* bytes = data;
    uint64_t hash = UINT64_C(0x9e3779b97f4a7c15) ^ UINT64_C(2);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int close_enough(float a, float b)
{
    return fabsf(a - b) < 1.0e-6f;
}

int main(void)
{
    uintptr_t identities[MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES + 1u];
    const uint8_t bump0 = 2u;
    const uint8_t bump7 = 9u;
    const uint8_t texcoord0 = 12u;
    uint8_t types[8] = { 0u, 5u, 0u };
    uint8_t sources[8] = { 4u, 12u, 6u };
    struct melee_vita_bump_plan plan;
    struct melee_vita_bump_plan second_plan;
    struct legacy_key legacy = { 0 };
    struct legacy_key second = { 0 };
    uint64_t bump_hash;
    uint8_t source_stage, bump_light;
    float output[2];
    const float uv[2] = { 0.25f, 0.75f };
    const float eye[3] = { 1.0f, 2.0f, 3.0f };
    const float light[3] = { 1.0f, 2.0f, 5.0f };
    const float tangent[3] = { 0.0f, 0.0f, 1.0f };
    const float binormal[3] = { 1.0f, 0.0f, 0.0f };

    _Static_assert(sizeof(struct gpu_vertex) == MELEE_VITA_GPU_VERTEX_BYTES,
                   "legacy GPU vertex layout changed");
    _Static_assert(
        sizeof(struct legacy_key) == MELEE_VITA_GXR_LEGACY_KEY_BYTES,
        "legacy GPU vertex key layout changed");
    _Static_assert(
        sizeof(struct bump_vertex) == MELEE_VITA_GPU_BUMP_VERTEX_BYTES,
        "bump GPU vertex layout changed");
    _Static_assert(
        offsetof(struct bump_vertex, binormal) ==
            MELEE_VITA_GPU_BUMP_BINORMAL_OFFSET,
        "bump binormal offset changed");
    _Static_assert(
        offsetof(struct bump_vertex, tangent) ==
            MELEE_VITA_GPU_BUMP_TANGENT_OFFSET,
        "bump tangent offset changed");

    for (uintptr_t i = 0;
         i < MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES + 1u; ++i) {
        identities[i] = i + 1u;
    }
    assert(melee_vita_bump_scope_generation(&identities[0]) == 0u);
    assert(!melee_vita_bump_scope_register(&identities[0]));
    melee_vita_bump_scope_begin();
    {
        uint32_t first_generation;
        assert(melee_vita_bump_scope_register(&identities[0]));
        assert(melee_vita_bump_scope_register(&identities[1]));
        assert(melee_vita_bump_scope_register(&identities[0]));
        first_generation =
            melee_vita_bump_scope_generation(&identities[0]);
        assert(first_generation != 0u);
        assert(melee_vita_bump_scope_generation(&identities[0]) ==
               first_generation);
        assert(melee_vita_bump_scope_generation(&identities[1]) ==
               first_generation);
        assert(melee_vita_bump_scope_generation(&identities[2]) == 0u);
        melee_vita_bump_scope_unregister(&identities[0]);
        assert(melee_vita_bump_scope_generation(&identities[0]) == 0u);
        assert(melee_vita_bump_scope_generation(&identities[1]) ==
               first_generation);
        melee_vita_bump_scope_unregister(&identities[1]);
        assert(melee_vita_bump_scope_generation(&identities[1]) == 0u);
        assert(!melee_vita_bump_scope_register(&identities[0]));

        melee_vita_bump_scope_begin();
        assert(melee_vita_bump_scope_register(&identities[0]));
        assert(melee_vita_bump_scope_generation(&identities[0]) !=
               first_generation);
        for (uintptr_t i = 1;
             i < MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES; ++i) {
            assert(melee_vita_bump_scope_register(&identities[i]));
        }
        assert(!melee_vita_bump_scope_register(
            &identities[MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES]));
        melee_vita_bump_scope_unregister(&identities[0]);
        assert(melee_vita_bump_scope_generation(&identities[0]) == 0u);
    }

    memset(&plan, 0xa5, sizeof(plan));
    assert(melee_vita_bump_plan_build(
               types, sources, 3u, bump0, bump7, texcoord0, &plan) ==
           MELEE_VITA_BUMP_PLAN_OK);
    assert(plan.count == 3u);
    assert(plan.mask == 2u);
    assert(plan.type[1] == 5u);
    assert(plan.source[1] == texcoord0);
    assert(plan.type[0] == 0u && plan.source[0] == 0u);
    assert(plan.type[2] == 0u && plan.source[2] == 0u);
    assert(melee_vita_bump_stage(
        &plan, 1u, bump0, texcoord0, &source_stage, &bump_light));
    assert(source_stage == 0u && bump_light == 3u);
    assert(!melee_vita_bump_stage(
        &plan, 0u, bump0, texcoord0, &source_stage, &bump_light));
    assert(!melee_vita_bump_stage(
        &plan, 3u, bump0, texcoord0, &source_stage, &bump_light));

    sources[1] = (uint8_t) (texcoord0 + 1u);
    assert(melee_vita_bump_plan_build(
               types, sources, 3u, bump0, bump7, texcoord0, &plan) ==
           MELEE_VITA_BUMP_PLAN_BAD_SOURCE);
    assert(plan.count == 0u && plan.mask == 0u);
    sources[1] = texcoord0;
    memset(&plan, 0xa5, sizeof(plan));
    assert(melee_vita_bump_plan_build(
               types, sources, 9u, bump0, bump7, texcoord0, &plan) ==
           MELEE_VITA_BUMP_PLAN_BAD_COUNT);
    assert(plan.count == 0u && plan.mask == 0u);
    assert(melee_vita_bump_plan_build(
               NULL, sources, 3u, bump0, bump7, texcoord0, &plan) ==
           MELEE_VITA_BUMP_PLAN_BAD_COUNT);
    assert(melee_vita_bump_plan_build(
               types, NULL, 3u, bump0, bump7, texcoord0, &plan) ==
           MELEE_VITA_BUMP_PLAN_BAD_COUNT);
    assert(melee_vita_bump_plan_build(
               types, sources, 3u, bump0, bump7, texcoord0, NULL) ==
           MELEE_VITA_BUMP_PLAN_BAD_COUNT);

    memset(types, 0, sizeof(types));
    assert(melee_vita_bump_plan_build(
               types, sources, 3u, bump0, bump7, texcoord0, &plan) ==
           MELEE_VITA_BUMP_PLAN_NONE);
    assert(plan.count == 3u && plan.mask == 0u);

    legacy.has_mtxidx = 1u;
    legacy.perspective = 1u;
    legacy.channel_count = 2u;
    legacy.texgen_count = 2u;
    legacy.channel[0].enabled = 1u;
    legacy.channel[0].lights = 3u;
    legacy.texgen[0].type = 1u;
    legacy.texgen[0].source = 4u;
    second = legacy;
    assert(melee_vita_gxr_legacy_vertex_hash(&legacy, sizeof(legacy)) ==
           reference_legacy_hash(&legacy, sizeof(legacy)));
    assert(melee_vita_gxr_legacy_vertex_hash(&legacy, sizeof(legacy)) ==
           UINT64_C(0x99a57daafa75b22c));
    assert(melee_vita_gxr_legacy_vertex_hash(&legacy, sizeof(legacy)) ==
           melee_vita_gxr_legacy_vertex_hash(&second, sizeof(second)));

    types[0] = 0u;
    types[1] = 5u;
    assert(melee_vita_bump_plan_build(
               types, sources, 2u, bump0, bump7, texcoord0, &plan) ==
           MELEE_VITA_BUMP_PLAN_OK);
    bump_hash = melee_vita_gxr_bump_vertex_hash(
        &legacy, sizeof(legacy), &plan);
    assert(bump_hash !=
           melee_vita_gxr_legacy_vertex_hash(&legacy, sizeof(legacy)));
    second_plan = plan;
    second_plan.type[1] = 6u;
    assert(melee_vita_gxr_bump_vertex_hash(
               &legacy, sizeof(legacy), &second_plan) != bump_hash);

    types[1] = 0u;
    types[2] = 5u;
    sources[2] = (uint8_t) (texcoord0 + 1u);
    assert(melee_vita_bump_plan_build(
               types, sources, 3u, bump0, bump7, texcoord0,
               &second_plan) == MELEE_VITA_BUMP_PLAN_OK);
    assert(melee_vita_gxr_bump_vertex_hash(
               &legacy, sizeof(legacy), &second_plan) != bump_hash);
    sources[2] = texcoord0;
    assert(melee_vita_bump_plan_build(
               types, sources, 3u, bump0, bump7, texcoord0,
               &second_plan) == MELEE_VITA_BUMP_PLAN_OK);
    assert(melee_vita_gxr_bump_vertex_hash(
               &legacy, sizeof(legacy), &second_plan) != bump_hash);

    melee_vita_bump_texcoord(
        uv, eye, light, tangent, binormal, true, output);
    assert(close_enough(output[0], 1.25f));
    assert(close_enough(output[1], 0.75f));
    melee_vita_bump_texcoord(
        uv, eye, eye, tangent, binormal, true, output);
    assert(close_enough(output[0], uv[0]));
    assert(close_enough(output[1], uv[1]));
    melee_vita_bump_texcoord(
        uv, eye, light, tangent, binormal, false, output);
    assert(close_enough(output[0], uv[0]));
    assert(close_enough(output[1], uv[1]));
    return 0;
}
