/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_GXR_SHADER_CACHE_H
#define MELEE_VITA_GXR_SHADER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GXR_WARM_MAGIC UINT32_C(0x35525847)
#define GXR_WARM_PROGRAM_MAX_SIZE (1024u * 1024u)
#define GXR_WARM_INDEX_CAPACITY 4096u

typedef struct GxrWarmHeader {
    uint32_t magic;
    uint32_t kind;
    uint32_t size;
    uint32_t reserved;
    uint64_t hash;
} GxrWarmHeader;

_Static_assert(sizeof(GxrWarmHeader) == 24u,
               "warm shader cache header format changed");

typedef enum GxrWarmSource {
    GXR_WARM_SOURCE_NONE = 0,
    GXR_WARM_SOURCE_WRITABLE = 1,
    GXR_WARM_SOURCE_PACKAGED = 2,
} GxrWarmSource;

typedef enum GxrWarmStatus {
    GXR_WARM_OK = 0,
    GXR_WARM_BAD_MAGIC,
    GXR_WARM_BAD_KIND,
    GXR_WARM_BAD_SIZE,
    GXR_WARM_TRUNCATED_ENTRY,
    GXR_WARM_TRAILING_BYTES,
    GXR_WARM_INVALID_PROGRAM,
} GxrWarmStatus;

typedef struct GxrWarmError {
    GxrWarmStatus status;
    uint32_t offset;
} GxrWarmError;

typedef bool (*GxrWarmValidateProgram)(
    const void* program, uint32_t size, void* user);

typedef struct GxrWarmIndexEntry {
    uint64_t hash;
    const uint8_t* program;
    uint32_t size;
    uint8_t kind;
    uint8_t source;
    bool occupied;
} GxrWarmIndexEntry;

typedef struct GxrWarmIndex {
    GxrWarmIndexEntry* entries;
    uint32_t capacity;
    uint32_t count;
    uint32_t duplicates;
    uint32_t unindexed;
} GxrWarmIndex;

void gxr_warm_index_init(
    GxrWarmIndex* index, GxrWarmIndexEntry* entries, uint32_t capacity);
GxrWarmStatus gxr_warm_index_add_buffer(
    GxrWarmIndex* index, const void* buffer, uint32_t size,
    GxrWarmSource source, GxrWarmValidateProgram validate, void* user,
    GxrWarmError* error);
const void* gxr_warm_index_lookup(
    const GxrWarmIndex* index, uint64_t hash, bool vertex, uint32_t* size,
    GxrWarmSource* source);
const void* gxr_warm_cache_scan(
    const void* buffer, uint32_t buffer_size, uint64_t hash, bool vertex,
    uint32_t* size);
const char* gxr_warm_status_name(GxrWarmStatus status);

typedef enum GxrShaderCompilerPath {
    GXR_SHADER_COMPILER_NORMAL = 0,
    GXR_SHADER_COMPILER_BUMP = 1,
    GXR_SHADER_COMPILER_PATH_COUNT = 2,
} GxrShaderCompilerPath;

typedef struct GxrShaderCompilePolicy {
    bool enabled;
    uint32_t blocked[GXR_SHADER_COMPILER_PATH_COUNT];
} GxrShaderCompilePolicy;

void gxr_shader_compile_policy_init(
    GxrShaderCompilePolicy* policy, bool enabled);
void gxr_shader_compile_policy_set(
    GxrShaderCompilePolicy* policy, bool enabled);
bool gxr_shader_compile_policy_allow(
    GxrShaderCompilePolicy* policy, GxrShaderCompilerPath path);
uint32_t gxr_shader_compile_policy_blocked(
    const GxrShaderCompilePolicy* policy);

#endif
