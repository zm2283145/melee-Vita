/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gxr_shader_cache.h"

#include <string.h>

static uint32_t warm_slot(uint64_t hash, uint8_t kind, uint32_t capacity)
{
    hash ^= (uint64_t) kind * UINT64_C(0x9e3779b97f4a7c15);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return (uint32_t) (hash % capacity);
}

static bool warm_entry_bounds(
    const uint8_t* buffer, uint32_t size, uint32_t offset,
    GxrWarmHeader* header, uint32_t* padded, GxrWarmError* error)
{
    if (size - offset < sizeof(*header)) {
        if (error != NULL) {
            error->status = GXR_WARM_TRAILING_BYTES;
            error->offset = offset;
        }
        return false;
    }
    memcpy(header, buffer + offset, sizeof(*header));
    if (header->magic != GXR_WARM_MAGIC) {
        if (error != NULL) {
            error->status = GXR_WARM_BAD_MAGIC;
            error->offset = offset;
        }
        return false;
    }
    if (header->kind > 1u) {
        if (error != NULL) {
            error->status = GXR_WARM_BAD_KIND;
            error->offset = offset;
        }
        return false;
    }
    if (header->size == 0u ||
        header->size > GXR_WARM_PROGRAM_MAX_SIZE) {
        if (error != NULL) {
            error->status = GXR_WARM_BAD_SIZE;
            error->offset = offset;
        }
        return false;
    }
    *padded = (header->size + 3u) & ~3u;
    if (*padded > size - offset - sizeof(*header)) {
        if (error != NULL) {
            error->status = GXR_WARM_TRUNCATED_ENTRY;
            error->offset = offset;
        }
        return false;
    }
    return true;
}

void gxr_warm_index_init(
    GxrWarmIndex* index, GxrWarmIndexEntry* entries, uint32_t capacity)
{
    memset(entries, 0, capacity * sizeof(*entries));
    index->entries = entries;
    index->capacity = capacity;
    index->count = 0u;
    index->duplicates = 0u;
    index->unindexed = 0u;
}

static GxrWarmIndexEntry* warm_find_slot(
    GxrWarmIndex* index, uint64_t hash, uint8_t kind, bool* duplicate)
{
    uint32_t slot;
    *duplicate = false;
    if (index->capacity == 0u) return NULL;
    slot = warm_slot(hash, kind, index->capacity);
    for (uint32_t probe = 0u; probe < index->capacity; ++probe) {
        GxrWarmIndexEntry* entry =
            &index->entries[(slot + probe) % index->capacity];
        if (!entry->occupied) return entry;
        if (entry->hash == hash && entry->kind == kind) {
            *duplicate = true;
            return entry;
        }
    }
    return NULL;
}

GxrWarmStatus gxr_warm_index_add_buffer(
    GxrWarmIndex* index, const void* buffer_value, uint32_t size,
    GxrWarmSource source, GxrWarmValidateProgram validate, void* user,
    GxrWarmError* error)
{
    const uint8_t* buffer = buffer_value;
    GxrWarmError local_error;
    uint32_t offset = 0u;
    if (error == NULL) error = &local_error;
    if (error != NULL) {
        error->status = GXR_WARM_OK;
        error->offset = 0u;
    }
    while (offset < size) {
        GxrWarmHeader header;
        uint32_t padded;
        if (!warm_entry_bounds(
                buffer, size, offset, &header, &padded, error)) {
            return error->status;
        }
        if (validate != NULL &&
            !validate(buffer + offset + sizeof(header),
                      header.size, user)) {
            if (error != NULL) {
                error->status = GXR_WARM_INVALID_PROGRAM;
                error->offset = offset;
            }
            return GXR_WARM_INVALID_PROGRAM;
        }
        offset += (uint32_t) sizeof(header) + padded;
    }

    offset = 0u;
    while (offset < size) {
        GxrWarmHeader header;
        GxrWarmIndexEntry* entry;
        uint32_t padded;
        bool duplicate;
        if (!warm_entry_bounds(
                buffer, size, offset, &header, &padded, NULL)) {
            return GXR_WARM_TRAILING_BYTES;
        }
        entry = warm_find_slot(
            index, header.hash, (uint8_t) header.kind, &duplicate);
        if (duplicate) {
            ++index->duplicates;
        } else if (entry == NULL) {
            ++index->unindexed;
        } else {
            entry->hash = header.hash;
            entry->program = buffer + offset + sizeof(header);
            entry->size = header.size;
            entry->kind = (uint8_t) header.kind;
            entry->source = (uint8_t) source;
            entry->occupied = true;
            ++index->count;
        }
        offset += (uint32_t) sizeof(header) + padded;
    }
    return GXR_WARM_OK;
}

