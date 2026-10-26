/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Decodes Slippi's game info block (the console's StartMeleeData, 0x138
 * big-endian bytes) into this port's StartMeleeData.
 *
 * Field offsets are the console's, from the names in melee/mn/types.h: a
 * bit-field called x2_1 is bit 1 of byte 2, counting from the top bit like
 * the console's compiler does.  The replay test logs the decoded stage,
 * timer and players when a match starts (slippi_replay.c), and a real
 * Slippi 3.14 block (Fox vs Falco on Final Destination) decoded to the
 * values Slippi Launcher shows for it. */
#ifdef MELEE_VITA_SLIPPI /* only in -EnableSlippi builds */
#include "slippi_match_block.h"

#include <stddef.h>
#include <string.h>

/* The console layout has 4-byte pointers, and so does the Vita. */
_Static_assert(sizeof(StartMeleeData) == SLIPPI_MATCH_BLOCK_SIZE, "StartMeleeData size");
_Static_assert(sizeof(StartMeleeRules) == 0x60, "StartMeleeRules size");
_Static_assert(sizeof(PlayerInitData) == 0x24, "PlayerInitData size");
_Static_assert(offsetof(StartMeleeRules, stkind) == 0xE, "stkind offset");
_Static_assert(offsetof(StartMeleeRules, time_limit) == 0x10, "time_limit offset");
_Static_assert(offsetof(StartMeleeRules, x20) == 0x20, "item mask offset");
_Static_assert(offsetof(StartMeleeRules, game_speed) == 0x34, "game_speed offset");
_Static_assert(offsetof(PlayerInitData, cpu_kind) == 0xE, "cpu_kind offset");
_Static_assert(offsetof(PlayerInitData, attack_ratio) == 0x18, "attack_ratio offset");

static u16 read_u16(const u8* p)
{
    return (u16) ((p[0] << 8) | p[1]);
}

static u32 read_u32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | (u32) p[3];
}

