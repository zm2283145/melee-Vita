/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GXM-backed frame sink used by the original game's GX compatibility layer. */
#include "gxm_game.h"
#include "copy_texture_lifetime.h"
#include "heap.h"
#include "../texture_decoder.h"
#include "gx_render.h"
#include "../vita_log.h"

#include <dolphin/gx/GXEnum.h>
#include <psp2/gxm.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef MELEE_VITA_INTERNAL_WIDTH
#define MELEE_VITA_INTERNAL_WIDTH MELEE_VITA_DISPLAY_WIDTH
#endif
#ifndef MELEE_VITA_INTERNAL_HEIGHT
#define MELEE_VITA_INTERNAL_HEIGHT MELEE_VITA_DISPLAY_HEIGHT
#endif
#ifndef MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH
#define MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH MELEE_VITA_INTERNAL_WIDTH
#endif
#ifndef MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT
#define MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT MELEE_VITA_INTERNAL_HEIGHT
#endif
#if MELEE_VITA_INTERNAL_WIDTH < 320 || MELEE_VITA_INTERNAL_WIDTH > 960 || \
    (MELEE_VITA_INTERNAL_WIDTH % 16) != 0
#error "MELEE_VITA_INTERNAL_WIDTH must be 320..960 and 16-pixel aligned"
#endif
#if MELEE_VITA_INTERNAL_HEIGHT < 240 || MELEE_VITA_INTERNAL_HEIGHT > 544 || \
    (MELEE_VITA_INTERNAL_HEIGHT % 8) != 0
#error "MELEE_VITA_INTERNAL_HEIGHT must be 240..544 and 8-pixel aligned"
#endif
#if MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH < 320 || \
    MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH > 960 || \
    (MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH % 16) != 0
#error "MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH must be 320..960 and 16-pixel aligned"
#endif
#if MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT < 240 || \
    MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT > 544 || \
    (MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT % 8) != 0
#error "MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT must be 240..544 and 8-pixel aligned"
#endif
#ifndef MELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION
#if MELEE_VITA_INTERNAL_WIDTH == 720 && MELEE_VITA_INTERNAL_HEIGHT == 408
#define MELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_75
#elif MELEE_VITA_INTERNAL_WIDTH == 576 && MELEE_VITA_INTERNAL_HEIGHT == 328
#define MELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_60
#elif MELEE_VITA_INTERNAL_WIDTH == 480 && MELEE_VITA_INTERNAL_HEIGHT == 272
#define MELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_50
#else
#define MELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_NATIVE
#endif
#endif
#ifndef MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION
#if MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH == 720 && \
    MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT == 408
#define MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_75
#elif MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH == 576 && \
    MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT == 328
#define MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_60
#elif MELEE_VITA_GAMEPLAY_INTERNAL_WIDTH == 480 && \
    MELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT == 272
#define MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_50
#else
#define MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION \
    MELEE_VITA_RESOLUTION_NATIVE
#endif
#endif

static int s_initialized;
static int s_texture_invalidation_pending;
static u32 s_texture_content_generation = 1;
static void* s_main_color_data;
static u32 s_main_color_stride;
static void* s_main_depth_data;
typedef struct MeleeVitaResolutionTarget {
    vita2d_texture* texture;
    SceGxmSyncObject* sync;
} MeleeVitaResolutionTarget;
static MeleeVitaResolutionTarget
    s_resolution_targets[MELEE_VITA_RESOLUTION_OPTION_COUNT];
#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
int g_melee_vita_menu_resolution_option =
    MELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION;
int g_melee_vita_gameplay_resolution_option =
    MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION;
#endif
static int s_menu_resolution_option =
    MELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION;
static int s_gameplay_resolution_option =
    MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION;
static int s_active_resolution_option = MELEE_VITA_RESOLUTION_NATIVE;
static vita2d_texture* s_active_internal_target;
static bool s_rendering_internal;
static u32 s_render_width = MELEE_VITA_DISPLAY_WIDTH;
static u32 s_render_height = MELEE_VITA_DISPLAY_HEIGHT;

#define VITA_MAIN_SCENES_PER_FRAME 8u

int __real_sceGxmCreateRenderTarget(
    const SceGxmRenderTargetParams* params,
    SceGxmRenderTarget** render_target);

int __wrap_sceGxmCreateRenderTarget(
    const SceGxmRenderTargetParams* params,
    SceGxmRenderTarget** render_target)
{
    SceGxmRenderTargetParams adjusted;

    if (params == NULL) {
        return __real_sceGxmCreateRenderTarget(params, render_target);
    }
    adjusted = *params;
    if (((adjusted.width == 960u && adjusted.height == 544u) ||
         (adjusted.width == 720u && adjusted.height == 408u) ||
         (adjusted.width == 576u && adjusted.height == 328u) ||
         (adjusted.width == 480u && adjusted.height == 272u)) &&
        adjusted.scenesPerFrame < VITA_MAIN_SCENES_PER_FRAME) {
        /* EFB copies end and resume the display scene. libvita2d declares one
         * scene per frame, but GXM requires this field to cover every resume. */
        adjusted.scenesPerFrame = VITA_MAIN_SCENES_PER_FRAME;
    }
    return __real_sceGxmCreateRenderTarget(&adjusted, render_target);
}

int __real_sceGxmBeginScene(
    SceGxmContext* context, unsigned int flags,
    const SceGxmRenderTarget* render_target,
    const SceGxmValidRegion* valid_region,
    SceGxmSyncObject* vertex_sync_object,
    SceGxmSyncObject* fragment_sync_object,
    const SceGxmColorSurface* color_surface,
    const SceGxmDepthStencilSurface* depth_stencil);

int __wrap_sceGxmBeginScene(
    SceGxmContext* context, unsigned int flags,
    const SceGxmRenderTarget* render_target,
    const SceGxmValidRegion* valid_region,
    SceGxmSyncObject* vertex_sync_object,
    SceGxmSyncObject* fragment_sync_object,
    const SceGxmColorSurface* color_surface,
    const SceGxmDepthStencilSurface* depth_stencil)
{
    SceGxmSyncObject* fragment_sync = fragment_sync_object;
    if (fragment_sync == NULL) {
        int option;
        for (option = MELEE_VITA_RESOLUTION_75;
             option < MELEE_VITA_RESOLUTION_OPTION_COUNT; ++option) {
            MeleeVitaResolutionTarget* target =
                &s_resolution_targets[option];
            if (target->texture != NULL &&
                render_target == target->texture->gxm_rtgt) {
                fragment_sync = target->sync;
                break;
            }
        }
    }
    const int result = __real_sceGxmBeginScene(
        context, flags, render_target, valid_region, vertex_sync_object,
        fragment_sync, color_surface, depth_stencil);
    if (result >= 0 && color_surface != NULL && depth_stencil != NULL) {
        s_main_color_data = sceGxmColorSurfaceGetData(color_surface);
        s_main_color_stride =
            sceGxmColorSurfaceGetStrideInPixels(color_surface);
        s_main_depth_data = depth_stencil->depthData;
    }
    return result;
}

typedef struct VitaTextureCacheEntry {
    MeleeVitaTextureSource source;
    vita2d_texture* texture;
    u32 content_generation;
    u32 sample_hash;
    u32 last_used_frame;
    u32 hash_frame;
    struct VitaTextureCacheEntry* next;
} VitaTextureCacheEntry;

static VitaTextureCacheEntry* s_textures;
/* Direct-mapped lookup in front of the entry list: a match draws hundreds of
 * textured primitives per frame and a list walk per draw was measurable. */
/* GXCopyTex destinations: the copied picture lives in a GPU texture keyed by
 * the destination buffer address (as aurora does), so sampling that buffer
 * binds the copy directly instead of decoding CPU memory. */
#define VITA_COPY_TEXTURES 16u
static struct {
    MeleeVitaCopyTextureBinding binding;
    vita2d_texture* texture;
    u32 width, height, frame;
} s_copy_textures[VITA_COPY_TEXTURES];
static void retire_texture(struct vita2d_texture* texture);

#define VITA_TEXTURE_HASH_SIZE 1024u
static VitaTextureCacheEntry* s_texture_hash[VITA_TEXTURE_HASH_SIZE];
static inline u32 texture_hash_slot(const MeleeVitaTextureSource* source)
{
    const uintptr_t key = (uintptr_t) source->data;
    return (u32) ((key >> 5) ^ (key >> 15) ^ source->format) & (VITA_TEXTURE_HASH_SIZE - 1u);
}
static u32 s_frame_counter;
static u32 s_texture_uploads;
static u32 s_texture_failures;

/* Archives are frequently reloaded at the same address with different
 * contents, so a cheap content sample is part of cache validation. */
static u32 texture_sample_hash(const MeleeVitaTextureSource* source)
{
    const u8* data = source->data;
    u32 hash = 2166136261u;
    /* Sample ~256 bytes spread over the whole (compressed) image so mutable
     * textures such as movie planes are noticed without hashing everything. */
    const u32 bytes = (u32) source->width * source->height / 2u;
    const u32 step = bytes > 256u ? bytes / 256u : 1u;
    u32 i;
    if (data == NULL) return 0;
    for (i = 0; i < bytes; i += step) { hash ^= data[i]; hash *= 16777619u; }
    if (source->palette != NULL) {
        const u8* palette = source->palette;
        for (i = 0; i < 32u; ++i) { hash ^= palette[i]; hash *= 16777619u; }
    }
    return hash;
}

static SceGxmTextureAddrMode address_mode(u32 mode)
{
    if (mode == 1u) return SCE_GXM_TEXTURE_ADDR_REPEAT;
    /* GXM rejects ADDR_MIRROR (0x805b0009) for these textures; the shader
     * folds mirrored coordinates into [0, 1], which must then be clamped so
     * the 1.0 edge does not wrap back to the opposite texel. */
    if (mode == 2u) return SCE_GXM_TEXTURE_ADDR_CLAMP;
    return SCE_GXM_TEXTURE_ADDR_CLAMP;
}

