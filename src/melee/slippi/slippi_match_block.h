/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi's game info block: the console's StartMeleeData as raw bytes. */
#ifndef MELEE_SLIPPI_MATCH_BLOCK_H
#define MELEE_SLIPPI_MATCH_BLOCK_H

#include <melee/mn/types.h>

/* Size of the block in a .slp file and in Dolphin's online match setup. */
#define SLIPPI_MATCH_BLOCK_SIZE 0x138

/* Fills *out from the console's big-endian bytes.  The 3DS port can memcpy
 * the block because its game code runs big-endian.  On the Vita the struct
 * is little-endian and GCC lays its bit-fields out from the lowest bit, so
 * every field is read by its console offset instead.  The callback pointers
 * (console addresses) come out NULL, as on the 3DS. */
void mp_slippi_decode_match_block(const u8* block, StartMeleeData* out);

#endif
