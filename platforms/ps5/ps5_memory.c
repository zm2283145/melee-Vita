/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Low-memory allocation and the 32-bit disc-pointer table for PS5.
 *
 * MAP_32BIT is ignored by the PS5 kernel, but fixed and hinted mappings
 * below 4 GiB work (probed on firmware 13.60), so low blocks are carved out
 * of a window starting at 0x10000000.  Pointers that live elsewhere (code,
 * static data, the libc heap) are stored in disc slots through the
 * external-pointer table, as on 64-bit PC builds (see pc/disc.h). */
#include "ps5_memory.h"
#include "ps5_log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define LOW_WINDOW_BASE 0x10000000u
#define LOW_WINDOW_END 0xF0000000u
#define LOW_GRANULE 0x10000u

static uintptr_t s_low_next = LOW_WINDOW_BASE;
static pthread_mutex_t s_low_lock = PTHREAD_MUTEX_INITIALIZER;

void* melee_ps5_alloc_low(size_t size)
{
    const size_t length = (size + LOW_GRANULE - 1u) & ~(size_t) (LOW_GRANULE - 1u);
    void* block = NULL;
    pthread_mutex_lock(&s_low_lock);
    while (s_low_next + length <= LOW_WINDOW_END) {
        void* wanted = (void*) s_low_next;
        /* MAP_FIXED would silently replace an existing mapping; ask for the
         * address as a hint and keep it only if the kernel honoured it. */
        block = mmap(wanted, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (block == wanted) {
            s_low_next += length;
            break;
        }
        if (block != MAP_FAILED) munmap(block, length);
        block = NULL;
        s_low_next += 0x1000000u;
    }
    pthread_mutex_unlock(&s_low_lock);
    if (block == NULL) {
        melee_ps5_log("melee_ps5_alloc_low(%zu): no address below 4 GiB", size);
        return NULL;
    }
    melee_ps5_log("low block %p..%p (%zu bytes)", block, (char*) block + length, length);
    memset(block, 0, length);
    return block;
}

void melee_ps5_free_low(void* block, size_t size)
{
    if (block == NULL) return;
    munmap(block, (size + LOW_GRANULE - 1u) & ~(size_t) (LOW_GRANULE - 1u));
}

/* ---- external pointers ------------------------------------------------------
 * Ids are 1-based and below 2^24 (disc.h tags them with 0x02000000).  A small
 * open-addressing hash makes re-registering a pointer O(1): the game stores
 * the same static tables into slots every frame. */
#define EXT_HASH_BITS 20u
#define EXT_HASH_SIZE (1u << EXT_HASH_BITS)

#define EXT_CAPACITY (EXT_HASH_SIZE / 2u)
/* Fixed storage: resolution (every DP() read) is then lock-free. */
static void* s_ext_ptrs[EXT_CAPACITY];
static volatile uint32_t s_ext_count;
static uint32_t* s_ext_hash;
static pthread_mutex_t s_ext_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t ext_hash(const void* p)
{
    uint64_t x = (uint64_t) (uintptr_t) p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (uint32_t) x & (EXT_HASH_SIZE - 1u);
}

uint32_t pc_register_ext_ptr(const void* p)
{
    uint32_t slot, id;
    if (p == NULL) return 0;
    pthread_mutex_lock(&s_ext_lock);
    if (s_ext_hash == NULL) {
        s_ext_hash = calloc(EXT_HASH_SIZE, sizeof(uint32_t));
        if (s_ext_hash == NULL) abort();
    }
    for (slot = ext_hash(p);; slot = (slot + 1u) & (EXT_HASH_SIZE - 1u)) {
        id = s_ext_hash[slot];
        if (id == 0u) break;
        if (s_ext_ptrs[id - 1u] == p) {
            pthread_mutex_unlock(&s_ext_lock);
            return id;
        }
    }
    if (s_ext_count >= EXT_CAPACITY) {
        pthread_mutex_unlock(&s_ext_lock);
        melee_ps5_log("pc_register_ext_ptr: table full (%u)", s_ext_count);
        melee_ps5_log_flush();
        abort();
    }
    s_ext_ptrs[s_ext_count] = (void*) p;
    __atomic_store_n(&s_ext_count, s_ext_count + 1u, __ATOMIC_RELEASE);
    id = s_ext_count;
    s_ext_hash[slot] = id;
    pthread_mutex_unlock(&s_ext_lock);
    return id;
}

void* pc_resolve_ext_ptr(uint32_t id)
{
    if (id == 0u || id > __atomic_load_n(&s_ext_count, __ATOMIC_ACQUIRE)) return NULL;
    return s_ext_ptrs[id - 1u];
}

void pc_disc_ptr_overflow(const void* p, const char* file, int line)
{
    melee_ps5_log("%s:%d: pointer %p does not fit a 32-bit disc slot", file, line, p);
    melee_ps5_log_flush();
    abort();
}
