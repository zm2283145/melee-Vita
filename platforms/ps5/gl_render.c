/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5 GX shader renderer (TEV -> GLSL).  Bring-up stub: reports itself
 * unavailable so gx.c draws through the CPU-transformed path. */
#include "gx_render.h"
#include "gl_game.h"

#include <stdlib.h>

void* gxr_arena_alloc(u32 size) { return malloc(size); }
void gxr_arena_free(void* block) { free(block); }
int gxr_init(void) { return -1; }
bool gxr_available(void) { return false; }
void gxr_set_runtime_shader_compilation_enabled(bool enabled) { (void) enabled; }
bool gxr_runtime_shader_compilation_enabled(void) { return true; }
GxrVertex* gxr_alloc_vertices(u32 count) { (void) count; return NULL; }
u16* gxr_alloc_indices(u32 count) { (void) count; return NULL; }
void gxr_log_stats(void) {}
void gxr_flush_warm_cache(void) {}

bool gxr_draw(const GxrDraw* draw, GxrVertex* vertices, const u16* indices, u32 count)
{
    (void) draw; (void) vertices; (void) indices; (void) count;
    return false;
}

bool gxr_draw_gpu(const GxrDraw* draw, const GxrVtxKey* vkey, const GxrVtxUniforms* uniforms,
                  const GxrGpuVertex* vertices, const u16* indices, u32 count, u8 cull)
{
    (void) draw; (void) vkey; (void) uniforms; (void) vertices; (void) indices; (void) count;
    (void) cull;
    return false;
}

bool gxr_draw_bump_gpu(const GxrDraw* draw, const GxrBumpVtxKey* vkey,
                       const GxrVtxUniforms* uniforms, const GxrGpuBumpVertex* vertices,
                       const u16* indices, u32 count, u8 cull)
{
    (void) draw; (void) vkey; (void) uniforms; (void) vertices; (void) indices; (void) count;
    (void) cull;
    return false;
}

bool gxr_draw_gpu_points(const GxrDraw* draw, const GxrVtxKey* vkey,
                         const GxrVtxUniforms* uniforms, const GxrPointParams* point,
                         const GxrGpuVertex* vertices, const u16* indices, u32 count)
{
    (void) draw; (void) vkey; (void) uniforms; (void) point; (void) vertices; (void) indices;
    (void) count;
    return false;
}

bool gxr_copy_color(const SceGxmTexture* texture, const GxrVertex* vertices, const u16* indices)
{
    (void) texture; (void) vertices; (void) indices;
    return false;
}

bool gxr_copy_depth(const SceGxmTexture* texture, const GxrVertex* vertices, const u16* indices)
{
    (void) texture; (void) vertices; (void) indices;
    return false;
}

bool melee_ps5_gl_copy_depth(unsigned depth_texture, int x0, int y0, int x1, int y1,
                             unsigned source_width, unsigned source_height,
                             vita2d_texture* target)
{
    (void) depth_texture; (void) x0; (void) y0; (void) x1; (void) y1;
    (void) source_width; (void) source_height; (void) target;
    return false;
}
u32 g_melee_vita_texture_memo_epoch = 1u;
