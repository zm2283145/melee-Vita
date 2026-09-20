/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * THP video frames on Vita.
 *
 * The portable decoder (aurora's THPVideoDecode) needs ~130 ms per frame here,
 * four times the movie's budget, so frames go through the Vita's JPEG codec
 * instead: the THP scan is restored to a standard JPEG byte stream, the codec
 * decodes it into planar YCbCr, and the planes are written into the GX I8 tile
 * layout the game uploads as textures.  The portable decoder stays as the
 * fallback until the codec has proved itself on a frame.
 *
 * Even through the codec a frame costs far more than the 16 ms the main thread
 * has, so the work runs on its own thread, one frame behind the game: each call
 * collects the frame the worker finished, hands it the new one, and returns.
 * The main thread is then left with two memcpys instead of a whole decode, and
 * the movie's own catch-up logic (lbmthp) keeps audio and video together by
 * dropping frames that arrive late.
 */
#include "gxm_game.h"
#include "jpeg_hw.h"
#include "../vita_log.h"

#include <dolphin/thp.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdlib.h>
#include <string.h>

#define THP_MAX_JPEG (512u * 1024u)
#define THP_DECODE_PRIORITY 0x10000100
#define THP_DECODE_STACK (128u * 1024u)

static struct melee_vita_opening_jpeg_hw* s_hw;
static unsigned char* s_standard;
static size_t s_standard_size;
static unsigned int s_width, s_height;
static int s_hw_ready = -1; /* -1 untried, 0 unavailable, 1 ready */

/* Timing and frame accounting, over a 60 frame window. */
static SceUInt64 s_main_us, s_worker_us, s_stage_us, s_bridge_us, s_decode_us,
    s_tile_us, s_copy_us;
static unsigned int s_codec_frames, s_fallback_frames, s_lost_frames;

/* Worker state. */
static SceUID s_thread = -1;
static SceUID s_work_sema = -1;
static SceUID s_done_sema = -1;
static int s_pipeline = -1; /* -1 untried, 0 unavailable, 1 running */
static int s_pending;       /* the worker holds a frame */
static unsigned char* s_staged;
static size_t s_staged_size;
static unsigned char* s_hold_y;
static unsigned char* s_hold_u;
static unsigned char* s_hold_v;
static size_t s_hold_y_size, s_hold_uv_size;
static int s_worker_result;

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

    /* The scan has to stop at the picture's end-of-image marker: the codec
     * rejects a stream that carries anything past the last MCU, so the window
     * cannot simply be stuffed wholesale. */
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
    /* The codec writes its output to CDRAM, where the 8-byte reads a tile needs
     * are painfully slow.  Each band of four lines is pulled across in whole
     * rows first, then tiled out of that cached copy. */
    static unsigned char band[4u * 1024u];
    const unsigned int tiles_x = (width + 7u) / 8u;
    const unsigned int tiles_y = (height + 3u) / 4u;
    unsigned int ty, tx, row;
    if (pitch > sizeof(band) / 4u) return;
    for (ty = 0; ty < tiles_y; ++ty) {
        const unsigned int rows = (ty * 4u + 4u <= height) ? 4u : height - ty * 4u;
        for (row = 0; row < rows; ++row)
            memcpy(band + row * pitch, source + (size_t) (ty * 4u + row) * pitch, pitch);
        for (tx = 0; tx < tiles_x; ++tx) {
            unsigned char* out = destination + ((size_t) (ty * tiles_x + tx) * 32u);
            for (row = 0; row < 4u; ++row) {
                if (row < rows) memcpy(out + row * 8u, band + row * pitch + tx * 8u, 8u);
                else memset(out + row * 8u, 0, 8u);
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
    {
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        const int bridged = make_standard_jpeg(jpeg, jpeg_size, s_standard,
                                               s_standard_size, &standard_size);
        s_bridge_us += sceKernelGetProcessTimeWide() - t0;
        if (!bridged) {
            /* One frame the bridge will not take is a lost frame, not a reason
             * to fall back to a decoder that costs 130 ms a frame. */
            static unsigned int bridge_failures;
            if (bridge_failures++ < 4u)
                melee_vita_log_info("[THP] bridge failed: %u bytes",
                                    (unsigned) jpeg_size);
            return 0;
        }
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
    {
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        const int ok = melee_vita_jpeg_hw_decode_planes(s_hw, s_standard, standard_size, &planes,
                                                        &pitch_width, &pitch_height);
        s_decode_us += sceKernelGetProcessTimeWide() - t0;
        if (!ok) {
            static unsigned int decode_failures;
            if (decode_failures++ < 8u)
                melee_vita_log_info("[THP] codec rejected frame: code 0x%08x, "
                                    "%u raw -> %u bridged, scan tail %02x %02x",
                                    (unsigned) melee_vita_jpeg_hw_last_error,
                                    (unsigned) jpeg_size, (unsigned) standard_size,
                                    ((const unsigned char*) jpeg)[jpeg_size - 2u],
                                    ((const unsigned char*) jpeg)[jpeg_size - 1u]);
            return 0;
        }
    }
    if (pitch_width < s_width || pitch_height < s_height) return 0;
    /* 4:2:0: a full-size luma plane followed by half-size Cb and Cr. */
    {
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        const unsigned char* cb = planes + (size_t) pitch_width * pitch_height;
        const unsigned char* cr = cb + (size_t) (pitch_width / 2u) * (pitch_height / 2u);
        tile_plane(planes, pitch_width, s_width, s_height, tile_y);
        tile_plane(cb, pitch_width / 2u, s_width / 2u, s_height / 2u, tile_u);
        tile_plane(cr, pitch_width / 2u, s_width / 2u, s_height / 2u, tile_v);
        s_tile_us += sceKernelGetProcessTimeWide() - t0;
    }
    s_hw_ready = 1;
    return 1;
}

/* ---- Worker. ---- */

/* The tiled planes the worker fills, sized once the codec reports the movie's
 * dimensions.  Tiling pads to whole 8x4 tiles, which for these sizes is exactly
 * the plane size. */
static int ensure_hold_buffers(void)
{
    const size_t y_size = (size_t) s_width * s_height;
    const size_t uv_size = (size_t) (s_width / 2u) * (s_height / 2u);
    if (s_width == 0u || s_height == 0u) return 0;
    if (s_hold_y != NULL && s_hold_y_size == y_size) return 1;
    free(s_hold_y);
    free(s_hold_u);
    free(s_hold_v);
    s_hold_y = malloc(y_size);
    s_hold_u = malloc(uv_size);
    s_hold_v = malloc(uv_size);
    if (s_hold_y == NULL || s_hold_u == NULL || s_hold_v == NULL) {
        free(s_hold_y);
        free(s_hold_u);
        free(s_hold_v);
        s_hold_y = s_hold_u = s_hold_v = NULL;
        s_hold_y_size = s_hold_uv_size = 0u;
        return 0;
    }
    s_hold_y_size = y_size;
    s_hold_uv_size = uv_size;
    return 1;
}

static int decode_thread(SceSize argument_size, void* argument)
{
    (void) argument_size;
    (void) argument;
    for (;;) {
        SceUInt64 begin;
        sceKernelWaitSema(s_work_sema, 1, NULL);
        begin = sceKernelGetProcessTimeWide();
        s_worker_result = decode_with_codec(s_staged, s_staged_size, s_hold_y,
                                            s_hold_u, s_hold_v);
        s_worker_us += sceKernelGetProcessTimeWide() - begin;
        sceKernelSignalSema(s_done_sema, 1);
    }
    return 0;
}

static int pipeline_start(void)
{
    if (s_pipeline >= 0) return s_pipeline;
    s_pipeline = 0;
    s_staged = malloc(THP_MAX_JPEG);
    if (s_staged == NULL) return 0;
    s_work_sema = sceKernelCreateSema("melee_thp_work", 0, 0, 1, NULL);
    s_done_sema = sceKernelCreateSema("melee_thp_done", 0, 0, 1, NULL);
    if (s_work_sema < 0 || s_done_sema < 0) return 0;
    s_thread = sceKernelCreateThread("melee_thp", decode_thread,
                                     THP_DECODE_PRIORITY, THP_DECODE_STACK, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_thread < 0) return 0;
    if (sceKernelStartThread(s_thread, 0, NULL) < 0) return 0;
    s_pipeline = 1;
    melee_vita_log_info("[THP] decode thread started");
    return 1;
}

/* The codec needs one frame to report the movie's dimensions, so the very
 * first frame creates it here, on the main thread, before the pipeline takes
 * over. */
static int prime_codec(const void* jpeg, size_t jpeg_size)
{
    if (s_hw != NULL) return s_hold_y != NULL;
    if (s_hw_ready == 0) return 0;
    {
        size_t standard_size = 0u;
        if (s_standard == NULL) {
            s_standard = malloc(THP_MAX_JPEG);
            s_standard_size = s_standard != NULL ? THP_MAX_JPEG : 0u;
            if (s_standard == NULL) { s_hw_ready = 0; return 0; }
        }
        if (!make_standard_jpeg(jpeg, jpeg_size, s_standard, s_standard_size,
                                &standard_size))
            return 0;
        {
            struct melee_vita_opening_jpeg_hw_info info;
            struct melee_vita_opening_jpeg_hw_error error;
            s_hw = melee_vita_opening_jpeg_hw_create(s_standard, standard_size,
                                                     THP_MAX_JPEG, false, &info,
                                                     &error);
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
    }
    return ensure_hold_buffers();
}

/* Where the picture ends.
 *
 * A frame does not carry its own size: the four byte header at the start of a
 * ring slot is the size of the frame loaded into the NEXT slot, and by the time
 * a frame is decoded the slot behind it has already been refilled, so the size
 * cannot be recovered from the ring either.  lbMthp does know it at the moment
 * it issues the read, and reports it here against the slot it is reading into.
 *
 * Getting this right matters: a 48 KiB frame holds around 330 FF bytes, so a
 * stray FF D9 turns up about once per frame by chance, and searching for the
 * end-of-image marker picked the wrong one about half the time. */
#define THP_WINDOW_MAX (128u * 1024u)
#define THP_SIZE_SLOTS 16u

static struct {
    const void* buffer;
    unsigned int size;
} s_sizes[THP_SIZE_SLOTS];
static unsigned int s_size_cursor;
static SceUInt64 s_window_bytes;
static unsigned int s_exact_frames, s_scanned_frames;

void pc_thp_note_frame_size(const void* buffer, unsigned int packed_size)
{
    unsigned int i;
    for (i = 0; i < THP_SIZE_SLOTS; ++i) {
        if (s_sizes[i].buffer == buffer) {
            s_sizes[i].size = packed_size;
            return;
        }
    }
    s_sizes[s_size_cursor].buffer = buffer;
    s_sizes[s_size_cursor].size = packed_size;
    s_size_cursor = (s_size_cursor + 1u) % THP_SIZE_SLOTS;
}

static unsigned int recorded_frame_size(const void* buffer)
{
    unsigned int i;
    for (i = 0; i < THP_SIZE_SLOTS; ++i)
        if (s_sizes[i].buffer == buffer) return s_sizes[i].size;
    return 0u;
}

static size_t frame_window(const void* jpeg)
{
    const unsigned char* bytes = jpeg;
    const unsigned char* header = bytes - 4;
    const size_t announced = (size_t) header[0] << 24 | (size_t) header[1] << 16 |
                             (size_t) header[2] << 8 | header[3];
    const size_t packed = recorded_frame_size(header);
    size_t scanned = 0u;
    size_t i;

    /* The packed size covers the four byte header as well as the picture. */
    if (packed > 8u && packed < THP_WINDOW_MAX) {
        const size_t payload = packed - 4u;
        if (bytes[payload - 2u] == 0xffu && bytes[payload - 1u] == 0xd9u) {
            s_exact_frames++;
            s_window_bytes += payload;
            return payload;
        }
    }

    if (announced > 4u && announced < THP_WINDOW_MAX) {
        for (i = announced > 64u ? announced - 64u : 1u;
             i + 1u < announced + 16384u; ++i) {
            if (bytes[i] == 0xffu && bytes[i + 1u] == 0xd9u) {
                scanned = i + 2u;
                break;
            }
        }
    }

    {
        static unsigned int reports;
        if (reports++ < 6u)
            melee_vita_log_info("[THP] size: packed=%u announced=%u scanned=%u",
                                (unsigned) packed, (unsigned) announced,
                                (unsigned) scanned);
    }

    if (scanned == 0u || scanned > THP_MAX_JPEG) return 0u;
    s_scanned_frames++;
    s_window_bytes += scanned;
    return scanned;
}
static void report_timing(void)
{
    static unsigned int frames;
    if ((++frames % 60u) != 0u) return;
    melee_vita_log_info(
        "[THP] main=%.1fms worker=%.1fms (stage=%.1f bridge=%.1f decode=%.1f "
        "tile=%.1f copy=%.1f) ok=%u lost=%u portable=%u sized=%u/%u",
        s_main_us / 60.0 / 1000.0, s_worker_us / 60.0 / 1000.0,
        s_stage_us / 60.0 / 1000.0, s_bridge_us / 60.0 / 1000.0,
        s_decode_us / 60.0 / 1000.0, s_tile_us / 60.0 / 1000.0,
        s_copy_us / 60.0 / 1000.0, s_codec_frames, s_lost_frames,
        s_fallback_frames, s_exact_frames, s_scanned_frames);
    s_main_us = s_worker_us = s_stage_us = s_bridge_us = s_decode_us = 0;
    s_tile_us = s_copy_us = 0;
    s_codec_frames = s_fallback_frames = s_lost_frames = 0;
    s_window_bytes = 0;
    s_exact_frames = s_scanned_frames = 0;
}

void pc_thp_decode_frame(const void* jpeg, void* tile_y, void* tile_u,
                         void* tile_v)
{
    static unsigned int failure_count;
    static unsigned int success_count;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    const size_t payload = frame_window(jpeg);
    int produced = 0;

    /* Once the codec is carrying the movie, a frame it cannot measure is
     * dropped rather than handed to the portable decoder: 130 ms on the main
     * thread costs more in stutter and lost audio sync than a repeat does. */
    if (payload == 0u && s_hw_ready == 1) {
        s_lost_frames++;
        s_main_us += sceKernelGetProcessTimeWide() - start;
        report_timing();
        return;
    }

    if (payload == 0u || s_hw_ready == 0 || !prime_codec(jpeg, payload) ||
        !pipeline_start()) {
        const s32 result = THPVideoDecode(jpeg, tile_y, tile_u, tile_v, NULL);
        s_fallback_frames++;
        s_main_us += sceKernelGetProcessTimeWide() - start;
        report_timing();
        if (result != 0) {
            if (failure_count++ < 8)
                melee_vita_log_info("[THP] decode failed: %d", result);
            return;
        }
        melee_vita_gxm_mark_texture_data_dirty();
        return;
    }

    /* Collect the frame handed over last time. */
    if (s_pending) {
        sceKernelWaitSema(s_done_sema, 1, NULL);
        s_pending = 0;
        if (s_worker_result) {
            const SceUInt64 t0 = sceKernelGetProcessTimeWide();
            memcpy(tile_y, s_hold_y, s_hold_y_size);
            memcpy(tile_u, s_hold_u, s_hold_uv_size);
            memcpy(tile_v, s_hold_v, s_hold_uv_size);
            s_copy_us += sceKernelGetProcessTimeWide() - t0;
            produced = 1;
            s_codec_frames++;
        } else {
            s_lost_frames++;
            if (failure_count++ < 8)
                melee_vita_log_info("[THP] worker produced no frame (%u bytes)",
                                    (unsigned) s_staged_size);
        }
    }

    /* Hand over the new one. */
    {
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        memcpy(s_staged, jpeg, payload);
        s_staged_size = payload;
        s_stage_us += sceKernelGetProcessTimeWide() - t0;
    }
    s_pending = 1;
    sceKernelSignalSema(s_work_sema, 1);

    s_main_us += sceKernelGetProcessTimeWide() - start;
    report_timing();
    if (produced) {
        if (success_count++ == 0)
            melee_vita_log_info("[THP] first video frame decoded");
        melee_vita_gxm_mark_texture_data_dirty();
    }
}

void pc_thp_decode_frame_sync(const void* jpeg, void* tile_y, void* tile_u,
                              void* tile_v)
{
    const size_t payload = frame_window(jpeg);
    int result = 0;

    if (s_pending) {
        sceKernelWaitSema(s_done_sema, 1, NULL);
        s_pending = 0;
    }

    if (payload != 0u && s_hw_ready != 0 &&
        prime_codec(jpeg, payload))
    {
        result = decode_with_codec(jpeg, payload, tile_y, tile_u, tile_v);
    }
    if (!result) {
        result = THPVideoDecode(jpeg, tile_y, tile_u, tile_v, NULL) == 0;
    }
    if (result) {
        melee_vita_log_info("[THP] decoded one-shot frame synchronously");
        melee_vita_gxm_mark_texture_data_dirty();
    } else {
        melee_vita_log_info("[THP] one-shot frame decode failed");
    }
}
