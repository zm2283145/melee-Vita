/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native Vita bridge to Aurora's portable THP decoder. */
#include "gxm_game.h"
#include "../vita_log.h"

#include <dolphin/thp.h>

void pc_thp_decode_frame(const void* jpeg, void* tile_y, void* tile_u,
                         void* tile_v)
{
    static unsigned int failure_count;
    static unsigned int success_count;
    const s32 result = THPVideoDecode(jpeg, tile_y, tile_u, tile_v, NULL);
    if (result != 0) {
        if (failure_count++ < 8)
            melee_vita_log_info("[THP] decode failed: %d", result);
        return;
    }
    if (success_count++ == 0)
        melee_vita_log_info("[THP] first video frame decoded");
    melee_vita_gxm_mark_texture_data_dirty();
}
