/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal Dolphin GX front end for the native Vita port.
 *
 * This is deliberately a state recorder rather than a collection of ABI
 * stubs.  HSD can configure GX exactly as it does on GameCube/PC while the
 * Vita renderer consumes this state and turns draw calls into GXM batches.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/gx.h>

#include "vita_platform.h"
#include <psp2/kernel/processmgr.h>
#include "gxm_game.h"
#include <vita2d.h>
#include "gx_render.h"
#include "../vita_log.h"

typedef struct VitaTexObj {
    const void* data;
    void* user_data;
    u16 width;
    u16 height;
    u32 tlut;
    GXTexFmt format;
    GXTexWrapMode wrap_s;
    GXTexWrapMode wrap_t;
    GXTexFilter min_filter;
    GXTexFilter mag_filter;
    GXAnisotropy anisotropy;
    f32 min_lod;
    f32 max_lod;
    f32 lod_bias;
    GXBool mipmap;
    GXBool bias_clamp;
    GXBool edge_lod;
} VitaTexObj;

typedef struct VitaTlutObj {
    const void* data;
    GXTlutFmt format;
    u16 entries;
    u16 reserved;
} VitaTlutObj;

typedef struct VitaLightObj {
    GXColor color;
    f32 a0, a1, a2;
    f32 k0, k1, k2;
    f32 px, py, pz;
    f32 nx, ny, nz;
} VitaLightObj;

_Static_assert(sizeof(VitaTexObj) <= sizeof(GXTexObj), "GXTexObj storage too small");
_Static_assert(sizeof(VitaTlutObj) <= sizeof(GXTlutObj), "GXTlutObj storage too small");
_Static_assert(sizeof(VitaLightObj) <= sizeof(GXLightObj), "GXLightObj storage too small");

typedef struct VitaFifoState {
    void* base;
    void* read_ptr;
    void* write_ptr;
    u32 size;
    u32 high_water;
    u32 low_water;
} VitaFifoState;

typedef struct VitaVtxFormat {
    GXCompCnt count;
    GXCompType type;
    u8 fraction;
} VitaVtxFormat;

typedef struct VitaArrayState {
    const void* data;
    u32 size;
    u8 stride;
    bool little_endian;
} VitaArrayState;

typedef struct VitaTexGenState {
    GXTexGenType type;
    GXTexGenSrc source;
    u32 matrix;
    GXBool normalize;
    u32 post_matrix;
} VitaTexGenState;

typedef struct VitaImmediateState {
    GXPrimitive primitive;
    GXVtxFmt format;
    u16 expected_vertices;
    u32 vertex_count;
    u32 values_written;
    u32 attribute_cursor;
    u8 nbt_vectors;
    f32 pending_components[9];
    u8 pending_count;
    u8 pending_matrix;
    GXBool has_pending_matrix;
    GXBool active;
} VitaImmediateState;

typedef struct VitaDecodedVertex {
    f32 position[3];
    f32 normal[3];
    f32 texture[2];
    u32 color;
    u32 color1;
    f32 tex[GX_MAX_TEXCOORD][2];
    u8 position_matrix;
    u8 has_position_matrix;
    u8 has_color[2];
} VitaDecodedVertex;

typedef struct VitaChanCtrl {
    GXBool enabled;
    u8 ambient_source;
    u8 material_source;
    u32 lights;
    u8 diffuse;
    u8 attenuation;
} VitaChanCtrl;

typedef struct VitaGXState {
    GXFifoObj fifo;
    VitaFifoState fifo_state;
    GXFifoObj* cpu_fifo;
    GXFifoObj* gp_fifo;
    OSThread* current_thread;
    GXDrawDoneCallback draw_done_callback;
    GXDrawSyncCallback draw_sync_callback;
    GXBool draw_pending;

    f32 projection[GX_PROJECTION_SZ];
    f32 viewport[6];
    u32 scissor[4];
    s32 scissor_offset[2];
    f32 position_matrices[10][3][4];
    f32 normal_matrices[10][3][4];
    f32 texture_matrices[20][3][4];
    f32 post_matrices[20][3][4];
    VitaTexGenState texture_generators[GX_MAX_TEXCOORD];
    u8 texture_generator_count;
    u32 current_matrix;

    GXAttrType descriptors[GX_VA_MAX_ATTR];
    VitaVtxFormat formats[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
    VitaArrayState arrays[GX_VA_MAX_ATTR];
    GXTexOffset line_offset;
    GXTexOffset point_offset;
    u8 tex_offset_lines[GXR_MAX_TEXCOORDS];
    u8 tex_offset_points[GXR_MAX_TEXCOORDS];
    u8 line_width;
    u8 point_size;
    GXClipMode clip_mode;
    GXCullMode cull_mode;
    VitaImmediateState immediate;

    GXColor clear_color;
    u32 clear_depth;
    const GXTexObj* textures[GX_MAX_TEXMAP];
    const GXTlutObj* tluts[20];
    /* GX latches texture/TLUT parameters into hardware registers when they
     * are loaded, so callers may reuse the object (HSD keeps some on the
     * stack).  Keep copies instead of pointers to the caller's objects. */
    GXTexObj texture_copies[GX_MAX_TEXMAP];
    const GXTexObj* texture_sources[GX_MAX_TEXMAP];
    GXTlutObj tlut_copies[20];
    VitaLightObj lights[8];
    GXColor ambient_colors[2];
    GXColor material_colors[2];
    GXBlendMode blend_mode;
    GXBlendFactor blend_source;
    GXBlendFactor blend_destination;
    GXLogicOp logic_operation;
    GXBool color_update;
    GXBool alpha_update;
    GXBool depth_compare;
    GXBool depth_update;
    GXCompare depth_function;
    GXTevMode tev_modes[GX_MAX_TEVSTAGE];
    GXTexCoordID tev_coordinates[GX_MAX_TEVSTAGE];
    GXTexMapID tev_maps[GX_MAX_TEVSTAGE];
    GXChannelID tev_colors[GX_MAX_TEVSTAGE];
    GXTevColorArg tev_color_inputs[GX_MAX_TEVSTAGE][4];
    GXTevAlphaArg tev_alpha_inputs[GX_MAX_TEVSTAGE][4];
    GXTevOp tev_color_operations[GX_MAX_TEVSTAGE];
    GXTevOp tev_alpha_operations[GX_MAX_TEVSTAGE];
    u8 tev_color_bias[GX_MAX_TEVSTAGE], tev_color_scale[GX_MAX_TEVSTAGE];
    u8 tev_color_clamp[GX_MAX_TEVSTAGE], tev_color_out[GX_MAX_TEVSTAGE];
    u8 tev_alpha_bias[GX_MAX_TEVSTAGE], tev_alpha_scale[GX_MAX_TEVSTAGE];
    u8 tev_alpha_clamp[GX_MAX_TEVSTAGE], tev_alpha_out[GX_MAX_TEVSTAGE];
    u8 tev_swap_ras[GX_MAX_TEVSTAGE], tev_swap_tex[GX_MAX_TEVSTAGE];
    u8 tev_swap_table[4][4];
    u8 alpha_comp[2], alpha_ref[2], alpha_op;
    VitaChanCtrl channels[4]; /* GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1 */
    u8 channel_count;
    f32 tev_registers_f[GX_MAX_TEVREG][4];
    GXTevKColorSel tev_kcolor_selection[GX_MAX_TEVSTAGE];
    GXTevKAlphaSel tev_kalpha_selection[GX_MAX_TEVSTAGE];
    GXColor tev_registers[GX_MAX_TEVREG];
    GXColor tev_kcolors[GX_MAX_KCOLOR];
    u8 tev_stage_count;
    u16 copy_source[4];
    u16 copy_width;
    u16 copy_height;
    u32 copied_frames;
} VitaGXState;

static VitaGXState s_gx;
static struct { u32 draws, tris_in, tris_culled, textured, tex_fail, lines; f32 zmin, zmax; u32 alpha0; u32 bm_none; } s_stats;
/* ---- lightweight zone profiler (reported as [PROF] every 120 frames) ---- */
enum {
    VPZ_GOBJ_RENDER, VPZ_PRESENT, VPZ_SWAP, VPZ_COPYTEX, VPZ_TEXUPLOAD,
    VPZ_DL_HASH, VPZ_DL_BUILD, VPZ_DRAW_SETUP, VPZ_GPU_SUBMIT, VPZ_IMMEDIATE, VPZ_AUDIO_MIX, VPZ_RT_EXEC,
    VPZ_VERTEX_PROGRAM, VPZ_FRAGMENT_PROGRAM, VPZ_FRAGMENT_PATCH,
    VPZ_RQ_PUSH, VPZ_RESOLVE_TEXTURES, VPZ_UNIFORM_COPY,
    VPZ_COUNT
};
static const char* const k_vpz_names[VPZ_COUNT] = {
    "gobj_render", "present", "rt_wait", "copytex", "tex_upload",
    "dl_hash", "dl_build", "draw_setup", "gpu_submit", "immediate", "audio_mix", "rt_exec",
    "vertex_program", "fragment_program", "fragment_patch",
    "rq_push", "resolve_textures", "uniform_copy",
};
static u64 s_vpz_us[VPZ_COUNT];
static u32 s_vpz_calls[VPZ_COUNT];
void melee_vita_prof_add(int zone, u64 us)
{
    if ((unsigned) zone < VPZ_COUNT) { s_vpz_us[zone] += us; ++s_vpz_calls[zone]; }
}

static VitaDecodedVertex* s_decode_vertices;
static u32 s_decode_capacity;
static MeleeVitaScreenVertex* s_triangle_vertices;
static u32 s_triangle_capacity;

static u16 read_u16(const u8* bytes, bool little_endian)
{
    return little_endian ? (u16) ((u16) bytes[1] << 8 | bytes[0])
                         : (u16) ((u16) bytes[0] << 8 | bytes[1]);
}

static u32 read_u32(const u8* bytes, bool little_endian)
{
    if (little_endian) {
        return (u32) bytes[0] | (u32) bytes[1] << 8 |
               (u32) bytes[2] << 16 | (u32) bytes[3] << 24;
    }
    return (u32) bytes[0] << 24 | (u32) bytes[1] << 16 |
           (u32) bytes[2] << 8 | bytes[3];
}

static f32 read_component(const u8* bytes, GXCompType type, u8 fraction,
                          bool little_endian)
{
    f32 value;
    switch (type) {
    case GX_U8: value = bytes[0]; break;
    case GX_S8: value = (s8) bytes[0]; break;
    case GX_U16: value = read_u16(bytes, little_endian); break;
    case GX_S16: value = (s16) read_u16(bytes, little_endian); break;
    default: {
        u32 bits = read_u32(bytes, little_endian);
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    }
    return value / (f32) (1u << fraction);
}

static u32 component_bytes(GXCompType type)
{
    return type <= GX_S8 ? 1u : type <= GX_S16 ? 2u : 4u;
}

static u32 attribute_components(GXAttr attr, GXCompCnt count)
{
    if (attr == GX_VA_POS) return count == GX_POS_XY ? 2u : 3u;
    if (attr == GX_VA_NRM || attr == GX_VA_NBT)
        return count == GX_NRM_XYZ ? 3u : 9u;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) return 1u;
    if (attr <= GX_VA_TEX7 && attr >= GX_VA_TEX0)
        return count == GX_TEX_S ? 1u : 2u;
    return 1u;
}

static u32 color_bytes(GXCompType type)
{
    if (type == GX_RGB565 || type == GX_RGBA4) return 2;
    if (type == GX_RGB8 || type == GX_RGBA6) return 3;
    return 4;
}

static u32 direct_bytes(GXAttr attr, const VitaVtxFormat* format)
{
    if (attr <= GX_VA_TEX7MTXIDX) return 1;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) return color_bytes(format->type);
    return attribute_components(attr, format->count) * component_bytes(format->type);
}

static u32 decode_color(const u8* bytes, GXCompType type, bool little_endian)
{
    u32 r = 255, g = 255, b = 255, a = 255;
    if (type == GX_RGB565) {
        u16 c = read_u16(bytes, little_endian);
        r = ((c >> 11) & 31u) * 255u / 31u;
        g = ((c >> 5) & 63u) * 255u / 63u;
        b = (c & 31u) * 255u / 31u;
    } else if (type == GX_RGBA4) {
        u16 c = read_u16(bytes, little_endian);
        r = ((c >> 12) & 15u) * 17u; g = ((c >> 8) & 15u) * 17u;
        b = ((c >> 4) & 15u) * 17u; a = (c & 15u) * 17u;
    } else if (type == GX_RGBA6) {
        /* 24 bits: RRRRRRGG GGGGBBBB BBAAAAAA */
        const u32 c = (u32) bytes[0] << 16 | (u32) bytes[1] << 8 | bytes[2];
        r = ((c >> 18) & 63u) * 255u / 63u; g = ((c >> 12) & 63u) * 255u / 63u;
        b = ((c >> 6) & 63u) * 255u / 63u; a = (c & 63u) * 255u / 63u;
    } else if (type == GX_RGB8) {
        r = bytes[0]; g = bytes[1]; b = bytes[2];
    } else {
        r = bytes[0]; g = bytes[1]; b = bytes[2];
        if (type == GX_RGBA8) a = bytes[3];
    }
    return r | g << 8 | b << 16 | a << 24;
}

static bool reserve_vertices(u32 count)
{
    VitaDecodedVertex* replacement;
    u32 capacity = s_decode_capacity != 0 ? s_decode_capacity : 256;
    if (count <= s_decode_capacity) return true;
    while (capacity < count && capacity <= UINT32_MAX / 2u) capacity *= 2u;
    if (capacity < count) return false;
    replacement = realloc(s_decode_vertices, (size_t) capacity * sizeof(*replacement));
    if (replacement == NULL) return false;
    s_decode_vertices = replacement;
    s_decode_capacity = capacity;
    return true;
}

static bool reserve_triangles(u32 count)
{
    MeleeVitaScreenVertex* replacement;
    u32 capacity = s_triangle_capacity != 0 ? s_triangle_capacity : 512;
    if (count <= s_triangle_capacity) return true;
    while (capacity < count && capacity <= UINT32_MAX / 2u) capacity *= 2u;
    if (capacity < count) return false;
    replacement = realloc(s_triangle_vertices, (size_t) capacity * sizeof(*replacement));
    if (replacement == NULL) return false;
    s_triangle_vertices = replacement;
    s_triangle_capacity = capacity;
    return true;
}

static bool valid_attr(GXAttr attr)
{
    return (unsigned) attr < GX_VA_MAX_ATTR;
}

static u32 packed_color(GXColor color)
{
    return (u32) color.r | (u32) color.g << 8 |
           (u32) color.b << 16 | (u32) color.a << 24;
}

static bool valid_format(GXVtxFmt format)
{
    return (unsigned) format < GX_MAX_VTXFMT;
}

static unsigned matrix_slot(u32 id)
{
    return (id / 3u) % 10u;
}

static void decode_attribute(VitaDecodedVertex* vertex, GXAttr attr,
                             const VitaVtxFormat* format, const u8* source,
                             bool little_endian)
{
    u32 step = component_bytes(format->type);
    if (attr == GX_VA_PNMTXIDX) {
        vertex->position_matrix = source[0];
        vertex->has_position_matrix = 1;
    } else if (attr == GX_VA_POS) {
        u32 components = attribute_components(attr, format->count);
        vertex->position[0] = read_component(source, format->type, format->fraction, little_endian);
        vertex->position[1] = read_component(source + step, format->type, format->fraction, little_endian);
        vertex->position[2] = components >= 3
            ? read_component(source + step * 2u, format->type, format->fraction, little_endian) : 0.0f;
    } else if (attr == GX_VA_NRM || attr == GX_VA_NBT) {
        vertex->normal[0] = read_component(source, format->type, format->fraction, little_endian);
        vertex->normal[1] = read_component(source + step, format->type, format->fraction, little_endian);
        vertex->normal[2] = read_component(source + step * 2u, format->type, format->fraction, little_endian);
    } else if (attr == GX_VA_CLR0) {
        vertex->color = decode_color(source, format->type, little_endian);
        vertex->has_color[0] = 1;
    } else if (attr == GX_VA_CLR1) {
        vertex->color1 = decode_color(source, format->type, little_endian);
        vertex->has_color[1] = 1;
    } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        u32 components = attribute_components(attr, format->count);
        const u32 index = (u32) (attr - GX_VA_TEX0);
        vertex->tex[index][0] = read_component(source, format->type, format->fraction, little_endian);
        vertex->tex[index][1] = components >= 2
            ? read_component(source + step, format->type, format->fraction, little_endian) : 0.0f;
        if (index == 0) {
            vertex->texture[0] = vertex->tex[0][0];
            vertex->texture[1] = vertex->tex[0][1];
        }
    }
}

static u32 active_texture_stage(void)
{
    u32 stage;
    for (stage = 0; stage < s_gx.tev_stage_count && stage < GX_MAX_TEVSTAGE;
         ++stage) {
        const GXTexMapID map = s_gx.tev_maps[stage];
        if (s_gx.tev_modes[stage] != GX_PASSCLR &&
            (unsigned) map < GX_MAX_TEXMAP && s_gx.textures[map] != NULL)
            return stage;
    }
    return GX_MAX_TEVSTAGE;
}

/* Logical 640x480 -> Vita screen mapping.  Widescreen scenes span all 960
 * columns; the rest are pillarboxed at Melee's 73:60 display aspect. */
extern int melee_vita_widescreen_active(void);
/* While an offscreen pass is bound (the HSD shadow map), the logical 640x480
 * frame maps 1:1 onto the target instead of onto the screen. */
static struct { f32 half_w, half_h; u8 active; } s_render_target = { 480.0f, 272.0f, 0u };

static void screen_mapping(f32* sx, f32* ox, f32* sy)
{
    if (s_render_target.active) {
        *sx = 1.0f;
        *ox = 0.0f;
        *sy = 1.0f;
        return;
    }
    *sy = 544.0f / 480.0f;
    if (melee_vita_widescreen_active()) {
        *sx = 960.0f / 640.0f;
        *ox = 0.0f;
    } else {
        const f32 width = 544.0f * (73.0f / 60.0f);
        *sx = width / 640.0f;
        *ox = (960.0f - width) * 0.5f;
    }
}

static void project_vertex(const VitaDecodedVertex* input,
                           MeleeVitaScreenVertex* output)
{
    const u32 matrix_id = input->has_position_matrix
        ? input->position_matrix : s_gx.current_matrix;
    const f32 (*matrix)[4] = s_gx.position_matrices[matrix_slot(matrix_id)];
    f32 x, y, z;
    f32 map_sx, map_ox, map_sy;
    GXProject(input->position[0], input->position[1], input->position[2],
              matrix, s_gx.projection, s_gx.viewport, &x, &y, &z);
    screen_mapping(&map_sx, &map_ox, &map_sy);
    output->x = map_ox + x * map_sx;
    output->y = y * map_sy;
    output->z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    f32 tex[3] = { input->texture[0], input->texture[1], 1.0f };
    const u32 texture_stage = active_texture_stage();
    GXTexCoordID coordinate = texture_stage < GX_MAX_TEVSTAGE
        ? s_gx.tev_coordinates[texture_stage] : GX_TEXCOORD_NULL;
    if ((unsigned) coordinate < GX_MAX_TEXCOORD &&
        (unsigned) coordinate < s_gx.texture_generator_count) {
        const VitaTexGenState* generator = &s_gx.texture_generators[coordinate];
        if (generator->source == GX_TG_POS) {
            tex[0] = input->position[0]; tex[1] = input->position[1]; tex[2] = input->position[2];
        } else if (generator->source == GX_TG_NRM) {
            tex[0] = input->normal[0]; tex[1] = input->normal[1]; tex[2] = input->normal[2];
        }
        if (generator->normalize) {
            const f32 length = sqrtf(tex[0] * tex[0] + tex[1] * tex[1] + tex[2] * tex[2]);
            if (length > 1.0e-8f) { tex[0] /= length; tex[1] /= length; tex[2] /= length; }
        }
        if (generator->matrix != GX_IDENTITY && generator->matrix / 3u < 20u) {
            const f32 (*m)[4] = s_gx.texture_matrices[generator->matrix / 3u];
            const f32 s = m[0][0] * tex[0] + m[0][1] * tex[1] + m[0][2] * tex[2] + m[0][3];
            const f32 t = m[1][0] * tex[0] + m[1][1] * tex[1] + m[1][2] * tex[2] + m[1][3];
            const f32 q = m[2][0] * tex[0] + m[2][1] * tex[1] + m[2][2] * tex[2] + m[2][3];
            tex[0] = s; tex[1] = t;
            if (generator->type == GX_TG_MTX3x4 && q != 0.0f) { tex[0] /= q; tex[1] /= q; }
        }
        if (generator->post_matrix != GX_PTIDENTITY &&
            generator->post_matrix >= GX_PTTEXMTX0) {
            const u32 post = (generator->post_matrix - GX_PTTEXMTX0) / 3u;
            if (post < 20u) {
                const f32 (*m)[4] = s_gx.post_matrices[post];
                const f32 s = m[0][0] * tex[0] + m[0][1] * tex[1] + m[0][3];
                const f32 t = m[1][0] * tex[0] + m[1][1] * tex[1] + m[1][3];
                tex[0] = s; tex[1] = t;
            }
        }
    }
    output->u = tex[0];
    output->v = tex[1];
    output->color = input->color;
}

