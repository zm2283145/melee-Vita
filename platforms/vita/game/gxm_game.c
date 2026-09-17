/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GXM-backed frame sink used by the original game's GX compatibility layer. */
#include "gxm_game.h"
#include "../texture_decoder.h"
#include "gx_render.h"
#include "../vita_log.h"

#include <psp2/gxm.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static int s_initialized;
static int s_texture_invalidation_pending;
static u32 s_texture_content_generation = 1;

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
#define VITA_COPY_TEXTURES 8u
static struct { const void* key; vita2d_texture* texture; u32 width, height, frame; } s_copy_textures[VITA_COPY_TEXTURES];
/* A copy destination that has not been copied to recently is stale: HSD may
 * have freed that buffer and reused the address for an ordinary texture. */
#define VITA_COPY_STALE_FRAMES 120u
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
     * folds mirrored coordinates and samples with REPEAT instead. */
    if (mode == 2u) return SCE_GXM_TEXTURE_ADDR_REPEAT;
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
void melee_vita_gxm_invalidate_textures(void)
{
    s_texture_invalidation_pending = 1;
    if (s_initialized) {
        rq_wait_idle();
        vita2d_wait_rendering_done();
    }
    free_textures();
    s_texture_invalidation_pending = 0;
}

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
        if (s_copy_textures[c].key == source->data && s_copy_textures[c].texture != NULL) {
            if (s_frame_counter - s_copy_textures[c].frame <= VITA_COPY_STALE_FRAMES)
                return s_copy_textures[c].texture;
            retire_texture(s_copy_textures[c].texture);
            s_copy_textures[c].texture = NULL;
            s_copy_textures[c].key = NULL;
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

static void rt_begin_scene(u32 clear_color, int clear)
{
    vita2d_start_drawing();
    ++g_melee_vita_gxm_state_epoch;
    /* vita2d's clear quad must not leave its own depth in the buffer: GX
     * scenes clear depth to the far plane and test against it. */
    rt_default_depth();
    if (clear) {
        vita2d_set_clear_color(clear_color);
        vita2d_clear_screen();
    }
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

int melee_vita_gxm_init(void)
{
    int result;
    if (s_initialized) return 0;
    result = vita2d_init_advanced(8u * 1024u * 1024u);
    if (result == 0) return -1;
    vita2d_set_vblank_wait(0);
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
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

vita2d_texture* melee_vita_gxm_copy_texture(const void* key, u32 width, u32 height)
{
    u32 c, free_slot = VITA_COPY_TEXTURES;
    for (c = 0; c < VITA_COPY_TEXTURES; ++c) {
        if (s_copy_textures[c].key == key) {
            if (s_copy_textures[c].width == width && s_copy_textures[c].height == height &&
                s_copy_textures[c].texture != NULL) {
                s_copy_textures[c].frame = s_frame_counter;
                return s_copy_textures[c].texture;
            }
            retire_texture(s_copy_textures[c].texture);
            s_copy_textures[c].texture = NULL;
            free_slot = c;
            break;
        }
        if (s_copy_textures[c].key == NULL && free_slot == VITA_COPY_TEXTURES) free_slot = c;
    }
    if (free_slot == VITA_COPY_TEXTURES) {
        free_slot = 0;
        retire_texture(s_copy_textures[0].texture);
    }
    s_copy_textures[free_slot].key = key;
    s_copy_textures[free_slot].width = width;
    s_copy_textures[free_slot].height = height;
    s_copy_textures[free_slot].frame = s_frame_counter;
    s_copy_textures[free_slot].texture = vita2d_create_empty_texture(width, height);
    if (s_copy_textures[free_slot].texture != NULL)
        vita2d_texture_set_filters(s_copy_textures[free_slot].texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
                                   SCE_GXM_TEXTURE_FILTER_LINEAR);
    else
        s_copy_textures[free_slot].key = NULL;
    return s_copy_textures[free_slot].texture;
}

vita2d_texture* melee_vita_gxm_texture(const MeleeVitaTextureSource* source)
{
    return get_texture(source);
}

/* ---- EFB copy ---- */

typedef struct RqCopy {
    vita2d_texture* target;
    u32 width, height;
    f32 x0, y0, sx, sy;
    u32 clear;
} RqCopy;

static void exec_copy(const void* payload)
{
    const RqCopy* c = payload;
    const u8* fb;
    vita2d_end_drawing();
    vita2d_wait_rendering_done();
    fb = vita2d_get_current_fb();
    if (fb != NULL && c->target != NULL) {
        const u8* out_base = vita2d_texture_get_datap(c->target);
        const u32 stride = vita2d_texture_get_stride(c->target);
        for (u32 y = 0; y < c->height; ++y) {
            s32 fy = (s32) (c->y0 + (y + 0.5f) * c->sy);
            u8* out = (u8*) out_base + (size_t) y * stride;
            if (fy < 0) fy = 0; else if (fy > 543) fy = 543;
            for (u32 x = 0; x < c->width; ++x) {
                s32 fx = (s32) (c->x0 + (x + 0.5f) * c->sx);
                if (fx < 0) fx = 0; else if (fx > 959) fx = 959;
                memcpy(out + x * 4u, fb + ((u32) fy * 960u + (u32) fx) * 4u, 3);
                out[x * 4u + 3u] = 0xff;
            }
        }
    }
    /* Continue the frame without clearing what was drawn so far unless the
     * copy asked for it. */
    rt_begin_scene(s_exec_frame->clear_color, c->clear ? 1 : 0);
}

void melee_vita_gxm_queue_copy(struct vita2d_texture* target, u32 width, u32 height,
                               f32 x0, f32 y0, f32 sx, f32 sy, int clear)
{
    RqCopy* c = melee_vita_rq_push(exec_copy, sizeof(RqCopy));
    if (c == NULL) return;
    c->target = target;
    c->width = width;
    c->height = height;
    c->x0 = x0; c->y0 = y0; c->sx = sx; c->sy = sy;
    c->clear = clear ? 1u : 0u;
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

/* ---- present ---- */

typedef struct RqPresent {
    f32 bar;
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
    vita2d_swap_buffers();
}

void melee_vita_gxm_present(u32 clear_color)
{
    extern int melee_vita_widescreen_active(void);
    RqPresent* p;
    if (!s_initialized) return;
    p = melee_vita_rq_push(exec_present, sizeof(RqPresent));
    if (p != NULL)
        p->bar = melee_vita_widescreen_active() ? 0.0f : (960.0f - 544.0f * (73.0f / 60.0f)) * 0.5f;
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
            s_texture_uploads = 0;
            s_texture_failures = 0;
        }
    }
    s_texture_invalidation_pending = 0;
}