static void free_textures(void)
{
    VitaTextureCacheEntry* entry = s_textures;
    while (entry != NULL) {
        VitaTextureCacheEntry* next = entry->next;
        if (entry->texture != NULL) vita2d_free_texture(entry->texture);
        free(entry);
        entry = next;
    }
    s_textures = NULL;
    memset(s_texture_hash, 0, sizeof(s_texture_hash));
}

static void rq_wait_idle(void);
void melee_vita_gxm_mark_texture_data_dirty(void)
{
    if (++s_texture_content_generation == 0) s_texture_content_generation = 1;
}

static int same_texture(const MeleeVitaTextureSource* a,
                        const MeleeVitaTextureSource* b)
{
    return a->key == b->key && a->data == b->data &&
           a->width == b->width && a->height == b->height &&
           a->format == b->format && a->wrap_s == b->wrap_s &&
           a->wrap_t == b->wrap_t &&
           a->min_filter == b->min_filter &&
           a->mag_filter == b->mag_filter &&
           a->palette == b->palette &&
           a->palette_format == b->palette_format &&
           a->palette_entries == b->palette_entries &&
           a->chroma_u == b->chroma_u && a->chroma_v == b->chroma_v &&
           a->chroma_width == b->chroma_width &&
           a->chroma_height == b->chroma_height;
}

static u8 tiled_i8_sample(const u8* data, u32 width, u32 x, u32 y)
{
    const u32 tiles_per_row = (width + 7u) / 8u;
    const u32 tile = (y / 4u) * tiles_per_row + x / 8u;
    return data[tile * 32u + (y & 3u) * 8u + (x & 7u)];
}

static u8 clamp_color(s32 value)
{
    return value < 0 ? 0 : value > 255 ? 255 : (u8) value;
}

static s32 s_yuv_luma[256];
static s32 s_yuv_red[256];
static s32 s_yuv_green_u[256];
static s32 s_yuv_green_v[256];
static s32 s_yuv_blue[256];
static int s_yuv_tables_ready;

static void initialize_yuv_tables(void)
{
    u32 value;
    if (s_yuv_tables_ready) return;
    for (value = 0; value < 256; ++value) {
        const s32 chroma = (s32) value - 128;
        const s32 luma = value > 16 ? (s32) value - 16 : 0;
        s_yuv_luma[value] = 298 * luma + 128;
        s_yuv_red[value] = 409 * chroma;
        s_yuv_green_u[value] = -100 * chroma;
        s_yuv_green_v[value] = -208 * chroma;
        s_yuv_blue[value] = 516 * chroma;
    }
    s_yuv_tables_ready = 1;
}

static u32 yuv_pixel(u8 luma, u8 chroma_u, u8 chroma_v)
{
    const s32 y = s_yuv_luma[luma];
    const u8 r = clamp_color((y + s_yuv_red[chroma_v]) >> 8);
    const u8 g = clamp_color(
        (y + s_yuv_green_u[chroma_u] + s_yuv_green_v[chroma_v]) >> 8);
    const u8 b = clamp_color((y + s_yuv_blue[chroma_u]) >> 8);
    return (u32) r | (u32) g << 8 | (u32) b << 16 | 0xff000000u;
}

static int decode_yuv420(const MeleeVitaTextureSource* source,
                         u8* destination, u32 stride)
{
    const u8* y_plane = source->data;
    const u8* u_plane = source->chroma_u;
    const u8* v_plane = source->chroma_v;
    u32 y;
    if (y_plane == NULL || u_plane == NULL || v_plane == NULL ||
        source->chroma_width == 0 || source->chroma_height == 0) return -1;
    initialize_yuv_tables();
    for (y = 0; y < source->height; ++y) {
        u32 x;
        u32* output = (u32*) (destination + (size_t) y * stride);
        for (x = 0; x < source->width; x += 2u) {
            const u8 chroma_u = tiled_i8_sample(
                u_plane, source->chroma_width, x >> 1, y >> 1);
            const u8 chroma_v = tiled_i8_sample(
                v_plane, source->chroma_width, x >> 1, y >> 1);
            output[x] = yuv_pixel(
                tiled_i8_sample(y_plane, source->width, x, y),
                chroma_u, chroma_v);
            if (x + 1u < source->width) {
                output[x + 1u] = yuv_pixel(
                    tiled_i8_sample(y_plane, source->width, x + 1u, y),
                    chroma_u, chroma_v);
            }
        }
    }
    return 0;
}

static int refresh_texture_impl(VitaTextureCacheEntry* entry,
                                const MeleeVitaTextureSource* source);
void melee_vita_prof_add(int zone, u64 us);
static int refresh_texture(VitaTextureCacheEntry* entry,
                           const MeleeVitaTextureSource* source)
{
    const u64 t0 = sceKernelGetProcessTimeWide();
    const int r = refresh_texture_impl(entry, source);
    melee_vita_prof_add(4 /* VPZ_TEXUPLOAD */, sceKernelGetProcessTimeWide() - t0);
    return r;
}

static int refresh_texture_impl(VitaTextureCacheEntry* entry,
                                const MeleeVitaTextureSource* source)
{
    u32* pixels;
    u8* destination;
    u32 stride;
    u32 y;
    destination = vita2d_texture_get_datap(entry->texture);
    stride = vita2d_texture_get_stride(entry->texture);
    if (source->chroma_u != NULL) {
        if (decode_yuv420(source, destination, stride) != 0) return -1;
        entry->content_generation = s_texture_content_generation;
        return 0;
    }
    pixels = malloc((size_t) source->width * source->height * sizeof(*pixels));
    if (pixels == NULL || melee_vita_decode_texture_raw(
            source->data, source->width, source->height, source->format,
            source->palette, source->palette_format, source->palette_entries,
            pixels, (u32) source->width * source->height) != 0) {
        free(pixels);
        return -1;
    }
    for (y = 0; y < source->height; ++y)
        memcpy(destination + (size_t) y * stride,
               pixels + (size_t) y * source->width,
               (size_t) source->width * sizeof(*pixels));
#ifdef MELEE_VITA_RENDER_TRACE
    {
        u32 min_r = 255u, min_g = 255u, min_b = 255u, min_a = 255u;
        u32 max_r = 0u, max_g = 0u, max_b = 0u, max_a = 0u;
        u32 alpha_zero = 0u, alpha_full = 0u;
        const u32 count = (u32) source->width * source->height;
        for (u32 i = 0; i < count; ++i) {
            const u32 pixel = pixels[i];
            const u32 r = pixel & 0xffu;
            const u32 g = pixel >> 8 & 0xffu;
            const u32 b = pixel >> 16 & 0xffu;
            const u32 a = pixel >> 24;
            if (r < min_r) min_r = r;
            if (g < min_g) min_g = g;
            if (b < min_b) min_b = b;
            if (a < min_a) min_a = a;
            if (r > max_r) max_r = r;
            if (g > max_g) max_g = g;
            if (b > max_b) max_b = b;
            if (a > max_a) max_a = a;
            if (a == 0u) ++alpha_zero;
            if (a == 255u) ++alpha_full;
        }
        melee_vita_log_info(
            "[TEXSTAT] data=%p fmt=%u size=%ux%u pal=%p palfmt=%u "
            "entries=%u rgba=%u-%u,%u-%u,%u-%u,%u-%u a0=%u a255=%u",
            source->data, source->format, source->width, source->height,
            source->palette, source->palette_format, source->palette_entries,
            min_r, max_r, min_g, max_g, min_b, max_b, min_a, max_a,
            alpha_zero, alpha_full);
    }
#endif
    free(pixels);
    entry->content_generation = s_texture_content_generation;
    return 0;
}

