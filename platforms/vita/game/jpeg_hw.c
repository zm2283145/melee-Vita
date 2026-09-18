#include "jpeg_hw.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * VitaSDK's current psp2/jpeg.h declares sceJpegDecodeMJpegYCbCr with an
 * incorrect argument order and omits sceJpegDecodeMJpeg.  Keep the small ABI
 * surface used here local and match the Sony 3.57 SDK declaration exactly.
 * Do not include psp2/jpeg.h in this translation unit.
 */
struct vita_jpeg_init_param_abi {
    uint32_t size;
    int32_t max_split_decoder;
    int32_t option;
};

struct vita_jpeg_pitch_abi {
    uint32_t x;
    uint32_t y;
};

struct vita_jpeg_output_info_abi {
    int32_t color_space;
    uint16_t image_width;
    uint16_t image_height;
    uint32_t output_buffer_size;
    uint32_t temp_buffer_size;
    uint32_t coefficient_buffer_size;
    struct vita_jpeg_pitch_abi pitch[4];
};

_Static_assert(sizeof(struct vita_jpeg_init_param_abi) == 0x0cu,
               "SceJpegMJpegInitParam ABI size changed");
_Static_assert(sizeof(struct vita_jpeg_output_info_abi) == 0x34u,
               "SceJpegOutputInfo ABI size changed");

extern int sceJpegInitMJpegWithParam(
    const struct vita_jpeg_init_param_abi* parameters);
extern int sceJpegFinishMJpeg(void);
extern int sceJpegGetOutputInfo(
    const uint8_t* jpeg, uint32_t jpeg_size, int32_t output_format,
    int32_t decode_mode, struct vita_jpeg_output_info_abi* output);
extern int sceJpegDecodeMJpegYCbCr(
    const uint8_t* jpeg, uint32_t jpeg_size, uint8_t* yuv,
    uint32_t yuv_size, int32_t decode_mode, void* coefficient_buffer,
    uint32_t coefficient_buffer_size);
extern int sceJpegMJpegCsc(
    void* rgba, const uint8_t* yuv, int32_t packed_width_height,
    int32_t output_pitch_pixels, int32_t pixel_format, int32_t sampling);

#define JPEG_MEMORY_ALIGNMENT 256u
#define JPEG_CDRAM_BLOCK_ALIGNMENT (256u * 1024u)
#define JPEG_INIT_OPTION_NONE 0
#define JPEG_DECODE_WITH_DHT 0
#define JPEG_DOWNSCALE_ONE_HALF (1 << 4)
#define JPEG_NO_CSC_OUTPUT (-1)
#define JPEG_PIXEL_RGBA8888 0
#define JPEG_COLOR_SPACE_MASK UINT32_C(0xffff0000)
#define JPEG_COLOR_SPACE_YCBCR UINT32_C(0x00020000)
#define JPEG_SAMPLING_MASK UINT32_C(0x0000ffff)
#define JPEG_SAMPLING_H2V2 UINT32_C(0x00000202)

struct melee_vita_opening_jpeg_hw {
    SceUID memory_block;
    uint8_t* memory;
    uint8_t* stream;
    uint8_t* yuv;
    void* coefficients;
    uint32_t stream_capacity;
    uint32_t yuv_capacity;
    uint32_t coefficient_capacity;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t decoded_width;
    uint32_t decoded_height;
    uint32_t color_space;
    int32_t decode_mode;
    bool jpeg_initialized;
};

static void clear_error(struct melee_vita_opening_jpeg_hw_error* error)
{
    if (error == NULL) return;
    error->stage = MELEE_VITA_OPENING_JPEG_HW_STAGE_NONE;
    error->code = 0;
}

static void set_error(struct melee_vita_opening_jpeg_hw_error* error,
                      enum melee_vita_opening_jpeg_hw_stage stage,
                      int32_t code)
{
    if (error == NULL) return;
    error->stage = stage;
    error->code = code;
}

static int align_size(size_t value, size_t alignment, size_t* result)
{
    if (result == NULL || alignment == 0u ||
        (alignment & (alignment - 1u)) != 0u ||
        value > SIZE_MAX - (alignment - 1u))
        return 0;
    *result = (value + alignment - 1u) & ~(alignment - 1u);
    return 1;
}

