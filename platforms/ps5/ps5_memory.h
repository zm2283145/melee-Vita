/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_PS5_MEMORY_H
#define MELEE_PS5_MEMORY_H

#include <stddef.h>

/* Zeroed memory below 4 GiB, for data that GameCube-format 32-bit pointer
 * slots may reference directly (MEM1, the arena, ARAM staging). */
void* melee_ps5_alloc_low(size_t size);
void melee_ps5_free_low(void* block, size_t size);

#endif
