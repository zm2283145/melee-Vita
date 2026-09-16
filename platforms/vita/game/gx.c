/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal Dolphin GX front end for the native Vita port.
 *
 * This is deliberately a state recorder rather than a collection of ABI
 * stubs.  HSD can configure GX exactly as it does on GameCube/PC while the
 * Vita renderer consumes this state and turns draw calls into GXM batches.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/gx.h>

#include "vita_platform.h"
#include "gxm_game.h"
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
    u8 pending_matrix;
    GXBool has_pending_matrix;
    GXBool active;
} VitaImmediateState;

typedef struct VitaDecodedVertex {
    f32 position[3];
    f32 normal[3];
    f32 texture[2];
    u32 color;
    u8 position_matrix;
    u8 has_position_matrix;
} VitaDecodedVertex;

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
    u8 line_width;
    u8 point_size;
    GXClipMode clip_mode;
    GXCullMode cull_mode;
    VitaImmediateState immediate;

    GXColor clear_color;
    u32 clear_depth;
    const GXTexObj* textures[GX_MAX_TEXMAP];
    const GXTlutObj* tluts[20];
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
    } else if (type == GX_RGB8 || type == GX_RGBA6) {
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
    } else if (attr == GX_VA_TEX0) {
        u32 components = attribute_components(attr, format->count);
        vertex->texture[0] = read_component(source, format->type, format->fraction, little_endian);
        vertex->texture[1] = components >= 2
            ? read_component(source + step, format->type, format->fraction, little_endian) : 0.0f;
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

static void project_vertex(const VitaDecodedVertex* input,
                           MeleeVitaScreenVertex* output)
{
    const u32 matrix_id = input->has_position_matrix
        ? input->position_matrix : s_gx.current_matrix;
    const f32 (*matrix)[4] = s_gx.position_matrices[matrix_slot(matrix_id)];
    f32 x, y, z;
    const f32 vita_scale = 544.0f / 480.0f;
    GXProject(input->position[0], input->position[1], input->position[2],
              matrix, s_gx.projection, s_gx.viewport, &x, &y, &z);
    output->x = (960.0f - 640.0f * vita_scale) * 0.5f + x * vita_scale;
    output->y = y * vita_scale;
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
    memset(source, 0, sizeof(*source));
    source->key = object;
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

void GXClearVtxDesc(void) { memset(s_gx.descriptors, 0, sizeof(s_gx.descriptors)); }

void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    if (valid_attr(attr)) s_gx.descriptors[attr] = type;
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
void GXEnableTexOffsets(GXTexCoordID coord, GXBool lines, GXBool points) { (void) coord; (void) lines; (void) points; }
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
    if (s_gx.immediate.active && s_gx.immediate.vertex_count != 0)
        submit_decoded(s_gx.immediate.primitive, s_decode_vertices,
                       s_gx.immediate.vertex_count);
    s_gx.immediate.active = GX_FALSE;
}

void GXCallDisplayList(const void* list, u32 bytes)
{
    const u8* stream = list;
    u32 cursor = 0;
    if (stream == NULL) return;
    while (cursor + 3u <= bytes) {
        const u8 command = stream[cursor];
        const GXPrimitive primitive = (GXPrimitive) (command & 0xf8u);
        const GXVtxFmt format = (GXVtxFmt) (command & 7u);
        const u32 count = (u32) stream[cursor + 1u] << 8 | stream[cursor + 2u];
        u32 vertex_index;
        if (command == 0 || primitive < GX_QUADS || primitive > GX_POINTS ||
            !valid_format(format)) break;
        cursor += 3u;
        if (!reserve_vertices(count)) return;
        for (vertex_index = 0; vertex_index < count; ++vertex_index) {
            VitaDecodedVertex* vertex = &s_decode_vertices[vertex_index];
            u32 attr_index;
            memset(vertex, 0, sizeof(*vertex));
            vertex->normal[2] = 1.0f;
            vertex->color = packed_color(s_gx.material_colors[0]);
            for (attr_index = 0; attr_index < GX_VA_MAX_ATTR; ++attr_index) {
                const GXAttr attr = (GXAttr) attr_index;
                const GXAttrType descriptor = s_gx.descriptors[attr_index];
                const VitaVtxFormat* attr_format = &s_gx.formats[format][attr_index];
                const u8* source;
                bool little_endian = false;
                u32 required;
                if (descriptor == GX_NONE) continue;
                if (descriptor == GX_DIRECT) {
                    required = direct_bytes(attr, attr_format);
                    if (required > bytes - cursor) return;
                    source = stream + cursor;
                    cursor += required;
                } else {
                    const u32 index_bytes = descriptor == GX_INDEX8 ? 1u : 2u;
                    const u32 index_count =
                        (attr == GX_VA_NRM || attr == GX_VA_NBT) &&
                        attr_format->count == GX_NRM_NBT3 ? 3u : 1u;
                    u32 array_index;
                    u32 source_bytes;
                    const VitaArrayState* array = &s_gx.arrays[attr_index];
                    if (index_count * index_bytes > bytes - cursor ||
                        array->data == NULL || array->stride == 0) return;
                    array_index = descriptor == GX_INDEX8
                        ? stream[cursor] : read_u16(stream + cursor, false);
                    cursor += index_count * index_bytes;
                    source_bytes = attr == GX_VA_CLR0 || attr == GX_VA_CLR1
                        ? color_bytes(attr_format->type)
                        : ((attr == GX_VA_NRM || attr == GX_VA_NBT) &&
                           attr_format->count == GX_NRM_NBT3
                               ? 3u * component_bytes(attr_format->type)
                               : direct_bytes(attr, attr_format));
                    if (array_index > UINT32_MAX / array->stride) return;
                    required = array_index * array->stride;
                    if (array->size != 0 &&
                        (required > array->size || source_bytes > array->size - required)) return;
                    source = (const u8*) array->data + required;
                    little_endian = array->little_endian;
                }
                decode_attribute(vertex, attr, attr_format, source, little_endian);
            }
        }
        submit_decoded(primitive, s_decode_vertices, count);
    }
}

#define GX_VALUE_FN_1(name, type) void name(type a) { (void) a; note_value(); }
#define GX_VALUE_FN_2(name, type) void name(type a, type b) { (void) a; (void) b; note_value(); }
#define GX_VALUE_FN_3(name, type) void name(type a, type b, type c) { (void) a; (void) b; (void) c; note_value(); }
#define GX_VALUE_FN_4(name, type) void name(type a, type b, type c, type d) { (void) a; (void) b; (void) c; (void) d; note_value(); }

static VitaDecodedVertex* immediate_vertex(void)
{
    if (!s_gx.immediate.active || s_gx.immediate.vertex_count == 0) return NULL;
    return &s_decode_vertices[s_gx.immediate.vertex_count - 1u];
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

void GXPosition3f32(f32 x, f32 y, f32 z) { immediate_position(x, y, z); }
void GXPosition3u16(u16 x, u16 y, u16 z) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), immediate_scaled(z, GX_VA_POS)); }
void GXPosition3s16(s16 x, s16 y, s16 z) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), immediate_scaled(z, GX_VA_POS)); }
void GXPosition3u8(u8 x, u8 y, u8 z) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), immediate_scaled(z, GX_VA_POS)); }
void GXPosition3s8(s8 x, s8 y, s8 z) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), immediate_scaled(z, GX_VA_POS)); }
void GXPosition2f32(f32 x, f32 y) { immediate_position(x, y, 0.0f); }
void GXPosition2u16(u16 x, u16 y) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), 0.0f); }
void GXPosition2s16(s16 x, s16 y) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), 0.0f); }
void GXPosition2u8(u8 x, u8 y) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), 0.0f); }
void GXPosition2s8(s8 x, s8 y) { immediate_position(immediate_scaled(x, GX_VA_POS), immediate_scaled(y, GX_VA_POS), 0.0f); }

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
void GXNormal3f32(f32 x, f32 y, f32 z) { immediate_normal(x, y, z); }
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
{ VitaDecodedVertex* v = immediate_vertex(); if (v != NULL) v->color = color; note_value(); immediate_advance(); }
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
{ VitaDecodedVertex* v = immediate_vertex(); if (v != NULL) { v->texture[0] = s; v->texture[1] = t; } note_value(); immediate_advance(); }
void GXTexCoord2f32(f32 s, f32 t) { immediate_texcoord(s, t); }
void GXTexCoord2u16(u16 s, u16 t) { immediate_texcoord(immediate_scaled(s, GX_VA_TEX0), immediate_scaled(t, GX_VA_TEX0)); }
void GXTexCoord2s16(s16 s, s16 t) { immediate_texcoord(immediate_scaled(s, GX_VA_TEX0), immediate_scaled(t, GX_VA_TEX0)); }
void GXTexCoord2u8(u8 s, u8 t) { immediate_texcoord(immediate_scaled(s, GX_VA_TEX0), immediate_scaled(t, GX_VA_TEX0)); }
void GXTexCoord2s8(s8 s, s8 t) { immediate_texcoord(immediate_scaled(s, GX_VA_TEX0), immediate_scaled(t, GX_VA_TEX0)); }
void GXTexCoord1f32(f32 s) { immediate_texcoord(s, 0.0f); }
void GXTexCoord1u16(u16 s) { immediate_texcoord(immediate_scaled(s, GX_VA_TEX0), 0.0f); }
void GXTexCoord1s16(s16 s) { immediate_texcoord(immediate_scaled(s, GX_VA_TEX0), 0.0f); }
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
void GXTexCoord1s8(s8 s) { immediate_texcoord(immediate_scaled(s, GX_VA_TEX0), 0.0f); }

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
    immediate_texcoord(decoded.texture[0], decoded.texture[1]);
}
void GXTexCoord1x16(u16 index) { immediate_indexed_texcoord(index); }
void GXTexCoord1x8(u8 index) { immediate_indexed_texcoord(index); }