static const MeleeVitaTextureSource* current_texture_source(
    MeleeVitaTextureSource* source)
{
    const u32 stage = active_texture_stage();
    GXTexMapID map;
    const GXTexObj* object;
    const VitaTexObj* texture;
    const VitaTlutObj* palette = NULL;
    bool movie_yuv = false;
    if (s_gx.tev_stage_count >= 4 &&
        s_gx.tev_maps[GX_TEVSTAGE0] == GX_TEXMAP1 &&
        s_gx.tev_maps[GX_TEVSTAGE1] == GX_TEXMAP2 &&
        s_gx.tev_maps[GX_TEVSTAGE2] == GX_TEXMAP0 &&
        s_gx.textures[GX_TEXMAP0] != NULL &&
        s_gx.textures[GX_TEXMAP1] != NULL &&
        s_gx.textures[GX_TEXMAP2] != NULL) {
        movie_yuv = true;
        map = GX_TEXMAP0;
    } else {
        if (stage >= GX_MAX_TEVSTAGE) return NULL;
        map = s_gx.tev_maps[stage];
    }
    object = s_gx.textures[map];
    if (object == NULL) return NULL;
    texture = (const VitaTexObj*) object;
    if ((unsigned) texture->format >= (unsigned) GX_TF_C4 &&
        (unsigned) texture->format <= (unsigned) GX_TF_C14X2 &&
        texture->tlut < 20u && s_gx.tluts[texture->tlut] != NULL)
        palette = (const VitaTlutObj*) s_gx.tluts[texture->tlut];
    {
        extern void* g_melee_vita_last_copy_dst;
        static u32 logged, window;
        if (window != s_gx.copied_frames / 600u) { window = s_gx.copied_frames / 600u; logged = 0; }
        if ((texture->data == g_melee_vita_last_copy_dst ||
             (texture->width == 256u && texture->height == 256u)) && logged++ < 4u)
            melee_vita_log_info("[GXCOPY] sampled as %ux%u fmt=%u tlut=%u", texture->width, texture->height,
                                (unsigned) texture->format, texture->tlut);
    }
    memset(source, 0, sizeof(*source));
    source->key = texture->data;
    source->data = texture->data;
    source->width = texture->width;
    source->height = texture->height;
    source->format = texture->format;
    source->wrap_s = texture->wrap_s;
    source->wrap_t = texture->wrap_t;
    source->min_filter = texture->min_filter;
    source->mag_filter = texture->mag_filter;
    if (palette != NULL) {
        source->palette = palette->data;
        source->palette_format = palette->format;
        source->palette_entries = palette->entries;
    }
    if (movie_yuv && texture->format == GX_TF_I8) {
        const VitaTexObj* chroma_u =
            (const VitaTexObj*) s_gx.textures[GX_TEXMAP1];
        const VitaTexObj* chroma_v =
            (const VitaTexObj*) s_gx.textures[GX_TEXMAP2];
        if (chroma_u->format == GX_TF_I8 && chroma_v->format == GX_TF_I8 &&
            chroma_u->width * 2u == texture->width &&
            chroma_u->height * 2u == texture->height &&
            chroma_v->width == chroma_u->width &&
            chroma_v->height == chroma_u->height) {
            source->chroma_u = chroma_u->data;
            source->chroma_v = chroma_v->data;
            source->chroma_width = chroma_u->width;
            source->chroma_height = chroma_u->height;
            static bool logged_movie_yuv;
            if (!logged_movie_yuv) {
                melee_vita_log_info(
                    "[THP] bound YUV420 planes: Y=%ux%u U/V=%ux%u",
                    texture->width, texture->height, chroma_u->width,
                    chroma_u->height);
                logged_movie_yuv = true;
            }
        }
    }
    return source;
}

static u32 multiply_color(u32 left, GXColor right)
{
    const u32 r = (left & 0xffu) * right.r / 255u;
    const u32 g = ((left >> 8) & 0xffu) * right.g / 255u;
    const u32 b = ((left >> 16) & 0xffu) * right.b / 255u;
    const u32 a = ((left >> 24) & 0xffu) * right.a / 255u;
    return r | g << 8 | b << 16 | a << 24;
}

static u32 approximate_tev_tint(u32 base, u32 texture_stage)
{
    u32 tint = s_gx.tev_modes[texture_stage] == GX_MODULATE
        ? base : 0xffffffffu;
    u32 stage;
    for (stage = texture_stage + 1u;
         stage < s_gx.tev_stage_count && stage < GX_MAX_TEVSTAGE; ++stage) {
        u32 i;
        bool applied = false;
        for (i = 0; i < 4u && !applied; ++i) {
            const GXTevColorArg argument = s_gx.tev_color_inputs[stage][i];
            if (argument == GX_CC_C0) { tint = multiply_color(tint, s_gx.tev_registers[GX_TEVREG0]); applied = true; }
            else if (argument == GX_CC_C1) { tint = multiply_color(tint, s_gx.tev_registers[GX_TEVREG1]); applied = true; }
            else if (argument == GX_CC_C2) { tint = multiply_color(tint, s_gx.tev_registers[GX_TEVREG2]); applied = true; }
            else if (argument == GX_CC_KONST) {
                const u32 selection = (u32) s_gx.tev_kcolor_selection[stage];
                if (selection >= (u32) GX_TEV_KCSEL_K0 &&
                    selection <= (u32) GX_TEV_KCSEL_K3) {
                    tint = multiply_color(tint,
                        s_gx.tev_kcolors[selection - (u32) GX_TEV_KCSEL_K0]);
                    applied = true;
                }
            }
        }
    }
    return tint;
}

/* ------------------------------------------------------------------------
 * Shader renderer submission (gx_render.c).  Vertex work stays on the CPU:
 * clip-space transform, GX colour-channel lighting and texture-coordinate
 * generation.  The recorded TEV/blend/depth state travels in a GxrDraw.
 * ------------------------------------------------------------------------ */

static GxrDraw s_gxr_draw;

static bool texture_source_for_map(u32 map, MeleeVitaTextureSource* source)
{
    const GXTexObj* object;
    const VitaTexObj* texture;
    if (map >= GX_MAX_TEXMAP) return false;
    object = s_gx.textures[map];
    if (object == NULL) return false;
    texture = (const VitaTexObj*) object;
    if (texture->width == 0 || texture->height == 0 ||
        texture->width > 2048u || texture->height > 2048u ||
        (uintptr_t) texture->data < 0x80000000u) {
        static u32 logged;
        if (logged++ < 8u) {
            const u8* raw = (const u8*) object;
            melee_vita_log_info(
                "[GXR] invalid texobj map=%u data=%p %ux%u fmt=%u loaded_from=%p raw=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                map, texture->data, texture->width, texture->height,
                (unsigned) texture->format, (void*) s_gx.texture_sources[map],
                raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
        }
        return false;
    }
    memset(source, 0, sizeof(*source));
    source->key = texture->data;
    source->data = texture->data;
    source->width = texture->width;
    source->height = texture->height;
    source->format = texture->format;
    source->wrap_s = texture->wrap_s;
    source->wrap_t = texture->wrap_t;
    source->min_filter = texture->min_filter;
    source->mag_filter = texture->mag_filter;
    if ((unsigned) texture->format >= (unsigned) GX_TF_C4 &&
        (unsigned) texture->format <= (unsigned) GX_TF_C14X2 &&
        texture->tlut < 20u && s_gx.tluts[texture->tlut] != NULL) {
        const VitaTlutObj* palette = (const VitaTlutObj*) s_gx.tluts[texture->tlut];
        source->palette = palette->data;
        source->palette_format = palette->format;
        source->palette_entries = palette->entries;
    }
    return source->data != NULL && source->width != 0 && source->height != 0;
}

static bool is_movie_yuv_draw(void)
{
    return s_gx.tev_stage_count >= 4 &&
           s_gx.tev_maps[GX_TEVSTAGE0] == GX_TEXMAP1 &&
           s_gx.tev_maps[GX_TEVSTAGE1] == GX_TEXMAP2 &&
           s_gx.tev_maps[GX_TEVSTAGE2] == GX_TEXMAP0;
}

static void color_to_float(u32 packed, f32 out[4])
{
    out[0] = (packed & 0xffu) / 255.0f;
    out[1] = ((packed >> 8) & 0xffu) / 255.0f;
    out[2] = ((packed >> 16) & 0xffu) / 255.0f;
    out[3] = ((packed >> 24) & 0xffu) / 255.0f;
}

static void gxcolor_to_float(GXColor c, f32 out[4])
{
    out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f; out[3] = c.a / 255.0f;
}

static f32 dot3(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* GX lighting for one component group (rgb when alpha == false). */
static void light_component(const VitaChanCtrl* control, const f32 material[4],
                            const f32 ambient[4], const f32 eye[3],
                            const f32 normal[3], bool alpha, f32 out[4])
{
    f32 lit[4];
    u32 light;
    if (!control->enabled) {
        if (alpha) out[3] = material[3];
        else { out[0] = material[0]; out[1] = material[1]; out[2] = material[2]; }
        return;
    }
    memcpy(lit, ambient, sizeof(lit));
    for (light = 0; light < 8u; ++light) {
        const VitaLightObj* l = &s_gx.lights[light];
        f32 ldir[3], color[4], attn = 1.0f, diff = 1.0f;
        f32 dist2, dist;
        if ((control->lights & (1u << light)) == 0u) continue;
        ldir[0] = l->px - eye[0]; ldir[1] = l->py - eye[1]; ldir[2] = l->pz - eye[2];
        dist2 = dot3(ldir, ldir);
        dist = sqrtf(dist2);
        if (dist > 1.0e-8f) { ldir[0] /= dist; ldir[1] /= dist; ldir[2] /= dist; }
        {
            const f32 dir[3] = { l->nx, l->ny, l->nz };
            if (control->attenuation == GX_AF_SPOT) {
                const f32 cosine = fmaxf(0.0f, dot3(ldir, dir));
                const f32 cos_attn = l->a0 + l->a1 * cosine + l->a2 * cosine * cosine;
                const f32 dist_attn = l->k0 + l->k1 * dist + l->k2 * dist2;
                attn = dist_attn != 0.0f ? fmaxf(0.0f, cos_attn / dist_attn) : 0.0f;
            } else if (control->attenuation == GX_AF_SPEC) {
                f32 dist_attn;
                attn = dot3(normal, ldir) >= 0.0f ? fmaxf(0.0f, dot3(normal, dir)) : 0.0f;
                {
                    const f32 cos_attn = l->a0 + l->a1 * attn + l->a2 * attn * attn;
                    if (control->diffuse != GX_DF_NONE) {
                        f32 k[3] = { l->k0, l->k1, l->k2 };
                        const f32 len = sqrtf(dot3(k, k));
                        if (len > 1.0e-8f) { k[0] /= len; k[1] /= len; k[2] /= len; }
                        dist_attn = k[0] + k[1] * attn + k[2] * attn * attn;
                    } else {
                        dist_attn = l->k0 + l->k1 * attn + l->k2 * attn * attn;
                    }
                    attn = dist_attn != 0.0f ? fmaxf(0.0f, cos_attn / dist_attn) : 0.0f;
                }
            }
        }
        if (control->diffuse == GX_DF_SIGN) diff = dot3(ldir, normal);
        else if (control->diffuse == GX_DF_CLAMP) diff = fmaxf(0.0f, dot3(ldir, normal));
        gxcolor_to_float(l->color, color);
        lit[0] += attn * diff * color[0]; lit[1] += attn * diff * color[1];
        lit[2] += attn * diff * color[2]; lit[3] += attn * diff * color[3];
    }
    for (light = 0; light < 4u; ++light)
        lit[light] = lit[light] < 0.0f ? 0.0f : lit[light] > 1.0f ? 1.0f : lit[light];
    if (alpha) out[3] = material[3] * lit[3];
    else { out[0] = material[0] * lit[0]; out[1] = material[1] * lit[1]; out[2] = material[2] * lit[2]; }
}

static void apply_3x4(const f32 (*m)[4], const f32 v[3], f32 w, f32 out[3])
{
    out[0] = m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2] + m[0][3] * w;
    out[1] = m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2] + m[1][3] * w;
    out[2] = m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] + m[2][3] * w;
}

/* Per-draw constants hoisted out of the per-vertex loop. */
typedef struct GxrXform {
    bool perspective;
    f32 p[7];
    f32 ax, bx, ay, by, z_far, z_range;
    bool need_normal;
    u32 channel_count;
    bool lit[4];
    f32 material[2][4];
    f32 ambient[2][4];
    u32 texgen_count;
} GxrXform;

static void prepare_xform(GxrXform* x)
{
    const f32* v = s_gx.viewport;
    f32 scale, offset, yscale;
    u32 i;
    x->perspective = s_gx.projection[0] == (f32) GX_PERSPECTIVE;
    memcpy(x->p, s_gx.projection, sizeof(x->p));
    screen_mapping(&scale, &offset, &yscale);
    x->ax = (offset + scale * (v[0] + v[2] * 0.5f)) / s_render_target.half_w - 1.0f;
    x->bx = scale * v[2] * 0.5f / s_render_target.half_w;
    x->ay = 1.0f - yscale * (v[1] + v[3] * 0.5f) / s_render_target.half_h;
    x->by = yscale * v[3] * 0.5f / s_render_target.half_h;
    x->z_far = v[5];
    x->z_range = v[5] - v[4];
    x->channel_count = s_gx.channel_count > 2 ? 2u : s_gx.channel_count;
    x->need_normal = false;
    for (i = 0; i < 4u; ++i) {
        x->lit[i] = s_gx.channels[i].enabled && s_gx.channels[i].lights != 0u;
        if (x->lit[i] && (i % 2u) < x->channel_count) x->need_normal = true;
    }
    for (i = 0; i < 2u; ++i) {
        gxcolor_to_float(s_gx.material_colors[i], x->material[i]);
        gxcolor_to_float(s_gx.ambient_colors[i], x->ambient[i]);
    }
    x->texgen_count = s_gx.texture_generator_count > GXR_MAX_TEXCOORDS
        ? GXR_MAX_TEXCOORDS : s_gx.texture_generator_count;
    for (i = 0; i < x->texgen_count; ++i)
        if (s_gx.texture_generators[i].source == GX_TG_NRM) x->need_normal = true;
}

static void build_gxr_vertex(const GxrXform* x, const VitaDecodedVertex* input, GxrVertex* out)
{
    const u32 matrix_id = input->has_position_matrix
        ? input->position_matrix : s_gx.current_matrix;
    const unsigned slot = matrix_slot(matrix_id);
    const f32* p = x->p;
    f32 eye[3], normal[3] = { 0.0f, 0.0f, 1.0f }, xc, yc, zc, wc;
    u32 i;

    apply_3x4(s_gx.position_matrices[slot], input->position, 1.0f, eye);
    if (x->need_normal) {
        apply_3x4(s_gx.normal_matrices[slot], input->normal, 0.0f, normal);
        const f32 len2 = dot3(normal, normal);
        if (len2 > 1.0e-16f) {
            const f32 inv = 1.0f / sqrtf(len2);
            normal[0] *= inv; normal[1] *= inv; normal[2] *= inv;
        }
    }

    if (x->perspective) {
        xc = eye[0] * p[1] + eye[2] * p[2];
        yc = eye[1] * p[3] + eye[2] * p[4];
        zc = p[6] + eye[2] * p[5];
        wc = -eye[2];
    } else {
        xc = p[2] + eye[0] * p[1];
        yc = p[4] + eye[1] * p[3];
        zc = p[6] + eye[2] * p[5];
        wc = 1.0f;
    }
    out->position[0] = x->ax * wc + x->bx * xc;
    out->position[1] = x->ay * wc + x->by * yc;
    out->position[2] = x->z_far * wc + zc * x->z_range;
    out->position[3] = wc;

    for (i = 0; i < 2u; ++i) {
        f32 vertex_color[4];
        const VitaChanCtrl* cc = &s_gx.channels[i];
        const VitaChanCtrl* ac = &s_gx.channels[2u + i];
        if (i >= x->channel_count) {
            out->color[i][0] = out->color[i][1] = out->color[i][2] = out->color[i][3] = 0.0f;
            continue;
        }
        color_to_float(input->has_color[i] ? (i == 0 ? input->color : input->color1) : 0xffffffffu,
                       vertex_color);
        {
            const f32* mat_c = cc->material_source == GX_SRC_VTX ? vertex_color : x->material[i];
            const f32* mat_a = ac->material_source == GX_SRC_VTX ? vertex_color : x->material[i];
            if (!x->lit[i]) {
                if (!cc->enabled) {
                    out->color[i][0] = mat_c[0]; out->color[i][1] = mat_c[1]; out->color[i][2] = mat_c[2];
                } else {
                    const f32* amb = cc->ambient_source == GX_SRC_VTX ? vertex_color : x->ambient[i];
                    out->color[i][0] = mat_c[0] * (amb[0] > 1.0f ? 1.0f : amb[0]);
                    out->color[i][1] = mat_c[1] * (amb[1] > 1.0f ? 1.0f : amb[1]);
                    out->color[i][2] = mat_c[2] * (amb[2] > 1.0f ? 1.0f : amb[2]);
                }
            } else {
                const f32* amb = cc->ambient_source == GX_SRC_VTX ? vertex_color : x->ambient[i];
                light_component(cc, mat_c, amb, eye, normal, false, out->color[i]);
            }
            if (!x->lit[2u + i]) {
                if (!ac->enabled) {
                    out->color[i][3] = mat_a[3];
                } else {
                    const f32* amb = ac->ambient_source == GX_SRC_VTX ? vertex_color : x->ambient[i];
                    out->color[i][3] = mat_a[3] * (amb[3] > 1.0f ? 1.0f : amb[3]);
                }
            } else {
                const f32* amb = ac->ambient_source == GX_SRC_VTX ? vertex_color : x->ambient[i];
                light_component(ac, mat_a, amb, eye, normal, true, out->color[i]);
            }
        }
    }

    for (i = 0; i < x->texgen_count; ++i) {
        f32 tex[3] = { 0.0f, 0.0f, 1.0f };
        const VitaTexGenState* generator = &s_gx.texture_generators[i];
        if (generator->source >= GX_TG_TEX0 && generator->source <= GX_TG_TEX7) {
            const u32 k = (u32) (generator->source - GX_TG_TEX0);
            tex[0] = input->tex[k][0]; tex[1] = input->tex[k][1];
        } else if (generator->source == GX_TG_POS) {
            memcpy(tex, input->position, sizeof(tex));
        } else if (generator->source == GX_TG_NRM) {
            memcpy(tex, input->normal, sizeof(tex));
        } else if (generator->source == GX_TG_COLOR0) {
            tex[0] = out->color[0][0]; tex[1] = out->color[0][1];
        } else if (generator->source == GX_TG_COLOR1) {
            tex[0] = out->color[1][0]; tex[1] = out->color[1][1];
        }
        if (generator->matrix != GX_IDENTITY) {
            const f32 (*m)[4] = generator->matrix < GX_TEXMTX0
                ? (const f32 (*)[4]) s_gx.position_matrices[matrix_slot(generator->matrix)]
                : (generator->matrix / 3u < 20u ? (const f32 (*)[4]) s_gx.texture_matrices[generator->matrix / 3u] : NULL);
            if (m != NULL) {
                const f32 w = (generator->source == GX_TG_NRM) ? 0.0f : 1.0f;
                const f32 src2 = generator->source == GX_TG_POS || generator->source == GX_TG_NRM ? tex[2] : 1.0f;
                if (generator->type == GX_TG_MTX2x4) {
                    const f32 s0 = m[0][0] * tex[0] + m[0][1] * tex[1] + m[0][2] * src2 + m[0][3] * w;
                    const f32 t0 = m[1][0] * tex[0] + m[1][1] * tex[1] + m[1][2] * src2 + m[1][3] * w;
                    tex[0] = s0; tex[1] = t0; tex[2] = 1.0f;
                } else {
                    const f32 src[3] = { tex[0], tex[1], src2 };
                    apply_3x4(m, src, w, tex);
                }
            }
        }
        if (generator->type == GX_TG_MTX3x4 && tex[2] != 0.0f &&
            generator->post_matrix == GX_PTIDENTITY) {
            tex[0] /= tex[2]; tex[1] /= tex[2]; tex[2] = 1.0f;
        }
        if (generator->post_matrix != GX_PTIDENTITY && generator->post_matrix >= GX_PTTEXMTX0) {
            const u32 post = (generator->post_matrix - GX_PTTEXMTX0) / 3u;
            if (generator->normalize) {
                const f32 len = sqrtf(dot3(tex, tex));
                if (len > 1.0e-8f) { tex[0] /= len; tex[1] /= len; tex[2] /= len; }
            }
            if (post < 20u) {
                f32 result[3];
                apply_3x4(s_gx.post_matrices[post], tex, 1.0f, result);
                memcpy(tex, result, sizeof(tex));
                if (result[2] != 0.0f) { tex[0] /= result[2]; tex[1] /= result[2]; }
            }
        }
        out->tex[i][0] = tex[0];
        out->tex[i][1] = tex[1];
    }
}

