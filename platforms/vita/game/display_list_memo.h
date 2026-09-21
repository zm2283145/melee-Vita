#ifndef MELEE_VITA_DISPLAY_LIST_MEMO_H
#define MELEE_VITA_DISPLAY_LIST_MEMO_H

#include <stdbool.h>
#include <stdint.h>

static inline bool melee_vita_dl_cache_identity_matches(
    const void* cached_list, uint32_t cached_bytes, uint32_t cached_state_hash,
    bool cached_immutable, const void* list, uint32_t bytes,
    uint32_t state_hash, bool immutable)
{
    return cached_list == list && cached_bytes == bytes &&
           cached_state_hash == state_hash &&
           cached_immutable == immutable;
}

static inline bool melee_vita_dl_rehash_bytecode(bool immutable)
{
    return !immutable;
}

static inline uint32_t melee_vita_dl_combine_array_hash(
    uint32_t content_hash, uint32_t array_hash)
{
    return content_hash ^
           (array_hash + 0x9e3779b9u + (content_hash << 6) +
            (content_hash >> 2));
}

static inline bool melee_vita_dl_release_invalidates(
    const void* cached_list, const void* released_list)
{
    return cached_list == released_list;
}

#endif
