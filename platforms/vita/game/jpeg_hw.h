#ifndef MELEE_VITA_OPENING_JPEG_HW_H
#define MELEE_VITA_OPENING_JPEG_HW_H

#include <vita2d.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct melee_vita_opening_jpeg_hw;

enum melee_vita_opening_jpeg_hw_stage {
    MELEE_VITA_OPENING_JPEG_HW_STAGE_NONE = 0,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_ARGUMENT,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_INITIALIZE,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_ALLOCATE,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_MAP,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_PROBE,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_FORMAT,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_TEXTURE,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_DECODE,
    MELEE_VITA_OPENING_JPEG_HW_STAGE_CSC,
};

enum melee_vita_opening_jpeg_hw_local_error {
    MELEE_VITA_OPENING_JPEG_HW_ERROR_INVALID_ARGUMENT = -1,
    MELEE_VITA_OPENING_JPEG_HW_ERROR_SIZE_OVERFLOW = -2,
    MELEE_VITA_OPENING_JPEG_HW_ERROR_UNSUPPORTED_FORMAT = -3,
    MELEE_VITA_OPENING_JPEG_HW_ERROR_INPUT_TOO_LARGE = -4,
    MELEE_VITA_OPENING_JPEG_HW_ERROR_TEXTURE_FORMAT = -5,
    MELEE_VITA_OPENING_JPEG_HW_ERROR_TEXTURE_SIZE = -6,
    MELEE_VITA_OPENING_JPEG_HW_ERROR_OUT_OF_MEMORY = -7,
};

struct melee_vita_opening_jpeg_hw_error {
    enum melee_vita_opening_jpeg_hw_stage stage;
    int32_t code;
};

struct melee_vita_opening_jpeg_hw_info {
    uint32_t source_width;
    uint32_t source_height;
    uint32_t decoded_width;
    uint32_t decoded_height;
    uint32_t color_space;
    uint32_t stream_capacity;
    uint32_t yuv_capacity;
    uint32_t coefficient_capacity;
};

struct melee_vita_opening_jpeg_hw_timing {
    uint64_t copy_us;
    uint64_t decode_us;
    uint64_t csc_us;
    uint64_t total_us;
    uint32_t pitch_width;
    uint32_t pitch_height;
};

/*
 * Initializes the Vita JPEG codec and probes one already-bridged, standard
 * JPEG frame.  maximum_jpeg_size must cover every bridged frame that will be
 * passed to decode; the backend copies those frames into physically
 * contiguous CDRAM before invoking the codec engine.
 *
 * The backend supports the opening movie's YCbCr 4:2:0 frames at full size or,
 * when half_scale is set, at 1/2 scale.
 */
struct melee_vita_opening_jpeg_hw* melee_vita_opening_jpeg_hw_create(
    const void* probe_jpeg, size_t probe_jpeg_size,
    size_t maximum_jpeg_size, bool half_scale,
    struct melee_vita_opening_jpeg_hw_info* info_out,
    struct melee_vita_opening_jpeg_hw_error* error_out);

/*
 * Decodes one already-bridged JPEG and performs the hardware YCbCr-to-RGBA
 * conversion directly into an inactive Vita2D A8B8G8R8 texture.
 */
int melee_vita_opening_jpeg_hw_decode(
    struct melee_vita_opening_jpeg_hw* decoder,
    const void* standard_jpeg, size_t standard_jpeg_size,
    vita2d_texture* target,
    struct melee_vita_opening_jpeg_hw_timing* timing_out,
    struct melee_vita_opening_jpeg_hw_error* error_out);

extern int melee_vita_jpeg_hw_last_error;

int melee_vita_jpeg_hw_decode_planes(
    struct melee_vita_opening_jpeg_hw* decoder,
    const void* standard_jpeg, size_t standard_jpeg_size,
    const unsigned char** planes_out, unsigned int* pitch_width_out,
    unsigned int* pitch_height_out);

void melee_vita_opening_jpeg_hw_destroy(
    struct melee_vita_opening_jpeg_hw* decoder);

#endif