GX_VALUE_FN_1(GXCmd1u8, u8)
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
void GXCopyDisp(void* destination, GXBool clear)
{
    const u32 color = (u32) s_gx.clear_color.r |
                      (u32) s_gx.clear_color.g << 8 |
                      (u32) s_gx.clear_color.b << 16 |
                      (u32) s_gx.clear_color.a << 24;
    (void) destination;
    (void) clear;
    ++s_gx.copied_frames;
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
void GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height)
{ (void) left; (void) top; (void) width; (void) height; }
void GXSetTexCopyDst(u16 width, u16 height, GXTexFmt format, GXBool mipmap)
{ (void) width; (void) height; (void) format; (void) mipmap; }
void GXCopyTex(void* destination, GXBool clear) { (void) destination; (void) clear; }
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
{ tex_obj(object)->data = data; melee_vita_gxm_invalidate_textures(); }
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
    if ((unsigned) id < GX_MAX_TEXMAP) s_gx.textures[id] = object;
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

void GXInvalidateTexAll(void) { melee_vita_gxm_invalidate_textures(); }

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
{ tlut_obj(object)->data = data; melee_vita_gxm_invalidate_textures(); }
void GXLoadTlut(const GXTlutObj* object, u32 index) { if (index < 20) s_gx.tluts[index] = object; }

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

