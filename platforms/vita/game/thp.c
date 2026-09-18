/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * THP video frames on Vita.
 *
 * The portable decoder (aurora's THPVideoDecode) needs ~130 ms per frame here,
 * four times the movie's budget, so frames go through the Vita's JPEG codec
 * instead: the THP scan is restored to a standard JPEG byte stream, the codec
 * decodes it into planar YCbCr, and the planes are written into the GX I8 tile
 * layout the game uploads as textures.  The portable decoder stays as the
 * fallback for anything the codec will not take.
 */
#include "gxm_game.h"
#include "jpeg_hw.h"
#include "../vita_log.h"

#include <dolphin/thp.h>

#include <psp2/kernel/processmgr.h>

#include <stdlib.h>
#include <string.h>

#define THP_MAX_JPEG (512u * 1024u)

static struct melee_vita_opening_jpeg_hw* s_hw;
static unsigned char* s_standard;
static size_t s_standard_size;
static unsigned int s_width, s_height;
static int s_hw_ready = -1; /* -1 untried, 0 unavailable, 1 ready */

/* THP stores an ordinary baseline JPEG whose entropy-coded scan has already
 * been unescaped, which an ordinary decoder reads as a marker: it stops early
 * and returns a mostly
 * neutral frame.  Restore JPEG byte stuffing (0xff -> 0xff 0x00) between the
 * SOS header and the final EOI marker before handing the frame to a decoder.
 *
 * MvOpen.mth does not use JPEG restart intervals.  Reject one if it appears
 * rather than accidentally converting a restart marker into entropy data. */
static int make_standard_jpeg(const unsigned char* source,
                              size_t source_size,
                              unsigned char* destination,
                              size_t destination_capacity,
                              size_t* destination_size)
{
    if (source == NULL || destination == NULL || destination_size == NULL ||
        source_size < 4u || source[0] != 0xffu || source[1] != 0xd8u)
        return 0;

    size_t position = 2u;
    size_t scan_start = 0u;
    int has_restart_interval = 0;
    while (position + 1u < source_size) {
        if (source[position] != 0xffu) return 0;
        while (position < source_size && source[position] == 0xffu)
            ++position;
        if (position >= source_size) return 0;
        const unsigned char marker = source[position++];
        if (marker == 0xd8u) continue;
        if (marker == 0xd9u) return 0;
        if (marker >= 0xd0u && marker <= 0xd7u) continue;
        if (marker == 0x01u) continue;
        if (position + 2u > source_size) return 0;
        const size_t segment_size =
            ((size_t) source[position] << 8u) | source[position + 1u];
        if (segment_size < 2u || segment_size > source_size - position)
            return 0;
        if (marker == 0xddu) has_restart_interval = 1;
        position += segment_size;
        if (marker == 0xdau) {
            scan_start = position;
            break;
        }
    }
    if (scan_start == 0u || has_restart_interval) return 0;

    size_t eoi = source_size;
    for (size_t i = source_size - 1u; i > scan_start; --i) {
        if (source[i - 1u] == 0xffu && source[i] == 0xd9u) {
            eoi = i - 1u;
            break;
        }
    }
    if (eoi == source_size) return 0;

    if (scan_start > destination_capacity) return 0;
    memcpy(destination, source, scan_start);
    size_t output = scan_start;
    for (size_t i = scan_start; i < eoi; ++i) {
        if (output + 2u > destination_capacity) return 0;
        destination[output++] = source[i];
        if (source[i] == 0xffu) destination[output++] = 0x00u;
    }
    if (destination_capacity - output < 2u) return 0;
    destination[output++] = 0xffu;
    destination[output++] = 0xd9u;
    *destination_size = output;
    return 1;
}

/* GX I8 textures are stored as 8x4 tiles in raster tile order. */
static void tile_plane(const unsigned char* source, unsigned int pitch,
                       unsigned int width, unsigned int height,
                       unsigned char* destination)
{
    const unsigned int tiles_x = (width + 7u) / 8u;
    const unsigned int tiles_y = (height + 3u) / 4u;
    unsigned int ty, tx, row;
    for (ty = 0; ty < tiles_y; ++ty) {
        for (tx = 0; tx < tiles_x; ++tx) {
            for (row = 0; row < 4u; ++row) {
                const unsigned int y = ty * 4u + row;
                const unsigned char* in = source + (size_t) y * pitch + tx * 8u;
                unsigned char* out = destination + ((size_t) (ty * tiles_x + tx) * 32u) + row * 8u;
                if (y < height) memcpy(out, in, 8u);
                else memset(out, 0, 8u);
            }
        }
    }
}

static int decode_with_codec(const void* jpeg, size_t jpeg_size, void* tile_y,
                             void* tile_u, void* tile_v)
{
    const unsigned char* planes = NULL;
    unsigned int pitch_width = 0u, pitch_height = 0u;
    size_t standard_size = 0u;
    if (s_hw_ready == 0) return 0;
    if (s_standard == NULL) {
        s_standard = malloc(THP_MAX_JPEG);
        s_standard_size = s_standard != NULL ? THP_MAX_JPEG : 0u;
        if (s_standard == NULL) { s_hw_ready = 0; return 0; }
    }
    if (!make_standard_jpeg(jpeg, jpeg_size, s_standard, s_standard_size, &standard_size)) {
        if (s_hw_ready == -1) melee_vita_log_info("[THP] frame is not a plain JPEG; using the portable decoder");
        s_hw_ready = 0;
        return 0;
    }
    if (s_hw == NULL) {
        struct melee_vita_opening_jpeg_hw_info info;
        struct melee_vita_opening_jpeg_hw_error error;
        s_hw = melee_vita_opening_jpeg_hw_create(s_standard, standard_size, THP_MAX_JPEG,
                                                 false, &info, &error);
        if (s_hw == NULL) {
            melee_vita_log_info("[THP] JPEG codec unavailable (stage %d, code 0x%08x)",
                                (int) error.stage, (unsigned) error.code);
            s_hw_ready = 0;
            return 0;
        }
        s_width = info.decoded_width;
        s_height = info.decoded_height;
        melee_vita_log_info("[THP] JPEG codec ready: %ux%u", s_width, s_height);
    }
    if (!melee_vita_jpeg_hw_decode_planes(s_hw, s_standard, standard_size, &planes,
                                          &pitch_width, &pitch_height))
        return 0;
    if (pitch_width < s_width || pitch_height < s_height) return 0;
    /* 4:2:0: a full-size luma plane followed by half-size Cb and Cr. */
    tile_plane(planes, pitch_width, s_width, s_height, tile_y);
    {
        const unsigned char* cb = planes + (size_t) pitch_width * pitch_height;
        const unsigned char* cr = cb + (size_t) (pitch_width / 2u) * (pitch_height / 2u);
        tile_plane(cb, pitch_width / 2u, s_width / 2u, s_height / 2u, tile_u);
        tile_plane(cr, pitch_width / 2u, s_width / 2u, s_height / 2u, tile_v);
    }
    s_hw_ready = 1;
    return 1;
}

void pc_thp_decode_frame(const void* jpeg, void* tile_y, void* tile_u,
                         void* tile_v)
{
    static unsigned int failure_count;
    static unsigned int success_count;
    static SceUInt64 total;
    static unsigned int frames;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    s32 result;
    /* THP frames carry their own size; the codec only needs an upper bound. */
    if (decode_with_codec(jpeg, THP_MAX_JPEG, tile_y, tile_u, tile_v)) {
        result = 0;
    } else {
        result = THPVideoDecode(jpeg, tile_y, tile_u, tile_v, NULL);
    }
    total += sceKernelGetProcessTimeWide() - start;
    if ((++frames % 60u) == 0u) {
        melee_vita_log_info("[THP] decode avg=%.1fms (%s)", total / 60.0 / 1000.0,
                            s_hw_ready == 1 ? "codec" : "portable");
        total = 0;
    }
    if (result != 0) {
        if (failure_count++ < 8) melee_vita_log_info("[THP] decode failed: %d", result);
        return;
    }
    if (success_count++ == 0) melee_vita_log_info("[THP] first video frame decoded");
    melee_vita_gxm_mark_texture_data_dirty();
}