static vita2d_texture* get_texture(const MeleeVitaTextureSource* source)
{
    VitaTextureCacheEntry* entry;
    u32 sample;
    if (source == NULL || source->data == NULL || source->width == 0 ||
        source->height == 0) return NULL;
    for (u32 c = 0; c < VITA_COPY_TEXTURES; ++c)
        if (s_copy_textures[c].binding.key == source->data &&
            s_copy_textures[c].texture != NULL) {
            const u32 generation =
                melee_vita_heap_allocation_generation(source->data);
            if (melee_vita_copy_texture_binding_matches(
                    &s_copy_textures[c].binding, source->data, generation))
                return s_copy_textures[c].texture;
#ifdef MELEE_VITA_RENDER_TRACE
            melee_vita_log_info(
                "[COPYREUSE] key=%p owner=%u current=%u copy=%ux%u "
                "sample=%ux%u fmt=%u",
                source->data,
                s_copy_textures[c].binding.allocation_generation, generation,
                s_copy_textures[c].width, s_copy_textures[c].height,
                source->width, source->height, source->format);
#endif
            retire_texture(s_copy_textures[c].texture);
            s_copy_textures[c].texture = NULL;
            melee_vita_copy_texture_bind(
                &s_copy_textures[c].binding, NULL, 0);
        }
    /* Only formats the decoder understands are hashed/uploaded; copy
     * textures and other special formats (e.g. 0x11) point at buffers that
     * may not be readable for width*height/2 bytes. */
    switch (source->format) {
    case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6:
    case 0x8: case 0x9: case 0xa: case 0xe:
        break;
    default:
        return NULL;
    }
    entry = s_texture_hash[texture_hash_slot(source)];
    if (entry == NULL || !same_texture(&entry->source, source)) {
        for (entry = s_textures; entry != NULL; entry = entry->next)
            if (same_texture(&entry->source, source)) break;
        if (entry != NULL) s_texture_hash[texture_hash_slot(source)] = entry;
    }
    if (entry != NULL) {
        {
            entry->last_used_frame = s_frame_counter;
            /* Content sampling once per frame per texture is enough to see
             * mutable textures change between frames. */
            if (entry->hash_frame == s_frame_counter &&
                entry->content_generation == s_texture_content_generation)
                return entry->texture;
            entry->hash_frame = s_frame_counter;
            sample = texture_sample_hash(source);
            if (entry->content_generation != s_texture_content_generation ||
                entry->sample_hash != sample) {
#ifdef MELEE_VITA_RENDER_TRACE
                melee_vita_log_info(
                    "[TEXREFRESH] data=%p fmt=%u size=%ux%u pal=%p "
                    "old=%08x new=%08x generation=%u,%u",
                    source->data, source->format, source->width, source->height,
                    source->palette, entry->sample_hash, sample,
                    entry->content_generation, s_texture_content_generation);
#endif
                ++s_texture_uploads;
                entry->sample_hash = sample;
                if (refresh_texture(entry, source) != 0) { ++s_texture_failures; return NULL; }
            }
            return entry->texture;
        }
    }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) return NULL;
    entry->source = *source;
    sample = texture_sample_hash(source);
    entry->sample_hash = sample;
    entry->hash_frame = s_frame_counter;
    entry->last_used_frame = s_frame_counter;
    entry->texture = vita2d_create_empty_texture(source->width, source->height);
    ++s_texture_uploads;
    if (entry->texture == NULL || refresh_texture(entry, source) != 0) {
        static u32 logged;
        ++s_texture_failures;
        if (logged++ < 16u)
            melee_vita_log_info("[GXR] texture decode failed fmt=%u %ux%u pal=%p",
                                (unsigned) source->format, source->width,
                                source->height, source->palette);
        if (entry->texture != NULL) vita2d_free_texture(entry->texture);
        free(entry);
        return NULL;
    }
    vita2d_texture_set_filters(entry->texture,
        source->min_filter == 0u ? SCE_GXM_TEXTURE_FILTER_POINT
                                 : SCE_GXM_TEXTURE_FILTER_LINEAR,
        source->mag_filter == 0u ? SCE_GXM_TEXTURE_FILTER_POINT
                                 : SCE_GXM_TEXTURE_FILTER_LINEAR);
    {
        const int ru = sceGxmTextureSetUAddrMode(&entry->texture->gxm_tex,
                                                 address_mode(source->wrap_s));
        const int rv = sceGxmTextureSetVAddrMode(&entry->texture->gxm_tex,
                                                 address_mode(source->wrap_t));
        if (source->wrap_s != 0u || source->wrap_t != 0u) {
            static u32 logged;
            if (logged++ < 24u)
                melee_vita_log_info("[GXR] wrap tex %ux%u fmt=%u wrap=%u,%u set=0x%08x,0x%08x",
                                    source->width, source->height,
                                    (unsigned) source->format,
                                    (unsigned) source->wrap_s,
                                    (unsigned) source->wrap_t,
                                    (unsigned) ru, (unsigned) rv);
        }
    }
    entry->next = s_textures;
    s_textures = entry;
    s_texture_hash[texture_hash_slot(source)] = entry;
    return entry->texture;
}

/* ===================================================================
 * Render thread
 *
 * The game thread records a frame as a list of commands (each an execute
 * callback plus a payload) and places vertex/index data in a per-frame GPU
 * arena.  At present time the frame is handed to a render thread on the second
 * CPU core, which issues every vita2d/GXM call, while the game thread moves on
 * to the next frame.  At most one frame is in flight: presenting waits for the
 * render thread to finish the previous one.  All GXM context use happens on the
 * render thread; the game thread only creates/uploads textures and compiles
 * shader programs.
 * =================================================================== */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>

#define RQ_GPU_ARENA_SIZE (16u * 1024u * 1024u)
#define RQ_CPU_MASK_USER_0 0x10000
#define RQ_CPU_MASK_USER_1 0x20000

typedef struct RqCommand {
    MeleeVitaRqExec exec;
    u32 size; /* payload bytes, aligned to 8 */
} RqCommand;

typedef struct RqFrame {
    u8* cmds;
    u32 size, capacity;
    u8* gpu;
    u32 gpu_used;
    SceUID gpu_uid;
    u32 clear_color;
    u32 native_reasons;
} RqFrame;

static RqFrame s_frames[2];
static u32 s_record;
static RqFrame* s_exec_frame;
static SceUID s_rq_work = -1, s_rq_done = -1, s_rq_thread = -1;
static volatile int s_rq_quit;
static u32 s_next_clear_color = 0xff000000u;
static u32 s_rq_gpu_overflow;

u32 g_melee_vita_gxm_state_epoch;
void melee_vita_prof_add(int zone, u64 us);
#define VPZ_SWAP 2
#define VPZ_TEXUPLOAD 4
#define VPZ_RT_EXEC 11

static SceGxmDepthFunc depth_function(u32 function) __attribute__((unused));
static SceGxmDepthFunc depth_function(u32 function)
{
    static const SceGxmDepthFunc functions[8] = {
        SCE_GXM_DEPTH_FUNC_NEVER, SCE_GXM_DEPTH_FUNC_LESS,
        SCE_GXM_DEPTH_FUNC_EQUAL, SCE_GXM_DEPTH_FUNC_LESS_EQUAL,
        SCE_GXM_DEPTH_FUNC_GREATER, SCE_GXM_DEPTH_FUNC_NOT_EQUAL,
        SCE_GXM_DEPTH_FUNC_GREATER_EQUAL, SCE_GXM_DEPTH_FUNC_ALWAYS,
    };
    return functions[function < 8u ? function : 7u];
}

/* ---- render-thread helpers ---- */

static void rt_default_depth(void)
{
    SceGxmContext* context = vita2d_get_context();
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
}

static void rt_set_viewport(u32 width, u32 height, bool clear_depth)
{
    const f32 half_width = (f32) width * 0.5f;
    const f32 half_height = (f32) height * 0.5f;
    sceGxmSetViewport(
        vita2d_get_context(), half_width, half_width,
        half_height, -half_height, clear_depth ? 1.0f : 0.0f,
        clear_depth ? 0.0f : 1.0f);
}

static u32 effective_native_reasons(u32 native_reasons)
{
    bool scaled_shadow_frames = false;
    bool scaled_gx_copy_frames = false;
#ifdef MELEE_VITA_SCALED_SHADOW_FRAMES
    scaled_shadow_frames = true;
#endif
#ifdef MELEE_VITA_SCALED_GX_COPY_FRAMES
    scaled_gx_copy_frames = true;
#endif
    return melee_vita_resolution_filter_native_reasons(
        native_reasons, scaled_shadow_frames, scaled_gx_copy_frames);
}