void GXSetChanAmbColor(GXChannelID channel, GXColor color)
{ s_gx.ambient_colors[(channel == GX_COLOR1 || channel == GX_ALPHA1 || channel == GX_COLOR1A1) ? 1 : 0] = color; }
void GXSetChanMatColor(GXChannelID channel, GXColor color)
{ s_gx.material_colors[(channel == GX_COLOR1 || channel == GX_ALPHA1 || channel == GX_COLOR1A1) ? 1 : 0] = color; }
void GXSetNumChans(u8 count) { (void) count; }
void GXSetChanCtrl(GXChannelID channel, GXBool enabled, GXColorSrc ambient, GXColorSrc material,
                   u32 lights, GXDiffuseFn diffuse, GXAttnFn attenuation)
{ (void) channel; (void) enabled; (void) ambient; (void) material; (void) lights; (void) diffuse; (void) attenuation; }

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
{ (void) type; (void) start; (void) end; (void) near_z; (void) far_z; (void) color; }
void GXSetFogRangeAdj(GXBool enabled, u16 center, GXFogAdjTable* table)
{ (void) enabled; (void) center; (void) table; }
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, const f32 projection[4][4])
{ (void) width; (void) projection; if (table != NULL) memset(table, 0, sizeof(*table)); }

void GXSetTevOp(GXTevStageID stage, GXTevMode mode)
{ if ((unsigned) stage < GX_MAX_TEVSTAGE) s_gx.tev_modes[stage] = mode; }
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
{ if ((unsigned) s < GX_MAX_TEVSTAGE) s_gx.tev_color_operations[s] = op; (void) b; (void) scale; (void) clamp; (void) out; }
void GXSetTevAlphaOp(GXTevStageID s, GXTevOp op, GXTevBias b, GXTevScale scale, GXBool clamp, GXTevRegID out)
{ if ((unsigned) s < GX_MAX_TEVSTAGE) s_gx.tev_alpha_operations[s] = op; (void) b; (void) scale; (void) clamp; (void) out; }
void GXSetTevColor(GXTevRegID id, GXColor color)
{ if ((unsigned) id < GX_MAX_TEVREG) s_gx.tev_registers[id] = color; }
void GXSetTevColorS10(GXTevRegID id, GXColorS10 color)
{
    GXColor converted = {
        (u8) (color.r < 0 ? 0 : color.r > 255 ? 255 : color.r),
        (u8) (color.g < 0 ? 0 : color.g > 255 ? 255 : color.g),
        (u8) (color.b < 0 ? 0 : color.b > 255 ? 255 : color.b),
        (u8) (color.a < 0 ? 0 : color.a > 255 ? 255 : color.a),
    };
    GXSetTevColor(id, converted);
}
void GXSetTevKColor(GXTevKColorID id, GXColor color)
{ if ((unsigned) id < GX_MAX_KCOLOR) s_gx.tev_kcolors[id] = color; }
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel selection)
{ if ((unsigned) stage < GX_MAX_TEVSTAGE) s_gx.tev_kcolor_selection[stage] = selection; }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel selection)
{ if ((unsigned) stage < GX_MAX_TEVSTAGE) s_gx.tev_kalpha_selection[stage] = selection; }
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel raster, GXTevSwapSel texture) { (void) stage; (void) raster; (void) texture; }
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a)
{ (void) table; (void) r; (void) g; (void) b; (void) a; }
void GXSetTevClampMode(GXTevStageID stage, GXTevClampMode mode) { (void) stage; (void) mode; }
void GXSetAlphaCompare(GXCompare c0, u8 r0, GXAlphaOp op, GXCompare c1, u8 r1)
{ (void) c0; (void) r0; (void) op; (void) c1; (void) r1; }
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
