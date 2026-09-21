/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_HEAP_H
#define MELEE_VITA_HEAP_H

#include <dolphin/types.h>

u32 melee_vita_heap_allocation_generation(const void* pointer);

#endif