static int allocate_cdram(const char* name, size_t requested_size,
                          SceUID* block_out, uint8_t** memory_out,
                          enum melee_vita_opening_jpeg_hw_stage* stage_out)
{
    if (stage_out != NULL)
        *stage_out = MELEE_VITA_OPENING_JPEG_HW_STAGE_ALLOCATE;
    size_t block_size = 0u;
    if (block_out == NULL || memory_out == NULL ||
        !align_size(requested_size, JPEG_CDRAM_BLOCK_ALIGNMENT,
                    &block_size) ||
        block_size == 0u || block_size > UINT32_MAX)
        return MELEE_VITA_OPENING_JPEG_HW_ERROR_SIZE_OVERFLOW;

    const SceUID block = sceKernelAllocMemBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
        (SceSize) block_size, NULL);
    if (block < 0) return block;

    void* memory = NULL;
    const int result = sceKernelGetMemBlockBase(block, &memory);
    if (result < 0 || memory == NULL) {
        sceKernelFreeMemBlock(block);
        if (stage_out != NULL)
            *stage_out = MELEE_VITA_OPENING_JPEG_HW_STAGE_MAP;
        return result < 0 ? result
                          : MELEE_VITA_OPENING_JPEG_HW_ERROR_INVALID_ARGUMENT;
    }

    *block_out = block;
    *memory_out = memory;
    return 0;
}

static void fill_info(const struct melee_vita_opening_jpeg_hw* decoder,
                      struct melee_vita_opening_jpeg_hw_info* info)
{
    if (decoder == NULL || info == NULL) return;
    info->source_width = decoder->source_width;
    info->source_height = decoder->source_height;
    info->decoded_width = decoder->decoded_width;
    info->decoded_height = decoder->decoded_height;
    info->color_space = decoder->color_space;
    info->stream_capacity = decoder->stream_capacity;
    info->yuv_capacity = decoder->yuv_capacity;
    info->coefficient_capacity = decoder->coefficient_capacity;
}