static bool resolution_target_create(int option)
{
    MeleeVitaResolutionTarget* target;
    u32 width;
    u32 height;
    int sync_result;

    if (option == MELEE_VITA_RESOLUTION_NATIVE)
        return true;
    if (!melee_vita_resolution_option_dimensions(option, &width, &height))
        return false;
    target = &s_resolution_targets[option];
    if (target->texture != NULL)
        return true;
    target->texture = vita2d_create_empty_texture_rendertarget(
        width, height, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (target->texture == NULL) {
        melee_vita_log_info(
            "[GXM] internal EFB allocation failed for %ux%u; setting unchanged",
            width, height);
        return false;
    }
    sync_result = sceGxmSyncObjectCreate(&target->sync);
    if (sync_result < 0) {
        melee_vita_log_info(
            "[GXM] internal EFB sync creation failed for %ux%u: 0x%08x",
            width, height, (u32) sync_result);
        vita2d_free_texture(target->texture);
        target->texture = NULL;
        target->sync = NULL;
        return false;
    }
    vita2d_texture_set_filters(
        target->texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
        SCE_GXM_TEXTURE_FILTER_LINEAR);
    melee_vita_log_info(
        "[GXM] internal EFB target ready %ux%u", width, height);
    return true;
}

static bool resolution_scaling_enabled(void)
{
    return s_menu_resolution_option != MELEE_VITA_RESOLUTION_NATIVE ||
           s_gameplay_resolution_option != MELEE_VITA_RESOLUTION_NATIVE;
}

static void rt_begin_scene(u32 clear_color, int clear)
{
    SceGxmContext* context;
    const u32 raw_native_reasons =
        s_exec_frame != NULL ? s_exec_frame->native_reasons : 0u;
    const u32 native_reasons =
        effective_native_reasons(raw_native_reasons);
    int option = s_menu_resolution_option;
    s_active_internal_target = NULL;
    if (melee_vita_resolution_is_gameplay_frame(raw_native_reasons))
        option = s_gameplay_resolution_option;
    if (native_reasons == 0u &&
        option > MELEE_VITA_RESOLUTION_NATIVE &&
        option < MELEE_VITA_RESOLUTION_OPTION_COUNT) {
        s_active_internal_target = s_resolution_targets[option].texture;
    }
    s_rendering_internal = s_active_internal_target != NULL;
    s_active_resolution_option = s_rendering_internal
        ? option : MELEE_VITA_RESOLUTION_NATIVE;
    if (!melee_vita_resolution_option_dimensions(
            s_active_resolution_option, &s_render_width, &s_render_height)) {
        s_active_resolution_option = MELEE_VITA_RESOLUTION_NATIVE;
        s_active_internal_target = NULL;
        s_rendering_internal = false;
        s_render_width = MELEE_VITA_DISPLAY_WIDTH;
        s_render_height = MELEE_VITA_DISPLAY_HEIGHT;
    }
    if (s_rendering_internal) {
        vita2d_pool_reset();
        vita2d_start_drawing_advanced(s_active_internal_target, 0u);
    } else {
        vita2d_start_drawing();
    }
    ++g_melee_vita_gxm_state_epoch;
    context = vita2d_get_context();
    rt_set_viewport(s_render_width, s_render_height, false);
    if (clear) {
        /* GXCopyDisp(..., GX_TRUE) clears the EFB depth buffer as well as
         * color.  Force the clear quad to the far plane so depth from the
         * previous frame cannot occlude the next one. */
        sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
        sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
        rt_set_viewport(s_render_width, s_render_height, true);
        vita2d_set_clear_color(clear_color);
        vita2d_clear_screen();
        rt_set_viewport(s_render_width, s_render_height, false);
    }
    rt_default_depth();
}

static int render_thread(SceSize args, void* argp)
{
    (void) args;
    (void) argp;
    for (;;) {
        RqFrame* frame;
        u32 offset = 0;
        u64 t0;
        sceKernelWaitSema(s_rq_work, 1, NULL);
        if (s_rq_quit) break;
        frame = s_exec_frame;
        t0 = sceKernelGetProcessTimeWide();
        rt_begin_scene(frame->clear_color, 1);
        while (offset < frame->size) {
            const RqCommand* command = (const RqCommand*) (frame->cmds + offset);
            offset += sizeof(RqCommand);
            command->exec(frame->cmds + offset);
            offset += command->size;
        }
        melee_vita_prof_add(VPZ_RT_EXEC, sceKernelGetProcessTimeWide() - t0);
        sceKernelSignalSema(s_rq_done, 1);
    }
    sceKernelSignalSema(s_rq_done, 1);
    return 0;
}

/* Blocks until the render thread is idle (no frame in flight). */
static void rq_wait_idle(void)
{
    if (s_rq_done < 0) return;
    sceKernelWaitSema(s_rq_done, 1, NULL);
    sceKernelSignalSema(s_rq_done, 1);
}

void melee_vita_gxm_wait_idle(void)
{
    if (s_initialized) rq_wait_idle();
}

/* ---- game-thread recording API ---- */

void* melee_vita_rq_push(MeleeVitaRqExec exec, u32 payload_size)
{
    RqFrame* frame = &s_frames[s_record];
    const u32 aligned = (payload_size + 7u) & ~7u;
    const u32 needed = frame->size + sizeof(RqCommand) + aligned;
    RqCommand* command;
    if (!s_initialized) return NULL;
    if (needed > frame->capacity) {
        u32 capacity = frame->capacity ? frame->capacity : 1u << 20;
        u8* grown;
        while (capacity < needed) capacity *= 2u;
        grown = realloc(frame->cmds, capacity);
        if (grown == NULL) return NULL;
        frame->cmds = grown;
        frame->capacity = capacity;
    }
    command = (RqCommand*) (frame->cmds + frame->size);
    command->exec = exec;
    command->size = aligned;
    frame->size = needed;
    return (u8*) command + sizeof(RqCommand);
}

void* melee_vita_rq_alloc_gpu(u32 size, u32 align)
{
    RqFrame* frame = &s_frames[s_record];
    u32 offset;
    if (!s_initialized || frame->gpu == NULL || size == 0) return NULL;
    if (align < 4u) align = 4u;
    offset = (frame->gpu_used + align - 1u) & ~(align - 1u);
    if (offset + size > RQ_GPU_ARENA_SIZE) {
        if (s_rq_gpu_overflow++ < 4u)
            melee_vita_log_info("[RQ] per-frame GPU arena full (%u bytes requested)", size);
        return NULL;
    }
    frame->gpu_used = offset + size;
    return frame->gpu + offset;
}

void melee_vita_gxm_begin_frame(void) {}

void melee_vita_gxm_require_full_resolution(u32 reason)
{
    RqFrame* frame;
    if (!resolution_scaling_enabled() || reason == 0u) return;
    frame = &s_frames[s_record];
    frame->native_reasons |= reason;
}

u32 melee_vita_gxm_render_width(void) { return s_render_width; }
u32 melee_vita_gxm_render_height(void) { return s_render_height; }

#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
#define VITA_RESOLUTION_CONFIG_PATH "ux0:data/melee/resolution-settings.bin"
#define VITA_RESOLUTION_CONFIG_TEMP_PATH \
    "ux0:data/melee/resolution-settings.tmp"
#define VITA_RESOLUTION_CONFIG_MAGIC 0x31525356u
#define VITA_RESOLUTION_CONFIG_VERSION 1u

typedef struct MeleeVitaResolutionConfig {
    u32 magic;
    u32 version;
    s32 menu_option;
    s32 gameplay_option;
} MeleeVitaResolutionConfig;

static void resolution_config_load(void)
{
    MeleeVitaResolutionConfig config;
    FILE* file = fopen(VITA_RESOLUTION_CONFIG_PATH, "rb");
    bool valid;
    if (file == NULL)
        return;
    valid = fread(&config, sizeof(config), 1u, file) == 1u &&
            fgetc(file) == EOF &&
            config.magic == VITA_RESOLUTION_CONFIG_MAGIC &&
            config.version == VITA_RESOLUTION_CONFIG_VERSION &&
            config.menu_option >= MELEE_VITA_RESOLUTION_NATIVE &&
            config.menu_option < MELEE_VITA_RESOLUTION_OPTION_COUNT &&
            config.gameplay_option >= MELEE_VITA_RESOLUTION_NATIVE &&
            config.gameplay_option < MELEE_VITA_RESOLUTION_OPTION_COUNT;
    fclose(file);
    if (!valid) {
        melee_vita_log_info(
            "[GXM] ignored malformed resolution settings; using build defaults");
        return;
    }
    g_melee_vita_menu_resolution_option = config.menu_option;
    g_melee_vita_gameplay_resolution_option = config.gameplay_option;
}

static void resolution_config_save(void)
{
    const MeleeVitaResolutionConfig config = {
        VITA_RESOLUTION_CONFIG_MAGIC,
        VITA_RESOLUTION_CONFIG_VERSION,
        g_melee_vita_menu_resolution_option,
        g_melee_vita_gameplay_resolution_option,
    };
    FILE* file = fopen(VITA_RESOLUTION_CONFIG_TEMP_PATH, "wb");
    bool valid = file != NULL;
    if (valid)
        valid = fwrite(&config, sizeof(config), 1u, file) == 1u;
    if (file != NULL && fclose(file) != 0)
        valid = false;
    if (valid) {
        remove(VITA_RESOLUTION_CONFIG_PATH);
        valid = rename(
                    VITA_RESOLUTION_CONFIG_TEMP_PATH,
                    VITA_RESOLUTION_CONFIG_PATH) == 0;
    }
    if (!valid) {
        remove(VITA_RESOLUTION_CONFIG_TEMP_PATH);
        melee_vita_log_info("[GXM] failed to persist resolution settings");
    }
}

bool melee_vita_gxm_apply_resolution_options(void)
{
    const int requested_menu = g_melee_vita_menu_resolution_option;
    const int requested_gameplay =
        g_melee_vita_gameplay_resolution_option;
    u32 menu_width;
    u32 menu_height;
    u32 gameplay_width;
    u32 gameplay_height;

    if (!s_initialized ||
        !melee_vita_resolution_option_dimensions(
            requested_menu, &menu_width, &menu_height) ||
        !melee_vita_resolution_option_dimensions(
            requested_gameplay, &gameplay_width, &gameplay_height)) {
        g_melee_vita_menu_resolution_option = s_menu_resolution_option;
        g_melee_vita_gameplay_resolution_option =
            s_gameplay_resolution_option;
        return false;
    }
    rq_wait_idle();
    vita2d_wait_rendering_done();
    if (!resolution_target_create(requested_menu) ||
        !resolution_target_create(requested_gameplay)) {
        g_melee_vita_menu_resolution_option = s_menu_resolution_option;
        g_melee_vita_gameplay_resolution_option =
            s_gameplay_resolution_option;
        return false;
    }
    s_menu_resolution_option = requested_menu;
    s_gameplay_resolution_option = requested_gameplay;
    resolution_config_save();
    melee_vita_log_info(
        "[GXM] live resolution menu=%ux%u gameplay=%ux%u",
        menu_width, menu_height, gameplay_width, gameplay_height);
    return true;
}
#endif

int melee_vita_gxm_init(void)
{
    int result;
    if (s_initialized) return 0;
    result = vita2d_init_advanced(8u * 1024u * 1024u);
    if (result == 0) return -1;
    vita2d_set_vblank_wait(0);
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
    resolution_config_load();
    s_menu_resolution_option = g_melee_vita_menu_resolution_option;
    s_gameplay_resolution_option =
        g_melee_vita_gameplay_resolution_option;
#endif
    if (!resolution_target_create(s_menu_resolution_option))
        s_menu_resolution_option = MELEE_VITA_RESOLUTION_NATIVE;
    if (!resolution_target_create(s_gameplay_resolution_option))
        s_gameplay_resolution_option = MELEE_VITA_RESOLUTION_NATIVE;
#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
    g_melee_vita_menu_resolution_option = s_menu_resolution_option;
    g_melee_vita_gameplay_resolution_option =
        s_gameplay_resolution_option;
#endif
    {
        u32 menu_width;
        u32 menu_height;
        u32 gameplay_width;
        u32 gameplay_height;
        melee_vita_resolution_option_dimensions(
            s_menu_resolution_option, &menu_width, &menu_height);
        melee_vita_resolution_option_dimensions(
            s_gameplay_resolution_option,
            &gameplay_width, &gameplay_height);
        melee_vita_log_info(
            "[GXM] resolution menu=%ux%u gameplay=%ux%u; "
            "presentation remains %ux%u",
            menu_width, menu_height, gameplay_width, gameplay_height,
            MELEE_VITA_DISPLAY_WIDTH, MELEE_VITA_DISPLAY_HEIGHT);
    }
    for (u32 i = 0; i < 2u; ++i) {
        void* base = NULL;
        s_frames[i].gpu_uid = sceKernelAllocMemBlock("melee_rq", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                                    RQ_GPU_ARENA_SIZE, NULL);
        if (s_frames[i].gpu_uid < 0 || sceKernelGetMemBlockBase(s_frames[i].gpu_uid, &base) < 0 ||
            sceGxmMapMemory(base, RQ_GPU_ARENA_SIZE,
                            SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
            melee_vita_log_info("[RQ] GPU arena allocation failed");
            return -1;
        }
        s_frames[i].gpu = base;
        s_frames[i].clear_color = 0xff000000u;
    }
    s_rq_work = sceKernelCreateSema("melee_rq_work", 0, 0, 1, NULL);
    s_rq_done = sceKernelCreateSema("melee_rq_done", 0, 1, 1, NULL);
    s_rq_thread = sceKernelCreateThread("melee_render", render_thread, 0x10000100,
                                        256 * 1024, 0, RQ_CPU_MASK_USER_1, NULL);
    if (s_rq_work < 0 || s_rq_done < 0 || s_rq_thread < 0 ||
        sceKernelStartThread(s_rq_thread, 0, NULL) < 0) {
        melee_vita_log_info("[RQ] render thread creation failed");
        return -1;
    }
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), RQ_CPU_MASK_USER_0);
    s_initialized = 1;
    melee_vita_log_info("[RQ] render thread started on core 1");
#ifndef MELEE_VITA_GX_LEGACY_RENDERER
    if (gxr_init() != 0)
        melee_vita_log_info("[GXR] falling back to the legacy vita2d GX path");
#endif
    return 0;
}

