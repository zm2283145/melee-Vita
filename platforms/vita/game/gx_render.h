/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Shader-based GX renderer for the Vita game build.
 *
 * gx.c records GX state and performs vertex work on the CPU (transform to clip
 * space, texture-coordinate generation and colour-channel lighting).  This
 * module turns the recorded TEV configuration into a Cg fragment program,
 * compiles it at runtime with the Vita shader compiler (vitaShaRK +
 * libshacccg), caches the result in memory and on ux0, and submits the draw
 * through GXM with GX blend, depth and alpha-compare semantics.
 */
#ifndef MELEE_VITA_GX_RENDER_H
#define MELEE_VITA_GX_RENDER_H

#include <dolphin/types.h>
#include <stdbool.h>

#include "gxm_game.h"

#define GXR_MAX_STAGES 16
#define GXR_MAX_TEXCOORDS 8
#define GXR_MAX_TEXMAPS 8

typedef struct GxrStage {
    u8 color_in[4];
    u8 alpha_in[4];
    u8 color_op, color_bias, color_scale, color_clamp, color_out;
    u8 alpha_op, alpha_bias, alpha_scale, alpha_clamp, alpha_out;
    u8 tex_coord, tex_map, channel, kcsel, kasel, swap_ras, swap_tex;
    u8 mirror; /* bit0: mirror S, bit1: mirror T (GXM rejects ADDR_MIRROR) */
} GxrStage;

/* Everything that changes the generated fragment source.  Zero padding is
 * part of the contract: callers memset the key so it can be hashed as bytes. */
typedef struct GxrShaderKey {
    u8 stage_count;
    u8 alpha_comp[2];
    u8 alpha_ref[2];
    u8 alpha_op;
    u8 swap[4][4];
    u8 reserved[2];
    GxrStage stages[GXR_MAX_STAGES];
} GxrShaderKey;

typedef struct GxrVertex {
    f32 position[4];      /* Vita clip space */
    f32 color[2][4];      /* lit GX colour channels 0/1, 0..1 */
    f32 tex[GXR_MAX_TEXCOORDS][2];
} GxrVertex;

enum {
    GXR_PRIM_TRIANGLES = 0,
    GXR_PRIM_LINES,
    GXR_PRIM_POINTS,
};

typedef struct GxrDraw {
    GxrShaderKey key;
    u8 blend_mode, blend_src, blend_dst, logic_op;
    u8 color_update, alpha_update;
    u8 depth_compare, depth_function, depth_write;
    u8 primitive;
    f32 line_width;       /* in Vita pixels */
    f32 registers[4][4];  /* GX_TEVPREV..GX_TEVREG2 */
    f32 konst[4][4];
    MeleeVitaTextureSource textures[GXR_MAX_TEXMAPS];
    u8 texture_valid[GXR_MAX_TEXMAPS];
} GxrDraw;

/* ---- GPU vertex pipeline ------------------------------------------------
 * Display lists are decoded once into GxrGpuVertex buffers that stay in GPU
 * memory; transform, lighting and texgen run in generated vertex shaders. */
#define GXR_GPU_TEX 4u

typedef struct GxrGpuVertex {
    f32 pos[3];
    f32 mtx;          /* GX_VA_PNMTXIDX value (0..27) */
    f32 nrm[3];
    u8 c0[4];
    u8 c1[4];
    f32 tex[GXR_GPU_TEX][2];
} GxrGpuVertex;       /* 68 bytes */

typedef struct GxrVtxChan {
    u8 enabled, amb_src, mat_src, lights, diffuse, atten;
} GxrVtxChan;

typedef struct GxrVtxTexGen {
    u8 type, source, has_matrix, normalize, has_post, reserved;
} GxrVtxTexGen;

typedef struct GxrVtxKey {
    u8 has_mtxidx;
    u8 perspective;
    u8 channel_count;
    u8 texgen_count;
    GxrVtxChan chan[4];
    GxrVtxTexGen tg[GXR_MAX_TEXCOORDS];
} GxrVtxKey;

typedef struct GxrVtxUniforms {
    f32 pos[30][4];    /* 10 position matrices, 3 rows each */
    f32 nrm[30][4];
    f32 proj[4][4];    /* p1..p6, ax bx ay by, z_far z_range, current slot */
    f32 tex[24][4];    /* resolved texgen matrix per texgen (3 rows) */
    f32 post[24][4];
    f32 light[40][4];  /* per light: color, pos, dir, a0-a2, k0-k2 */
    f32 mat[2][4];
    f32 amb[2][4];
} GxrVtxUniforms;

enum { GXR_CULL_NONE = 0, GXR_CULL_FRONT, GXR_CULL_BACK, GXR_CULL_ALL };

/* Persistent GPU memory for cached geometry. */
void* gxr_arena_alloc(u32 size);
void gxr_arena_free(void* block);

bool gxr_draw_gpu(const GxrDraw* draw, const GxrVtxKey* vkey,
                  const GxrVtxUniforms* uniforms, const GxrGpuVertex* vertices,
                  const u16* indices, u32 count, u8 cull);

int gxr_init(void);
bool gxr_available(void);
/* Vertex memory for gxr_draw must come from gxr_alloc_vertices (GPU-visible,
 * valid until the end of the frame). */
GxrVertex* gxr_alloc_vertices(u32 count);
u16* gxr_alloc_indices(u32 count);
/* Returns false if the draw could not be issued (caller may fall back).
 * With indices == NULL the vertices are drawn in order (count vertices);
 * otherwise count indices into vertices are drawn. */
bool gxr_draw(const GxrDraw* draw, GxrVertex* vertices, const u16* indices,
              u32 count);
void gxr_log_stats(void);
void gxr_flush_warm_cache(void);

#endif