static void fill_gxr_draw(GxrDraw* draw)
{
    u32 i;
    memset(&draw->key, 0, sizeof(draw->key));
    draw->key.stage_count = s_gx.tev_stage_count == 0 ? 1u :
        (u8) (s_gx.tev_stage_count > GXR_MAX_STAGES ? GXR_MAX_STAGES : s_gx.tev_stage_count);
    draw->key.alpha_comp[0] = s_gx.alpha_comp[0];
    draw->key.alpha_comp[1] = s_gx.alpha_comp[1];
    draw->key.alpha_ref[0] = s_gx.alpha_ref[0];
    draw->key.alpha_ref[1] = s_gx.alpha_ref[1];
    draw->key.alpha_op = s_gx.alpha_op;
    memcpy(draw->key.swap, s_gx.tev_swap_table, sizeof(draw->key.swap));
    memset(draw->texture_valid, 0, sizeof(draw->texture_valid));
    for (i = 0; i < draw->key.stage_count; ++i) {
        GxrStage* st = &draw->key.stages[i];
        const u32 map = (u32) s_gx.tev_maps[i];
        for (u32 j = 0; j < 4u; ++j) {
            st->color_in[j] = (u8) s_gx.tev_color_inputs[i][j];
            st->alpha_in[j] = (u8) s_gx.tev_alpha_inputs[i][j];
        }
        st->color_op = (u8) s_gx.tev_color_operations[i];
        st->color_bias = s_gx.tev_color_bias[i];
        st->color_scale = s_gx.tev_color_scale[i];
        st->color_clamp = s_gx.tev_color_clamp[i] ? 1u : 0u;
        st->color_out = s_gx.tev_color_out[i];
        st->alpha_op = (u8) s_gx.tev_alpha_operations[i];
        st->alpha_bias = s_gx.tev_alpha_bias[i];
        st->alpha_scale = s_gx.tev_alpha_scale[i];
        st->alpha_clamp = s_gx.tev_alpha_clamp[i] ? 1u : 0u;
        st->alpha_out = s_gx.tev_alpha_out[i];
        st->tex_map = (map & GX_TEX_DISABLE) == 0u && map < GXR_MAX_TEXMAPS ? (u8) map : 0xffu;
        {
            /* Stages that never read TEXC/TEXA must not bind a texture: their
             * texmap slot can still point at a GXTexObj that no longer exists
             * (HSD keeps some on the stack), exactly as aurora's
             * uses_texture_sample() check avoids. */
            bool samples = false;
            for (u32 j = 0; j < 4u; ++j) {
                if (st->color_in[j] == GX_CC_TEXC || st->color_in[j] == GX_CC_TEXA ||
                    st->alpha_in[j] == GX_CA_TEXA)
                    samples = true;
            }
            if (!samples) st->tex_map = 0xffu;
        }
        st->tex_coord = (u32) s_gx.tev_coordinates[i] < GXR_MAX_TEXCOORDS ? (u8) s_gx.tev_coordinates[i] : 0xffu;
        st->channel = (u8) s_gx.tev_colors[i];
        st->kcsel = (u8) s_gx.tev_kcolor_selection[i];
        st->kasel = (u8) s_gx.tev_kalpha_selection[i];
        st->swap_ras = s_gx.tev_swap_ras[i];
        st->swap_tex = s_gx.tev_swap_tex[i];
        if (st->tex_map < GXR_MAX_TEXMAPS && !draw->texture_valid[st->tex_map])
            draw->texture_valid[st->tex_map] =
                texture_source_for_map(st->tex_map, &draw->textures[st->tex_map]);
        if (st->tex_map < GXR_MAX_TEXMAPS && draw->texture_valid[st->tex_map]) {
            st->mirror = (u8) ((draw->textures[st->tex_map].wrap_s == GX_MIRROR ? 1u : 0u) |
                               (draw->textures[st->tex_map].wrap_t == GX_MIRROR ? 2u : 0u));
        }
    }
    draw->blend_mode = (u8) s_gx.blend_mode;
    draw->blend_src = (u8) s_gx.blend_source;
    draw->blend_dst = (u8) s_gx.blend_destination;
    draw->logic_op = (u8) s_gx.logic_operation;
    draw->color_update = s_gx.color_update ? 1u : 0u;
    draw->alpha_update = s_gx.alpha_update ? 1u : 0u;
#ifdef MELEE_VITA_GX_IGNORE_DEPTH
    draw->depth_compare = 0;
#else
    draw->depth_compare = s_gx.depth_compare ? 1u : 0u;
#endif
    draw->depth_function = (u8) s_gx.depth_function;
    draw->depth_write = s_gx.depth_update ? 1u : 0u;
    if (s_render_target.active) {
        /* The offscreen shadow target has no depth buffer: the pass paints
         * silhouettes in draw order. */
        draw->depth_compare = 0;
        draw->depth_write = 0;
    }
    memcpy(draw->registers, s_gx.tev_registers_f, sizeof(draw->registers));
    for (i = 0; i < 4u; ++i) gxcolor_to_float(s_gx.tev_kcolors[i], draw->konst[i]);
}

static u64 s_prof_vertex_us, s_prof_draw_us;
static u32 s_prof_vertices;

static u64 s_prof_decode_us, s_prof_fill_us;
static u32 s_prof_draws;

/* GX draws points as screen-aligned sprites: the point size is a screen-space
 * square around the vertex, and GXEnableTexOffsets spreads texture coordinates
 * across it.  GXM points are single-texel dots, so each point becomes a quad
 * here, the same way the hardware would have rasterised it.  Particle effects
 * (Bowser's fire, Mario's coins, hit sparks) are drawn this way. */
static f32 tex_offset_span(GXTexOffset offset)
{
    switch (offset) {
    case GX_TO_SIXTEENTH: return 1.0f / 16.0f;
    case GX_TO_EIGHTH: return 1.0f / 8.0f;
    case GX_TO_FOURTH: return 0.25f;
    case GX_TO_HALF: return 0.5f;
    case GX_TO_ONE: return 1.0f;
    default: return 0.0f;
    }
}

static void expand_points(const GxrXform* xform, const VitaDecodedVertex* vertices,
                          u32 count, GxrVertex* out, u16* indices)
{
    static const f32 corner[4][2] = { { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f } };
    const f32 size = s_gx.point_size / 6.0f; /* GX units are sixths of a pixel */
    const f32 span = tex_offset_span(s_gx.point_offset);
    f32 scale, offset, yscale;
    f32 half_x, half_y;
    u32 i, c, t, out_index = 0, index_out = 0;
    screen_mapping(&scale, &offset, &yscale);
    half_x = 0.5f * size * scale / 480.0f;   /* clip units per Vita pixel: 1/480 */
    half_y = 0.5f * size * yscale / 272.0f;
    for (i = 0; i < count; ++i) {
        GxrVertex base;
        build_gxr_vertex(xform, &vertices[i], &base);
        for (c = 0; c < 4u; ++c) {
            GxrVertex* v = &out[out_index + c];
            *v = base;
            v->position[0] += corner[c][0] * half_x * base.position[3];
            v->position[1] += corner[c][1] * half_y * base.position[3];
            if (span > 0.0f) {
                for (t = 0; t < GXR_MAX_TEXCOORDS; ++t) {
                    if (!s_gx.tex_offset_points[t]) continue;
                    v->tex[t][0] = base.tex[t][0] + (corner[c][0] * 0.5f + 0.5f) * span;
                    v->tex[t][1] = base.tex[t][1] + (0.5f - corner[c][1] * 0.5f) * span;
                }
            }
        }
        indices[index_out++] = (u16) (out_index);
        indices[index_out++] = (u16) (out_index + 1u);
        indices[index_out++] = (u16) (out_index + 2u);
        indices[index_out++] = (u16) (out_index);
        indices[index_out++] = (u16) (out_index + 2u);
        indices[index_out++] = (u16) (out_index + 3u);
        out_index += 4u;
    }
}

static bool submit_gxr(GXPrimitive primitive, const VitaDecodedVertex* vertices, u32 count)
{
    const u64 prof_start = sceKernelGetProcessTimeWide();
    u32 output = 0, i;
    u32 needed;
    GxrVertex* out;
    u16* idx;
    GxrDraw* draw = &s_gxr_draw;
    if (!gxr_available() || is_movie_yuv_draw()) return false;

    if (primitive == GX_TRIANGLES) needed = count / 3u * 3u;
    else if (primitive == GX_QUADS) needed = count / 4u * 6u;
    else if (primitive == GX_TRIANGLESTRIP || primitive == GX_TRIANGLEFAN)
        needed = count >= 3u ? (count - 2u) * 3u : 0u;
    else if (primitive == GX_LINES) needed = count / 2u * 2u;
    else if (primitive == GX_LINESTRIP) needed = count >= 2u ? (count - 1u) * 2u : 0u;
    else if (primitive == GX_POINTS) needed = count * 6u; /* sprite quads */
    else return false;
    if (needed == 0) return true;
    if (count > 0xffffu) return false;
    {
        const u32 vertex_count = primitive == GX_POINTS ? count * 4u : count;
        if (vertex_count > 0xffffu) return false;
        out = gxr_alloc_vertices(vertex_count);
    }
    idx = gxr_alloc_indices(needed);
    if (out == NULL || idx == NULL) return false;

    /* Transform each source vertex once straight into GPU-visible pool
     * memory and describe the primitives with a u16 index list. */
    {
        GxrXform xform;
        prepare_xform(&xform);
        if (primitive == GX_POINTS) {
            expand_points(&xform, vertices, count, out, idx);
            output = needed;
        } else
        for (i = 0; i < count; ++i) build_gxr_vertex(&xform, &vertices[i], &out[i]);
#define EMIT(index) idx[output++] = (u16) (index)
        if (primitive == GX_POINTS) { /* already expanded */ } else if (primitive == GX_TRIANGLES || primitive == GX_LINES) {
            for (i = 0; i < needed; ++i) EMIT(i);
        } else if (primitive == GX_QUADS) {
            for (i = 0; i + 3u < count; i += 4u) { EMIT(i); EMIT(i + 1u); EMIT(i + 2u); EMIT(i); EMIT(i + 2u); EMIT(i + 3u); }
        } else if (primitive == GX_TRIANGLESTRIP) {
            for (i = 2; i < count; ++i) {
                if (i & 1u) { EMIT(i - 1u); EMIT(i - 2u); } else { EMIT(i - 2u); EMIT(i - 1u); }
                EMIT(i);
            }
        } else if (primitive == GX_TRIANGLEFAN) {
            for (i = 2; i < count; ++i) { EMIT(0); EMIT(i - 1u); EMIT(i); }
        } else if (primitive == GX_LINESTRIP) {
            for (i = 1; i < count; ++i) { EMIT(i - 1u); EMIT(i); }
        } else {
            for (i = 0; i < count; ++i) EMIT(i);
        }
#undef EMIT
    }

    if (primitive <= GX_TRIANGLEFAN && primitive >= GX_QUADS &&
        primitive != GX_LINES && s_gx.cull_mode != GX_CULL_NONE) {
        u32 kept = 0;
        if (s_gx.cull_mode == GX_CULL_ALL) {
            output = 0;
        } else {
            for (i = 0; i + 2u < output; i += 3u) {
                const GxrVertex* a = &out[idx[i]];
                const GxrVertex* b = &out[idx[i + 1u]];
                const GxrVertex* c = &out[idx[i + 2u]];
                bool culled = false;
                if (a->position[3] > 0.0f && b->position[3] > 0.0f && c->position[3] > 0.0f) {
                    const f32 ax = a->position[0] / a->position[3], ay = a->position[1] / a->position[3];
                    const f32 bx = b->position[0] / b->position[3], by = b->position[1] / b->position[3];
                    const f32 cx = c->position[0] / c->position[3], cy = c->position[1] / c->position[3];
                    /* Legacy path measured area with y pointing down. */
                    const f32 area = -((bx - ax) * (cy - ay) - (by - ay) * (cx - ax));
                    culled = (s_gx.cull_mode == GX_CULL_FRONT && area >= 0.0f) ||
                             (s_gx.cull_mode == GX_CULL_BACK && area < 0.0f);
                }
                if (!culled) {
                    if (kept != i) { idx[kept] = idx[i]; idx[kept + 1u] = idx[i + 1u]; idx[kept + 2u] = idx[i + 2u]; }
                    kept += 3u;
                }
            }
            output = kept;
        }
    }
    s_prof_vertex_us += sceKernelGetProcessTimeWide() - prof_start;
    s_prof_vertices += count;
    if (output == 0) return true;

    {
        const u64 fill_start = sceKernelGetProcessTimeWide();
        fill_gxr_draw(draw);
        s_prof_fill_us += sceKernelGetProcessTimeWide() - fill_start;
    }
    if (primitive == GX_LINES || primitive == GX_LINESTRIP) {
        draw->primitive = GXR_PRIM_LINES;
        draw->line_width = (s_gx.line_width / 6.0f) * (544.0f / 480.0f);
    } else {
        draw->primitive = GXR_PRIM_TRIANGLES;
        draw->line_width = 1.0f;
    }
    {
        const u64 draw_start = sceKernelGetProcessTimeWide();
        const bool ok = gxr_draw(draw, out, idx, output);
        s_prof_draw_us += sceKernelGetProcessTimeWide() - draw_start;
        ++s_prof_draws;
        return ok;
    }
}

static void submit_decoded(GXPrimitive primitive,
                           const VitaDecodedVertex* vertices, u32 count)
{
    MeleeVitaTextureSource texture;
    MeleeVitaRenderState render_state;
    u32 triangle_vertices = 0;
    u32 output = 0;
    u32 i;
    render_state.depth_compare = s_gx.depth_compare;
    render_state.depth_function = s_gx.depth_function;
    render_state.depth_write = s_gx.depth_update;
    render_state.additive_blend = s_gx.blend_mode == GX_BM_BLEND &&
                                  s_gx.blend_destination == GX_BL_ONE;
    render_state.line_width = s_gx.line_width >= 6 ? s_gx.line_width / 6 : 1;
    render_state.point_size = s_gx.point_size >= 6 ? s_gx.point_size / 6 : 1;
    if (submit_gxr(primitive, vertices, count)) return;

    if (primitive == GX_LINES || primitive == GX_LINESTRIP || primitive == GX_POINTS) {
        u32 primitive_vertices = primitive == GX_LINESTRIP
            ? (count >= 2 ? (count - 1u) * 2u : 0u) : count;
        if (primitive == GX_LINES) primitive_vertices = count / 2u * 2u;
        if (primitive_vertices == 0 || !reserve_triangles(primitive_vertices)) return;
        if (primitive == GX_LINESTRIP) {
            for (i = 1; i < count; ++i) {
                project_vertex(&vertices[i - 1u], &s_triangle_vertices[output++]);
                project_vertex(&vertices[i], &s_triangle_vertices[output++]);
            }
        } else {
            for (i = 0; i < primitive_vertices; ++i)
                project_vertex(&vertices[i], &s_triangle_vertices[output++]);
        }
        if (primitive == GX_POINTS)
            melee_vita_gxm_draw_points(s_triangle_vertices, output, &render_state);
        else
            melee_vita_gxm_draw_lines(s_triangle_vertices, output, &render_state);
        return;
    }
    if (primitive == GX_TRIANGLES) triangle_vertices = count / 3u * 3u;
    else if (primitive == GX_QUADS) triangle_vertices = count / 4u * 6u;
    else if (primitive == GX_TRIANGLESTRIP || primitive == GX_TRIANGLEFAN)
        triangle_vertices = count >= 3u ? (count - 2u) * 3u : 0u;
    if (triangle_vertices == 0 || !reserve_triangles(triangle_vertices)) return;

#define PROJECT_INDEX(index) project_vertex(&vertices[(index)], &s_triangle_vertices[output++])
    if (primitive == GX_TRIANGLES) {
        for (i = 0; i < triangle_vertices; ++i) PROJECT_INDEX(i);
    } else if (primitive == GX_QUADS) {
        for (i = 0; i + 3u < count; i += 4u) {
            PROJECT_INDEX(i); PROJECT_INDEX(i + 1u); PROJECT_INDEX(i + 2u);
            PROJECT_INDEX(i); PROJECT_INDEX(i + 2u); PROJECT_INDEX(i + 3u);
        }
    } else if (primitive == GX_TRIANGLESTRIP) {
        for (i = 2; i < count; ++i) {
            if (i & 1u) { PROJECT_INDEX(i - 1u); PROJECT_INDEX(i - 2u); }
            else { PROJECT_INDEX(i - 2u); PROJECT_INDEX(i - 1u); }
            PROJECT_INDEX(i);
        }
    } else if (primitive == GX_TRIANGLEFAN) {
        for (i = 2; i < count; ++i) {
            PROJECT_INDEX(0); PROJECT_INDEX(i - 1u); PROJECT_INDEX(i);
        }
    }
#undef PROJECT_INDEX
    s_stats.draws++; s_stats.tris_in += output / 3u;
    for (i = 0; i < output; ++i) { if (s_triangle_vertices[i].z < s_stats.zmin) s_stats.zmin = s_triangle_vertices[i].z; if (s_triangle_vertices[i].z > s_stats.zmax) s_stats.zmax = s_triangle_vertices[i].z; }
    { const u32 before_cull = output;
    if (s_gx.cull_mode != GX_CULL_NONE) {
        u32 kept = 0;
        for (i = 0; i + 2u < output; i += 3u) {
            const MeleeVitaScreenVertex* a = &s_triangle_vertices[i];
            const MeleeVitaScreenVertex* b = a + 1;
            const MeleeVitaScreenVertex* c = a + 2;
            const f32 area = (b->x - a->x) * (c->y - a->y) -
                             (b->y - a->y) * (c->x - a->x);
            const bool culled = s_gx.cull_mode == GX_CULL_ALL ||
                (s_gx.cull_mode == GX_CULL_FRONT && area >= 0.0f) ||
                (s_gx.cull_mode == GX_CULL_BACK && area < 0.0f);
            if (!culled) {
                if (kept != i) memcpy(&s_triangle_vertices[kept], a,
                                      3u * sizeof(*a));
                kept += 3u;
            }
        }
        output = kept;
    }
    s_stats.tris_culled += (before_cull - output) / 3u; }
    if (output == 0) return;
    const MeleeVitaTextureSource* texture_source =
        current_texture_source(&texture);
    const u32 texture_stage = active_texture_stage();
    u32 tint = texture_source != NULL && texture_source->chroma_u != NULL
        ? 0xffffffffu
        : texture_source != NULL && texture_stage < GX_MAX_TEVSTAGE
        ? approximate_tev_tint(s_triangle_vertices[0].color, texture_stage)
        : 0xffffffffu;
    if (texture_source != NULL) s_stats.textured++;
    if ((tint >> 24) == 0u) s_stats.alpha0++;
    if (s_gx.blend_mode != GX_BM_BLEND) s_stats.bm_none++;
    if (s_gx.blend_mode != GX_BM_BLEND) {
        /* With blending disabled GX ignores fragment alpha when writing the
         * framebuffer. HSD materials frequently carry alpha 0 in their
         * diffuse colour, so forcing opaque output here prevents vita2d's
         * always-on alpha blend from discarding the whole draw. */
        tint |= 0xff000000u;
        for (i = 0; i < output; ++i) s_triangle_vertices[i].color |= 0xff000000u;
    }
    melee_vita_gxm_draw_triangles(s_triangle_vertices, output,
                                   texture_source, tint, &render_state);
}