struct melee_vita_opening_jpeg_hw* melee_vita_opening_jpeg_hw_create(
    const void* probe_jpeg, size_t probe_jpeg_size,
    size_t maximum_jpeg_size, bool half_scale,
    struct melee_vita_opening_jpeg_hw_info* info_out,
    struct melee_vita_opening_jpeg_hw_error* error_out)
{
    clear_error(error_out);
    if (info_out != NULL) memset(info_out, 0, sizeof(*info_out));
    if (probe_jpeg == NULL || probe_jpeg_size == 0u ||
        maximum_jpeg_size < probe_jpeg_size ||
        maximum_jpeg_size > UINT32_MAX) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_ARGUMENT,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    size_t stream_capacity = 0u;
    if (!align_size(maximum_jpeg_size, JPEG_MEMORY_ALIGNMENT,
                    &stream_capacity) ||
        stream_capacity > UINT32_MAX) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_ARGUMENT,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_SIZE_OVERFLOW);
        return NULL;
    }

    struct melee_vita_opening_jpeg_hw* decoder =
        calloc(1u, sizeof(*decoder));
    if (decoder == NULL) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_ALLOCATE,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_OUT_OF_MEMORY);
        return NULL;
    }
    decoder->memory_block = -1;

    const struct vita_jpeg_init_param_abi init = {
        .size = sizeof(init),
        .max_split_decoder = 0,
        .option = JPEG_INIT_OPTION_NONE,
    };
    int result = sceJpegInitMJpegWithParam(&init);
    if (result < 0) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_INITIALIZE,
                  result);
        free(decoder);
        return NULL;
    }
    decoder->jpeg_initialized = true;

    SceUID probe_block = -1;
    uint8_t* probe_memory = NULL;
    enum melee_vita_opening_jpeg_hw_stage allocation_stage =
        MELEE_VITA_OPENING_JPEG_HW_STAGE_ALLOCATE;
    result = allocate_cdram("melee jpeg probe", stream_capacity,
                            &probe_block, &probe_memory,
                            &allocation_stage);
    if (result < 0) {
        set_error(error_out, allocation_stage, result);
        melee_vita_opening_jpeg_hw_destroy(decoder);
        return NULL;
    }
    memcpy(probe_memory, probe_jpeg, probe_jpeg_size);

    struct vita_jpeg_output_info_abi output = { 0 };
    result = sceJpegGetOutputInfo(
        probe_memory, (uint32_t) probe_jpeg_size, JPEG_NO_CSC_OUTPUT,
        JPEG_DECODE_WITH_DHT, &output);
    sceKernelFreeMemBlock(probe_block);
    if (result < 0) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_PROBE,
                  result);
        melee_vita_opening_jpeg_hw_destroy(decoder);
        return NULL;
    }

    const uint32_t color_space = (uint32_t) output.color_space;
    if ((color_space & JPEG_COLOR_SPACE_MASK) != JPEG_COLOR_SPACE_YCBCR ||
        (color_space & JPEG_SAMPLING_MASK) != JPEG_SAMPLING_H2V2 ||
        output.image_width == 0u || output.image_height == 0u ||
        output.output_buffer_size == 0u) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_FORMAT,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_UNSUPPORTED_FORMAT);
        melee_vita_opening_jpeg_hw_destroy(decoder);
        return NULL;
    }

    size_t yuv_capacity = 0u;
    size_t coefficient_capacity = 0u;
    if (!align_size(output.output_buffer_size, JPEG_MEMORY_ALIGNMENT,
                    &yuv_capacity) ||
        !align_size(output.coefficient_buffer_size,
                    JPEG_MEMORY_ALIGNMENT, &coefficient_capacity) ||
        yuv_capacity > UINT32_MAX || coefficient_capacity > UINT32_MAX ||
        stream_capacity > SIZE_MAX - yuv_capacity ||
        stream_capacity + yuv_capacity > SIZE_MAX - coefficient_capacity) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_ALLOCATE,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_SIZE_OVERFLOW);
        melee_vita_opening_jpeg_hw_destroy(decoder);
        return NULL;
    }

    const size_t decoder_bytes =
        stream_capacity + yuv_capacity + coefficient_capacity;
    allocation_stage = MELEE_VITA_OPENING_JPEG_HW_STAGE_ALLOCATE;
    result = allocate_cdram("melee jpeg decode", decoder_bytes,
                            &decoder->memory_block, &decoder->memory,
                            &allocation_stage);
    if (result < 0) {
        set_error(error_out, allocation_stage, result);
        melee_vita_opening_jpeg_hw_destroy(decoder);
        return NULL;
    }

    decoder->stream = decoder->memory;
    decoder->yuv = decoder->stream + stream_capacity;
    decoder->coefficients = coefficient_capacity == 0u
        ? NULL : decoder->yuv + yuv_capacity;
    decoder->stream_capacity = (uint32_t) stream_capacity;
    decoder->yuv_capacity = (uint32_t) yuv_capacity;
    decoder->coefficient_capacity = (uint32_t) coefficient_capacity;
    decoder->source_width = output.image_width;
    decoder->source_height = output.image_height;
    decoder->decoded_width = half_scale
        ? (output.image_width + 1u) / 2u : output.image_width;
    decoder->decoded_height = half_scale
        ? (output.image_height + 1u) / 2u : output.image_height;
    decoder->color_space = color_space;
    decoder->decode_mode = half_scale
        ? JPEG_DECODE_WITH_DHT | JPEG_DOWNSCALE_ONE_HALF
        : JPEG_DECODE_WITH_DHT;
    memcpy(decoder->stream, probe_jpeg, probe_jpeg_size);

    fill_info(decoder, info_out);
    return decoder;
}

