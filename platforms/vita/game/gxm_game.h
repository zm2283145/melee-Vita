/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_GXM_GAME_H
#define MELEE_VITA_GXM_GAME_H

#include <dolphin/types.h>

struct vita2d_texture;

typedef struct MeleeVitaScreenVertex {
    f32 x;
    f32 y;
    f32 z;
    f32 u;
    f32 v;
    u32 color;
} MeleeVitaScreenVertex;

typedef struct MeleeVitaTextureSource {
    const void* key;
    const void* data;
    u16 width;
    u16 height;
    u32 format;
    u32 wrap_s;
    u32 wrap_t;
    u32 min_filter;
    u32 mag_filter;
    const void* palette;
    u32 palette_format;
    u16 palette_entries;
    const void* chroma_u;
    const void* chroma_v;
    u16 chroma_width;
    u16 chroma_height;
} MeleeVitaTextureSource;

typedef struct MeleeVitaRenderState {
    u32 depth_compare;
    u32 depth_function;
    u32 depth_write;
    u32 additive_blend;
    u8 line_width;
    u8 point_size;
} MeleeVitaRenderState;

int melee_vita_gxm_init(void);
void melee_vita_gxm_shutdown(void);
void melee_vita_gxm_draw_triangles(const MeleeVitaScreenVertex* vertices,
                                   u32 count,
                                   const MeleeVitaTextureSource* texture,
                                   u32 tint,
                                   const MeleeVitaRenderState* state);
void melee_vita_gxm_draw_lines(const MeleeVitaScreenVertex* vertices,
                               u32 count,
                               const MeleeVitaRenderState* state);
void melee_vita_gxm_draw_points(const MeleeVitaScreenVertex* vertices,
                                u32 count,
                                const MeleeVitaRenderState* state);
void melee_vita_gxm_present(u32 clear_color);
/* Render thread command queue (see gxm_game.c). */
typedef void (*MeleeVitaRqExec)(const void* payload);
void* melee_vita_rq_push(MeleeVitaRqExec exec, u32 payload_size);
void* melee_vita_rq_alloc_gpu(u32 size, u32 align);
void melee_vita_gxm_queue_copy(struct vita2d_texture* target, u32 width, u32 height,
                               f32 x0, f32 y0, f32 sx, f32 sy, int clear);
struct vita2d_texture* melee_vita_gxm_copy_texture(const void* key, u32 width, u32 height);
void melee_vita_gxm_invalidate_textures(void);
void melee_vita_gxm_mark_texture_data_dirty(void);

#endif