static void note_value(void)
{
    if (s_gx.immediate.active) {
        ++s_gx.immediate.values_written;
    }
}

static GXAttr immediate_attribute(void)
{
    while (s_gx.immediate.attribute_cursor < GX_VA_MAX_ATTR &&
           s_gx.descriptors[s_gx.immediate.attribute_cursor] == GX_NONE)
        ++s_gx.immediate.attribute_cursor;
    return s_gx.immediate.attribute_cursor < GX_VA_MAX_ATTR
        ? (GXAttr) s_gx.immediate.attribute_cursor : GX_VA_NULL;
}

static void immediate_advance(void)
{
    ++s_gx.immediate.attribute_cursor;
    if (immediate_attribute() == GX_VA_NULL) s_gx.immediate.attribute_cursor = 0;
}

GXFifoObj* GXInit(void* base, u32 size)
{
    memset(&s_gx, 0, sizeof(s_gx));
    s_gx.fifo_state.base = base;
    s_gx.fifo_state.read_ptr = base;
    s_gx.fifo_state.write_ptr = base;
    s_gx.fifo_state.size = size;
    s_gx.cpu_fifo = &s_gx.fifo;
    s_gx.gp_fifo = &s_gx.fifo;
    s_gx.projection[0] = (f32) GX_PERSPECTIVE;
    s_gx.viewport[2] = 640.0f;
    s_gx.viewport[3] = 480.0f;
    s_gx.viewport[5] = 1.0f;
    s_gx.scissor[2] = 640;
    s_gx.scissor[3] = 480;
    s_gx.line_width = 6;
    s_gx.point_size = 6;
    s_gx.material_colors[0] = (GXColor) { 255, 255, 255, 255 };
    s_gx.material_colors[1] = (GXColor) { 255, 255, 255, 255 };
    s_gx.ambient_colors[0] = s_gx.material_colors[0];
    s_gx.ambient_colors[1] = s_gx.material_colors[1];
    s_gx.color_update = GX_TRUE;
    s_gx.alpha_update = GX_TRUE;
    {
        static const u8 swaps[4][4] = { { 0, 1, 2, 3 }, { 0, 0, 0, 3 }, { 1, 1, 1, 3 }, { 2, 2, 2, 3 } };
        memcpy(s_gx.tev_swap_table, swaps, sizeof(swaps));
    }
    s_gx.alpha_comp[0] = s_gx.alpha_comp[1] = GX_ALWAYS;
    s_gx.alpha_op = GX_AOP_AND;
    s_gx.channel_count = 1;
    for (u32 i = 0; i < GX_MAX_TEVSTAGE; ++i) {
        s_gx.tev_color_clamp[i] = s_gx.tev_alpha_clamp[i] = GX_TRUE;
        s_gx.tev_maps[i] = GX_TEXMAP_NULL;
        s_gx.tev_coordinates[i] = GX_TEXCOORD_NULL;
        s_gx.tev_colors[i] = GX_COLOR_NULL;
    }
    for (u32 i = 0; i < GX_MAX_TEXCOORD; ++i) {
        s_gx.texture_generators[i].type = GX_TG_MTX2x4;
        s_gx.texture_generators[i].source = (GXTexGenSrc) (GX_TG_TEX0 + i);
        s_gx.texture_generators[i].matrix = GX_IDENTITY;
        s_gx.texture_generators[i].post_matrix = GX_PTIDENTITY;
    }
    return &s_gx.fifo;
}

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size)
{
    (void) fifo;
    s_gx.fifo_state.base = base;
    s_gx.fifo_state.read_ptr = base;
    s_gx.fifo_state.write_ptr = base;
    s_gx.fifo_state.size = size;
}

void GXInitFifoPtrs(GXFifoObj* fifo, void* read_ptr, void* write_ptr)
{
    (void) fifo;
    s_gx.fifo_state.read_ptr = read_ptr;
    s_gx.fifo_state.write_ptr = write_ptr;
}

void GXGetFifoPtrs(GXFifoObj* fifo, void** read_ptr, void** write_ptr)
{
    (void) fifo;
    if (read_ptr != NULL) {
        *read_ptr = s_gx.fifo_state.read_ptr;
    }
    if (write_ptr != NULL) {
        *write_ptr = s_gx.fifo_state.write_ptr;
    }
}

void GXInitFifoLimits(GXFifoObj* fifo, u32 high, u32 low)
{
    (void) fifo;
    s_gx.fifo_state.high_water = high;
    s_gx.fifo_state.low_water = low;
}

void* GXGetFifoBase(const GXFifoObj* fifo)
{
    (void) fifo;
    return s_gx.fifo_state.base;
}

u32 GXGetFifoSize(const GXFifoObj* fifo)
{
    (void) fifo;
    return s_gx.fifo_state.size;
}

GXFifoObj* GXGetCPUFifo(void) { return s_gx.cpu_fifo; }
GXFifoObj* GXGetGPFifo(void) { return s_gx.gp_fifo; }
void GXSetCPUFifo(GXFifoObj* fifo) { s_gx.cpu_fifo = fifo; }
void GXSetGPFifo(GXFifoObj* fifo) { s_gx.gp_fifo = fifo; }

void GXSaveCPUFifo(GXFifoObj* fifo)
{
    if (fifo != NULL && s_gx.cpu_fifo != NULL) {
        memcpy(fifo, s_gx.cpu_fifo, sizeof(*fifo));
    }
}

void GXGetFifoStatus(GXFifoObj* fifo, GXBool* overhi, GXBool* underlow,
                     u32* count, GXBool* cpu_write, GXBool* gp_read,
                     GXBool* wrap)
{
    (void) fifo;
    if (overhi != NULL) *overhi = GX_FALSE;
    if (underlow != NULL) *underlow = GX_FALSE;
    if (count != NULL) *count = 0;
    if (cpu_write != NULL) *cpu_write = GX_TRUE;
    if (gp_read != NULL) *gp_read = GX_TRUE;
    if (wrap != NULL) *wrap = GX_FALSE;
}

void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* read_idle,
                   GXBool* command_idle, GXBool* breakpoint)
{
    if (overhi != NULL) *overhi = GX_FALSE;
    if (underlow != NULL) *underlow = GX_FALSE;
    if (read_idle != NULL) *read_idle = GX_TRUE;
    if (command_idle != NULL) *command_idle = GX_TRUE;
    if (breakpoint != NULL) *breakpoint = GX_FALSE;
}

OSThread* GXSetCurrentGXThread(void)
{
    return s_gx.current_thread;
}

OSThread* GXGetCurrentGXThread(void)
{
    return s_gx.current_thread;
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback callback)
{
    GXDrawDoneCallback previous = s_gx.draw_done_callback;
    s_gx.draw_done_callback = callback;
    return previous;
}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback callback)
{
    GXDrawSyncCallback previous = s_gx.draw_sync_callback;
    s_gx.draw_sync_callback = callback;
    return previous;
}

void GXSetDrawDone(void)
{
    s_gx.draw_pending = GX_TRUE;
    /* GXM submission is synchronous until the queued backend is connected. */
    s_gx.draw_pending = GX_FALSE;
    if (s_gx.draw_done_callback != NULL) {
        s_gx.draw_done_callback();
    }
}

void GXWaitDrawDone(void) { s_gx.draw_pending = GX_FALSE; }
void GXDrawDone(void) { GXSetDrawDone(); }
void GXFlush(void) {}
void GXPixModeSync(void) {}
void GXTexModeSync(void) {}
void GXAbortFrame(void) { s_gx.immediate.active = GX_FALSE; }
void GXSetMisc(GXMiscToken token, u32 value) { (void) token; (void) value; }

void GXSetProjection(const void* matrix, GXProjectionType type)
{
    const f32 (*m)[4] = matrix;
    s_gx.projection[0] = (f32) type;
    s_gx.projection[1] = m[0][0];
    s_gx.projection[2] = type == GX_ORTHOGRAPHIC ? m[0][3] : m[0][2];
    s_gx.projection[3] = m[1][1];
    s_gx.projection[4] = type == GX_ORTHOGRAPHIC ? m[1][3] : m[1][2];
    s_gx.projection[5] = m[2][2];
    s_gx.projection[6] = m[2][3];
}

void GXSetProjectionv(const f32* projection)
{
    if (projection != NULL) memcpy(s_gx.projection, projection, sizeof(s_gx.projection));
}

void GXGetProjectionv(f32* projection)
{
    if (projection != NULL) memcpy(projection, s_gx.projection, sizeof(s_gx.projection));
}

void GXSetViewportJitter(f32 left, f32 top, f32 width, f32 height,
                         f32 near_z, f32 far_z, u32 field)
{
    if (field == 0) top -= 0.5f;
    s_gx.viewport[0] = left;
    s_gx.viewport[1] = top;
    s_gx.viewport[2] = width;
    s_gx.viewport[3] = height;
    s_gx.viewport[4] = near_z;
    s_gx.viewport[5] = far_z;
}

void GXSetViewport(f32 left, f32 top, f32 width, f32 height,
                   f32 near_z, f32 far_z)
{
    GXSetViewportJitter(left, top, width, height, near_z, far_z, 1);
}

void GXGetViewportv(f32* viewport)
{
    if (viewport != NULL) memcpy(viewport, s_gx.viewport, sizeof(s_gx.viewport));
}

void GXSetScissor(u32 left, u32 top, u32 width, u32 height)
{
    s_gx.scissor[0] = left;
    s_gx.scissor[1] = top;
    s_gx.scissor[2] = width;
    s_gx.scissor[3] = height;
}

void GXGetScissor(u32* left, u32* top, u32* width, u32* height)
{
    if (left != NULL) *left = s_gx.scissor[0];
    if (top != NULL) *top = s_gx.scissor[1];
    if (width != NULL) *width = s_gx.scissor[2];
    if (height != NULL) *height = s_gx.scissor[3];
}

void GXSetScissorBoxOffset(s32 x, s32 y)
{
    s_gx.scissor_offset[0] = x;
    s_gx.scissor_offset[1] = y;
}

void GXSetClipMode(GXClipMode mode) { s_gx.clip_mode = mode; }
void GXSetCullMode(GXCullMode mode) { s_gx.cull_mode = mode; }
void GXGetCullMode(GXCullMode* mode) { if (mode != NULL) *mode = s_gx.cull_mode; }
void GXSetCoPlanar(GXBool enable) { (void) enable; }

void GXLoadPosMtxImm(const void* matrix, u32 id)
{
    memcpy(s_gx.position_matrices[matrix_slot(id)], matrix, sizeof(s_gx.position_matrices[0]));
}

void GXLoadNrmMtxImm(const void* matrix, u32 id)
{
    memcpy(s_gx.normal_matrices[matrix_slot(id)], matrix, sizeof(s_gx.normal_matrices[0]));
}

void GXLoadTexMtxImm(const void* matrix, u32 id, GXTexMtxType type)
{
    f32 (*destination)[4];
    unsigned slot;
    if (id >= GX_PTTEXMTX0) {
        slot = (id - GX_PTTEXMTX0) / 3u;
        if (slot >= 20u) return;
        destination = s_gx.post_matrices[slot];
    } else {
        slot = id / 3u;
        if (slot >= 20u) return;
        destination = s_gx.texture_matrices[slot];
    }
    memset(destination, 0, 12u * sizeof(f32));
    memcpy(destination, matrix, type == GX_MTX2x4 ? 8u * sizeof(f32)
                                                   : 12u * sizeof(f32));
}

void GXLoadPosMtxIndx(u16 index, u32 id) { (void) index; (void) id; }
void GXSetCurrentMtx(u32 id) { s_gx.current_matrix = id; }

void GXProject(f32 x, f32 y, f32 z, const f32 m[3][4], const f32* p,
               const f32* v, f32* sx, f32* sy, f32* sz)
{
    const f32 ex = m[0][3] + m[0][0] * x + m[0][1] * y + m[0][2] * z;
    const f32 ey = m[1][3] + m[1][0] * x + m[1][1] * y + m[1][2] * z;
    const f32 ez = m[2][3] + m[2][0] * x + m[2][1] * y + m[2][2] * z;
    f32 xc, yc, zc, wc;
    if (p[0] == 0.0f) {
        xc = ex * p[1] + ez * p[2];
        yc = ey * p[3] + ez * p[4];
        zc = p[6] + ez * p[5];
        wc = 1.0f / -ez;
    } else {
        xc = p[2] + ex * p[1];
        yc = p[4] + ey * p[3];
        zc = p[6] + ez * p[5];
        wc = 1.0f;
    }
    *sx = v[0] + v[2] * 0.5f + wc * xc * v[2] * 0.5f;
    *sy = v[1] + v[3] * 0.5f - wc * yc * v[3] * 0.5f;
    *sz = v[5] + wc * zc * (v[5] - v[4]);
}

static u32 s_vtx_state_gen = 1;
static u32 s_array_epoch = 1;
static void forget_array_hash(const void* data);

void GXClearVtxDesc(void) { memset(s_gx.descriptors, 0, sizeof(s_gx.descriptors)); ++s_vtx_state_gen; }

void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    if (valid_attr(attr) && s_gx.descriptors[attr] != type) { s_gx.descriptors[attr] = type; ++s_vtx_state_gen; }
}

void GXSetVtxDescv(GXVtxDescList* list)
{
    if (list == NULL) return;
    while (list->attr != GX_VA_NULL) {
        GXSetVtxDesc(list->attr, list->type);
        ++list;
    }
}

void GXGetVtxDesc(GXAttr attr, GXAttrType* type)
{
    if (type != NULL) *type = valid_attr(attr) ? s_gx.descriptors[attr] : GX_NONE;
}

void GXGetVtxDescv(GXVtxDescList* list)
{
    unsigned i;
    if (list == NULL) return;
    for (i = 0; i < GX_VA_MAX_ATTR; ++i) {
        list[i].attr = (GXAttr) i;
        list[i].type = s_gx.descriptors[i];
    }
    list[GX_VA_MAX_ATTR].attr = GX_VA_NULL;
    list[GX_VA_MAX_ATTR].type = GX_NONE;
}

void GXSetVtxAttrFmt(GXVtxFmt format, GXAttr attr, GXCompCnt count,
                     GXCompType type, u8 fraction)
{
    if (valid_format(format) && valid_attr(attr)) {
        VitaVtxFormat* out = &s_gx.formats[format][attr];
        if (out->count != count || out->type != type || out->fraction != fraction) ++s_vtx_state_gen;
        out->count = count;
        out->type = type;
        out->fraction = fraction;
    }
}

void GXSetVtxAttrFmtv(GXVtxFmt format, const GXVtxAttrFmtList* list)
{
    if (list == NULL) return;
    while (list->attr != GX_VA_NULL) {
        GXSetVtxAttrFmt(format, list->attr, list->cnt, list->type, list->frac);
        ++list;
    }
}

void GXGetVtxAttrFmt(GXVtxFmt format, GXAttr attr, GXCompCnt* count,
                     GXCompType* type, u8* fraction)
{
    VitaVtxFormat empty = { 0 };
    const VitaVtxFormat* value = &empty;
    if (valid_format(format) && valid_attr(attr)) value = &s_gx.formats[format][attr];
    if (count != NULL) *count = value->count;
    if (type != NULL) *type = value->type;
    if (fraction != NULL) *fraction = value->fraction;
}

void GXGetVtxAttrFmtv(GXVtxFmt format, GXVtxAttrFmtList* list)
{
    unsigned i;
    if (list == NULL) return;
    for (i = 0; i < GX_VA_MAX_ATTR; ++i) {
        list[i].attr = (GXAttr) i;
        GXGetVtxAttrFmt(format, (GXAttr) i, &list[i].cnt, &list[i].type, &list[i].frac);
    }
    list[GX_VA_MAX_ATTR].attr = GX_VA_NULL;
}

void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool little_endian)
{
    if (valid_attr(attr)) {
        if (s_gx.arrays[attr].data != data || s_gx.arrays[attr].size != size ||
            s_gx.arrays[attr].stride != stride) ++s_vtx_state_gen;
        /* Buffers are often rewritten before being bound again (skinning,
         * shape animation), so cached content hashes go stale here. */
        ++s_array_epoch;
        forget_array_hash(data);
        s_gx.arrays[attr].data = data;
        s_gx.arrays[attr].size = size;
        s_gx.arrays[attr].stride = stride;
        s_gx.arrays[attr].little_endian = little_endian;
    }
}

void GXSetLineWidth(u8 width, GXTexOffset offset) { s_gx.line_width = width; s_gx.line_offset = offset; }
void GXSetPointSize(u8 size, GXTexOffset offset) { s_gx.point_size = size; s_gx.point_offset = offset; }
void GXGetLineWidth(u8* width, GXTexOffset* offset) { if (width) *width = s_gx.line_width; if (offset) *offset = s_gx.line_offset; }
void GXGetPointSize(u8* size, GXTexOffset* offset) { if (size) *size = s_gx.point_size; if (offset) *offset = s_gx.point_offset; }
void GXInvalidateVtxCache(void) {}
void GXEnableTexOffsets(GXTexCoordID coord, GXBool lines, GXBool points)
{
    if ((unsigned) coord < GXR_MAX_TEXCOORDS) {
        s_gx.tex_offset_lines[coord] = lines ? 1u : 0u;
        s_gx.tex_offset_points[coord] = points ? 1u : 0u;
    }
}
void GXSetNumTexGens(u8 count)
{ s_gx.texture_generator_count = count > GX_MAX_TEXCOORD ? GX_MAX_TEXCOORD : count; }
void GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType type, GXTexGenSrc src,
                       u32 matrix, GXBool normalize, u32 post_matrix)
{
    if ((unsigned) dst < GX_MAX_TEXCOORD) {
        VitaTexGenState* generator = &s_gx.texture_generators[dst];
        generator->type = type;
        generator->source = src;
        generator->matrix = matrix;
        generator->normalize = normalize;
        generator->post_matrix = post_matrix;
    }
}

void GXBegin(GXPrimitive primitive, GXVtxFmt format, u16 vertices)
{
    s_gx.immediate.primitive = primitive;
    s_gx.immediate.format = format;
    s_gx.immediate.expected_vertices = vertices;
    s_gx.immediate.vertex_count = 0;
    s_gx.immediate.values_written = 0;
    s_gx.immediate.pending_count = 0;
    s_gx.immediate.attribute_cursor = 0;
    s_gx.immediate.nbt_vectors = 0;
    s_gx.immediate.has_pending_matrix = GX_FALSE;
    s_gx.immediate.active = GX_TRUE;
    if (vertices != GX_AUTO) (void) reserve_vertices(vertices);
}

void GXBeginIndexed(GXVtxFmt format, u16 vertices, const u16* indices, u32 count)
{
    (void) indices;
    (void) count;
    GXBegin(GX_TRIANGLES, format, vertices);
}

void GXEnd(void)
{
    if (s_gx.immediate.active && s_gx.immediate.vertex_count != 0) {
        const u64 t0 = sceKernelGetProcessTimeWide();
        submit_decoded(s_gx.immediate.primitive, s_decode_vertices,
                       s_gx.immediate.vertex_count);
        melee_vita_prof_add(VPZ_IMMEDIATE, sceKernelGetProcessTimeWide() - t0);
    }
    s_gx.immediate.active = GX_FALSE;
}

typedef struct VitaAttrPlan {
    GXAttr attr;
    GXAttrType descriptor;
    const VitaVtxFormat* format;
    const VitaArrayState* array;
    u32 direct_bytes;   /* bytes consumed in the stream for GX_DIRECT */
    u32 index_skip;     /* bytes consumed in the stream for indexed attrs */
    u32 source_bytes;   /* bytes read from the array for indexed attrs */
} VitaAttrPlan;

/* ------------------------------------------------------------------------
 * GPU display-list cache.  A display list plus the vertex arrays it indexes
 * is decoded once into GxrGpuVertex/u16 buffers in persistent GPU memory and
 * redrawn from there; vertex shaders do transform, lighting and texgen.  The
 * cached copy is validated every frame with a content hash of the list and of
 * each array it reads (arrays are hashed once per frame, since HSD skinning
 * and shape animation rewrite vertex buffers in place).
 * ------------------------------------------------------------------------ */

#define DL_CACHE_BUCKETS 2048u
#define ARRAY_HASH_SLOTS 512u

