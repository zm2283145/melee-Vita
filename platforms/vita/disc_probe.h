#pragma once

#include <stdint.h>

#define MELEE_VITA_DISC_PATH "ux0:data/melee/GALE01.iso"
#define MELEE_VITA_EXPECTED_DISC_SIZE UINT64_C(1459978240)

enum melee_vita_disc_result {
    MELEE_VITA_DISC_OK = 0,
    MELEE_VITA_DISC_OPEN_FAILED,
    MELEE_VITA_DISC_READ_FAILED,
    MELEE_VITA_DISC_BAD_MAGIC,
    MELEE_VITA_DISC_WRONG_GAME,
    MELEE_VITA_DISC_WRONG_REVISION,
    MELEE_VITA_DISC_WRONG_SIZE,
};

struct melee_vita_disc_info {
    char game_id[7];
    uint8_t disc_number;
    uint8_t revision;
    uint64_t size;
};

enum melee_vita_disc_result melee_vita_probe_disc(
    const char* path, struct melee_vita_disc_info* info);
const char* melee_vita_disc_result_text(enum melee_vita_disc_result result);