void melee_vita_gxm_shutdown(void)
{
    if (!s_initialized) return;
    rq_wait_idle();
    s_rq_quit = 1;
    sceKernelSignalSema(s_rq_work, 1);
    sceKernelWaitThreadEnd(s_rq_thread, NULL, NULL);
    vita2d_wait_rendering_done();
    free_textures();
    for (int option = MELEE_VITA_RESOLUTION_75;
         option < MELEE_VITA_RESOLUTION_OPTION_COUNT; ++option) {
        MeleeVitaResolutionTarget* target =
            &s_resolution_targets[option];
        if (target->texture != NULL) {
            vita2d_free_texture(target->texture);
            target->texture = NULL;
        }
        if (target->sync != NULL) {
            sceGxmSyncObjectDestroy(target->sync);
            target->sync = NULL;
        }
    }
    s_active_internal_target = NULL;
    vita2d_fini();
    s_initialized = 0;
}

/* ---- deferred texture frees (the render thread may still sample them) ---- */

#define RQ_GRAVEYARD 32u
static struct { vita2d_texture* texture; u32 frame; } s_graveyard[RQ_GRAVEYARD];

static void retire_texture(vita2d_texture* texture)
{
    u32 i;
    if (texture == NULL) return;
    for (i = 0; i < RQ_GRAVEYARD; ++i)
        if (s_graveyard[i].texture == NULL) {
            s_graveyard[i].texture = texture;
            s_graveyard[i].frame = s_frame_counter;
            return;
        }
    rq_wait_idle();
    vita2d_free_texture(texture);
}

static void collect_graveyard(void)
{
    for (u32 i = 0; i < RQ_GRAVEYARD; ++i)
        if (s_graveyard[i].texture != NULL && s_frame_counter - s_graveyard[i].frame >= 3u) {
            vita2d_free_texture(s_graveyard[i].texture);
            s_graveyard[i].texture = NULL;
        }
}

static void free_copy_textures(void)
{
    for (u32 i = 0; i < VITA_COPY_TEXTURES; ++i) {
        if (s_copy_textures[i].texture != NULL)
            vita2d_free_texture(s_copy_textures[i].texture);
    }
    memset(s_copy_textures, 0, sizeof(s_copy_textures));
    for (u32 i = 0; i < RQ_GRAVEYARD; ++i) {
        if (s_graveyard[i].texture != NULL)
            vita2d_free_texture(s_graveyard[i].texture);
    }
    memset(s_graveyard, 0, sizeof(s_graveyard));
}

void melee_vita_gxm_prepare_texture_invalidation(void)
{
#ifndef MELEE_VITA_RELEASE
    const u64 started = sceKernelGetProcessTimeWide();
#endif
    if (s_texture_invalidation_pending) return;
    s_texture_invalidation_pending = 1;
    if (s_initialized) rq_wait_idle();
#ifndef MELEE_VITA_RELEASE
    melee_vita_log_info(
        "[TRANSITION] texture queue drain=%lluus",
        (unsigned long long) (sceKernelGetProcessTimeWide() - started));
#endif
}

void melee_vita_gxm_invalidate_textures(void)
{
#ifndef MELEE_VITA_RELEASE
    u32 textures = 0;
    u32 copies = 0;
    u32 retired = 0;
    u64 gpu_done;
    const u64 started = sceKernelGetProcessTimeWide();
    for (VitaTextureCacheEntry* entry = s_textures; entry != NULL;
         entry = entry->next)
        ++textures;
    for (u32 i = 0; i < VITA_COPY_TEXTURES; ++i)
        if (s_copy_textures[i].texture != NULL) ++copies;
    for (u32 i = 0; i < RQ_GRAVEYARD; ++i)
        if (s_graveyard[i].texture != NULL) ++retired;
#endif
    if (!s_texture_invalidation_pending)
        melee_vita_gxm_prepare_texture_invalidation();
    if (s_initialized) {
        vita2d_wait_rendering_done();
    }
#ifndef MELEE_VITA_RELEASE
    gpu_done = sceKernelGetProcessTimeWide();
#endif
    free_textures();
    free_copy_textures();
    gxr_flush_warm_cache();
    s_texture_invalidation_pending = 0;
#ifndef MELEE_VITA_RELEASE
    melee_vita_log_info(
        "[TRANSITION] texture gpu-wait=%lluus free=%lluus "
        "textures=%u copies=%u retired=%u",
        (unsigned long long) (gpu_done - started),
        (unsigned long long) (sceKernelGetProcessTimeWide() - gpu_done),
        textures, copies, retired);
#endif
}

void melee_vita_gxm_log_memory(const char* phase)
{
#ifndef MELEE_VITA_RELEASE
    SceKernelFreeMemorySizeInfo memory = { 0 };
    u32 textures = 0;
    u32 copies = 0;
    u32 retired = 0;
    for (VitaTextureCacheEntry* entry = s_textures; entry != NULL;
         entry = entry->next)
        ++textures;
    for (u32 i = 0; i < VITA_COPY_TEXTURES; ++i)
        if (s_copy_textures[i].texture != NULL) ++copies;
    for (u32 i = 0; i < RQ_GRAVEYARD; ++i)
        if (s_graveyard[i].texture != NULL) ++retired;
    memory.size = sizeof(memory);
    if (sceKernelGetFreeMemorySize(&memory) < 0) {
        melee_vita_log_info("[MEM] %s query failed", phase);
        return;
    }
    melee_vita_log_info(
        "[MEM] %s free user=%uKiB cdram=%uKiB phycont=%uKiB "
        "textures=%u copies=%u retired=%u",
        phase, (unsigned) memory.size_user / 1024u,
        (unsigned) memory.size_cdram / 1024u,
        (unsigned) memory.size_phycont / 1024u, textures, copies, retired);
#else
    (void) phase;
#endif
}

/* A plain vita2d texture with a GXM render target over its own memory, so a
 * texture copy can be drawn into it on the GPU.  vita2d_free_texture destroys
 * gxm_rtgt, so the texture can be retired like any other. */
static vita2d_texture* create_copy_target(u32 width, u32 height)
{
    SceGxmRenderTargetParams params;
    vita2d_texture* texture = vita2d_create_empty_texture(width, height);
    int err;
    if (texture == NULL) {
        melee_vita_log_info("[GXCOPY] texture alloc failed %ux%u", width, height);
        return NULL;
    }
    memset(&params, 0, sizeof(params));
    params.flags = 0;
    params.width = (u16) width;
    params.height = (u16) height;
    params.scenesPerFrame = 1;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.multisampleLocations = 0;
    params.driverMemBlock = -1;
    err = sceGxmCreateRenderTarget(&params, &texture->gxm_rtgt);
    if (err < 0) {
        melee_vita_log_info("[GXCOPY] render target %ux%u failed 0x%08x", width, height, (unsigned) err);
        texture->gxm_rtgt = NULL;
        return texture;
    }
    err = sceGxmColorSurfaceInit(&texture->gxm_sfc, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                 SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                 SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, width, height,
                                 vita2d_texture_get_stride(texture) / 4u,
                                 vita2d_texture_get_datap(texture));
    if (err < 0) {
        melee_vita_log_info("[GXCOPY] color surface %ux%u failed 0x%08x", width, height, (unsigned) err);
        sceGxmDestroyRenderTarget(texture->gxm_rtgt);
        texture->gxm_rtgt = NULL;
    }
    return texture;
}