typedef struct DlCacheEntry {
    const void* list;
    u32 bytes;
    u32 state_hash;
    u32 content_hash;
    u32 last_frame;
    u32 validated_frame;
    GxrGpuVertex* vertices;
    u16* indices;          /* triangles, then lines, then points */
    u32 tri_count, line_count, point_count;
    u32 vertex_count;
    u8 has_mtxidx;
    u8 array_count;
    struct { u8 attr; u32 end; } ranges[GX_VA_MAX_ATTR];
    struct DlCacheEntry* next;
} DlCacheEntry;

static DlCacheEntry* s_dl_cache[DL_CACHE_BUCKETS];
static u32 s_dl_state_hash, s_dl_state_gen;
static struct { u32 hits, builds, rebuilds, fallbacks, entries; u64 hash_us; } s_dl_stats;
static struct { const void* data; u32 length; u32 hash; u32 frame; } s_array_hash[ARRAY_HASH_SLOTS];

static u32 hash_bytes(const u8* p, u32 n, u32 h)
{
    u32 i = 0;
    for (; i + 4u <= n; i += 4u) {
        u32 w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 0x9e3779b1u;
        h ^= h >> 15;
    }
    for (; i < n; ++i) { h = (h ^ p[i]) * 0x01000193u; }
    return h;
}

static u32 current_state_hash(void)
{
    if (s_dl_state_gen != s_vtx_state_gen) {
        u32 h = 0x811c9dc5u;
        for (u32 attr = 0; attr < GX_VA_MAX_ATTR; ++attr) {
            const u32 d = (u32) s_gx.descriptors[attr];
            h = hash_bytes((const u8*) &d, sizeof(d), h);
            if (d == GX_NONE) continue;
            for (u32 f = 0; f < GX_MAX_VTXFMT; ++f)
                h = hash_bytes((const u8*) &s_gx.formats[f][attr], sizeof(VitaVtxFormat), h);
            if (d != GX_DIRECT)
                h = hash_bytes((const u8*) &s_gx.arrays[attr], sizeof(VitaArrayState), h);
        }
        s_dl_state_hash = h;
        s_dl_state_gen = s_vtx_state_gen;
    }
    return s_dl_state_hash;
}

/* Vertex arrays are validated with a sparse content sample: skinning and
 * shape animation rewrite whole buffers, so 48 spread-out 8-byte windows catch
 * real changes without hashing every byte every frame. */
static u32 sample_hash(const u8* data, u32 length)
{
    if (length <= 512u) return hash_bytes(data, length, 0x2545f491u ^ length);
    {
        u32 h = 0x2545f491u ^ length;
        const u32 step = (length - 8u) / 47u;
        for (u32 i = 0; i < 48u; ++i) h = hash_bytes(data + i * step, 8u, h);
        return h;
    }
}

static u32 array_slot(const void* data)
{
    return (u32) (((uintptr_t) data >> 4) ^ ((uintptr_t) data >> 13)) & (ARRAY_HASH_SLOTS - 1u);
}

static void forget_array_hash(const void* data)
{
    const u32 slot = array_slot(data);
    if (s_array_hash[slot].data == data) s_array_hash[slot].data = NULL;
}

static u32 array_content_hash(const void* data, u32 length)
{
    const u32 slot = array_slot(data);
    if (s_array_hash[slot].data == data && s_array_hash[slot].frame == s_array_epoch &&
        s_array_hash[slot].length == length)
        return s_array_hash[slot].hash;
    {
        const u32 h = sample_hash(data, length);
        s_array_hash[slot].data = data;
        s_array_hash[slot].length = length;
        s_array_hash[slot].hash = h;
        s_array_hash[slot].frame = s_array_epoch;
        return h;
    }
}

static u32 entry_content_hash(const DlCacheEntry* e)
{
    u32 h = hash_bytes(e->list, e->bytes, 0x1234567u);
    for (u32 i = 0; i < e->array_count; ++i) {
        const VitaArrayState* array = &s_gx.arrays[e->ranges[i].attr];
        u32 length = array->size != 0 ? array->size : e->ranges[i].end;
        h ^= array_content_hash(array->data, length) + 0x9e3779b9u + (h << 6) + (h >> 2);
    }
    return h;
}

/* GPU commands for the current and previous frames may still reference a
 * buffer, so arena memory is released a few frames after it was dropped. */
#define DEFERRED_FREE_MAX 8192u
static struct { void* block; u32 frame; } s_deferred[DEFERRED_FREE_MAX];
static u32 s_deferred_count;

static void defer_free(void* block)
{
    if (block == NULL) return;
    if (s_deferred_count == DEFERRED_FREE_MAX) {
        /* Should not happen; leak rather than risk a use-after-free. */
        return;
    }
    s_deferred[s_deferred_count].block = block;
    s_deferred[s_deferred_count].frame = s_gx.copied_frames;
    ++s_deferred_count;
}

static void process_deferred_frees(void)
{
    u32 kept = 0;
    for (u32 i = 0; i < s_deferred_count; ++i) {
        if (s_gx.copied_frames - s_deferred[i].frame >= 4u) gxr_arena_free(s_deferred[i].block);
        else s_deferred[kept++] = s_deferred[i];
    }
    s_deferred_count = kept;
}

static void free_entry_buffers(DlCacheEntry* e)
{
    defer_free(e->vertices);
    defer_free(e->indices);
    e->vertices = NULL;
    e->indices = NULL;
}

static void evict_dl_cache(u32 max_age)
{
    const u32 frame = s_gx.copied_frames;
    for (u32 b = 0; b < DL_CACHE_BUCKETS; ++b) {
        DlCacheEntry** link = &s_dl_cache[b];
        while (*link != NULL) {
            DlCacheEntry* e = *link;
            if (frame - e->last_frame > max_age) {
                *link = e->next;
                free_entry_buffers(e);
                free(e);
                --s_dl_stats.entries;
                continue;
            }
            link = &e->next;
        }
    }
}

typedef struct U16Vec { u16* data; u32 count, capacity; } U16Vec;
static bool u16vec_push(U16Vec* v, u16 value)
{
    if (v->count == v->capacity) {
        u32 cap = v->capacity ? v->capacity * 2u : 1024u;
        u16* d = realloc(v->data, cap * sizeof(u16));
        if (d == NULL) return false;
        v->data = d; v->capacity = cap;
    }
    v->data[v->count++] = value;
    return true;
}

static GxrGpuVertex* s_build_vertices;
static u32 s_build_capacity;

static void to_gpu_vertex(const VitaDecodedVertex* in, GxrGpuVertex* out)
{
    memcpy(out->pos, in->position, sizeof(out->pos));
    out->mtx = (f32) in->position_matrix;
    memcpy(out->nrm, in->normal, sizeof(out->nrm));
    {
        const u32 c0 = in->has_color[0] ? in->color : 0xffffffffu;
        const u32 c1 = in->has_color[1] ? in->color1 : 0xffffffffu;
        out->c0[0] = (u8) c0; out->c0[1] = (u8) (c0 >> 8); out->c0[2] = (u8) (c0 >> 16); out->c0[3] = (u8) (c0 >> 24);
        out->c1[0] = (u8) c1; out->c1[1] = (u8) (c1 >> 8); out->c1[2] = (u8) (c1 >> 16); out->c1[3] = (u8) (c1 >> 24);
    }
    for (u32 t = 0; t < GXR_GPU_TEX; ++t) { out->tex[t][0] = in->tex[t][0]; out->tex[t][1] = in->tex[t][1]; }
}

/* Decodes one primitive's vertices into s_decode_vertices (count vertices).
 * Tracks, per indexed attribute, the furthest array byte read. */
static bool decode_primitive_vertices(const u8* stream, u32 bytes, u32* cursor_io,
                                      GXVtxFmt format, u32 count, u32* max_end,
                                      bool* has_mtxidx)
{
    typedef struct { GXAttr attr; GXAttrType descriptor; const VitaVtxFormat* format;
                     const VitaArrayState* array; u32 direct_bytes, index_skip, source_bytes; } Plan;
    Plan plan[GX_VA_MAX_ATTR];
    u32 plan_count = 0, cursor = *cursor_io;
    const u32 default_color = packed_color(s_gx.material_colors[0]);
    for (u32 attr_index = 0; attr_index < GX_VA_MAX_ATTR; ++attr_index) {
        const GXAttrType descriptor = s_gx.descriptors[attr_index];
        Plan* e;
        if (descriptor == GX_NONE) continue;
        e = &plan[plan_count++];
        e->attr = (GXAttr) attr_index;
        e->descriptor = descriptor;
        e->format = &s_gx.formats[format][attr_index];
        e->array = &s_gx.arrays[attr_index];
        e->direct_bytes = direct_bytes(e->attr, e->format);
        e->index_skip = e->source_bytes = 0;
        if (e->attr == GX_VA_PNMTXIDX) *has_mtxidx = true;
        if (descriptor != GX_DIRECT) {
            const u32 index_bytes = descriptor == GX_INDEX8 ? 1u : 2u;
            const bool nbt3 = (e->attr == GX_VA_NRM || e->attr == GX_VA_NBT) &&
                              e->format->count == GX_NRM_NBT3;
            e->index_skip = (nbt3 ? 3u : 1u) * index_bytes;
            e->source_bytes = e->attr == GX_VA_CLR0 || e->attr == GX_VA_CLR1
                ? color_bytes(e->format->type)
                : nbt3 ? 3u * component_bytes(e->format->type) : e->direct_bytes;
            if (e->array->data == NULL || e->array->stride == 0) return false;
        }
    }
    if (!reserve_vertices(count)) return false;
    for (u32 v = 0; v < count; ++v) {
        VitaDecodedVertex* vertex = &s_decode_vertices[v];
        memset(vertex, 0, sizeof(*vertex));
        vertex->normal[2] = 1.0f;
        vertex->color = default_color;
        for (u32 pi = 0; pi < plan_count; ++pi) {
            const Plan* e = &plan[pi];
            const u8* source;
            bool little_endian = false;
            if (e->descriptor == GX_DIRECT) {
                if (e->direct_bytes > bytes - cursor) return false;
                source = stream + cursor;
                cursor += e->direct_bytes;
            } else {
                const VitaArrayState* array = e->array;
                u32 array_index, offset;
                if (e->index_skip > bytes - cursor) return false;
                array_index = e->descriptor == GX_INDEX8 ? stream[cursor]
                    : (u32) stream[cursor] << 8 | stream[cursor + 1u];
                cursor += e->index_skip;
                offset = array_index * array->stride;
                if (array->size != 0 &&
                    (offset > array->size || e->source_bytes > array->size - offset)) return false;
                if (offset + e->source_bytes > max_end[e->attr]) max_end[e->attr] = offset + e->source_bytes;
                source = (const u8*) array->data + offset;
                little_endian = array->little_endian;
            }
            decode_attribute(vertex, e->attr, e->format, source, little_endian);
        }
    }
    *cursor_io = cursor;
    return true;
}

static DlCacheEntry* build_dl_entry(const void* list, u32 bytes, u32 state_hash)
{
    static U16Vec tris, lines, points;
    const u8* stream = list;
    u32 cursor = 0, total = 0;
    u32 max_end[GX_VA_MAX_ATTR] = { 0 };
    bool has_mtxidx = false;
    DlCacheEntry* e;
    tris.count = lines.count = points.count = 0;

    while (cursor + 3u <= bytes) {
        const u8 command = stream[cursor];
        const GXPrimitive primitive = (GXPrimitive) (command & 0xf8u);
        const GXVtxFmt format = (GXVtxFmt) (command & 7u);
        const u32 count = (u32) stream[cursor + 1u] << 8 | stream[cursor + 2u];
        u32 base, i;
        if (command == 0 || primitive < GX_QUADS || primitive > GX_POINTS || !valid_format(format)) break;
        cursor += 3u;
        if (total + count > 0xffffu) return NULL;
        if (!decode_primitive_vertices(stream, bytes, &cursor, format, count, max_end, &has_mtxidx))
            return NULL;
        if (total + count > s_build_capacity) {
            u32 cap = s_build_capacity ? s_build_capacity : 4096u;
            while (cap < total + count) cap *= 2u;
            GxrGpuVertex* r = realloc(s_build_vertices, cap * sizeof(GxrGpuVertex));
            if (r == NULL) return NULL;
            s_build_vertices = r; s_build_capacity = cap;
        }
        base = total;
        for (i = 0; i < count; ++i) to_gpu_vertex(&s_decode_vertices[i], &s_build_vertices[base + i]);
        total += count;
#define TRI(a, b, c) (u16vec_push(&tris, (u16) (base + (a))) && u16vec_push(&tris, (u16) (base + (b))) && u16vec_push(&tris, (u16) (base + (c))))
        if (primitive == GX_TRIANGLES) {
            for (i = 0; i + 2u < count; i += 3u) if (!TRI(i, i + 1u, i + 2u)) return NULL;
        } else if (primitive == GX_QUADS) {
            for (i = 0; i + 3u < count; i += 4u)
                if (!TRI(i, i + 1u, i + 2u) || !TRI(i, i + 2u, i + 3u)) return NULL;
        } else if (primitive == GX_TRIANGLESTRIP) {
            for (i = 2; i < count; ++i) {
                bool ok = (i & 1u) ? TRI(i - 1u, i - 2u, i) : TRI(i - 2u, i - 1u, i);
                if (!ok) return NULL;
            }
        } else if (primitive == GX_TRIANGLEFAN) {
            for (i = 2; i < count; ++i) if (!TRI(0u, i - 1u, i)) return NULL;
        } else if (primitive == GX_LINES) {
            for (i = 0; i + 1u < count; i += 2u)
                if (!u16vec_push(&lines, (u16) (base + i)) || !u16vec_push(&lines, (u16) (base + i + 1u))) return NULL;
        } else if (primitive == GX_LINESTRIP) {
            for (i = 1; i < count; ++i)
                if (!u16vec_push(&lines, (u16) (base + i - 1u)) || !u16vec_push(&lines, (u16) (base + i))) return NULL;
        } else {
            for (i = 0; i < count; ++i) if (!u16vec_push(&points, (u16) (base + i))) return NULL;
        }
#undef TRI
    }
    if (total == 0) return NULL;

    e = calloc(1, sizeof(*e));
    if (e == NULL) return NULL;
    {
        const u32 index_total = tris.count + lines.count + points.count;
        e->vertices = gxr_arena_alloc(total * sizeof(GxrGpuVertex));
        e->indices = index_total ? gxr_arena_alloc(index_total * sizeof(u16)) : NULL;
        if (e->vertices == NULL || (index_total && e->indices == NULL)) {
            free_entry_buffers(e);
            evict_dl_cache(2);
            e->vertices = gxr_arena_alloc(total * sizeof(GxrGpuVertex));
            e->indices = index_total ? gxr_arena_alloc(index_total * sizeof(u16)) : NULL;
            if (e->vertices == NULL || (index_total && e->indices == NULL)) {
                free_entry_buffers(e);
                free(e);
                return NULL;
            }
        }
        memcpy(e->vertices, s_build_vertices, total * sizeof(GxrGpuVertex));
        if (tris.count) memcpy(e->indices, tris.data, tris.count * sizeof(u16));
        if (lines.count) memcpy(e->indices + tris.count, lines.data, lines.count * sizeof(u16));
        if (points.count) memcpy(e->indices + tris.count + lines.count, points.data, points.count * sizeof(u16));
    }
    e->list = list;
    e->bytes = bytes;
    e->state_hash = state_hash;
    e->vertex_count = total;
    e->tri_count = tris.count;
    e->line_count = lines.count;
    e->point_count = points.count;
    e->has_mtxidx = has_mtxidx;
    for (u32 attr = 0; attr < GX_VA_MAX_ATTR; ++attr) {
        if (s_gx.descriptors[attr] == GX_INDEX8 || s_gx.descriptors[attr] == GX_INDEX16) {
            e->ranges[e->array_count].attr = (u8) attr;
            e->ranges[e->array_count].end = max_end[attr];
            ++e->array_count;
        }
    }
    e->content_hash = entry_content_hash(e);
    ++s_dl_stats.entries;
    return e;
}

static void copy_rows(f32 (*dst)[4], const f32 (*src)[4], u32 rows)
{
    for (u32 r = 0; r < rows; ++r) memcpy(dst[r], src[r], 4u * sizeof(f32));
}

static void fill_vertex_key_uniforms(GxrVtxKey* key, GxrVtxUniforms* u, bool has_mtxidx)
{
    const f32* v = s_gx.viewport;
    f32 scale, offset, yscale;
    memset(key, 0, sizeof(*key));
    key->has_mtxidx = has_mtxidx ? 1u : 0u;
    key->perspective = s_gx.projection[0] == (f32) GX_PERSPECTIVE ? 1u : 0u;
    key->channel_count = s_gx.channel_count > 2 ? 2u : s_gx.channel_count;
    key->texgen_count = s_gx.texture_generator_count > GXR_MAX_TEXCOORDS
        ? GXR_MAX_TEXCOORDS : s_gx.texture_generator_count;
    for (u32 i = 0; i < 4u; ++i) {
        const VitaChanCtrl* c = &s_gx.channels[i];
        GxrVtxChan* k = &key->chan[i];
        if ((i % 2u) >= key->channel_count) continue;
        k->mat_src = c->material_source;
        k->enabled = c->enabled ? 1u : 0u;
        if (k->enabled) {
            k->amb_src = c->ambient_source;
            k->lights = (u8) c->lights;
            k->diffuse = c->lights ? c->diffuse : 0u;
            k->atten = c->lights ? c->attenuation : 0u;
        }
    }
    if (has_mtxidx) {
        for (u32 m = 0; m < 10u; ++m) {
            copy_rows(u->pos + m * 3u, (const f32 (*)[4]) s_gx.position_matrices[m], 3u);
            copy_rows(u->nrm + m * 3u, (const f32 (*)[4]) s_gx.normal_matrices[m], 3u);
        }
    } else {
        /* Only the current matrix is used: it goes in slot 0. */
        const unsigned cur = matrix_slot(s_gx.current_matrix);
        copy_rows(u->pos, (const f32 (*)[4]) s_gx.position_matrices[cur], 3u);
        copy_rows(u->nrm, (const f32 (*)[4]) s_gx.normal_matrices[cur], 3u);
    }
    u->proj[0][0] = s_gx.projection[1]; u->proj[0][1] = s_gx.projection[2];
    u->proj[0][2] = s_gx.projection[3]; u->proj[0][3] = s_gx.projection[4];
    u->proj[1][0] = s_gx.projection[5]; u->proj[1][1] = s_gx.projection[6];
    screen_mapping(&scale, &offset, &yscale);
    u->proj[1][2] = (offset + scale * (v[0] + v[2] * 0.5f)) / s_render_target.half_w - 1.0f;
    u->proj[1][3] = scale * v[2] * 0.5f / s_render_target.half_w;
    u->proj[2][0] = 1.0f - yscale * (v[1] + v[3] * 0.5f) / s_render_target.half_h;
    u->proj[2][1] = yscale * v[3] * 0.5f / s_render_target.half_h;
    u->proj[2][2] = v[5];
    u->proj[2][3] = v[5] - v[4];
    u->proj[3][0] = u->proj[3][1] = u->proj[3][2] = 0.0f;
    u->proj[3][3] = (f32) matrix_slot(s_gx.current_matrix);
    for (u32 i = 0; i < key->texgen_count; ++i) {
        const VitaTexGenState* g = &s_gx.texture_generators[i];
        GxrVtxTexGen* k = &key->tg[i];
        const f32 (*m)[4] = NULL;
        k->type = g->type == GX_TG_MTX2x4 ? GX_TG_MTX2x4 : GX_TG_MTX3x4;
        k->source = (u8) g->source;
        if (g->matrix != GX_IDENTITY) {
            m = g->matrix < GX_TEXMTX0
                ? (const f32 (*)[4]) s_gx.position_matrices[matrix_slot(g->matrix)]
                : (g->matrix / 3u < 20u ? (const f32 (*)[4]) s_gx.texture_matrices[g->matrix / 3u] : NULL);
        }
        if (m != NULL) { k->has_matrix = 1u; copy_rows(u->tex + i * 3u, m, 3u); }
        if (g->post_matrix != GX_PTIDENTITY && g->post_matrix >= GX_PTTEXMTX0 &&
            (g->post_matrix - GX_PTTEXMTX0) / 3u < 20u) {
            k->has_post = 1u;
            k->normalize = g->normalize ? 1u : 0u;
            copy_rows(u->post + i * 3u, (const f32 (*)[4]) s_gx.post_matrices[(g->post_matrix - GX_PTTEXMTX0) / 3u], 3u);
        }
    }
    for (u32 l = 0; l < 8u; ++l) {
        const VitaLightObj* o = &s_gx.lights[l];
        f32 (*row)[4] = u->light + l * 5u;
        gxcolor_to_float(o->color, row[0]);
        row[1][0] = o->px; row[1][1] = o->py; row[1][2] = o->pz; row[1][3] = 1.0f;
        row[2][0] = o->nx; row[2][1] = o->ny; row[2][2] = o->nz; row[2][3] = 0.0f;
        row[3][0] = o->a0; row[3][1] = o->a1; row[3][2] = o->a2; row[3][3] = 0.0f;
        row[4][0] = o->k0; row[4][1] = o->k1; row[4][2] = o->k2; row[4][3] = 0.0f;
    }
    for (u32 i = 0; i < 2u; ++i) {
        gxcolor_to_float(s_gx.material_colors[i], u->mat[i]);
        gxcolor_to_float(s_gx.ambient_colors[i], u->amb[i]);
    }
}