const void* gxr_warm_index_lookup(
    const GxrWarmIndex* index, uint64_t hash, bool vertex, uint32_t* size,
    GxrWarmSource* source)
{
    const uint8_t kind = vertex ? 1u : 0u;
    uint32_t slot;
    if (size != NULL) *size = 0u;
    if (source != NULL) *source = GXR_WARM_SOURCE_NONE;
    if (index->capacity == 0u) return NULL;
    slot = warm_slot(hash, kind, index->capacity);
    for (uint32_t probe = 0u; probe < index->capacity; ++probe) {
        const GxrWarmIndexEntry* entry =
            &index->entries[(slot + probe) % index->capacity];
        if (!entry->occupied) return NULL;
        if (entry->hash == hash && entry->kind == kind) {
            if (size != NULL) *size = entry->size;
            if (source != NULL) *source = (GxrWarmSource) entry->source;
            return entry->program;
        }
    }
    return NULL;
}

const void* gxr_warm_cache_scan(
    const void* buffer_value, uint32_t buffer_size, uint64_t hash,
    bool vertex, uint32_t* size)
{
    const uint8_t* buffer = buffer_value;
    uint32_t offset = 0u;
    if (size != NULL) *size = 0u;
    while (offset < buffer_size) {
        GxrWarmHeader header;
        uint32_t padded;
        if (!warm_entry_bounds(
                buffer, buffer_size, offset, &header, &padded, NULL)) {
            return NULL;
        }
        if (header.hash == hash &&
            header.kind == (vertex ? 1u : 0u)) {
            if (size != NULL) *size = header.size;
            return buffer + offset + sizeof(header);
        }
        offset += (uint32_t) sizeof(header) + padded;
    }
    return NULL;
}

const char* gxr_warm_status_name(GxrWarmStatus status)
{
    switch (status) {
    case GXR_WARM_OK: return "ok";
    case GXR_WARM_BAD_MAGIC: return "bad-magic";
    case GXR_WARM_BAD_KIND: return "bad-kind";
    case GXR_WARM_BAD_SIZE: return "bad-size";
    case GXR_WARM_TRUNCATED_ENTRY: return "truncated-entry";
    case GXR_WARM_TRAILING_BYTES: return "trailing-bytes";
    case GXR_WARM_INVALID_PROGRAM: return "invalid-program";
    }
    return "unknown";
}

void gxr_shader_compile_policy_init(
    GxrShaderCompilePolicy* policy, bool enabled)
{
    memset(policy, 0, sizeof(*policy));
    policy->enabled = enabled;
}

void gxr_shader_compile_policy_set(
    GxrShaderCompilePolicy* policy, bool enabled)
{
    policy->enabled = enabled;
}

bool gxr_shader_compile_policy_allow(
    GxrShaderCompilePolicy* policy, GxrShaderCompilerPath path)
{
    if (policy->enabled) return true;
    if ((unsigned) path < GXR_SHADER_COMPILER_PATH_COUNT)
        ++policy->blocked[path];
    return false;
}

uint32_t gxr_shader_compile_policy_blocked(
    const GxrShaderCompilePolicy* policy)
{
    uint32_t total = 0u;
    for (unsigned i = 0u; i < GXR_SHADER_COMPILER_PATH_COUNT; ++i)
        total += policy->blocked[i];
    return total;
}
