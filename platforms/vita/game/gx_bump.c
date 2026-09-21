#include "gx_bump.h"

#include <math.h>
#include <string.h>

#define FNV1A_PRIME UINT64_C(1099511628211)
#define LEGACY_VERTEX_SEED \
    (UINT64_C(0x9e3779b97f4a7c15) ^ \
     (uint64_t) MELEE_VITA_GXR_LEGACY_KEY_VERSION)
#define BUMP_VERTEX_SEED UINT64_C(0x42554d5056545831)

static const void*
    s_scope_identities[MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES];
static uint32_t s_scope_generation;
static uint8_t s_scope_active;

static uint64_t hash_bytes(
    const void* data, size_t size, uint64_t hash)
{
    const uint8_t* bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

int melee_vita_bump_plan_build(
    const uint8_t* type, const uint8_t* source, uint8_t count,
    uint8_t bump0, uint8_t bump7, uint8_t texcoord0,
    struct melee_vita_bump_plan* plan)
{
    bool found = false;
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    if (plan == NULL || type == NULL || source == NULL ||
        count > MELEE_VITA_BUMP_TEXGEN_MAX) {
        return MELEE_VITA_BUMP_PLAN_BAD_COUNT;
    }
    plan->count = count;
    for (uint8_t i = 0; i < count; ++i) {
        if (type[i] < bump0 || type[i] > bump7) continue;
        if (source[i] < texcoord0 ||
            (uint8_t) (source[i] - texcoord0) >= i) {
            memset(plan, 0, sizeof(*plan));
            return MELEE_VITA_BUMP_PLAN_BAD_SOURCE;
        }
        plan->mask |= (uint8_t) (1u << i);
        plan->type[i] = type[i];
        plan->source[i] = source[i];
        found = true;
    }
    return found ? MELEE_VITA_BUMP_PLAN_OK : MELEE_VITA_BUMP_PLAN_NONE;
}

bool melee_vita_bump_stage(
    const struct melee_vita_bump_plan* plan, uint8_t stage,
    uint8_t bump0, uint8_t texcoord0, uint8_t* source_stage,
    uint8_t* light)
{
    if (plan == NULL || source_stage == NULL || light == NULL ||
        stage >= plan->count || (plan->mask & (1u << stage)) == 0u ||
        plan->type[stage] < bump0 ||
        plan->source[stage] < texcoord0) {
        return false;
    }
    *source_stage = (uint8_t) (plan->source[stage] - texcoord0);
    *light = (uint8_t) (plan->type[stage] - bump0);
    return true;
}

static void bump_scope_advance_generation(void)
{
    ++s_scope_generation;
    if (s_scope_generation == 0u) ++s_scope_generation;
}

void melee_vita_bump_scope_begin(void)
{
    for (uint32_t i = 0; i < MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES; ++i)
        s_scope_identities[i] = NULL;
    bump_scope_advance_generation();
    s_scope_active = 1u;
}

bool melee_vita_bump_scope_register(const void* identity)
{
    if (!s_scope_active || identity == NULL) return false;
    for (uint32_t i = 0; i < MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES; ++i) {
        if (s_scope_identities[i] == identity) return true;
    }
    for (uint32_t i = 0; i < MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES; ++i) {
        if (s_scope_identities[i] == NULL) {
            s_scope_identities[i] = identity;
            return true;
        }
    }
    return false;
}

void melee_vita_bump_scope_unregister(const void* identity)
{
    bool any = false;
    if (!s_scope_active || identity == NULL) return;
    for (uint32_t i = 0; i < MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES; ++i) {
        if (s_scope_identities[i] == identity)
            s_scope_identities[i] = NULL;
        if (s_scope_identities[i] != NULL) any = true;
    }
    if (!any) {
        s_scope_active = 0u;
        bump_scope_advance_generation();
    }
}

uint32_t melee_vita_bump_scope_generation(const void* identity)
{
    if (!s_scope_active || identity == NULL) return 0u;
    for (uint32_t i = 0; i < MELEE_VITA_BUMP_SCOPE_MAX_IDENTITIES; ++i) {
        if (s_scope_identities[i] == identity)
            return s_scope_generation;
    }
    return 0u;
}

uint64_t melee_vita_gxr_legacy_vertex_hash(
    const void* key, size_t key_size)
{
    return hash_bytes(key, key_size, LEGACY_VERTEX_SEED);
}

uint64_t melee_vita_gxr_bump_vertex_hash(
    const void* legacy_key, size_t legacy_key_size,
    const struct melee_vita_bump_plan* plan)
{
    uint64_t hash = hash_bytes(
        legacy_key, legacy_key_size, BUMP_VERTEX_SEED);
    return hash_bytes(plan, sizeof(*plan), hash);
}

void melee_vita_bump_texcoord(
    const float source_uv[2], const float eye[3], const float light[3],
    const float tangent[3], const float binormal[3], bool has_nbt,
    float output_uv[2])
{
    float ldir[3] = {
        light[0] - eye[0],
        light[1] - eye[1],
        light[2] - eye[2],
    };
    const float len2 =
        ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2];
    output_uv[0] = source_uv[0];
    output_uv[1] = source_uv[1];
    if (has_nbt && len2 > 1.0e-16f) {
        const float inv = 1.0f / sqrtf(len2);
        ldir[0] *= inv;
        ldir[1] *= inv;
        ldir[2] *= inv;
        output_uv[0] += ldir[0] * tangent[0] +
                        ldir[1] * tangent[1] +
                        ldir[2] * tangent[2];
        output_uv[1] += ldir[0] * binormal[0] +
                        ldir[1] * binormal[1] +
                        ldir[2] * binormal[2];
    }
}
