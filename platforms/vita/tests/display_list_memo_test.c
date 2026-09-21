#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../game/display_list_memo.h"

static uint32_t hash_bytes(const uint8_t* data, uint32_t length)
{
    uint32_t hash = 0x01234567u;
    for (uint32_t i = 0; i < length; ++i)
        hash = (hash ^ data[i]) * 0x01000193u;
    return hash;
}

static uint32_t validate(
    bool immutable, uint32_t memoized_bytecode_hash,
    const uint8_t* bytecode, uint32_t bytecode_length, uint32_t array_hash,
    uint32_t* bytecode_hash_calls)
{
    uint32_t content_hash = memoized_bytecode_hash;
    if (melee_vita_dl_rehash_bytecode(immutable)) {
        ++*bytecode_hash_calls;
        content_hash = hash_bytes(bytecode, bytecode_length);
    }
    return melee_vita_dl_combine_array_hash(content_hash, array_hash);
}

static void test_immutable_bytecode_is_memoized(void)
{
    uint8_t bytecode[] = { 1u, 2u, 3u, 4u };
    const uint32_t initial = hash_bytes(bytecode, sizeof(bytecode));
    uint32_t calls = 0u;
    const uint32_t first =
        validate(true, initial, bytecode, sizeof(bytecode), 10u, &calls);

    bytecode[1] ^= 0xffu;
    assert(validate(true, initial, bytecode, sizeof(bytecode), 10u, &calls) ==
           first);
    assert(calls == 0u);
}

static void test_mutable_bytecode_is_revalidated(void)
{
    uint8_t bytecode[] = { 1u, 2u, 3u, 4u };
    const uint32_t initial = hash_bytes(bytecode, sizeof(bytecode));
    uint32_t calls = 0u;
    const uint32_t first =
        validate(false, initial, bytecode, sizeof(bytecode), 10u, &calls);

    bytecode[1] ^= 0xffu;
    assert(validate(false, initial, bytecode, sizeof(bytecode), 10u, &calls) !=
           first);
    assert(calls == 2u);
}

static void test_arrays_remain_validated_for_immutable_bytecode(void)
{
    const uint8_t bytecode[] = { 1u, 2u, 3u, 4u };
    const uint32_t initial = hash_bytes(bytecode, sizeof(bytecode));
    uint32_t calls = 0u;
    const uint32_t first =
        validate(true, initial, bytecode, sizeof(bytecode), 10u, &calls);

    assert(validate(true, initial, bytecode, sizeof(bytecode), 11u, &calls) !=
           first);
    assert(calls == 0u);
}

static void test_policy_and_lifecycle_are_fail_safe(void)
{
    const uint8_t list_a[4] = { 0u };
    const uint8_t list_b[4] = { 0u };

    assert(melee_vita_dl_cache_identity_matches(
        list_a, 4u, 9u, true, list_a, 4u, 9u, true));
    assert(!melee_vita_dl_cache_identity_matches(
        list_a, 4u, 9u, true, list_a, 4u, 9u, false));
    assert(!melee_vita_dl_cache_identity_matches(
        list_a, 4u, 9u, true, list_b, 4u, 9u, true));
    assert(melee_vita_dl_release_invalidates(list_a, list_a));
    assert(!melee_vita_dl_release_invalidates(list_a, list_b));
}

int main(void)
{
    test_immutable_bytecode_is_memoized();
    test_mutable_bytecode_is_revalidated();
    test_arrays_remain_validated_for_immutable_bytecode();
    test_policy_and_lifecycle_are_fail_safe();
    puts("display-list memo tests passed");
    return 0;
}