static u8 current_cull(void)
{
    switch (s_gx.cull_mode) {
    case GX_CULL_FRONT: return GXR_CULL_FRONT;
    case GX_CULL_BACK: return GXR_CULL_BACK;
    case GX_CULL_ALL: return GXR_CULL_ALL;
    default: return GXR_CULL_NONE;
    }
}

static bool draw_display_list_gpu(const void* list, u32 bytes) __attribute__((unused));
static bool draw_display_list_gpu(const void* list, u32 bytes)
{
    const u32 frame = s_gx.copied_frames;
    u32 state_hash;
    u32 bucket;
    DlCacheEntry* e;
    if (!gxr_available() || is_movie_yuv_draw() || bytes < 3u) return false;
    state_hash = current_state_hash();
    bucket = (u32) (((uintptr_t) list >> 3) ^ bytes ^ state_hash) & (DL_CACHE_BUCKETS - 1u);
    for (e = s_dl_cache[bucket]; e != NULL; e = e->next)
        if (e->list == list && e->bytes == bytes && e->state_hash == state_hash) break;
    {
        const u64 t0 = sceKernelGetProcessTimeWide();
        if (e != NULL && e->validated_frame != s_array_epoch) {
            const u32 h = entry_content_hash(e);
            e->validated_frame = s_array_epoch;
            if (h != e->content_hash) {
                /* Contents changed (skinning/shape animation): rebuild. */
                DlCacheEntry** link = &s_dl_cache[bucket];
                while (*link != e) link = &(*link)->next;
                *link = e->next;
                free_entry_buffers(e);
                free(e);
                --s_dl_stats.entries;
                e = NULL;
                ++s_dl_stats.rebuilds;
            }
        }
        s_dl_stats.hash_us += sceKernelGetProcessTimeWide() - t0;
        melee_vita_prof_add(VPZ_DL_HASH, sceKernelGetProcessTimeWide() - t0);
    }
    if (e == NULL) {
        const u64 t0 = sceKernelGetProcessTimeWide();
        e = build_dl_entry(list, bytes, state_hash);
        s_prof_decode_us += sceKernelGetProcessTimeWide() - t0;
        melee_vita_prof_add(VPZ_DL_BUILD, sceKernelGetProcessTimeWide() - t0);
        if (e == NULL) { ++s_dl_stats.fallbacks; return false; }
        e->validated_frame = s_array_epoch;
        e->next = s_dl_cache[bucket];
        s_dl_cache[bucket] = e;
        ++s_dl_stats.builds;
    } else {
        ++s_dl_stats.hits;
    }
    e->last_frame = frame;

    {
        GxrVtxKey key;
        static GxrVtxUniforms uniforms;
        GxrDraw* draw = &s_gxr_draw;
        const u64 fill_start = sceKernelGetProcessTimeWide();
        const u8 cull = current_cull();
        bool ok = true;
        fill_vertex_key_uniforms(&key, &uniforms, e->has_mtxidx != 0);
        fill_gxr_draw(draw);
        s_prof_fill_us += sceKernelGetProcessTimeWide() - fill_start;
        melee_vita_prof_add(VPZ_DRAW_SETUP, sceKernelGetProcessTimeWide() - fill_start);
        {
            const u64 draw_start = sceKernelGetProcessTimeWide();
            if (e->tri_count) {
                draw->primitive = GXR_PRIM_TRIANGLES;
                draw->line_width = 1.0f;
                ok = gxr_draw_gpu(draw, &key, &uniforms, e->vertices, e->indices, e->tri_count, cull) && ok;
                ++s_prof_draws;
            }
            if (e->line_count) {
                draw->primitive = GXR_PRIM_LINES;
                draw->line_width = (s_gx.line_width / 6.0f) * (544.0f / 480.0f);
                ok = gxr_draw_gpu(draw, &key, &uniforms, e->vertices, e->indices + e->tri_count, e->line_count, GXR_CULL_NONE) && ok;
                ++s_prof_draws;
            }
            if (e->point_count) {
                draw->primitive = GXR_PRIM_POINTS;
                draw->line_width = (s_gx.point_size / 6.0f) * (544.0f / 480.0f);
                ok = gxr_draw_gpu(draw, &key, &uniforms, e->vertices, e->indices + e->tri_count + e->line_count, e->point_count, GXR_CULL_NONE) && ok;
                ++s_prof_draws;
            }
            s_prof_draw_us += sceKernelGetProcessTimeWide() - draw_start;
            melee_vita_prof_add(VPZ_GPU_SUBMIT, sceKernelGetProcessTimeWide() - draw_start);
        }
        s_prof_vertices += e->vertex_count;
        /* A shader failure falls back to the CPU path for this draw only if
         * nothing was drawn; partial success is accepted. */
        return ok;
    }
}

void GXCallDisplayList(const void* list, u32 bytes)
{
    const u8* stream = list;
    u32 cursor = 0;
    if (stream == NULL) return;
#ifndef MELEE_VITA_GX_CPU_VERTEX
    if (draw_display_list_gpu(list, bytes)) return;
#endif
    while (cursor + 3u <= bytes) {
        const u8 command = stream[cursor];
        const GXPrimitive primitive = (GXPrimitive) (command & 0xf8u);
        const GXVtxFmt format = (GXVtxFmt) (command & 7u);
        const u32 count = (u32) stream[cursor + 1u] << 8 | stream[cursor + 2u];
        const u64 decode_start = sceKernelGetProcessTimeWide();
        VitaAttrPlan plan[GX_VA_MAX_ATTR];
        u32 plan_count = 0;
        u32 vertex_index;
        u32 default_color;
        if (command == 0 || primitive < GX_QUADS || primitive > GX_POINTS ||
            !valid_format(format)) break;
        cursor += 3u;
        if (!reserve_vertices(count)) return;

        /* The vertex layout is fixed for the whole primitive, so work out the
         * active attributes and their sizes once instead of per vertex. */
        for (u32 attr_index = 0; attr_index < GX_VA_MAX_ATTR; ++attr_index) {
            const GXAttrType descriptor = s_gx.descriptors[attr_index];
            VitaAttrPlan* entry;
            if (descriptor == GX_NONE) continue;
            entry = &plan[plan_count++];
            entry->attr = (GXAttr) attr_index;
            entry->descriptor = descriptor;
            entry->format = &s_gx.formats[format][attr_index];
            entry->array = &s_gx.arrays[attr_index];
            entry->direct_bytes = direct_bytes(entry->attr, entry->format);
            if (descriptor != GX_DIRECT) {
                const u32 index_bytes = descriptor == GX_INDEX8 ? 1u : 2u;
                const bool nbt3 = (entry->attr == GX_VA_NRM || entry->attr == GX_VA_NBT) &&
                                  entry->format->count == GX_NRM_NBT3;
                entry->index_skip = (nbt3 ? 3u : 1u) * index_bytes;
                entry->source_bytes = entry->attr == GX_VA_CLR0 || entry->attr == GX_VA_CLR1
                    ? color_bytes(entry->format->type)
                    : nbt3 ? 3u * component_bytes(entry->format->type)
                           : entry->direct_bytes;
                if (entry->array->data == NULL || entry->array->stride == 0) return;
            }
        }
        default_color = packed_color(s_gx.material_colors[0]);

        for (vertex_index = 0; vertex_index < count; ++vertex_index) {
            VitaDecodedVertex* vertex = &s_decode_vertices[vertex_index];
            u32 p_index;
            memset(vertex, 0, sizeof(*vertex));
            vertex->normal[2] = 1.0f;
            vertex->color = default_color;
            for (p_index = 0; p_index < plan_count; ++p_index) {
                const VitaAttrPlan* entry = &plan[p_index];
                const u8* source;
                bool little_endian = false;
                if (entry->descriptor == GX_DIRECT) {
                    if (entry->direct_bytes > bytes - cursor) return;
                    source = stream + cursor;
                    cursor += entry->direct_bytes;
                } else {
                    const VitaArrayState* array = entry->array;
                    u32 array_index, offset;
                    if (entry->index_skip > bytes - cursor) return;
                    array_index = entry->descriptor == GX_INDEX8
                        ? stream[cursor]
                        : (u32) stream[cursor] << 8 | stream[cursor + 1u];
                    cursor += entry->index_skip;
                    offset = array_index * array->stride;
                    if (array->size != 0 &&
                        (offset > array->size || entry->source_bytes > array->size - offset)) return;
                    source = (const u8*) array->data + offset;
                    little_endian = array->little_endian;
                }
                decode_attribute(vertex, entry->attr, entry->format, source, little_endian);
            }
        }
        s_prof_decode_us += sceKernelGetProcessTimeWide() - decode_start;
        submit_decoded(primitive, s_decode_vertices, count);
    }
}

#define GX_VALUE_FN_1(name, type) void name(type a) { (void) a; note_value(); }

/* Inside GXBegin/GXEnd the GameCube FIFO is a byte stream, so sysdolphin
 * writes some vertex attributes with whichever GX helper emits the right
 * number of bytes.  psdisp sends a particle quad's indexed texture
 * coordinates with GXCmd1u8, which is why they arrived here as zeroes. */
static void immediate_indexed_position(u32 index);
static void immediate_indexed_normal(u32 index);
static void immediate_indexed_color(u32 index);
static void immediate_indexed_texcoord(u32 index);
static GXAttr immediate_attribute(void);
static void note_value(void);
static void immediate_advance(void);

static void immediate_raw_byte(u8 value)
{
    const GXAttr attr = immediate_attribute();
    if (attr == GX_VA_NULL || s_gx.descriptors[attr] != GX_INDEX8) {
        note_value();
        return;
    }
    if (attr == GX_VA_POS) immediate_indexed_position(value);
    else if (attr == GX_VA_NRM || attr == GX_VA_NBT) immediate_indexed_normal(value);
    else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) immediate_indexed_color(value);
    else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) immediate_indexed_texcoord(value);
    else { note_value(); immediate_advance(); }
}
#define GX_VALUE_FN_2(name, type) void name(type a, type b) { (void) a; (void) b; note_value(); }
#define GX_VALUE_FN_3(name, type) void name(type a, type b, type c) { (void) a; (void) b; (void) c; note_value(); }
#define GX_VALUE_FN_4(name, type) void name(type a, type b, type c, type d) { (void) a; (void) b; (void) c; (void) d; note_value(); }

static VitaDecodedVertex* immediate_vertex(void)
{
    if (!s_gx.immediate.active || s_gx.immediate.vertex_count == 0) return NULL;
    return &s_decode_vertices[s_gx.immediate.vertex_count - 1u];
}

/* GX vertex data is a byte stream: the helper a caller picks says how many
 * bytes it writes, not which attribute they belong to.  sysdolphin relies on
 * that -- psdisp writes particle positions with GXTexCoord1f32 and indexed
 * texture coordinates with GXCmd1u8 -- so components are routed by the vertex
 * descriptor, exactly as the hardware would consume them. */
static u32 attribute_float_components(GXAttr attr)
{
    const VitaVtxFormat* format = valid_attr(attr) ? &s_gx.formats[s_gx.immediate.format][attr] : NULL;
    if (attr == GX_VA_POS) return format != NULL ? attribute_components(attr, format->count) : 3u;
    if (attr == GX_VA_NRM) return 3u;
    if (attr == GX_VA_NBT) return 9u;
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7)
        return format != NULL ? attribute_components(attr, format->count) : 2u;
    return 1u;
}

static void immediate_commit_components(GXAttr attr, const f32* v, u32 count)
{
    VitaDecodedVertex* vertex;
    if (attr == GX_VA_POS) {
        if (!reserve_vertices(s_gx.immediate.vertex_count + 1u)) return;
        vertex = &s_decode_vertices[s_gx.immediate.vertex_count++];
        memset(vertex, 0, sizeof(*vertex));
        vertex->position[0] = v[0];
        vertex->position[1] = count > 1u ? v[1] : 0.0f;
        vertex->position[2] = count > 2u ? v[2] : 0.0f;
        vertex->normal[2] = 1.0f;
        vertex->color = packed_color(s_gx.material_colors[0]);
        if (s_gx.immediate.has_pending_matrix) {
            vertex->position_matrix = s_gx.immediate.pending_matrix;
            vertex->has_position_matrix = 1;
            s_gx.immediate.has_pending_matrix = GX_FALSE;
        }
        return;
    }
    vertex = immediate_vertex();
    if (vertex == NULL) return;
    if (attr == GX_VA_NRM || attr == GX_VA_NBT) {
        vertex->normal[0] = v[0];
        vertex->normal[1] = count > 1u ? v[1] : 0.0f;
        vertex->normal[2] = count > 2u ? v[2] : 0.0f;
    } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        const u32 index = (u32) (attr - GX_VA_TEX0);
        vertex->tex[index][0] = v[0];
        vertex->tex[index][1] = count > 1u ? v[1] : 0.0f;
        if (index == 0) { vertex->texture[0] = vertex->tex[0][0]; vertex->texture[1] = vertex->tex[0][1]; }
    }
}

static void immediate_float(f32 value)
{
    GXAttr attr;
    u32 needed;
    if (!s_gx.immediate.active) { note_value(); return; }
    attr = immediate_attribute();
    if (attr == GX_VA_NULL || s_gx.descriptors[attr] != GX_DIRECT) { note_value(); return; }
    needed = attribute_float_components(attr);
    if (needed > sizeof(s_gx.immediate.pending_components) / sizeof(f32))
        needed = sizeof(s_gx.immediate.pending_components) / sizeof(f32);
    if (s_gx.immediate.pending_count < needed)
        s_gx.immediate.pending_components[s_gx.immediate.pending_count++] = value;
    if (s_gx.immediate.pending_count < needed) return;
    immediate_commit_components(attr, s_gx.immediate.pending_components, needed);
    s_gx.immediate.pending_count = 0;
    note_value();
    immediate_advance();
}

static f32 immediate_component_scaled(s32 value)
{
    const GXAttr attr = immediate_attribute();
    const u8 fraction = valid_attr(attr) ? s_gx.formats[s_gx.immediate.format][attr].fraction : 0u;
    return (f32) value / (f32) (1u << fraction);
}

static void immediate_position(f32 x, f32 y, f32 z)
{
    VitaDecodedVertex* vertex;
    if (!s_gx.immediate.active ||
        !reserve_vertices(s_gx.immediate.vertex_count + 1u)) return;
    vertex = &s_decode_vertices[s_gx.immediate.vertex_count++];
    memset(vertex, 0, sizeof(*vertex));
    vertex->position[0] = x; vertex->position[1] = y; vertex->position[2] = z;
    vertex->normal[2] = 1.0f;
    vertex->color = packed_color(s_gx.material_colors[0]);
    if (s_gx.immediate.has_pending_matrix) {
        vertex->position_matrix = s_gx.immediate.pending_matrix;
        vertex->has_position_matrix = 1;
        s_gx.immediate.has_pending_matrix = GX_FALSE;
    }
    note_value();
    immediate_advance();
}

static f32 immediate_scaled(s32 value, GXAttr attr)
{
    const u8 fraction = s_gx.formats[s_gx.immediate.format][attr].fraction;
    return (f32) value / (f32) (1u << fraction);
}

void GXPosition3f32(f32 x, f32 y, f32 z) { immediate_float(x); immediate_float(y); immediate_float(z); }
void GXPosition3u16(u16 x, u16 y, u16 z) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); immediate_float(immediate_component_scaled(z)); }
void GXPosition3s16(s16 x, s16 y, s16 z) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); immediate_float(immediate_component_scaled(z)); }
void GXPosition3u8(u8 x, u8 y, u8 z) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); immediate_float(immediate_component_scaled(z)); }
void GXPosition3s8(s8 x, s8 y, s8 z) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); immediate_float(immediate_component_scaled(z)); }
void GXPosition2f32(f32 x, f32 y) { immediate_float(x); immediate_float(y); }
void GXPosition2u16(u16 x, u16 y) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); }
void GXPosition2s16(s16 x, s16 y) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); }
void GXPosition2u8(u8 x, u8 y) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); }
void GXPosition2s8(s8 x, s8 y) { immediate_float(immediate_component_scaled(x)); immediate_float(immediate_component_scaled(y)); }

static void immediate_indexed_position(u32 index)
{
    const VitaArrayState* array = &s_gx.arrays[GX_VA_POS];
    const VitaVtxFormat* format = &s_gx.formats[s_gx.immediate.format][GX_VA_POS];
    VitaDecodedVertex decoded;
    u32 offset;
    if (array->data == NULL || array->stride == 0 || index > UINT32_MAX / array->stride) return;
    offset = index * array->stride;
    if (array->size != 0 && offset >= array->size) return;
    memset(&decoded, 0, sizeof(decoded));
    decode_attribute(&decoded, GX_VA_POS, format, (const u8*) array->data + offset, array->little_endian);
    immediate_position(decoded.position[0], decoded.position[1], decoded.position[2]);
}
void GXPosition1x16(u16 index) { immediate_indexed_position(index); }
void GXPosition1x8(u8 index) { immediate_indexed_position(index); }

static void immediate_normal(f32 x, f32 y, f32 z)
{
    VitaDecodedVertex* v = immediate_vertex();
    GXAttr attr = immediate_attribute();
    if (v != NULL && s_gx.immediate.nbt_vectors == 0) {
        v->normal[0] = x; v->normal[1] = y; v->normal[2] = z;
    }
    note_value();
    if (attr == GX_VA_NBT && ++s_gx.immediate.nbt_vectors < 3) return;
    s_gx.immediate.nbt_vectors = 0;
    immediate_advance();
}
void GXNormal3f32(f32 x, f32 y, f32 z) { immediate_float(x); immediate_float(y); immediate_float(z); }
void GXNormal3s16(s16 x, s16 y, s16 z) { immediate_normal(immediate_scaled(x, GX_VA_NRM), immediate_scaled(y, GX_VA_NRM), immediate_scaled(z, GX_VA_NRM)); }
void GXNormal3s8(s8 x, s8 y, s8 z) { immediate_normal(immediate_scaled(x, GX_VA_NRM), immediate_scaled(y, GX_VA_NRM), immediate_scaled(z, GX_VA_NRM)); }
static void immediate_indexed_normal(u32 index)
{
    GXAttr attr = immediate_attribute();
    const VitaArrayState* array = valid_attr(attr) ? &s_gx.arrays[attr] : NULL;
    const VitaVtxFormat* format = valid_attr(attr) ? &s_gx.formats[s_gx.immediate.format][attr] : NULL;
    VitaDecodedVertex decoded;
    if (array == NULL || format == NULL || array->data == NULL || array->stride == 0 ||
        index > UINT32_MAX / array->stride) { note_value(); immediate_advance(); return; }
    u32 offset = index * array->stride;
    if (array->size != 0 && offset >= array->size) { note_value(); immediate_advance(); return; }
    memset(&decoded, 0, sizeof(decoded));
    decode_attribute(&decoded, attr, format, (const u8*) array->data + offset, array->little_endian);
    immediate_normal(decoded.normal[0], decoded.normal[1], decoded.normal[2]);
}
void GXNormal1x16(u16 index) { immediate_indexed_normal(index); }
void GXNormal1x8(u8 index) { immediate_indexed_normal(index); }