vita2d_texture* melee_vita_gxm_copy_texture(const void* key, u32 width,
                                            u32 height, bool* created)
{
    u32 c, free_slot = VITA_COPY_TEXTURES;
    if (created != NULL) *created = false;
    for (c = 0; c < VITA_COPY_TEXTURES; ++c) {
        if (s_copy_textures[c].binding.key == key) {
            const u32 generation =
                melee_vita_heap_allocation_generation(key);
            if (s_copy_textures[c].width == width &&
                s_copy_textures[c].height == height &&
                s_copy_textures[c].texture != NULL &&
                melee_vita_copy_texture_binding_matches(
                    &s_copy_textures[c].binding, key, generation)) {
                s_copy_textures[c].frame = s_frame_counter;
                return s_copy_textures[c].texture;
            }
            retire_texture(s_copy_textures[c].texture);
            s_copy_textures[c].texture = NULL;
            free_slot = c;
            break;
        }
        if (s_copy_textures[c].binding.key == NULL &&
            free_slot == VITA_COPY_TEXTURES)
            free_slot = c;
    }
    if (free_slot == VITA_COPY_TEXTURES) {
        /* Recycle the map that has gone longest without a copy, so a live one
         * (the Pokemon Stadium monitor) is never the one dropped. */
        free_slot = 0;
        for (c = 1; c < VITA_COPY_TEXTURES; ++c)
            if (s_copy_textures[c].frame < s_copy_textures[free_slot].frame) free_slot = c;
        retire_texture(s_copy_textures[free_slot].texture);
        s_copy_textures[free_slot].texture = NULL;
    }
    melee_vita_copy_texture_bind(
        &s_copy_textures[free_slot].binding, key,
        melee_vita_heap_allocation_generation(key));
    s_copy_textures[free_slot].width = width;
    s_copy_textures[free_slot].height = height;
    s_copy_textures[free_slot].frame = s_frame_counter;
    s_copy_textures[free_slot].texture = create_copy_target(width, height);
    if (s_copy_textures[free_slot].texture != NULL) {
        if (created != NULL) *created = true;
        vita2d_texture_set_filters(s_copy_textures[free_slot].texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
                                   SCE_GXM_TEXTURE_FILTER_LINEAR);
        /* Copied maps are sampled by projection (HSD shadow maps especially),
         * so anything outside the map must clamp to its edge, not repeat. */
        sceGxmTextureSetUAddrMode(&s_copy_textures[free_slot].texture->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&s_copy_textures[free_slot].texture->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    } else
        melee_vita_copy_texture_bind(
            &s_copy_textures[free_slot].binding, NULL, 0);
    return s_copy_textures[free_slot].texture;
}

vita2d_texture* melee_vita_gxm_texture(const MeleeVitaTextureSource* source)
{
    return get_texture(source);
}

/* ---- EFB copy ---- */

typedef struct RqCopy {
    vita2d_texture* target;
    GxrVertex* vertices;
    u16* indices;
    u32 width, height;
    f32 x0, y0, sx, sy;
    u32 format;
    u32 clear;
} RqCopy;

typedef struct RqCopyClear {
    f32 x, y, width, height;
} RqCopyClear;

static void* render_fb(void)
{
    return s_main_color_data;
}

static void clear_copy_region(f32 x, f32 y, f32 width, f32 height)
{
    SceGxmContext* context = vita2d_get_context();
    const f32 scale_x =
        (f32) s_render_width / (f32) MELEE_VITA_DISPLAY_WIDTH;
    const f32 scale_y =
        (f32) s_render_height / (f32) MELEE_VITA_DISPLAY_HEIGHT;
    ++g_melee_vita_gxm_state_epoch;
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
    /* zScale 0 / zOffset 1 writes the far plane, as a GX clear does. */
    rt_set_viewport(s_render_width, s_render_height, true);
    vita2d_set_blend_mode_add(0);
    vita2d_draw_rectangle(x * scale_x, y * scale_y,
                          width > 0.0f ? width * scale_x : 1.0f,
                          height > 0.0f ? height * scale_y : 1.0f,
                          s_exec_frame->clear_color);
    rt_set_viewport(s_render_width, s_render_height, false);
    rt_default_depth();
}

static void exec_copy_clear(const void* payload)
{
    const RqCopyClear* clear = payload;
    clear_copy_region(clear->x, clear->y, clear->width, clear->height);
}

static void exec_copy(const void* payload)
{
    const RqCopy* c = payload;
    SceGxmContext* context = vita2d_get_context();
    const bool depth_copy = c->format == GX_TF_Z24X8;
    const void* fb = depth_copy ? s_main_depth_data : render_fb();
    vita2d_end_drawing();
#ifdef MELEE_VITA_RENDER_TRACE
    {
        static u32 logged, window;
        if (window != s_frame_counter / 600u) { window = s_frame_counter / 600u; logged = 0; }
        if (logged++ < 4u)
            melee_vita_log_info("[GXCOPY] exec %ux%u fmt=0x%x src=%.1f,%.1f step=%.3f,%.3f rt=%p",
                                c->width, c->height, (unsigned) c->format,
                                c->x0, c->y0, c->sx, c->sy,
                                c->target ? (void*) c->target->gxm_rtgt : NULL);
    }
#endif
    if (fb != NULL && c->target != NULL && c->target->gxm_rtgt != NULL) {
        /* GPU copy: sample the partially rendered back buffer, including its
         * alpha, into the render-target texture.  Results portraits and Snag
         * a Trophy composite these captures over another scene. */
        static vita2d_texture source;
        const f32 tex_w = (f32) c->width * c->sx;
        const f32 tex_h = (f32) c->height * c->sy;
        int texture_error;
        if (depth_copy)
            texture_error = sceGxmTextureInitTiled(
                &source.gxm_tex, fb, SCE_GXM_TEXTURE_FORMAT_U24X8_DS,
                s_render_width, s_render_height, 0);
        else
            texture_error = sceGxmTextureInitLinearStrided(
                &source.gxm_tex, fb, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8,
                s_render_width, s_render_height,
                s_main_color_stride * sizeof(u32));
        if (texture_error < 0) {
            melee_vita_log_info(
                "[GXCOPY] source texture init failed fmt=0x%x error=0x%08x",
                (unsigned) c->format, (unsigned) texture_error);
        }
        sceGxmTextureSetMinFilter(
            &source.gxm_tex, depth_copy ? SCE_GXM_TEXTURE_FILTER_POINT
                                        : SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(
            &source.gxm_tex, depth_copy ? SCE_GXM_TEXTURE_FILTER_POINT
                                        : SCE_GXM_TEXTURE_FILTER_LINEAR);
        {
            const int err = sceGxmBeginScene(context, 0, c->target->gxm_rtgt, NULL, NULL, NULL,
                                             &c->target->gxm_sfc, NULL);
            static u32 logged;
            if (err < 0 && logged++ < 8u)
                melee_vita_log_info("[GXCOPY] begin scene failed 0x%08x", (unsigned) err);
        }
        sceGxmSetViewport(context, (f32) c->width * 0.5f, (f32) c->width * 0.5f,
                          (f32) c->height * 0.5f, -(f32) c->height * 0.5f, 0.0f, 1.0f);
        sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
        rt_default_depth();
        vita2d_set_blend_mode_add(0);
        if (depth_copy && texture_error >= 0) {
            if (!gxr_copy_depth(&source.gxm_tex, c->vertices, c->indices))
                melee_vita_log_info("[GXCOPY] depth copy draw unavailable");
        } else if (!depth_copy && texture_error >= 0 &&
                   tex_w > 0.0f && tex_h > 0.0f) {
            if (!gxr_copy_color(&source.gxm_tex, c->vertices, c->indices))
                melee_vita_log_info("[GXCOPY] color copy draw unavailable");
        }
        sceGxmEndScene(context, NULL, NULL);
        rt_set_viewport(s_render_width, s_render_height, false);
    }
    /* GXCopyTex(clear) clears the rectangle it copied, not the whole frame:
     * clearing everything would wipe a scene that was already drawn (the
     * shadow map copy happens part-way through the frame). */
    rt_begin_scene(s_exec_frame->clear_color, 0);
    if (c->clear) {
        const f32 w = (f32) c->width * c->sx;
        const f32 h = (f32) c->height * c->sy;
        clear_copy_region(c->x0, c->y0, w, h);
    }
}
/* ---- offscreen passes -----------------------------------------------------
 *
 * HSD renders a shadow map by drawing silhouettes into a corner of the frame
 * and copying that rectangle out.  Doing the same here made the map depend on
 * whatever else had been drawn into that corner, so those passes are given
 * their own render target instead: the drawing goes straight into the map. */

typedef struct RqTarget {
    vita2d_texture* target;
    u32 width, height;
    u32 clear_color;
    u32 begin;
} RqTarget;

static void exec_target(const void* payload)
{
    const RqTarget* t = payload;
    SceGxmContext* context = vita2d_get_context();
#ifdef MELEE_VITA_RENDER_TRACE
    {
        static u32 logged, window;
        if (window != s_frame_counter / 600u) { window = s_frame_counter / 600u; logged = 0; }
        if (logged++ < 4u)
            melee_vita_log_info("[TARGET] %s %ux%u rt=%p", t->begin ? "begin" : "end", t->width, t->height,
                                t->target ? (void*) t->target->gxm_rtgt : NULL);
    }
#endif
    if (t->begin) {
        if (t->target == NULL || t->target->gxm_rtgt == NULL) return;
        vita2d_end_drawing();
        s_render_width = t->width;
        s_render_height = t->height;
        sceGxmBeginScene(context, 0, t->target->gxm_rtgt, NULL, NULL, NULL,
                         &t->target->gxm_sfc, NULL);
        ++g_melee_vita_gxm_state_epoch;
        {
            const f32 hw = (f32) t->width * 0.5f;
            const f32 hh = (f32) t->height * 0.5f;
            sceGxmSetViewport(context, hw, hw, hh, -hh, 0.0f, 1.0f);
        }
        sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
        rt_default_depth();
        vita2d_set_blend_mode_add(0);
        /* The map starts fully lit; silhouettes darken it. */
        vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, t->clear_color);
    } else {
        sceGxmEndScene(context, NULL, NULL);
        rt_begin_scene(s_exec_frame->clear_color, 0);
    }
}

void melee_vita_gxm_begin_target(struct vita2d_texture* target, u32 width, u32 height,
                                 u32 clear_color)
{
    RqTarget* t = melee_vita_rq_push(exec_target, sizeof(RqTarget));
    if (t == NULL) return;
    t->target = target;
    t->width = width;
    t->height = height;
    t->clear_color = clear_color;
    t->begin = 1u;
}

void melee_vita_gxm_end_target(void)
{
    RqTarget* t = melee_vita_rq_push(exec_target, sizeof(RqTarget));
    if (t == NULL) return;
    t->target = NULL;
    t->width = t->height = 0u;
    t->clear_color = 0u;
    t->begin = 0u;
}

void melee_vita_gxm_queue_copy(struct vita2d_texture* target, u32 width, u32 height,
                               f32 x0, f32 y0, f32 sx, f32 sy, u32 format,
                               int clear)
{
    RqCopy* c = melee_vita_rq_push(exec_copy, sizeof(RqCopy));
    if (c == NULL) {
        melee_vita_gxm_require_full_resolution(
            MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->target = target;
    c->width = width;
    c->height = height;
    c->x0 = x0; c->y0 = y0; c->sx = sx; c->sy = sy;
    c->format = format;
    c->clear = clear ? 1u : 0u;
    if (target == NULL || width == 0u || height == 0u ||
        x0 < 0.0f || y0 < 0.0f || sx <= 0.0f || sy <= 0.0f ||
        x0 + (f32) width * sx > (f32) MELEE_VITA_DISPLAY_WIDTH ||
        y0 + (f32) height * sy > (f32) MELEE_VITA_DISPLAY_HEIGHT ||
        !gxr_available()) {
        melee_vita_gxm_require_full_resolution(
            MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY);
    } else {
        static const u16 quad_indices[6] = { 0, 1, 2, 2, 3, 0 };
        const f32 u0 = x0 / 960.0f;
        const f32 v0 = y0 / 544.0f;
        const f32 u1 = (x0 + (f32) width * sx) / 960.0f;
        const f32 v1 = (y0 + (f32) height * sy) / 544.0f;
        c->vertices = gxr_alloc_vertices(4);
        c->indices = gxr_alloc_indices(6);
        if (c->vertices != NULL && c->indices != NULL) {
            memset(c->vertices, 0, 4u * sizeof(*c->vertices));
            c->vertices[0].position[0] = -1.0f;
            c->vertices[0].position[1] = 1.0f;
            c->vertices[1].position[0] = 1.0f;
            c->vertices[1].position[1] = 1.0f;
            c->vertices[2].position[0] = 1.0f;
            c->vertices[2].position[1] = -1.0f;
            c->vertices[3].position[0] = -1.0f;
            c->vertices[3].position[1] = -1.0f;
            for (u32 i = 0; i < 4u; ++i) {
                c->vertices[i].position[3] = 1.0f;
                c->vertices[i].color[0][0] = c->vertices[i].color[0][1] =
                    c->vertices[i].color[0][2] = c->vertices[i].color[0][3] =
                    1.0f;
                c->vertices[i].color[1][0] = c->vertices[i].color[1][1] =
                    c->vertices[i].color[1][2] = c->vertices[i].color[1][3] =
                    1.0f;
            }
            c->vertices[0].tex[0][0] = u0;
            c->vertices[0].tex[0][1] = v0;
            c->vertices[1].tex[0][0] = u1;
            c->vertices[1].tex[0][1] = v0;
            c->vertices[2].tex[0][0] = u1;
            c->vertices[2].tex[0][1] = v1;
            c->vertices[3].tex[0][0] = u0;
            c->vertices[3].tex[0][1] = v1;
            memcpy(c->indices, quad_indices, sizeof(quad_indices));
        } else {
            melee_vita_gxm_require_full_resolution(
                MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY);
        }
    }
}

void melee_vita_gxm_queue_copy_clear(f32 x, f32 y, f32 width, f32 height)
{
    RqCopyClear* clear = melee_vita_rq_push(exec_copy_clear,
                                            sizeof(RqCopyClear));
    if (clear == NULL) {
        melee_vita_gxm_require_full_resolution(
            MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY);
        return;
    }
    clear->x = x;
    clear->y = y;
    clear->width = width;
    clear->height = height;
}

/* ---- legacy vita2d draws ---- */

typedef struct RqLegacy {
    MeleeVitaRenderState state;
    u8 has_state;
    SceGxmPrimitiveType primitive;
    vita2d_texture* texture;
    u32 tint;
    const void* vertices;
    u32 count;
} RqLegacy;

static void apply_render_state(const MeleeVitaRenderState* state)
{
    SceGxmContext* context = vita2d_get_context();
    SceGxmDepthFunc function = SCE_GXM_DEPTH_FUNC_ALWAYS;
    SceGxmDepthWriteMode write = SCE_GXM_DEPTH_WRITE_DISABLED;
    u32 line_width = 1;
    u32 point_size = 1;
    if (state != NULL) {
#ifdef MELEE_VITA_GX_LEGACY_DEPTH /* legacy path: depth kept off, see gx_render.c */
        if (state->depth_compare) function = depth_function(state->depth_function);
#endif
        if (state->depth_write) write = SCE_GXM_DEPTH_WRITE_ENABLED;
        line_width = state->line_width != 0 ? state->line_width : 1;
        point_size = state->point_size != 0 ? state->point_size : 1;
        vita2d_set_blend_mode_add(state->additive_blend != 0);
    } else {
        vita2d_set_blend_mode_add(0);
    }
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, function);
    sceGxmSetBackDepthFunc(context, function);
    sceGxmSetFrontDepthWriteEnable(context, write);
    sceGxmSetBackDepthWriteEnable(context, write);
    if (point_size > line_width) line_width = point_size;
    sceGxmSetFrontPointLineWidth(context, line_width);
    sceGxmSetBackPointLineWidth(context, line_width);
}

static void exec_legacy(const void* payload)
{
    const RqLegacy* d = payload;
    ++g_melee_vita_gxm_state_epoch;
    apply_render_state(d->has_state ? &d->state : NULL);
    if (d->texture != NULL)
        vita2d_draw_array_textured(d->texture, d->primitive, d->vertices, d->count, d->tint);
    else
        vita2d_draw_array(d->primitive, d->vertices, d->count);
}

static RqLegacy* push_legacy(SceGxmPrimitiveType primitive, u32 count,
                             const MeleeVitaRenderState* state)
{
    RqLegacy* d = melee_vita_rq_push(exec_legacy, sizeof(RqLegacy));
    if (d == NULL) return NULL;
    memset(d, 0, sizeof(*d));
    d->primitive = primitive;
    d->count = count;
    if (state != NULL) { d->state = *state; d->has_state = 1; }
    return d;
}

static void draw_colored(const MeleeVitaScreenVertex* vertices, u32 count,
                         SceGxmPrimitiveType primitive,
                         const MeleeVitaRenderState* state)
{
    vita2d_color_vertex* output;
    RqLegacy* d;
    u32 i;
    if (!s_initialized || vertices == NULL || count == 0) return;
    output = melee_vita_rq_alloc_gpu(count * sizeof(*output), 16);
    if (output == NULL) return;
    for (i = 0; i < count; ++i) {
        output[i].x = vertices[i].x;
        output[i].y = vertices[i].y;
        output[i].z = vertices[i].z;
        output[i].color = vertices[i].color;
    }
    d = push_legacy(primitive, count, state);
    if (d != NULL) d->vertices = output;
}

void melee_vita_gxm_draw_triangles(const MeleeVitaScreenVertex* vertices,
                                   u32 count,
                                   const MeleeVitaTextureSource* source,
                                   u32 tint,
                                   const MeleeVitaRenderState* state)
{
    vita2d_texture* texture;
    u32 i;
    if (!s_initialized || vertices == NULL || count < 3) return;
    if (source != NULL && source->chroma_u != NULL)
        melee_vita_gxm_require_full_resolution(
            MELEE_VITA_NATIVE_REASON_THP);
    texture = get_texture(source);
    if (texture != NULL) {
        vita2d_texture_vertex* output = melee_vita_rq_alloc_gpu(count * sizeof(*output), 16);
        RqLegacy* d;
        if (output == NULL) return;
        for (i = 0; i < count; ++i) {
            output[i].x = vertices[i].x;
            output[i].y = vertices[i].y;
            output[i].z = vertices[i].z;
            output[i].u = vertices[i].u;
            output[i].v = vertices[i].v;
        }
        d = push_legacy(SCE_GXM_PRIMITIVE_TRIANGLES, count, state);
        if (d != NULL) { d->vertices = output; d->texture = texture; d->tint = tint; }
        return;
    }
    draw_colored(vertices, count, SCE_GXM_PRIMITIVE_TRIANGLES, state);
}

void melee_vita_gxm_draw_lines(const MeleeVitaScreenVertex* vertices,
                               u32 count,
                               const MeleeVitaRenderState* state)
{
    draw_colored(vertices, count, SCE_GXM_PRIMITIVE_LINES, state);
}

void melee_vita_gxm_draw_points(const MeleeVitaScreenVertex* vertices,
                                u32 count,
                                const MeleeVitaRenderState* state)
{
    draw_colored(vertices, count, SCE_GXM_PRIMITIVE_POINTS, state);
}

typedef struct RqOverlay {
    vita2d_texture* texture;
    u32 width;
    u32 height;
    bool fill_width;
} RqOverlay;

static void exec_overlay(const void* payload)
{
    const RqOverlay* overlay = payload;
    const f32 width = overlay->fill_width
        ? 960.0f : 544.0f * (73.0f / 60.0f);
    const f32 scale_y = 544.0f / (f32) overlay->height;
    const f32 scale_x = width / (f32) overlay->width;
    ++g_melee_vita_gxm_state_epoch;
    rt_default_depth();
    vita2d_set_blend_mode_add(0);
    vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f,
                          RGBA8(0, 0, 0, 255));
    if (overlay->texture != NULL) {
        vita2d_draw_texture_part_scale(
            overlay->texture, (960.0f - width) * 0.5f, 0.0f,
            0.0f, 0.0f, (f32) overlay->width, (f32) overlay->height,
            scale_x, scale_y);
    }
}

static void queue_overlay(vita2d_texture* texture, u32 width, u32 height,
                          bool fill_width)
{
    RqOverlay* overlay;
    if (!s_initialized || width == 0u || height == 0u)
        return;
    melee_vita_gxm_require_full_resolution(MELEE_VITA_NATIVE_REASON_THP);
    overlay = melee_vita_rq_push(exec_overlay, sizeof(*overlay));
    if (overlay == NULL) return;
    overlay->texture = texture;
    overlay->width = width;
    overlay->height = height;
    overlay->fill_width = fill_width;
}

void melee_vita_gxm_queue_overlay(vita2d_texture* texture,
                                  u32 width, u32 height)
{
    extern int melee_vita_widescreen_active(void);
    queue_overlay(texture, width, height,
                  melee_vita_widescreen_active() != 0);
}

void melee_vita_gxm_queue_overlay_full_width(vita2d_texture* texture,
                                             u32 width, u32 height)
{
    queue_overlay(texture, width, height, true);
}

/* ---- present ---- */

typedef struct RqPresent {
    f32 bar;
    GxrVertex* vertices;
    u16* indices;
} RqPresent;

static void exec_present(const void* payload)
{
    const RqPresent* p = payload;
    if (p->bar > 0.0f) {
        ++g_melee_vita_gxm_state_epoch;
        rt_default_depth();
        vita2d_set_blend_mode_add(0);
        vita2d_draw_rectangle(0.0f, 0.0f, p->bar + 1.0f, 544.0f, RGBA8(0, 0, 0, 255));
        vita2d_draw_rectangle(960.0f - p->bar - 1.0f, 0.0f, p->bar + 1.0f, 544.0f, RGBA8(0, 0, 0, 255));
    }
    vita2d_end_drawing();
    if (s_rendering_internal && s_active_internal_target != NULL) {
        vita2d_texture* presented_target = s_active_internal_target;
        s_rendering_internal = false;
        s_active_internal_target = NULL;
        s_active_resolution_option = MELEE_VITA_RESOLUTION_NATIVE;
        s_render_width = MELEE_VITA_DISPLAY_WIDTH;
        s_render_height = MELEE_VITA_DISPLAY_HEIGHT;
        vita2d_start_drawing();
        ++g_melee_vita_gxm_state_epoch;
        rt_set_viewport(
            MELEE_VITA_DISPLAY_WIDTH, MELEE_VITA_DISPLAY_HEIGHT, false);
        rt_default_depth();
        vita2d_set_blend_mode_add(0);
        vita2d_draw_rectangle(
            0.0f, 0.0f, (f32) MELEE_VITA_DISPLAY_WIDTH,
            (f32) MELEE_VITA_DISPLAY_HEIGHT, RGBA8(0, 0, 0, 255));
        if (p->vertices == NULL || p->indices == NULL ||
            !gxr_copy_color(
                &presented_target->gxm_tex, p->vertices, p->indices)) {
            static u32 logged;
            if (logged++ < 4u)
                melee_vita_log_info(
                    "[GXM] internal EFB presentation draw unavailable");
        }
        vita2d_end_drawing();
    }
#ifdef MELEE_VITA_UPDATER
    {
        extern void melee_vita_updater_render_dialog(void);
        melee_vita_updater_render_dialog();
    }
#endif
    vita2d_swap_buffers();
}

void melee_vita_gxm_present(u32 clear_color)
{
    extern int melee_vita_widescreen_active(void);
    static u32 native_copy_frames, native_readback_frames;
    static u32 native_shadow_frames, native_thp_frames;
    static u32 native_unsupported_copy_frames;
    static u32 native_debug_ui_frames;
    static u32 scaled_copy_frames, scaled_shadow_frames;
    RqFrame* recorded;
    RqPresent* p;
    u32 native_reasons;
    if (!s_initialized) return;
    recorded = &s_frames[s_record];
    p = melee_vita_rq_push(exec_present, sizeof(RqPresent));
    if (p != NULL) {
        static const u16 quad_indices[6] = { 0, 1, 2, 2, 3, 0 };
        memset(p, 0, sizeof(*p));
        p->bar = melee_vita_widescreen_active()
            ? 0.0f
            : (960.0f - 544.0f * (73.0f / 60.0f)) * 0.5f;
        if (resolution_scaling_enabled() && !gxr_available()) {
            recorded->native_reasons |= MELEE_VITA_NATIVE_REASON_READBACK;
        } else if (resolution_scaling_enabled()) {
            p->vertices = gxr_alloc_vertices(4u);
            p->indices = gxr_alloc_indices(6u);
            if (p->vertices == NULL || p->indices == NULL) {
                recorded->native_reasons |=
                    MELEE_VITA_NATIVE_REASON_READBACK;
                p->vertices = NULL;
                p->indices = NULL;
            } else {
                memset(p->vertices, 0, 4u * sizeof(*p->vertices));
                p->vertices[0].position[0] = -1.0f;
                p->vertices[0].position[1] = 1.0f;
                p->vertices[1].position[0] = 1.0f;
                p->vertices[1].position[1] = 1.0f;
                p->vertices[2].position[0] = 1.0f;
                p->vertices[2].position[1] = -1.0f;
                p->vertices[3].position[0] = -1.0f;
                p->vertices[3].position[1] = -1.0f;
                for (u32 i = 0u; i < 4u; ++i) {
                    p->vertices[i].position[3] = 1.0f;
                    p->vertices[i].color[0][0] =
                    p->vertices[i].color[0][1] =
                    p->vertices[i].color[0][2] =
                    p->vertices[i].color[0][3] =
                    p->vertices[i].color[1][0] =
                    p->vertices[i].color[1][1] =
                    p->vertices[i].color[1][2] =
                    p->vertices[i].color[1][3] = 1.0f;
                }
                p->vertices[0].tex[0][0] = 0.0f;
                p->vertices[0].tex[0][1] = 0.0f;
                p->vertices[1].tex[0][0] = 1.0f;
                p->vertices[1].tex[0][1] = 0.0f;
                p->vertices[2].tex[0][0] = 1.0f;
                p->vertices[2].tex[0][1] = 1.0f;
                p->vertices[3].tex[0][0] = 0.0f;
                p->vertices[3].tex[0][1] = 1.0f;
                memcpy(
                    p->indices, quad_indices, sizeof(quad_indices));
            }
        }
    }
    native_reasons = effective_native_reasons(recorded->native_reasons);
    if (native_reasons & MELEE_VITA_NATIVE_REASON_COPY)
        ++native_copy_frames;
    else if (recorded->native_reasons & MELEE_VITA_NATIVE_REASON_COPY)
        ++scaled_copy_frames;
    if (native_reasons & MELEE_VITA_NATIVE_REASON_READBACK)
        ++native_readback_frames;
    if (native_reasons & MELEE_VITA_NATIVE_REASON_SHADOW)
        ++native_shadow_frames;
    else if (recorded->native_reasons & MELEE_VITA_NATIVE_REASON_SHADOW)
        ++scaled_shadow_frames;
    if (native_reasons & MELEE_VITA_NATIVE_REASON_THP)
        ++native_thp_frames;
    if (native_reasons & MELEE_VITA_NATIVE_REASON_UNSUPPORTED_COPY)
        ++native_unsupported_copy_frames;
    if (native_reasons & MELEE_VITA_NATIVE_REASON_DEBUG_UI)
        ++native_debug_ui_frames;
    {
        /* Hand the recorded frame to the render thread once it has finished
         * the previous one; this wait is the only game/render sync point. */
        const u64 t0 = sceKernelGetProcessTimeWide();
        sceKernelWaitSema(s_rq_done, 1, NULL);
        melee_vita_prof_add(VPZ_SWAP, sceKernelGetProcessTimeWide() - t0);
    }
    s_exec_frame = &s_frames[s_record];
    sceKernelSignalSema(s_rq_work, 1);
    s_record ^= 1u;
    s_frames[s_record].size = 0;
    s_frames[s_record].gpu_used = 0;
    s_frames[s_record].clear_color = s_next_clear_color;
    s_frames[s_record].native_reasons = 0u;
    s_next_clear_color = clear_color;

    ++s_frame_counter;
    collect_graveyard();
    if ((s_frame_counter % 60u) == 0u) {
        /* Evict textures that have not been sampled for a few seconds. */
        VitaTextureCacheEntry** link = &s_textures;
        u32 count = 0;
        while (*link != NULL) {
            VitaTextureCacheEntry* entry = *link;
            if (s_frame_counter - entry->last_used_frame > 180u) {
                *link = entry->next;
                memset(s_texture_hash, 0, sizeof(s_texture_hash));
                retire_texture(entry->texture);
                free(entry);
                continue;
            }
            ++count;
            link = &entry->next;
        }
        if ((s_frame_counter % 300u) == 0u) {
            melee_vita_log_info("[GXR] textures live=%u uploads=%u failures=%u",
                                count, s_texture_uploads, s_texture_failures);
            if (resolution_scaling_enabled())
                melee_vita_log_info(
                    "[GXM] menu-option=%d gameplay-option=%d "
                    "native-fallback frames "
                    "copy=%u readback=%u shadow=%u thp=%u unsupported-copy=%u "
                    "debug-ui=%u "
                    "scaled-copy=%u scaled-shadow=%u",
                    s_menu_resolution_option,
                    s_gameplay_resolution_option,
                    native_copy_frames, native_readback_frames,
                    native_shadow_frames, native_thp_frames,
                    native_unsupported_copy_frames, native_debug_ui_frames,
                    scaled_copy_frames,
                    scaled_shadow_frames);
            s_texture_uploads = 0;
            s_texture_failures = 0;
            native_copy_frames = native_readback_frames = 0u;
            native_shadow_frames = native_thp_frames = 0u;
            native_unsupported_copy_frames = 0u;
            native_debug_ui_frames = 0u;
            scaled_copy_frames = scaled_shadow_frames = 0u;
        }
    }
    s_texture_invalidation_pending = 0;
}