int melee_vita_opening_jpeg_hw_decode(
    struct melee_vita_opening_jpeg_hw* decoder,
    const void* standard_jpeg, size_t standard_jpeg_size,
    vita2d_texture* target,
    struct melee_vita_opening_jpeg_hw_timing* timing_out,
    struct melee_vita_opening_jpeg_hw_error* error_out)
{
    clear_error(error_out);
    if (timing_out != NULL) memset(timing_out, 0, sizeof(*timing_out));
    if (decoder == NULL || standard_jpeg == NULL ||
        standard_jpeg_size == 0u || target == NULL) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_ARGUMENT,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (standard_jpeg_size > decoder->stream_capacity) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_ARGUMENT,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_INPUT_TOO_LARGE);
        return 0;
    }

    if (vita2d_texture_get_format(target) !=
        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_TEXTURE,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_TEXTURE_FORMAT);
        return 0;
    }
    const uint32_t texture_width = vita2d_texture_get_width(target);
    const uint32_t texture_height = vita2d_texture_get_height(target);
    const uint32_t texture_stride = vita2d_texture_get_stride(target);
    if (texture_width < decoder->decoded_width ||
        texture_height < decoder->decoded_height ||
        texture_stride == 0u || (texture_stride & 3u) != 0u ||
        texture_stride / 4u > INT32_MAX ||
        vita2d_texture_get_datap(target) == NULL) {
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_TEXTURE,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_TEXTURE_SIZE);
        return 0;
    }

    struct melee_vita_opening_jpeg_hw_timing timing = { 0 };
    const uint64_t total_started = sceKernelGetProcessTimeWide();
    uint64_t started = total_started;
    memcpy(decoder->stream, standard_jpeg, standard_jpeg_size);
    timing.copy_us = sceKernelGetProcessTimeWide() - started;

    started = sceKernelGetProcessTimeWide();
    const int decoded = sceJpegDecodeMJpegYCbCr(
        decoder->stream, (uint32_t) standard_jpeg_size,
        decoder->yuv, decoder->yuv_capacity, decoder->decode_mode,
        decoder->coefficients, decoder->coefficient_capacity);
    timing.decode_us = sceKernelGetProcessTimeWide() - started;
    if (decoded < 0) {
        timing.total_us = sceKernelGetProcessTimeWide() - total_started;
        if (timing_out != NULL) *timing_out = timing;
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_DECODE,
                  decoded);
        return 0;
    }

    timing.pitch_width = (uint32_t) decoded >> 16u;
    timing.pitch_height = (uint32_t) decoded & UINT32_C(0xffff);
    const uint32_t texture_pitch_pixels = texture_stride / 4u;
    if (timing.pitch_width == 0u || timing.pitch_height == 0u ||
        timing.pitch_width > texture_pitch_pixels ||
        timing.pitch_height > texture_height) {
        timing.total_us = sceKernelGetProcessTimeWide() - total_started;
        if (timing_out != NULL) *timing_out = timing;
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_TEXTURE,
                  MELEE_VITA_OPENING_JPEG_HW_ERROR_TEXTURE_SIZE);
        return 0;
    }

    started = sceKernelGetProcessTimeWide();
    const int csc_result = sceJpegMJpegCsc(
        vita2d_texture_get_datap(target), decoder->yuv, decoded,
        (int32_t) texture_pitch_pixels, JPEG_PIXEL_RGBA8888,
        (int32_t) (decoder->color_space & JPEG_SAMPLING_MASK));
    timing.csc_us = sceKernelGetProcessTimeWide() - started;
    timing.total_us = sceKernelGetProcessTimeWide() - total_started;
    if (csc_result < 0) {
        if (timing_out != NULL) *timing_out = timing;
        set_error(error_out, MELEE_VITA_OPENING_JPEG_HW_STAGE_CSC,
                  csc_result);
        return 0;
    }

    __sync_synchronize();
    if (timing_out != NULL) *timing_out = timing;
    return 1;
}

/* Decode one bridged JPEG and hand back the codec's YCbCr output.  The game
 * build needs the planes rather than an RGBA image: HSD uploads Y, Cb and Cr
 * as separate I8 textures and combines them in the TEV. */
int melee_vita_jpeg_hw_decode_planes(
    struct melee_vita_opening_jpeg_hw* decoder,
    const void* standard_jpeg, size_t standard_jpeg_size,
    const unsigned char** planes_out, unsigned int* pitch_width_out,
    unsigned int* pitch_height_out)
{
    int decoded;
    if (decoder == NULL || standard_jpeg == NULL || standard_jpeg_size == 0u ||
        planes_out == NULL || pitch_width_out == NULL || pitch_height_out == NULL)
        return 0;
    if (standard_jpeg_size > decoder->stream_capacity) return 0;
    memcpy(decoder->stream, standard_jpeg, standard_jpeg_size);
    decoded = sceJpegDecodeMJpegYCbCr(
        decoder->stream, (uint32_t) standard_jpeg_size,
        decoder->yuv, decoder->yuv_capacity, decoder->decode_mode,
        decoder->coefficients, decoder->coefficient_capacity);
    if (decoded < 0) return 0;
    *planes_out = decoder->yuv;
    *pitch_width_out = (uint32_t) decoded >> 16u;
    *pitch_height_out = (uint32_t) decoded & UINT32_C(0xffff);
    return 1;
}

void melee_vita_opening_jpeg_hw_destroy(
    struct melee_vita_opening_jpeg_hw* decoder)
{
    if (decoder == NULL) return;
    if (decoder->jpeg_initialized) {
        sceJpegFinishMJpeg();
        decoder->jpeg_initialized = false;
    }
    if (decoder->memory_block >= 0) {
        sceKernelFreeMemBlock(decoder->memory_block);
        decoder->memory_block = -1;
    }
    free(decoder);
}
