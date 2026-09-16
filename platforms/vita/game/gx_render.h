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
    u8 reserved;
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

int gxr_init(void);
bool gxr_available(void);
/* Returns false if the draw could not be issued (caller may fall back). */
bool gxr_draw(const GxrDraw* draw, const GxrVertex* vertices, u32 count);
void gxr_log_stats(void);

#endif