static float read_float(const u8* p)
{
    u32 bits = read_u32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Bit number "bit" of byte "byte", where bit 0 is the top bit. */
static u32 read_bit(const u8* block, int byte, int bit)
{
    return (block[byte] >> (7 - bit)) & 1;
}

static void decode_rules(const u8* b, StartMeleeRules* r)
{
    /* Byte 0: match_kind is the top 3 bits, then x0_3 (3 bits), then the
     * two timer flags. */
    r->match_kind = (b[0] >> 5) & 7;
    r->x0_3 = (b[0] >> 2) & 7;
    r->timer_enabled = read_bit(b, 0, 6);
    r->timer_counts_up = read_bit(b, 0, 7);

    r->x1_0 = read_bit(b, 1, 0);
    r->x1_1 = read_bit(b, 1, 1);
    r->x1_2 = read_bit(b, 1, 2);
    r->x1_3 = read_bit(b, 1, 3);
    r->x1_4 = read_bit(b, 1, 4);
    r->x1_5 = read_bit(b, 1, 5);
    r->timer_shows_hours = read_bit(b, 1, 6);
    r->friendly_fire = read_bit(b, 1, 7);

    r->is_stock = read_bit(b, 2, 0);
    r->x2_1 = read_bit(b, 2, 1);
    r->x2_2 = read_bit(b, 2, 2);
    r->single_button = read_bit(b, 2, 3);
    r->disable_pausing = read_bit(b, 2, 4);
    r->x2_5 = read_bit(b, 2, 5);
    r->x2_6 = read_bit(b, 2, 6);
    r->x2_7 = read_bit(b, 2, 7);

    r->x3_0 = read_bit(b, 3, 0);
    r->x3_1 = read_bit(b, 3, 1);
    r->x3_2 = read_bit(b, 3, 2);
    r->x3_3 = read_bit(b, 3, 3);
    r->x3_4 = read_bit(b, 3, 4);
    r->x3_5 = read_bit(b, 3, 5);
    r->x3_6 = read_bit(b, 3, 6);
    r->x3_7 = read_bit(b, 3, 7);

    r->x4_0 = read_bit(b, 4, 0);
    r->is_vs = read_bit(b, 4, 1);
    r->x4_2 = read_bit(b, 4, 2);
    r->x4_3 = read_bit(b, 4, 3);
    r->x4_4 = read_bit(b, 4, 4);
    r->x4_5 = read_bit(b, 4, 5);
    r->x4_6 = read_bit(b, 4, 6);
    r->x4_7 = read_bit(b, 4, 7);

    r->x5_0 = read_bit(b, 5, 0);
    r->x5_1 = read_bit(b, 5, 1);
    r->x5_2 = read_bit(b, 5, 2);
    r->x5_3 = read_bit(b, 5, 3);
    r->x5_4 = read_bit(b, 5, 4);
    r->x5_5 = read_bit(b, 5, 5);
    r->x5_6 = read_bit(b, 5, 6);
    r->x5_7 = read_bit(b, 5, 7);

    r->x6 = b[0x6];
    r->x7 = b[0x7];
    r->is_teams = b[0x8];
    r->x9 = b[0x9];
    r->xA = b[0xA];
    r->item_freq = (s8) b[0xB];
    r->sd_penalty = (s8) b[0xC];
    r->xD = b[0xD];
    r->stkind = read_u16(b + 0xE);
    r->time_limit = read_u32(b + 0x10);
    r->x14 = b[0x14];
    r->x18 = read_u32(b + 0x18);
    r->x20 = ((u64) read_u32(b + 0x20) << 32) | read_u32(b + 0x24);
    r->x28 = (int) read_u32(b + 0x28);
    r->x2C = read_float(b + 0x2C);
    r->x30 = read_float(b + 0x30);
    r->game_speed = read_float(b + 0x34);

    /* 0x38..0x5B are callbacks and pointers into the console's memory. */
    r->on_unpause_override = NULL;
    r->on_pause_override = NULL;
    r->check_for_pauser_override = NULL;
    r->on_match_start = NULL;
    r->on_frame_start = NULL;
    r->on_frame_end = NULL;
    r->on_match_end = NULL;
    r->x54 = NULL;
    r->x58 = NULL;
}

static void decode_player(const u8* b, PlayerInitData* p)
{
    p->ckind = (s8) b[0x0];
    p->slot_type = b[0x1];
    p->stocks = (s8) b[0x2];
    p->color = b[0x3];
    p->slot = b[0x4];
    p->spawn_pos = (s8) b[0x5];
    p->spawn_dir = (s8) b[0x6];
    p->sub_color = b[0x7];
    p->handicap = (s8) b[0x8];
    p->team = b[0x9];
    p->nametag = b[0xA];
    p->xB = b[0xB];

    p->rumble_enabled = read_bit(b, 0xC, 0);
    p->xC_b1 = read_bit(b, 0xC, 1);
    p->vs_metal = read_bit(b, 0xC, 2);
    p->xC_b3 = read_bit(b, 0xC, 3);
    p->vs_invisible = read_bit(b, 0xC, 4);
    p->xC_b5 = read_bit(b, 0xC, 5);
    p->xC_b6 = read_bit(b, 0xC, 6);
    p->xC_b7 = read_bit(b, 0xC, 7);
    p->xD_b0 = read_bit(b, 0xD, 0);
    p->xD_b1 = read_bit(b, 0xD, 1);
    p->xD_b2 = read_bit(b, 0xD, 2);
    p->xD_b3 = read_bit(b, 0xD, 3);
    p->xD_b4 = read_bit(b, 0xD, 4);
    p->xD_b5 = read_bit(b, 0xD, 5);
    p->xD_b6 = read_bit(b, 0xD, 6);
    p->xD_b7 = read_bit(b, 0xD, 7);

    p->cpu_kind = b[0xE];
    p->cpu_level = b[0xF];
    p->damage = read_u16(b + 0x10);
    p->damage1 = read_u16(b + 0x12);
    p->hp = read_u16(b + 0x14);
    p->attack_ratio = read_float(b + 0x18);
    p->defense_ratio = read_float(b + 0x1C);
    p->model_scale = read_float(b + 0x20);
}

void mp_slippi_decode_match_block(const u8* block, StartMeleeData* out)
{
    int i;

    memset(out, 0, sizeof(*out));
    decode_rules(block, &out->rules);
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        decode_player(block + 0x60 + i * 0x24, &out->players[i]);
    }
}

#endif /* MELEE_VITA_SLIPPI */
