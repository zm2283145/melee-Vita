/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GXM-backed frame sink used by the original game's GX compatibility layer. */
#include "gxm_game.h"
#include "../texture_decoder.h"
#include "gx_render.h"
#include "../vita_log.h"

#include <psp2/gxm.h>
#include <vita2d.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static int s_initialized;
static int s_frame_open;
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
    if (mode == 2u) return SCE_GXM_TEXTURE_ADDR_MIRROR;
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
}

void melee_vita_gxm_invalidate_textures(void)
{
    s_texture_invalidation_pending = 1;
    if (!s_frame_open) {
        if (s_initialized) vita2d_wait_rendering_done();
        free_textures();
        s_texture_invalidation_pending = 0;
    }
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

static int refresh_texture(VitaTextureCacheEntry* entry,
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
    for (entry = s_textures; entry != NULL; entry = entry->next) {
        if (same_texture(&entry->source, source)) {
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
    sceGxmTextureSetUAddrMode(&entry->texture->gxm_tex,
                              address_mode(source->wrap_s));
    sceGxmTextureSetVAddrMode(&entry->texture->gxm_tex,
                              address_mode(source->wrap_t));
    entry->next = s_textures;
    s_textures = entry;
    return entry->texture;
}

int melee_vita_gxm_init(void)
{
    int result;
    if (s_initialized) return 0;
    result = vita2d_init_advanced(32u * 1024u * 1024u);
    if (result == 0) return -1;
    vita2d_set_vblank_wait(0);
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    s_initialized = 1;
#ifndef MELEE_VITA_GX_LEGACY_RENDERER
    if (gxr_init() != 0)
        melee_vita_log_info("[GXR] falling back to the legacy vita2d GX path");
#endif
    return 0;
}

void melee_vita_gxm_shutdown(void)
{
    if (!s_initialized) return;
    if (s_frame_open) {
        vita2d_end_drawing();
        s_frame_open = 0;
    }
    vita2d_wait_rendering_done();
    free_textures();
    vita2d_fini();
    s_initialized = 0;
}

static void begin_frame(void)
{
    SceGxmContext* context;
    if (!s_initialized || s_frame_open) return;
    vita2d_start_drawing();
    /* vita2d's clear quad must not leave its own depth in the buffer: GX
     * scenes clear depth to the far plane and test against it. */
    context = vita2d_get_context();
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    vita2d_clear_screen();
    s_frame_open = 1;
}

void melee_vita_gxm_begin_frame(void) { begin_frame(); }

vita2d_texture* melee_vita_gxm_texture(const MeleeVitaTextureSource* source)
{
    return get_texture(source);
}

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

static void draw_colored(const MeleeVitaScreenVertex* vertices, u32 count,
                         SceGxmPrimitiveType primitive,
                         const MeleeVitaRenderState* state)
{
    vita2d_color_vertex* output;
    u32 i;
    if (!s_initialized || vertices == NULL || count == 0) return;
    begin_frame();
    apply_render_state(state);
    output = vita2d_pool_memalign(count * sizeof(*output), 16);
    if (output == NULL) return;
    for (i = 0; i < count; ++i) {
        output[i].x = vertices[i].x;
        output[i].y = vertices[i].y;
        output[i].z = vertices[i].z;
        output[i].color = vertices[i].color;
    }
    vita2d_draw_array(primitive, output, count);
}

void melee_vita_gxm_draw_triangles(const MeleeVitaScreenVertex* vertices,
                                   u32 count,
                                   const MeleeVitaTextureSource* source,
                                   u32 tint,
                                   const MeleeVitaRenderState* state)
{
    vita2d_color_vertex* output;
    vita2d_texture_vertex* textured_output;
    vita2d_texture* texture;
    u32 i;
    if (!s_initialized || vertices == NULL || count < 3) return;
    texture = get_texture(source);
    begin_frame();
    apply_render_state(state);
    if (texture != NULL) {
        textured_output = vita2d_pool_memalign(count * sizeof(*textured_output), 16);
        if (textured_output == NULL) return;
        for (i = 0; i < count; ++i) {
            textured_output[i].x = vertices[i].x;
            textured_output[i].y = vertices[i].y;
            textured_output[i].z = vertices[i].z;
            textured_output[i].u = vertices[i].u;
            textured_output[i].v = vertices[i].v;
        }
        vita2d_draw_array_textured(texture, SCE_GXM_PRIMITIVE_TRIANGLES,
                                   textured_output, count, tint);
        return;
    }
    output = vita2d_pool_memalign(count * sizeof(*output), 16);
    if (output == NULL) return;
    for (i = 0; i < count; ++i) {
        output[i].x = vertices[i].x;
        output[i].y = vertices[i].y;
        output[i].z = vertices[i].z;
        output[i].color = vertices[i].color;
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, output, count);
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

void melee_vita_gxm_present(u32 clear_color)
{
    if (!s_initialized) return;
    if (!s_frame_open) begin_frame();
    vita2d_end_drawing();
    vita2d_swap_buffers();
    ++s_frame_counter;
    if ((s_frame_counter % 60u) == 0u) {
        /* Evict textures that have not been sampled for a few seconds. */
        VitaTextureCacheEntry** link = &s_textures;
        bool waited = false;
        u32 count = 0;
        while (*link != NULL) {
            VitaTextureCacheEntry* entry = *link;
            if (s_frame_counter - entry->last_used_frame > 180u) {
                if (!waited) { vita2d_wait_rendering_done(); waited = true; }
                *link = entry->next;
                if (entry->texture != NULL) vita2d_free_texture(entry->texture);
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
    vita2d_set_clear_color(clear_color);
    s_frame_open = 0;
}