static void immediate_color(u32 color)
{
    VitaDecodedVertex* v = immediate_vertex();
    const GXAttr attr = immediate_attribute();
    if (v != NULL) {
        if (attr == GX_VA_CLR1) { v->color1 = color; v->has_color[1] = 1; }
        else { v->color = color; v->has_color[0] = 1; }
    }
    note_value();
    immediate_advance();
}
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { immediate_color((u32) r | (u32) g << 8 | (u32) b << 16 | (u32) a << 24); }
void GXColor3u8(u8 r, u8 g, u8 b) { GXColor4u8(r, g, b, 255); }
void GXColor1u32(u32 color) { immediate_color(color); }
void GXColor1u16(u16 color) { u8 bytes[2] = { (u8) (color >> 8), (u8) color }; immediate_color(decode_color(bytes, GX_RGB565, false)); }
static void immediate_indexed_color(u32 index)
{
    GXAttr attr = immediate_attribute();
    const VitaArrayState* array = valid_attr(attr) ? &s_gx.arrays[attr] : NULL;
    const VitaVtxFormat* format = valid_attr(attr) ? &s_gx.formats[s_gx.immediate.format][attr] : NULL;
    if (array != NULL && format != NULL && array->data != NULL && array->stride != 0 &&
        index <= UINT32_MAX / array->stride) {
        u32 offset = index * array->stride;
        if (array->size == 0 || offset + color_bytes(format->type) <= array->size)
            immediate_color(decode_color((const u8*) array->data + offset, format->type, array->little_endian));
        else { note_value(); immediate_advance(); }
    } else { note_value(); immediate_advance(); }
}
void GXColor1x16(u16 index) { immediate_indexed_color(index); }
void GXColor1x8(u8 index) { immediate_indexed_color(index); }

static void immediate_texcoord(f32 s, f32 t)
{
    VitaDecodedVertex* v = immediate_vertex();
    const GXAttr attr = immediate_attribute();
    u32 index = attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7 ? (u32) (attr - GX_VA_TEX0) : 0u;
    if (v != NULL) {
        v->tex[index][0] = s; v->tex[index][1] = t;
        if (index == 0) { v->texture[0] = s; v->texture[1] = t; }
    }
    note_value();
    immediate_advance();
}
void GXTexCoord2f32(f32 s, f32 t) { immediate_float(s); immediate_float(t); }
void GXTexCoord2u16(u16 s, u16 t) { immediate_float(immediate_component_scaled(s)); immediate_float(immediate_component_scaled(t)); }
void GXTexCoord2s16(s16 s, s16 t) { immediate_float(immediate_component_scaled(s)); immediate_float(immediate_component_scaled(t)); }
void GXTexCoord2u8(u8 s, u8 t) { immediate_float(immediate_component_scaled(s)); immediate_float(immediate_component_scaled(t)); }
void GXTexCoord2s8(s8 s, s8 t) { immediate_float(immediate_component_scaled(s)); immediate_float(immediate_component_scaled(t)); }
void GXTexCoord1f32(f32 s) { immediate_float(s); }
void GXTexCoord1u16(u16 s) { immediate_float(immediate_component_scaled(s)); }
void GXTexCoord1s16(s16 s) { immediate_float(immediate_component_scaled(s)); }
void GXTexCoord1u8(u8 s)
{
    GXAttr attr = immediate_attribute();
    if (attr <= GX_VA_TEX7MTXIDX) {
        if (attr == GX_VA_PNMTXIDX) {
            s_gx.immediate.pending_matrix = s;
            s_gx.immediate.has_pending_matrix = GX_TRUE;
        }
        note_value();
        immediate_advance();
    } else immediate_texcoord(immediate_scaled(s, attr), 0.0f);
}
void GXTexCoord1s8(s8 s) { immediate_float(immediate_component_scaled(s)); }

static void immediate_indexed_texcoord(u32 index)
{
    GXAttr attr = immediate_attribute();
    const VitaArrayState* array = valid_attr(attr) ? &s_gx.arrays[attr] : NULL;
    const VitaVtxFormat* format = valid_attr(attr) ? &s_gx.formats[s_gx.immediate.format][attr] : NULL;
    VitaDecodedVertex decoded;
    if (array == NULL || format == NULL || array->data == NULL || array->stride == 0 ||
        index > UINT32_MAX / array->stride) { note_value(); immediate_advance(); return; }
    u32 offset = index * array->stride;
    if (array->size != 0 && offset >= array->size) { note_value(); immediate_advance(); return; }
    memset(&decoded, 0, sizeof(decoded));
    decode_attribute(&decoded, attr, format, (const u8*) array->data + offset, array->little_endian);
    {
        const u32 index = attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7 ? (u32) (attr - GX_VA_TEX0) : 0u;
        immediate_texcoord(decoded.tex[index][0], decoded.tex[index][1]);
    }
}
void GXTexCoord1x16(u16 index) { immediate_indexed_texcoord(index); }
void GXTexCoord1x8(u8 index) { immediate_indexed_texcoord(index); }

void GXCmd1u8(u8 a) { if (s_gx.immediate.active) immediate_raw_byte(a); else note_value(); }
GX_VALUE_FN_1(GXCmd1u16, u16)
GX_VALUE_FN_1(GXCmd1u32, u32)
GX_VALUE_FN_1(GXCmd1u64, u64)
GX_VALUE_FN_1(GXParam1u8, u8)
GX_VALUE_FN_1(GXParam1u16, u16)
GX_VALUE_FN_1(GXParam1u32, u32)
GX_VALUE_FN_1(GXParam1s8, s8)
GX_VALUE_FN_1(GXParam1s16, s16)
GX_VALUE_FN_1(GXParam1s32, s32)
GX_VALUE_FN_1(GXParam1f32, f32)
GX_VALUE_FN_3(GXParam3f32, f32)
GX_VALUE_FN_4(GXParam4f32, f32)

void GXSetCopyClear(GXColor color, u32 depth) { s_gx.clear_color = color; s_gx.clear_depth = depth; }
void GXSetDispCopySrc(u16 left, u16 top, u16 width, u16 height)
{ s_gx.copy_source[0] = left; s_gx.copy_source[1] = top; s_gx.copy_source[2] = width; s_gx.copy_source[3] = height; }
void GXSetDispCopyDst(u16 width, u16 height) { s_gx.copy_width = width; s_gx.copy_height = height; }
u32 GXSetDispCopyYScale(f32 scale) { return (u32) (s_gx.copy_source[3] * scale); }

#ifndef MELEE_VITA_RELEASE
void melee_vita_prof_reset_window(void)
{
    memset(s_vpz_us, 0, sizeof(s_vpz_us));
    memset(s_vpz_calls, 0, sizeof(s_vpz_calls));
    s_dl_stats.hits = 0;
    s_dl_stats.builds = 0;
    s_dl_stats.rebuilds = 0;
    s_dl_stats.fallbacks = 0;
    s_dl_stats.hash_us = 0;
    s_prof_decode_us = 0;
    s_prof_fill_us = 0;
    s_prof_draw_us = 0;
    s_prof_vertices = 0;
    s_prof_draws = 0;
}

void melee_vita_prof_log_window(const char* label)
{
    char line[1024];
    size_t n = snprintf(line, sizeof(line), "%s", label);
    u64 gx_total = 0;
    for (u32 z = 0; z < VPZ_COUNT; ++z) {
        n += snprintf(line + n, sizeof(line) - n, " %s=%.1fms/%u",
                      k_vpz_names[z], s_vpz_us[z] / 1000.0,
                      s_vpz_calls[z]);
        if (z >= VPZ_COPYTEX && z < VPZ_AUDIO_MIX) gx_total += s_vpz_us[z];
    }
    snprintf(line + n, sizeof(line) - n,
             " hsd_other=%.1fms dl=%u/%u/%u/%u decode=%.1fms fill=%.1fms"
             " draws=%u verts=%u",
             ((double) s_vpz_us[VPZ_GOBJ_RENDER] - (double) gx_total) / 1000.0,
             s_dl_stats.hits, s_dl_stats.builds, s_dl_stats.rebuilds,
             s_dl_stats.fallbacks, s_prof_decode_us / 1000.0,
             s_prof_fill_us / 1000.0, s_prof_draws, s_prof_vertices);
    melee_vita_log_info("%s", line);
}
#endif

void GXCopyDisp(void* destination, GXBool clear)
{
    const u32 color = (u32) s_gx.clear_color.r |
                      (u32) s_gx.clear_color.g << 8 |
                      (u32) s_gx.clear_color.b << 16 |
                      (u32) s_gx.clear_color.a << 24;
    (void) destination;
    (void) clear;
    ++s_gx.copied_frames;
    ++s_array_epoch;
    process_deferred_frees();
    {
        static u64 last_us, sum_us, max_us;
        const u64 now_us = sceKernelGetProcessTimeWide();
        if (last_us != 0) {
            const u64 delta = now_us - last_us;
            sum_us += delta;
            if (delta > max_us) max_us = delta;
        }
        last_us = now_us;
        if ((s_gx.copied_frames % 120u) == 0u) {
            extern u64 g_melee_vita_vi_wait_us;
            extern u32 g_melee_vita_vi_calls;
            extern u64 g_melee_vita_update_us, g_melee_vita_render_us;
            extern u32 g_melee_vita_update_ticks;
            melee_vita_log_info("[PERF] display_fps=%.1f game_fps=%.1f (60 = full speed) update=%.1fms render=%.1fms ticks/frame=%.2f", sum_us > 0 ? 120.0 * 1000000.0 / sum_us : 0.0, sum_us > 0 ? g_melee_vita_update_ticks * 1000000.0 / sum_us : 0.0, g_melee_vita_update_us / 120.0 / 1000.0, g_melee_vita_render_us / 120.0 / 1000.0, g_melee_vita_update_ticks / 120.0);
            g_melee_vita_update_us = g_melee_vita_render_us = 0;
            g_melee_vita_update_ticks = 0;
            {
                char line[1024];
                size_t n = snprintf(line, sizeof(line), "[PROF] ms/frame:");
                u64 gx_total = 0;
                for (u32 z = 0; z < VPZ_COUNT; ++z) {
                    n += snprintf(line + n, sizeof(line) - n, " %s=%.1f", k_vpz_names[z], s_vpz_us[z] / 120.0 / 1000.0);
                    if (z >= VPZ_COPYTEX && z < VPZ_AUDIO_MIX) gx_total += s_vpz_us[z];
                }
                n += snprintf(line + n, sizeof(line) - n, " | hsd_other=%.1f copies=%u uploads=%u",
                              ((double) s_vpz_us[VPZ_GOBJ_RENDER] - (double) gx_total) / 120.0 / 1000.0,
                              s_vpz_calls[VPZ_COPYTEX], s_vpz_calls[VPZ_TEXUPLOAD]);
                melee_vita_log_info("%s", line);
                memset(s_vpz_us, 0, sizeof(s_vpz_us));
                memset(s_vpz_calls, 0, sizeof(s_vpz_calls));
            }
            melee_vita_log_info("[DLCACHE] hits/frame=%u builds=%u rebuilds=%u fallbacks=%u entries=%u hash=%.1fms",
                                s_dl_stats.hits / 120u, s_dl_stats.builds, s_dl_stats.rebuilds,
                                s_dl_stats.fallbacks, s_dl_stats.entries, s_dl_stats.hash_us / 120.0 / 1000.0);
            s_dl_stats.hits = s_dl_stats.builds = s_dl_stats.rebuilds = s_dl_stats.fallbacks = 0;
            s_dl_stats.hash_us = 0;
            evict_dl_cache(600u);
            melee_vita_log_info("[GXPROF] decode=%.1fms fill=%.1fms draws/frame=%u",
                                s_prof_decode_us / 120.0 / 1000.0,
                                s_prof_fill_us / 120.0 / 1000.0, s_prof_draws / 120u);
            s_prof_decode_us = s_prof_fill_us = 0;
            s_prof_draws = 0;
            melee_vita_log_info("[FRAMETIME] avg=%.1fms max=%.1fms gx_vertex=%.1fms gx_draw=%.1fms vi_wait=%.1fms vi_calls/frame=%.2f verts/frame=%u",
                                sum_us / 120.0 / 1000.0, max_us / 1000.0,
                                s_prof_vertex_us / 120.0 / 1000.0,
                                s_prof_draw_us / 120.0 / 1000.0,
                                g_melee_vita_vi_wait_us / 120.0 / 1000.0,
                                g_melee_vita_vi_calls / 120.0,
                                s_prof_vertices / 120u);
            g_melee_vita_vi_wait_us = 0; g_melee_vita_vi_calls = 0;
            s_prof_vertex_us = s_prof_draw_us = 0; s_prof_vertices = 0;
            sum_us = 0; max_us = 0;
        }
    }
    if ((s_gx.copied_frames % 300u) == 1u) gxr_log_stats();
    if ((s_gx.copied_frames % 120u) == 1u) {
        melee_vita_log_info("[GXSTAT] frame=%u draws=%u tris=%u culled=%u textured=%u alpha0=%u bm_none=%u z=[%.3f,%.3f] cull=%d proj=%.0f",
            s_gx.copied_frames, s_stats.draws, s_stats.tris_in, s_stats.tris_culled, s_stats.textured, s_stats.alpha0, s_stats.bm_none, s_stats.zmin, s_stats.zmax, (int) s_gx.cull_mode, s_gx.projection[0]);
    }
    memset(&s_stats, 0, sizeof(s_stats)); s_stats.zmin = 1e9f; s_stats.zmax = -1e9f;
    melee_vita_gxm_present(color);
}
void GXSetDispCopyGamma(GXGamma gamma) { (void) gamma; }
void GXSetCopyClamp(GXFBClamp clamp) { (void) clamp; }
void GXSetCopyFilter(GXBool aa, u8 pattern[12][2], GXBool vertical, u8 filter[7])
{ (void) aa; (void) pattern; (void) vertical; (void) filter; }
void GXSetPixelFmt(GXPixelFmt color, GXZFmt16 depth) { (void) color; (void) depth; }
#ifndef MELEE_VITA_COPY_INTERVAL
#define MELEE_VITA_COPY_INTERVAL 4u
#endif
static u16 s_tex_copy_src[4] = { 0, 0, 640, 480 };
void* g_melee_vita_last_copy_dst;
void GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height)
{ s_tex_copy_src[0] = left; s_tex_copy_src[1] = top; s_tex_copy_src[2] = width; s_tex_copy_src[3] = height; }
static struct { u16 width, height; u32 format; GXBool mipmap; } s_tex_copy_dst;

void GXSetTexCopyDst(u16 width, u16 height, GXTexFmt format, GXBool mipmap)
{
    s_tex_copy_dst.width = width;
    s_tex_copy_dst.height = height;
    s_tex_copy_dst.format = (u32) format;
    s_tex_copy_dst.mipmap = mipmap;
}

/* GXCopyTex: like aurora, the copy becomes a GPU texture keyed by the
 * destination pointer.  The scene is flushed, the requested EFB rectangle is
 * sampled from the Vita back buffer into that texture, and the scene resumes.
 * HSD shadow maps (GX_CTF_R4) stay fully lit because the shadow pass itself is
 * skipped on Vita. */
static void copy_tex_impl(void* destination, GXBool clear);
/* Shadow maps: HSD draws silhouettes and copies them out of the frame.  Here
 * the pass is given its own render target, so the map never depends on what
 * else happened to be on screen. */
void melee_vita_gx_begin_shadow(void* key, u32 width, u32 height)
{
    struct vita2d_texture* target;
    if (key == NULL || width == 0u || height == 0u || width > 1024u || height > 1024u) return;
    target = melee_vita_gxm_copy_texture(key, width, height, NULL);
    if (target == NULL) return;
    g_melee_vita_last_copy_dst = key;
    s_render_target.active = 1u;
    s_render_target.half_w = (f32) width * 0.5f;
    s_render_target.half_h = (f32) height * 0.5f;
    melee_vita_gxm_begin_target(target, width, height, 0xffffffffu);
}

void melee_vita_gx_end_shadow(void)
{
    if (!s_render_target.active) return;
    s_render_target.active = 0u;
    s_render_target.half_w = 480.0f;
    s_render_target.half_h = 272.0f;
    melee_vita_gxm_end_target();
}

void GXCopyTex(void* destination, GXBool clear)
{
    const u64 t0 = sceKernelGetProcessTimeWide();
    copy_tex_impl(destination, clear);
    melee_vita_prof_add(VPZ_COPYTEX, sceKernelGetProcessTimeWide() - t0);
}

static void copy_tex_impl(void* destination, GXBool clear)
{
    u32 dst_w, dst_h;
    bool target_created;
    struct vita2d_texture* target;
    if (destination == NULL || s_tex_copy_dst.width == 0 || s_tex_copy_dst.height == 0) return;
    dst_w = s_tex_copy_dst.width;
    dst_h = s_tex_copy_dst.height;
    g_melee_vita_last_copy_dst = destination;
    {
        static u32 logged;
        static u32 window;
        if (window != s_gx.copied_frames / 600u) { window = s_gx.copied_frames / 600u; logged = 0; }
        if (logged++ < 4u)
            melee_vita_log_info("[GXCOPY] dst=%p %ux%u fmt=0x%x mip=%u src=%u,%u %ux%u clear=%u",
                                destination, dst_w, dst_h, (unsigned) s_tex_copy_dst.format,
                                (unsigned) s_tex_copy_dst.mipmap, s_tex_copy_src[0], s_tex_copy_src[1],
                                s_tex_copy_src[2], s_tex_copy_src[3], (unsigned) clear);
    }
    /* GX_CTF_R4 is an HSD shadow map; with the shadow pass off, leave it
     * fully lit. */
    if (s_tex_copy_dst.format == 0x20u) {
        u32 size = GXGetTexBufferSize(dst_w, dst_h, GX_TF_I4, GX_FALSE, 0);
        if (size != 0 && size <= 4u * 1024u * 1024u) memset(destination, 0xff, size);
        return;
    }
    if (dst_w > 1024u || dst_h > 1024u) return;
    target = melee_vita_gxm_copy_texture(destination, dst_w, dst_h,
                                         &target_created);
    if (target == NULL) return;
    {
        /* Each copy forces the GPU to finish the scene so far (tens of ms).
         * Live screens such as the Pokemon Stadium monitor are refreshed at a
         * reduced rate; the previous copy stays bound in between. */
        static struct { const void* key; u32 frame; } recent[8];
        u32 slot = 0, i;
        for (i = 0; i < 8u; ++i) {
            if (recent[i].key == destination) { slot = i; break; }
            if (recent[i].frame < recent[slot].frame) slot = i;
        }
        bool throttle =
            !clear ||
            (dst_w == 640u && dst_h == 480u &&
             s_tex_copy_dst.format == GX_TF_RGB5A3 &&
             s_tex_copy_src[0] == 0u && s_tex_copy_src[1] == 0u &&
             s_tex_copy_src[2] == 640u && s_tex_copy_src[3] == 480u);
        if (throttle && !target_created && i < 8u &&
            s_gx.copied_frames - recent[slot].frame <
                MELEE_VITA_COPY_INTERVAL)
            return;
        recent[slot].key = destination;
        recent[slot].frame = s_gx.copied_frames;
    }
    {
        f32 scale, offset, yscale;
        screen_mapping(&scale, &offset, &yscale);
        /* Only the shadow pass needs its rectangle wiped afterwards (its
         * silhouettes would otherwise show on screen).  Other copies are of
         * scene content that is drawn again anyway, and blanking those
         * rectangles punched holes in the frame. */
        /* Nothing is cleared after a copy: the scene is drawn over these
         * rectangles afterwards, and wiping them removed content other copies
         * (the Pokemon Stadium screen) still needed. */
        const int clear_region = 0;
        (void) clear;
        melee_vita_gxm_queue_copy(target, dst_w, dst_h,
                                  offset + s_tex_copy_src[0] * scale, s_tex_copy_src[1] * yscale,
                                  s_tex_copy_src[2] * scale / (f32) dst_w,
                                  s_tex_copy_src[3] * yscale / (f32) dst_h, clear_region);
    }
}

u16 GXGetNumXfbLines(u16 height, f32 scale) { return (u16) (height * scale); }
f32 GXGetYScaleFactor(u16 efb_height, u16 xfb_height)
{ return efb_height != 0 ? (f32) xfb_height / (f32) efb_height : 1.0f; }

void GXAdjustForOverscan(GXRenderModeObj* input, GXRenderModeObj* output, u16 horizontal, u16 vertical)
{
    if (input == NULL || output == NULL) return;
    *output = *input;
    if (output->fbWidth > horizontal * 2u) output->fbWidth -= horizontal * 2u;
    if (output->efbHeight > vertical * 2u) output->efbHeight -= vertical * 2u;
    if (output->xfbHeight > vertical * 2u) output->xfbHeight -= vertical * 2u;
    output->viXOrigin += horizontal;
    output->viYOrigin += vertical;
}

