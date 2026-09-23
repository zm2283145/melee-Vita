#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "gxr_shader_cache.h"

typedef struct TestBuffer {
    uint8_t bytes[256];
    uint32_t size;
} TestBuffer;

static void append_record(
    TestBuffer* buffer, uint32_t kind, uint64_t hash,
    const uint8_t* program, uint32_t size)
{
    GxrWarmHeader header = {
        GXR_WARM_MAGIC, kind, size, 0u, hash
    };
    const uint32_t padded = (size + 3u) & ~3u;
    memcpy(buffer->bytes + buffer->size, &header, sizeof(header));
    buffer->size += (uint32_t) sizeof(header);
    memcpy(buffer->bytes + buffer->size, program, size);
    memset(buffer->bytes + buffer->size + size, 0, padded - size);
    buffer->size += padded;
}

static bool validate_program(
    const void* program, uint32_t size, void* user)
{
    (void) user;
    return size >= 2u && ((const uint8_t*) program)[0] == 0x47u;
}

int main(void)
{
    const uint8_t writable_program[] = { 0x47u, 0x11u, 0x12u };
    const uint8_t duplicate_program[] = { 0x47u, 0x21u };
    const uint8_t packaged_program[] = { 0x47u, 0x31u, 0x32u, 0x33u };
    TestBuffer writable = { { 0 }, 0u };
    TestBuffer packaged = { { 0 }, 0u };
    GxrWarmIndexEntry entries[8];
    GxrWarmIndex index;
    GxrWarmError error;
    GxrWarmSource source;
    GxrShaderCompilePolicy policy;
    const uint8_t* found;
    uint32_t size;

    append_record(
        &writable, 0u, UINT64_C(0x101), writable_program,
        sizeof(writable_program));
    append_record(
        &writable, 0u, UINT64_C(0x101), duplicate_program,
        sizeof(duplicate_program));
    append_record(
        &packaged, 0u, UINT64_C(0x101), packaged_program,
        sizeof(packaged_program));
    append_record(
        &packaged, 1u, UINT64_C(0x202), packaged_program,
        sizeof(packaged_program));

    gxr_warm_index_init(&index, entries, 8u);
    assert(gxr_warm_index_add_buffer(
               &index, writable.bytes, writable.size,
               GXR_WARM_SOURCE_WRITABLE, validate_program, NULL,
               &error) == GXR_WARM_OK);
    assert(gxr_warm_index_add_buffer(
               &index, packaged.bytes, packaged.size,
               GXR_WARM_SOURCE_PACKAGED, validate_program, NULL,
               &error) == GXR_WARM_OK);
    assert(index.count == 2u);
    assert(index.duplicates == 2u);
    found = gxr_warm_index_lookup(
        &index, UINT64_C(0x101), false, &size, &source);
    assert(found != NULL && found[1] == writable_program[1]);
    assert(size == sizeof(writable_program));
    assert(source == GXR_WARM_SOURCE_WRITABLE);
    found = gxr_warm_index_lookup(
        &index, UINT64_C(0x202), true, &size, &source);
    assert(found != NULL && found[1] == packaged_program[1]);
    assert(source == GXR_WARM_SOURCE_PACKAGED);

    gxr_shader_compile_policy_init(&policy, false);
    assert(gxr_warm_index_lookup(
               &index, UINT64_C(0x101), false, NULL, NULL) != NULL);
    assert(!gxr_shader_compile_policy_allow(
        &policy, GXR_SHADER_COMPILER_NORMAL));
    assert(!gxr_shader_compile_policy_allow(
        &policy, GXR_SHADER_COMPILER_BUMP));
    assert(policy.blocked[GXR_SHADER_COMPILER_NORMAL] == 1u);
    assert(policy.blocked[GXR_SHADER_COMPILER_BUMP] == 1u);
    assert(gxr_shader_compile_policy_blocked(&policy) == 2u);
    gxr_shader_compile_policy_set(&policy, true);
    assert(gxr_shader_compile_policy_allow(
        &policy, GXR_SHADER_COMPILER_NORMAL));

    {
        TestBuffer malformed = writable;
        ((GxrWarmHeader*) malformed.bytes)->magic = 0u;
        gxr_warm_index_init(&index, entries, 8u);
        assert(gxr_warm_index_add_buffer(
                   &index, malformed.bytes, malformed.size,
                   GXR_WARM_SOURCE_WRITABLE, validate_program, NULL,
                   &error) == GXR_WARM_BAD_MAGIC);
        assert(error.offset == 0u && index.count == 0u);
    }
    {
        TestBuffer truncated = writable;
        gxr_warm_index_init(&index, entries, 8u);
        assert(gxr_warm_index_add_buffer(
                   &index, truncated.bytes, truncated.size - 1u,
                   GXR_WARM_SOURCE_WRITABLE, validate_program, NULL,
                   &error) == GXR_WARM_TRUNCATED_ENTRY);
        assert(index.count == 0u);
    }
    {
        TestBuffer trailing = writable;
        trailing.bytes[trailing.size++] = 0u;
        gxr_warm_index_init(&index, entries, 8u);
        assert(gxr_warm_index_add_buffer(
                   &index, trailing.bytes, trailing.size,
                   GXR_WARM_SOURCE_WRITABLE, validate_program, NULL,
                   &error) == GXR_WARM_TRAILING_BYTES);
        assert(index.count == 0u);
    }
    {
        TestBuffer invalid = { { 0 }, 0u };
        const uint8_t bad[] = { 0u, 1u };
        append_record(&invalid, 0u, UINT64_C(9), bad, sizeof(bad));
        gxr_warm_index_init(&index, entries, 8u);
        assert(gxr_warm_index_add_buffer(
                   &index, invalid.bytes, invalid.size,
                   GXR_WARM_SOURCE_WRITABLE, validate_program, NULL,
                   &error) == GXR_WARM_INVALID_PROGRAM);
        assert(index.count == 0u);
    }
    return 0;
}
