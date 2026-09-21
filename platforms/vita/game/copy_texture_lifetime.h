/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_COPY_TEXTURE_LIFETIME_H
#define MELEE_VITA_COPY_TEXTURE_LIFETIME_H

#include <stdint.h>

typedef struct MeleeVitaCopyTextureBinding {
    const void* key;
    uint32_t allocation_generation;
} MeleeVitaCopyTextureBinding;

static inline void melee_vita_copy_texture_bind(
    MeleeVitaCopyTextureBinding* binding, const void* key,
    uint32_t allocation_generation)
{
    binding->key = key;
    binding->allocation_generation = allocation_generation;
}

static inline int melee_vita_copy_texture_binding_matches(
    const MeleeVitaCopyTextureBinding* binding, const void* key,
    uint32_t allocation_generation)
{
    return binding->key == key &&
           binding->allocation_generation == allocation_generation;
}

#endif