static VitaTexObj* tex_obj(GXTexObj* object) { return (VitaTexObj*) object; }
static const VitaTexObj* const_tex_obj(const GXTexObj* object) { return (const VitaTexObj*) object; }
static VitaTlutObj* tlut_obj(GXTlutObj* object) { return (VitaTlutObj*) object; }
static VitaLightObj* light_obj(GXLightObj* object) { return (VitaLightObj*) object; }

void GXInitTexObj(GXTexObj* object, const void* data, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s,
                  GXTexWrapMode wrap_t, GXBool mipmap)
{
    VitaTexObj* texture;
    memset(object, 0, sizeof(*object));
    texture = tex_obj(object);
    texture->data = data;
    texture->width = width;
    texture->height = height;
    texture->format = format;
    texture->wrap_s = wrap_s;
    texture->wrap_t = wrap_t;
    texture->mipmap = mipmap;
    texture->min_filter = GX_NEAR;
    texture->mag_filter = GX_NEAR;
    texture->anisotropy = GX_ANISO_1;
}

void GXInitTexObjCI(GXTexObj* object, const void* data, u16 width, u16 height,
                    GXCITexFmt format, GXTexWrapMode wrap_s,
                    GXTexWrapMode wrap_t, GXBool mipmap, u32 tlut)
{
    GXInitTexObj(object, data, width, height, (GXTexFmt) format, wrap_s, wrap_t, mipmap);
    tex_obj(object)->tlut = tlut;
}

void GXInitTexObjLOD(GXTexObj* object, GXTexFilter min_filter,
                     GXTexFilter mag_filter, f32 min_lod, f32 max_lod,
                     f32 lod_bias, GXBool bias_clamp, GXBool edge_lod,
                     GXAnisotropy anisotropy)
{
    VitaTexObj* texture = tex_obj(object);
    texture->min_filter = min_filter;
    texture->mag_filter = mag_filter;
    texture->min_lod = min_lod;
    texture->max_lod = max_lod;
    texture->lod_bias = lod_bias;
    texture->bias_clamp = bias_clamp;
    texture->edge_lod = edge_lod;
    texture->anisotropy = anisotropy;
}

void GXInitTexObjData(GXTexObj* object, const void* data)
{ tex_obj(object)->data = data; }
void GXInitTexObjFilter(GXTexObj* object, GXTexFilter min, GXTexFilter mag) { tex_obj(object)->min_filter = min; tex_obj(object)->mag_filter = mag; }
void GXInitTexObjMaxLOD(GXTexObj* object, f32 value) { tex_obj(object)->max_lod = value; }
void GXInitTexObjMinLOD(GXTexObj* object, f32 value) { tex_obj(object)->min_lod = value; }
void GXInitTexObjLODBias(GXTexObj* object, f32 value) { tex_obj(object)->lod_bias = value; }
void GXInitTexObjBiasClamp(GXTexObj* object, GXBool value) { tex_obj(object)->bias_clamp = value; }
void GXInitTexObjEdgeLOD(GXTexObj* object, GXBool value) { tex_obj(object)->edge_lod = value; }
void GXInitTexObjMaxAniso(GXTexObj* object, GXAnisotropy value) { tex_obj(object)->anisotropy = value; }
void GXInitTexObjUserData(GXTexObj* object, void* value) { tex_obj(object)->user_data = value; }
void* GXGetTexObjUserData(const GXTexObj* object) { return (void*) const_tex_obj(object)->user_data; }
void GXInitTexObjTlut(GXTexObj* object, u32 value) { tex_obj(object)->tlut = value; }
void GXInitTexObjWrapMode(GXTexObj* object, GXTexWrapMode s, GXTexWrapMode t) { tex_obj(object)->wrap_s = s; tex_obj(object)->wrap_t = t; }

void GXLoadTexObj(GXTexObj* object, GXTexMapID id)
{
    if ((unsigned) id < GX_MAX_TEXMAP) {
        if (object == NULL) {
            s_gx.textures[id] = NULL;
            return;
        }
        s_gx.texture_copies[id] = *object;
        s_gx.texture_sources[id] = object;
        s_gx.textures[id] = &s_gx.texture_copies[id];
    }
}

u16 GXGetTexObjWidth(GXTexObj* object) { return tex_obj(object)->width; }
u16 GXGetTexObjHeight(GXTexObj* object) { return tex_obj(object)->height; }
GXTexFmt GXGetTexObjFmt(GXTexObj* object) { return tex_obj(object)->format; }
GXTexWrapMode GXGetTexObjWrapS(GXTexObj* object) { return tex_obj(object)->wrap_s; }
GXTexWrapMode GXGetTexObjWrapT(GXTexObj* object) { return tex_obj(object)->wrap_t; }
GXBool GXGetTexObjMipMap(GXTexObj* object) { return tex_obj(object)->mipmap; }
void* GXGetTexObjData(GXTexObj* object) { return (void*) tex_obj(object)->data; }
u32 GXGetTexObjTlut(const GXTexObj* object) { return const_tex_obj(object)->tlut; }

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod)
{
    u32 shift_x = 2, shift_y = 2;
    u32 block_size = 32;
    u32 result = 0;
    if (format == GX_TF_I4 || format == GX_TF_C4 || format == GX_TF_CMPR || format == GX_CTF_R4) {
        shift_x = 3; shift_y = 3;
    } else if (format == GX_TF_I8 || format == GX_TF_IA4 || format == GX_TF_C8 ||
               format == GX_TF_Z8 || format == GX_CTF_RA4 || format == GX_CTF_A8 ||
               format == GX_CTF_R8 || format == GX_CTF_G8 || format == GX_CTF_B8) {
        shift_x = 3; shift_y = 2;
    }
    if (format == GX_TF_RGBA8 || format == GX_TF_Z24X8) block_size = 64;
    do {
        result += block_size * ((width + (1u << shift_x) - 1u) >> shift_x) *
                  ((height + (1u << shift_y) - 1u) >> shift_y);
        if (!mipmap || max_lod == 0 || (width == 1 && height == 1)) break;
        width = width < 2 ? 1 : width / 2;
        height = height < 2 ? 1 : height / 2;
        --max_lod;
    } while (true);
    return result;
}

/* GXInvalidateTexAll only discards GX's cached texture register state; it
 * does not mean texture memory changed.  HSD calls it every frame, so it must
 * not flush the Vita texture cache (that forced a full re-decode per frame
 * and left mid-frame draws untextured). */
void GXInvalidateTexAll(void) {}

void GXInitTlutObj(GXTlutObj* object, const void* data, GXTlutFmt format, u16 entries)
{
    VitaTlutObj* tlut;
    memset(object, 0, sizeof(*object));
    tlut = tlut_obj(object);
    tlut->data = data;
    tlut->format = format;
    tlut->entries = entries;
}

void GXInitTlutObjData(GXTlutObj* object, const void* data)
{ tlut_obj(object)->data = data; }
void GXLoadTlut(const GXTlutObj* object, u32 index)
{
    if (index >= 20) return;
    if (object == NULL) { s_gx.tluts[index] = NULL; return; }
    s_gx.tlut_copies[index] = *object;
    s_gx.tluts[index] = &s_gx.tlut_copies[index];
}

void GXInitLightAttn(GXLightObj* object, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)
{
    VitaLightObj* light = light_obj(object);
    light->a0 = a0; light->a1 = a1; light->a2 = a2;
    light->k0 = k0; light->k1 = k1; light->k2 = k2;
}

void GXInitLightAttnA(GXLightObj* object, f32 a0, f32 a1, f32 a2)
{ VitaLightObj* light = light_obj(object); light->a0 = a0; light->a1 = a1; light->a2 = a2; }
void GXInitLightAttnK(GXLightObj* object, f32 k0, f32 k1, f32 k2)
{ VitaLightObj* light = light_obj(object); light->k0 = k0; light->k1 = k1; light->k2 = k2; }
void GXInitLightPos(GXLightObj* object, f32 x, f32 y, f32 z)
{ VitaLightObj* light = light_obj(object); light->px = x; light->py = y; light->pz = z; }
void GXInitLightDir(GXLightObj* object, f32 x, f32 y, f32 z)
{ VitaLightObj* light = light_obj(object); light->nx = -x; light->ny = -y; light->nz = -z; }
void GXInitLightColor(GXLightObj* object, GXColor color) { light_obj(object)->color = color; }

void GXInitLightSpot(GXLightObj* object, f32 cutoff, GXSpotFn function)
{
    f32 a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    f32 cosine;
    if (cutoff <= 0.0f || cutoff > 90.0f) function = GX_SP_OFF;
    cosine = cosf(cutoff * (f32) M_PI / 180.0f);
    if (function == GX_SP_COS) { a0 = -cosine / (1.0f - cosine); a1 = 1.0f / (1.0f - cosine); }
    else if (function == GX_SP_COS2) { a0 = 0.0f; a1 = -cosine / (1.0f - cosine); a2 = 1.0f / (1.0f - cosine); }
    GXInitLightAttnA(object, a0, a1, a2);
}

void GXInitLightDistAttn(GXLightObj* object, f32 distance, f32 brightness, GXDistAttnFn function)
{
    f32 k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    if (distance <= 0.0f || brightness <= 0.0f || brightness >= 1.0f) function = GX_DA_OFF;
    if (function == GX_DA_GENTLE) k1 = (1.0f - brightness) / (brightness * distance);
    else if (function == GX_DA_MEDIUM) {
        k1 = 0.5f * (1.0f - brightness) / (brightness * distance);
        k2 = 0.5f * (1.0f - brightness) / (brightness * distance * distance);
    } else if (function == GX_DA_STEEP) k2 = (1.0f - brightness) / (brightness * distance * distance);
    GXInitLightAttnK(object, k0, k1, k2);
}

void GXLoadLightObjImm(GXLightObj* object, GXLightID id)
{
    unsigned slot = 0;
    unsigned mask = (unsigned) id;
    while (slot < 7 && (mask & 1u) == 0u) { mask >>= 1; ++slot; }
    s_gx.lights[slot] = *light_obj(object);
}

static void set_channel_color(GXColor* targets, GXChannelID channel, GXColor color)
{
    const u32 index = (channel == GX_COLOR1 || channel == GX_ALPHA1 || channel == GX_COLOR1A1) ? 1u : 0u;
    if (channel == GX_COLOR0 || channel == GX_COLOR1) {
        targets[index].r = color.r; targets[index].g = color.g; targets[index].b = color.b;
    } else if (channel == GX_ALPHA0 || channel == GX_ALPHA1) {
        targets[index].a = color.a;
    } else {
        targets[index] = color;
    }
}
void GXSetChanAmbColor(GXChannelID channel, GXColor color) { set_channel_color(s_gx.ambient_colors, channel, color); }
void GXSetChanMatColor(GXChannelID channel, GXColor color) { set_channel_color(s_gx.material_colors, channel, color); }
void GXSetNumChans(u8 count) { s_gx.channel_count = count; }
void GXSetChanCtrl(GXChannelID channel, GXBool enabled, GXColorSrc ambient, GXColorSrc material,
                   u32 lights, GXDiffuseFn diffuse, GXAttnFn attenuation)
{
    const VitaChanCtrl control = { enabled, (u8) ambient, (u8) material, lights, (u8) diffuse, (u8) attenuation };
    switch (channel) {
    case GX_COLOR0: s_gx.channels[0] = control; break;
    case GX_COLOR1: s_gx.channels[1] = control; break;
    case GX_ALPHA0: s_gx.channels[2] = control; break;
    case GX_ALPHA1: s_gx.channels[3] = control; break;
    case GX_COLOR0A0: s_gx.channels[0] = control; s_gx.channels[2] = control; break;
    case GX_COLOR1A1: s_gx.channels[1] = control; s_gx.channels[3] = control; break;
    default: break;
    }
}

void GXSetBlendMode(GXBlendMode mode, GXBlendFactor source, GXBlendFactor destination, GXLogicOp operation)
{ s_gx.blend_mode = mode; s_gx.blend_source = source; s_gx.blend_destination = destination; s_gx.logic_operation = operation; }
void GXSetColorUpdate(GXBool enabled) { s_gx.color_update = enabled; }
void GXSetAlphaUpdate(GXBool enabled) { s_gx.alpha_update = enabled; }
void GXSetZMode(GXBool compare, GXCompare function, GXBool update)
{ s_gx.depth_compare = compare; s_gx.depth_function = function; s_gx.depth_update = update; }
void GXSetZCompLoc(GXBool before_texture) { (void) before_texture; }
void GXSetDither(GXBool enabled) { (void) enabled; }
void GXSetDstAlpha(GXBool enabled, u8 alpha) { (void) enabled; (void) alpha; }
void GXSetFieldMode(GXBool field, GXBool half_aspect) { (void) field; (void) half_aspect; }
void GXSetFog(GXFogType type, f32 start, f32 end, f32 near_z, f32 far_z, GXColor color)
{
    static u32 logged, window;
    if (window != s_gx.copied_frames / 600u) { window = s_gx.copied_frames / 600u; logged = 0; }
    if (logged++ < 3u)
        melee_vita_log_info("[FOG] type=%u start=%.1f end=%.1f near=%.1f far=%.1f color=%u,%u,%u,%u",
                            (unsigned) type, start, end, near_z, far_z,
                            (unsigned) color.r, (unsigned) color.g, (unsigned) color.b, (unsigned) color.a);
}
void GXSetFogRangeAdj(GXBool enabled, u16 center, GXFogAdjTable* table)
{ (void) enabled; (void) center; (void) table; }
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, const f32 projection[4][4])
{ (void) width; (void) projection; if (table != NULL) memset(table, 0, sizeof(*table)); }

void GXSetTevOp(GXTevStageID stage, GXTevMode mode)
{
    /* Expand the SDK convenience modes exactly like GXSetTevOp does. */
    GXTevColorArg carg = GX_CC_RASC;
    GXTevAlphaArg aarg = GX_CA_RASA;
    if ((unsigned) stage >= GX_MAX_TEVSTAGE) return;
    s_gx.tev_modes[stage] = mode;
    if (stage != GX_TEVSTAGE0) { carg = GX_CC_CPREV; aarg = GX_CA_APREV; }
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
        break;
    case GX_DECAL:
        GXSetTevColorIn(stage, carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
        break;
    case GX_BLEND:
        GXSetTevColorIn(stage, carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    default:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
        break;
    }
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
void GXSetTevColorIn(GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d)
{
    if ((unsigned) s < GX_MAX_TEVSTAGE) {
        s_gx.tev_color_inputs[s][0] = a; s_gx.tev_color_inputs[s][1] = b;
        s_gx.tev_color_inputs[s][2] = c; s_gx.tev_color_inputs[s][3] = d;
    }
}
void GXSetTevAlphaIn(GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d)
{
    if ((unsigned) s < GX_MAX_TEVSTAGE) {
        s_gx.tev_alpha_inputs[s][0] = a; s_gx.tev_alpha_inputs[s][1] = b;
        s_gx.tev_alpha_inputs[s][2] = c; s_gx.tev_alpha_inputs[s][3] = d;
    }
}
void GXSetTevColorOp(GXTevStageID s, GXTevOp op, GXTevBias b, GXTevScale scale, GXBool clamp, GXTevRegID out)
{
    if ((unsigned) s < GX_MAX_TEVSTAGE) {
        s_gx.tev_color_operations[s] = op; s_gx.tev_color_bias[s] = (u8) b;
        s_gx.tev_color_scale[s] = (u8) scale; s_gx.tev_color_clamp[s] = clamp; s_gx.tev_color_out[s] = (u8) out;
    }
}
void GXSetTevAlphaOp(GXTevStageID s, GXTevOp op, GXTevBias b, GXTevScale scale, GXBool clamp, GXTevRegID out)
{
    if ((unsigned) s < GX_MAX_TEVSTAGE) {
        s_gx.tev_alpha_operations[s] = op; s_gx.tev_alpha_bias[s] = (u8) b;
        s_gx.tev_alpha_scale[s] = (u8) scale; s_gx.tev_alpha_clamp[s] = clamp; s_gx.tev_alpha_out[s] = (u8) out;
    }
}
void GXSetTevColor(GXTevRegID id, GXColor color)
{
    if ((unsigned) id < GX_MAX_TEVREG) {
        s_gx.tev_registers[id] = color;
        s_gx.tev_registers_f[id][0] = color.r / 255.0f; s_gx.tev_registers_f[id][1] = color.g / 255.0f;
        s_gx.tev_registers_f[id][2] = color.b / 255.0f; s_gx.tev_registers_f[id][3] = color.a / 255.0f;
    }
}
void GXSetTevColorS10(GXTevRegID id, GXColorS10 color)
{
    GXColor converted = {
        (u8) (color.r < 0 ? 0 : color.r > 255 ? 255 : color.r),
        (u8) (color.g < 0 ? 0 : color.g > 255 ? 255 : color.g),
        (u8) (color.b < 0 ? 0 : color.b > 255 ? 255 : color.b),
        (u8) (color.a < 0 ? 0 : color.a > 255 ? 255 : color.a),
    };
    GXSetTevColor(id, converted);
    if ((unsigned) id < GX_MAX_TEVREG) {
        s_gx.tev_registers_f[id][0] = color.r / 255.0f; s_gx.tev_registers_f[id][1] = color.g / 255.0f;
        s_gx.tev_registers_f[id][2] = color.b / 255.0f; s_gx.tev_registers_f[id][3] = color.a / 255.0f;
    }
}
void GXSetTevKColor(GXTevKColorID id, GXColor color)
{ if ((unsigned) id < GX_MAX_KCOLOR) s_gx.tev_kcolors[id] = color; }
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel selection)
{ if ((unsigned) stage < GX_MAX_TEVSTAGE) s_gx.tev_kcolor_selection[stage] = selection; }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel selection)
{ if ((unsigned) stage < GX_MAX_TEVSTAGE) s_gx.tev_kalpha_selection[stage] = selection; }
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel raster, GXTevSwapSel texture)
{ if ((unsigned) stage < GX_MAX_TEVSTAGE) { s_gx.tev_swap_ras[stage] = (u8) raster; s_gx.tev_swap_tex[stage] = (u8) texture; } }
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a)
{
    if ((unsigned) table < 4u) {
        s_gx.tev_swap_table[table][0] = (u8) r; s_gx.tev_swap_table[table][1] = (u8) g;
        s_gx.tev_swap_table[table][2] = (u8) b; s_gx.tev_swap_table[table][3] = (u8) a;
    }
}
void GXSetTevClampMode(GXTevStageID stage, GXTevClampMode mode) { (void) stage; (void) mode; }
void GXSetAlphaCompare(GXCompare c0, u8 r0, GXAlphaOp op, GXCompare c1, u8 r1)
{ s_gx.alpha_comp[0] = (u8) c0; s_gx.alpha_ref[0] = r0; s_gx.alpha_op = (u8) op; s_gx.alpha_comp[1] = (u8) c1; s_gx.alpha_ref[1] = r1; }
void GXSetZTexture(GXZTexOp operation, GXTexFmt format, u32 bias) { (void) operation; (void) format; (void) bias; }
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coordinate, GXTexMapID map, GXChannelID color)
{
    if ((unsigned) stage < GX_MAX_TEVSTAGE) {
        s_gx.tev_coordinates[stage] = coordinate;
        s_gx.tev_maps[stage] = map;
        s_gx.tev_colors[stage] = color;
    }
}
void GXSetNumTevStages(u8 count)
{ s_gx.tev_stage_count = count > GX_MAX_TEVSTAGE ? GX_MAX_TEVSTAGE : count; }

void GXSetNumIndStages(u8 count) { (void) count; }
void GXSetIndTexMtx(GXIndTexMtxID matrix, const void* values, s8 exponent)
{ (void) matrix; (void) values; (void) exponent; }
void GXSetIndTexOrder(GXIndTexStageID stage, GXTexCoordID coordinate, GXTexMapID map)
{ (void) stage; (void) coordinate; (void) map; }
void GXSetIndTexCoordScale(GXIndTexStageID stage, GXIndTexScale s, GXIndTexScale t)
{ (void) stage; (void) s; (void) t; }
void GXSetTevIndirect(GXTevStageID tev, GXIndTexStageID ind, GXIndTexFormat format,
                      GXIndTexBiasSel bias, GXIndTexMtxID matrix, GXIndTexWrap wrap_s,
                      GXIndTexWrap wrap_t, GXBool add_previous, GXBool lod,
                      GXIndTexAlphaSel alpha)
{ (void) tev; (void) ind; (void) format; (void) bias; (void) matrix; (void) wrap_s; (void) wrap_t; (void) add_previous; (void) lod; (void) alpha; }
void GXSetTevDirect(GXTevStageID stage)
{ GXSetTevIndirect(stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF); }

void GXInsertDebugMarker(const char* label) { (void) label; }
