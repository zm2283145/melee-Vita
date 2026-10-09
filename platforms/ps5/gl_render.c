/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * GX shader renderer for PS5: the OpenGL 4.6 counterpart of
 * platforms/vita/game/gx_render.c.
 *
 * gx.c describes every draw as a TEV shader key (GxrShaderKey), a vertex
 * pipeline key (GxrVtxKey, or the CPU-transformed GxrVertex path) and the GX
 * blend/depth state.  As on Vita, each key becomes generated shader source:
 * the stage, lighting and texgen logic below is the Vita generator's,
 * emitted as GLSL instead of Cg.  Programs are compiled by the driver at
 * first use and cached for the session.
 *
 * Coordinates: the Vita code produces Vita clip space with +Y up and a
 * viewport that puts NDC +1 on the top row.  Every PS5 framebuffer stores
 * its top row first (see gl_game.c), which OpenGL rasterises from NDC -1,
 * so vertex shaders negate Y.  Pixel rows are then identical to the Vita's,
 * window-space winding included.
 */
#include "gx_render.h"
#include "gx_submit.h"
#include "fragment_alpha_key.h"
#include "gl_game.h"
#include "ps5_log.h"
#include "../vita/vita_log.h"

#include <dolphin/gx/GXEnum.h>
#include <psp2/kernel/processmgr.h>

#include <GL/gl.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GXR_PROGRAM_BUCKETS 512u
#define GXR_SOURCE_CAPACITY 32768u
#define GXR_LINK_BUCKETS 1024u

/* Vertex streaming: one ring per frame, reset at frame start. */
/* Per-frame streaming segments, persistently mapped.  glBufferSubData on a
 * buffer the GPU still references drains the driver's batch and waits for
 * the GPU (ps5_buffer_subdata), so every draw used to be a full sync. */
#define GXR_RING_FRAMES 3u
#define GXR_VERTEX_RING (24u * 1024u * 1024u)
#define GXR_INDEX_RING (6u * 1024u * 1024u)

_Static_assert(sizeof(GxrGpuVertex) == MELEE_VITA_GPU_VERTEX_BYTES, "GPU vertex layout");
_Static_assert(sizeof(GxrGpuBumpVertex) == MELEE_VITA_GPU_BUMP_VERTEX_BYTES, "bump vertex layout");

vita2d_texture* melee_vita_gxm_texture(const MeleeVitaTextureSource* source);
extern u32 g_melee_vita_gxm_state_epoch;
u32 g_melee_vita_texture_memo_epoch = 1u;

/* ------------------------------------------------------------------ types */

enum { LAYOUT_CPU = 0, LAYOUT_GPU, LAYOUT_BUMP, LAYOUT_COUNT };

typedef struct GxrProgram {
    u64 hash;
    GxrShaderKey key;
    GLuint shader;
    bool failed;
    struct GxrProgram* next;
} GxrProgram;

typedef struct GxrVtxProgram {
    u64 hash;
    GxrVtxKey key;
    bool point;
    u8 point_tex_mask;
    bool failed;
    GLuint shader;
    struct GxrVtxProgram* next;
} GxrVtxProgram;

typedef struct GxrBumpVtxProgram {
    u64 hash;
    GxrBumpVtxKey key;
    bool failed;
    GLuint shader;
    struct GxrBumpVtxProgram* next;
} GxrBumpVtxProgram;

/* A linked vertex + fragment pair and its uniform locations. */
typedef struct GxrLinked {
    GLuint vs, fs;
    GLuint program;
    bool failed;
    GLint u_prev, u_reg[3], u_k[4], u_alpha_ref[2], u_z_bias, u_fog_color, u_fog_params,
        u_ind_mtx[2];
    GLint u_pos, u_nrm, u_proj, u_tex, u_post, u_light, u_mat, u_amb, u_point, u_point_size;
    struct GxrLinked* next;
} GxrLinked;

static bool s_ready;
static bool s_failed;
static GLuint s_cpu_vs;
static GxrProgram* s_programs[GXR_PROGRAM_BUCKETS];
static GxrVtxProgram* s_vtx_programs[GXR_PROGRAM_BUCKETS];
static GxrBumpVtxProgram* s_bump_programs[GXR_PROGRAM_BUCKETS];
static GxrLinked* s_linked[GXR_LINK_BUCKETS];
static vita2d_texture* s_white;
static GLuint s_vao[LAYOUT_COUNT];
static GLuint s_vbo, s_ibo; /* the current frame's segment */
static u8* s_vbo_ptr;
static u8* s_ibo_ptr;
static u32 s_vbo_used, s_ibo_used;
static struct {
    GLuint vbo, ibo;
    u8* vbo_ptr;
    u8* ibo_ptr;
    GLsync fence;
} s_ring[GXR_RING_FRAMES];
static u32 s_ring_index;
static u32 s_ring_frame = UINT32_MAX;
static GLuint s_depth_copy_prog;
static GLuint s_empty_vao;
static GLint s_depth_copy_rect;

static struct {
    u32 draws, fallback, compiled, compile_failed, links;
    u64 compile_us;
} s_stats;

/* ------------------------------------------------------------------ utils */

static u64 fnv1a(const void* data, size_t size, u64 hash)
{
    const u8* bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

typedef struct Source {
    char* text;
    size_t length;
    size_t capacity;
    bool overflow;
} Source;

static void emit(Source* s, const char* format, ...)
{
    va_list args;
    int written;
    if (s->overflow) return;
    va_start(args, format);
    written = vsnprintf(s->text + s->length, s->capacity - s->length, format, args);
    va_end(args);
    if (written < 0 || (size_t) written >= s->capacity - s->length) {
        s->overflow = true;
        return;
    }
    s->length += (size_t) written;
}

/* The TEV/lighting generators below emit Cg spellings; turn the text into
 * GLSL in place (every replacement is no longer than what it replaces). */
static void cg_to_glsl(char* text)
{
    static const struct { const char* from; const char* to; } words[] = {
        { "float2", "vec2" }, { "float3", "vec3" }, { "float4", "vec4" },
        { "lerp(", "mix(" }, { "frac(", "fract(" }, { "tex2D(", "texture(" },
    };
    char* out = text;
    const char* in = text;
    while (*in != '\0') {
        bool replaced = false;
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
            const size_t n = strlen(words[i].from);
            if (strncmp(in, words[i].from, n) == 0) {
                const size_t m = strlen(words[i].to);
                memmove(out, words[i].to, m);
                out += m;
                in += n;
                replaced = true;
                break;
            }
        }
        if (!replaced) *out++ = *in++;
    }
    *out = '\0';
}

static void log_source(const char* tag, const char* text)
{
    const size_t length = strlen(text);
    for (size_t off = 0; off < length; off += 900) {
        char chunk[901];
        const size_t n = length - off < 900 ? length - off : 900;
        memcpy(chunk, text + off, n);
        chunk[n] = '\0';
        melee_ps5_log("%s %s", tag, chunk);
    }
}

static GLuint compile_glsl(GLenum type, char* source, const char* what)
{
    const u64 started = sceKernelGetProcessTimeWide();
    GLint ok = GL_FALSE;
    GLuint shader;
    const char* text = source;
    cg_to_glsl(source);
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &text, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    s_stats.compile_us += sceKernelGetProcessTimeWide() - started;
    if (!ok) {
        char log[2048];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        if (s_stats.compile_failed++ < 16u) {
            melee_ps5_log("[GXR] %s compile failed: %.*s", what, (int) length, log);
            log_source("[GXR] src:", source);
        }
        glDeleteShader(shader);
        return 0;
    }
    ++s_stats.compiled;
    return shader;
}

/* ------------------------------------------------- fragment (TEV) source */

static const char* channel_name(u8 channel)
{
    static const char* names[] = { "r", "g", "b", "a" };
    return names[channel & 3u];
}

static void swapped(char* out, size_t size, const char* base, const GxrShaderKey* key, u8 table,
                    bool alpha)
{
    const u8* swap = key->swap[table & 3u];
    if (alpha)
        snprintf(out, size, "%s.%s", base, channel_name(swap[3]));
    else
        snprintf(out, size, "%s.%s%s%s", base, channel_name(swap[0]), channel_name(swap[1]),
                 channel_name(swap[2]));
}

static int color_channel_index(u8 channel)
{
    if (channel == GX_COLOR0 || channel == GX_ALPHA0 || channel == GX_COLOR0A0) return 0;
    if (channel == GX_COLOR1 || channel == GX_ALPHA1 || channel == GX_COLOR1A1) return 1;
    return -1;
}

static const char* konst_fraction(u8 sel)
{
    static const char* values[] = { "1.0", "0.875", "0.75", "0.625",
                                    "0.5", "0.375", "0.25", "0.125" };
    return sel < 8u ? values[sel] : "1.0";
}

static void color_arg(char* out, size_t size, const GxrShaderKey* key, const GxrStage* st,
                      u32 stage_index, u8 arg)
{
    char tmp[64];
    const int ras = color_channel_index(st->channel);
    switch (arg) {
    case GX_CC_CPREV: snprintf(out, size, "prev.rgb"); return;
    case GX_CC_APREV: snprintf(out, size, "prev.aaa"); return;
    case GX_CC_C0: snprintf(out, size, "r0.rgb"); return;
    case GX_CC_A0: snprintf(out, size, "r0.aaa"); return;
    case GX_CC_C1: snprintf(out, size, "r1.rgb"); return;
    case GX_CC_A1: snprintf(out, size, "r1.aaa"); return;
    case GX_CC_C2: snprintf(out, size, "r2.rgb"); return;
    case GX_CC_A2: snprintf(out, size, "r2.aaa"); return;
    case GX_CC_TEXC:
        if (st->tex_map >= GXR_MAX_TEXMAPS) { snprintf(out, size, "float3(1.0,1.0,1.0)"); return; }
        snprintf(tmp, sizeof(tmp), "s%u", stage_index);
        swapped(out, size, tmp, key, st->swap_tex, false);
        return;
    case GX_CC_TEXA: {
        char a[64];
        if (st->tex_map >= GXR_MAX_TEXMAPS) { snprintf(out, size, "float3(1.0,1.0,1.0)"); return; }
        snprintf(tmp, sizeof(tmp), "s%u", stage_index);
        swapped(a, sizeof(a), tmp, key, st->swap_tex, true);
        snprintf(out, size, "float3(%s,%s,%s)", a, a, a);
        return;
    }
    case GX_CC_RASC:
        if (ras < 0) { snprintf(out, size, "float3(0.0,0.0,0.0)"); return; }
        snprintf(tmp, sizeof(tmp), "ras%d", ras);
        swapped(out, size, tmp, key, st->swap_ras, false);
        return;
    case GX_CC_RASA: {
        char a[64];
        if (ras < 0) { snprintf(out, size, "float3(0.0,0.0,0.0)"); return; }
        snprintf(tmp, sizeof(tmp), "ras%d", ras);
        swapped(a, sizeof(a), tmp, key, st->swap_ras, true);
        snprintf(out, size, "float3(%s,%s,%s)", a, a, a);
        return;
    }
    case GX_CC_ONE: snprintf(out, size, "float3(1.0,1.0,1.0)"); return;
    case GX_CC_HALF: snprintf(out, size, "float3(0.5,0.5,0.5)"); return;
    case GX_CC_KONST: {
        const u8 sel = st->kcsel;
        if (sel < 8u) {
            snprintf(out, size, "float3(%s,%s,%s)", konst_fraction(sel), konst_fraction(sel),
                     konst_fraction(sel));
            return;
        }
        if (sel >= 0x0cu && sel <= 0x0fu) { snprintf(out, size, "k%u.rgb", sel - 0x0cu); return; }
        if (sel >= 0x10u && sel <= 0x1fu) {
            const char* c = channel_name((sel - 0x10u) >> 2);
            snprintf(out, size, "k%u.%s%s%s", (sel - 0x10u) & 3u, c, c, c);
            return;
        }
        snprintf(out, size, "float3(1.0,1.0,1.0)");
        return;
    }
    default: snprintf(out, size, "float3(0.0,0.0,0.0)"); return;
    }
}

static void alpha_arg(char* out, size_t size, const GxrShaderKey* key, const GxrStage* st,
                      u32 stage_index, u8 arg)
{
    char tmp[64];
    const int ras = color_channel_index(st->channel);
    switch (arg) {
    case GX_CA_APREV: snprintf(out, size, "prev.a"); return;
    case GX_CA_A0: snprintf(out, size, "r0.a"); return;
    case GX_CA_A1: snprintf(out, size, "r1.a"); return;
    case GX_CA_A2: snprintf(out, size, "r2.a"); return;
    case GX_CA_TEXA:
        if (st->tex_map >= GXR_MAX_TEXMAPS) { snprintf(out, size, "1.0"); return; }
        snprintf(tmp, sizeof(tmp), "s%u", stage_index);
        swapped(out, size, tmp, key, st->swap_tex, true);
        return;
    case GX_CA_RASA:
        if (ras < 0) { snprintf(out, size, "0.0"); return; }
        snprintf(tmp, sizeof(tmp), "ras%d", ras);
        swapped(out, size, tmp, key, st->swap_ras, true);
        return;
    case GX_CA_KONST: {
        const u8 sel = st->kasel;
        if (sel < 8u) { snprintf(out, size, "%s", konst_fraction(sel)); return; }
        if (sel >= 0x10u && sel <= 0x1fu) {
            snprintf(out, size, "k%u.%s", (sel - 0x10u) & 3u, channel_name((sel - 0x10u) >> 2));
            return;
        }
        snprintf(out, size, "1.0");
        return;
    }
    default: snprintf(out, size, "0.0"); return;
    }
}

static const char* bias_text(u8 bias)
{
    return bias == GX_TB_ADDHALF ? " + 0.5" : bias == GX_TB_SUBHALF ? " - 0.5" : "";
}

static const char* scale_text(u8 scale)
{
    return scale == GX_CS_SCALE_2 ? " * 2.0" : scale == GX_CS_SCALE_4 ? " * 4.0"
         : scale == GX_CS_DIVIDE_2 ? " * 0.5" : "";
}

static void op_expr(Source* s, u8 op, u8 bias, u8 scale, bool is_color, const char* a,
                    const char* b, const char* c, const char* d)
{
    const char* zero = is_color ? "float3(0.0,0.0,0.0)" : "0.0";
    switch (op) {
    case GX_TEV_ADD:
    case GX_TEV_SUB:
        emit(s, "((%slerp(%s, %s, %s) + %s)%s)%s", op == GX_TEV_SUB ? "-" : "", a, b, c, d,
             bias_text(bias), scale_text(scale));
        return;
    case GX_TEV_COMP_R8_GT:
        emit(s, "(((floor(%s.r*255.0+0.5) > floor(%s.r*255.0+0.5)) ? %s : %s) + %s)", a, b, c, zero, d);
        return;
    case GX_TEV_COMP_R8_EQ:
        emit(s, "(((floor(%s.r*255.0+0.5) == floor(%s.r*255.0+0.5)) ? %s : %s) + %s)", a, b, c, zero, d);
        return;
    case GX_TEV_COMP_GR16_GT:
        emit(s, "(((dot(floor(%s.rg*255.0+0.5), float2(1.0,256.0)) > dot(floor(%s.rg*255.0+0.5), float2(1.0,256.0))) ? %s : %s) + %s)", a, b, c, zero, d);
        return;
    case GX_TEV_COMP_GR16_EQ:
        emit(s, "(((dot(floor(%s.rg*255.0+0.5), float2(1.0,256.0)) == dot(floor(%s.rg*255.0+0.5), float2(1.0,256.0))) ? %s : %s) + %s)", a, b, c, zero, d);
        return;
    case GX_TEV_COMP_BGR24_GT:
        emit(s, "(((dot(floor(%s.rgb*255.0+0.5), float3(1.0,256.0,65536.0)) > dot(floor(%s.rgb*255.0+0.5), float3(1.0,256.0,65536.0))) ? %s : %s) + %s)", a, b, c, zero, d);
        return;
    case GX_TEV_COMP_BGR24_EQ:
        emit(s, "(((dot(floor(%s.rgb*255.0+0.5), float3(1.0,256.0,65536.0)) == dot(floor(%s.rgb*255.0+0.5), float3(1.0,256.0,65536.0))) ? %s : %s) + %s)", a, b, c, zero, d);
        return;
    case GX_TEV_COMP_RGB8_GT:
        if (is_color)
            emit(s, "(%s * step(floor(%s*255.0+0.5) + 0.5, floor(%s*255.0+0.5)) + %s)", c, b, a, d);
        else
            emit(s, "(((floor(%s*255.0+0.5) > floor(%s*255.0+0.5)) ? %s : 0.0) + %s)", a, b, c, d);
        return;
    case GX_TEV_COMP_RGB8_EQ:
        if (is_color)
            emit(s, "(%s * (float3(1.0,1.0,1.0) - step(0.5, abs(floor(%s*255.0+0.5) - floor(%s*255.0+0.5)))) + %s)", c, a, b, d);
        else
            emit(s, "(((floor(%s*255.0+0.5) == floor(%s*255.0+0.5)) ? %s : 0.0) + %s)", a, b, c, d);
        return;
    default:
        emit(s, "%s", d);
        return;
    }
}

static void alpha_compare_expr(Source* s, u8 comp, const char* ref)
{
    static const char* ops[] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
    if (comp == GX_NEVER) { emit(s, "false"); return; }
    if (comp == GX_ALWAYS) { emit(s, "true"); return; }
    emit(s, "(ac %s %s)", ops[comp & 7u], ref);
}

static bool alpha_test_trivially_passes(const GxrShaderKey* key)
{
    return melee_vita_alpha_test_trivially_passes(key->alpha_op, key->alpha_comp[0],
                                                  key->alpha_comp[1]);
}

static const char k_varyings_in[] =
    "in vec4 vColor0;\nin vec4 vColor1;\n"
    "in vec2 vTex0;\nin vec2 vTex1;\nin vec2 vTex2;\nin vec2 vTex3;\n"
    "in vec2 vTex4;\nin vec2 vTex5;\nin vec2 vTex6;\nin vec2 vTex7;\n";
static const char k_varyings_out[] =
    "out vec4 vColor0;\nout vec4 vColor1;\n"
    "out vec2 vTex0;\nout vec2 vTex1;\nout vec2 vTex2;\nout vec2 vTex3;\n"
    "out vec2 vTex4;\nout vec2 vTex5;\nout vec2 vTex6;\nout vec2 vTex7;\n";

static bool build_fragment_source(const GxrShaderKey* key, Source* s)
{
    static const char* reg_names[] = { "prev", "r0", "r1", "r2" };
    const u8 z_tex_op = key->z_tex_op & 3u;
    const u8 fog_type = key->z_tex_op >> 4;
    bool uses_map[GXR_MAX_TEXMAPS] = { false };
    bool uses_indirect = false;
    for (u32 i = 0; i < key->stage_count; ++i) {
        const GxrStage* st = &key->stages[i];
        if (st->tex_map < GXR_MAX_TEXMAPS) {
            uses_map[st->tex_map] = true;
            if ((st->mirror & 0x80u) != 0u) {
                uses_indirect = true;
                uses_map[(st->mirror >> 5) & 3u] = true;
            }
        }
    }
    const bool writes_depth = z_tex_op != GX_ZT_DISABLE && key->z_tex_format == GX_TF_Z24X8 &&
                              key->stage_count > 0u;
    const bool needs_position = fog_type != GX_FOG_NONE || z_tex_op == GX_ZT_ADD;
    const bool trivially_pass = alpha_test_trivially_passes(key);

    emit(s, "#version 460 core\n%s", k_varyings_in);
    emit(s, "layout(location=0) out vec4 oColor;\n");
    for (u32 i = 0; i < GXR_MAX_TEXMAPS; ++i)
        if (uses_map[i]) emit(s, "uniform sampler2D uMap%u;\n", i);
    emit(s, "uniform vec4 uPrev;\nuniform vec4 uReg0;\nuniform vec4 uReg1;\nuniform vec4 uReg2;\n");
    emit(s, "uniform vec4 uK0;\nuniform vec4 uK1;\nuniform vec4 uK2;\nuniform vec4 uK3;\n");
    if (uses_indirect) emit(s, "uniform vec4 uIndMtx0;\nuniform vec4 uIndMtx1;\n");
    if (fog_type != GX_FOG_NONE) emit(s, "uniform vec4 uFogColor;\nuniform vec4 uFogParams;\n");
    for (u32 i = 0; i < 2u; ++i)
        if (key->alpha_comp[i] != GX_NEVER && key->alpha_comp[i] != GX_ALWAYS)
            emit(s, "uniform float uAlphaRef%u;\n", i);
    if (writes_depth) emit(s, "uniform float uZBias;\n");
    emit(s, "void main()\n{\n");
    if (needs_position) emit(s, "    vec4 vPosition = gl_FragCoord;\n");
    emit(s, "    float4 prev = uPrev;\n    float4 r0 = uReg0;\n    float4 r1 = uReg1;\n    float4 r2 = uReg2;\n");
    emit(s, "    float4 k0 = uK0;\n    float4 k1 = uK1;\n    float4 k2 = uK2;\n    float4 k3 = uK3;\n");
    emit(s, "    float4 ras0 = vColor0;\n    float4 ras1 = vColor1;\n");

    for (u32 i = 0; i < key->stage_count; ++i) {
        const GxrStage* st = &key->stages[i];
        char a[96], b[96], c[96], d[96];
        if (st->tex_map < GXR_MAX_TEXMAPS) {
            if (st->tex_coord < GXR_MAX_TEXCOORDS && (st->mirror & 0x80u) != 0u) {
                emit(s, "    float3 ind%u = float3(tex2D(uMap%u, vTex%u).abg) * 255.0;\n", i,
                     (u32) ((st->mirror >> 5) & 3u), (u32) ((st->mirror >> 2) & 7u));
                emit(s, "    float2 uv%u = vTex%u + float2(dot(uIndMtx0.xyz, ind%u) + uIndMtx0.w, dot(uIndMtx1.xyz, ind%u) + uIndMtx1.w);\n",
                     i, st->tex_coord, i, i);
                if (st->mirror & 1u) emit(s, "    uv%u.x = 1.0 - abs(frac(uv%u.x * 0.5) * 2.0 - 1.0);\n", i, i);
                if (st->mirror & 2u) emit(s, "    uv%u.y = 1.0 - abs(frac(uv%u.y * 0.5) * 2.0 - 1.0);\n", i, i);
                emit(s, "    float4 s%u = tex2D(uMap%u, uv%u);\n", i, st->tex_map, i);
            } else if (st->tex_coord < GXR_MAX_TEXCOORDS && (st->mirror & 3u) != 0u) {
                emit(s, "    float2 uv%u = vTex%u;\n", i, st->tex_coord);
                if (st->mirror & 1u) emit(s, "    uv%u.x = 1.0 - abs(frac(uv%u.x * 0.5) * 2.0 - 1.0);\n", i, i);
                if (st->mirror & 2u) emit(s, "    uv%u.y = 1.0 - abs(frac(uv%u.y * 0.5) * 2.0 - 1.0);\n", i, i);
                emit(s, "    float4 s%u = tex2D(uMap%u, uv%u);\n", i, st->tex_map, i);
            } else if (st->tex_coord < GXR_MAX_TEXCOORDS) {
                emit(s, "    float4 s%u = tex2D(uMap%u, vTex%u);\n", i, st->tex_map, st->tex_coord);
            } else {
                emit(s, "    float4 s%u = tex2D(uMap%u, float2(0.0,0.0));\n", i, st->tex_map);
            }
        }
        color_arg(a, sizeof(a), key, st, i, st->color_in[0]);
        color_arg(b, sizeof(b), key, st, i, st->color_in[1]);
        color_arg(c, sizeof(c), key, st, i, st->color_in[2]);
        color_arg(d, sizeof(d), key, st, i, st->color_in[3]);
        emit(s, "    float3 c%u = clamp(", i);
        op_expr(s, st->color_op, st->color_bias, st->color_scale, true, a, b, c, d);
        emit(s, st->color_clamp ? ", 0.0, 1.0);\n" : ", -4.0, 4.0);\n");
        alpha_arg(a, sizeof(a), key, st, i, st->alpha_in[0]);
        alpha_arg(b, sizeof(b), key, st, i, st->alpha_in[1]);
        alpha_arg(c, sizeof(c), key, st, i, st->alpha_in[2]);
        alpha_arg(d, sizeof(d), key, st, i, st->alpha_in[3]);
        emit(s, "    float a%u = clamp(", i);
        op_expr(s, st->alpha_op, st->alpha_bias, st->alpha_scale, false, a, b, c, d);
        emit(s, st->alpha_clamp ? ", 0.0, 1.0);\n" : ", -4.0, 4.0);\n");
        emit(s, "    %s.rgb = c%u;\n    %s.a = a%u;\n", reg_names[st->color_out & 3u], i,
             reg_names[st->alpha_out & 3u], i);
    }
    if (key->stage_count > 0) {
        const GxrStage* last = &key->stages[key->stage_count - 1];
        if ((last->color_out & 3u) != 0u)
            emit(s, "    prev.rgb = %s.rgb;\n", reg_names[last->color_out & 3u]);
        if ((last->alpha_out & 3u) != 0u)
            emit(s, "    prev.a = %s.a;\n", reg_names[last->alpha_out & 3u]);
    }
    if (!trivially_pass) {
        static const char* ops[] = { "&&", "||", "!=", "==" };
        emit(s, "    float ac = floor(clamp(prev.a, 0.0, 1.0) * 255.0 + 0.5);\n");
        emit(s, "    if (!(");
        alpha_compare_expr(s, key->alpha_comp[0], "uAlphaRef0");
        emit(s, " %s ", ops[key->alpha_op & 3u]);
        alpha_compare_expr(s, key->alpha_comp[1], "uAlphaRef1");
        emit(s, ")) discard;\n");
    }
    if (fog_type != GX_FOG_NONE) {
        if ((fog_type & 8u) != 0u)
            emit(s, "    float fogBase = uFogParams.x * vPosition.z;\n");
        else
            emit(s, "    float fogBase = (1.0 / max(vPosition.w, 0.00000001)) * uFogParams.w;\n");
        emit(s, "    float fogF = clamp(fogBase - uFogParams.z, 0.0, 1.0);\n");
        switch (fog_type) {
        case GX_FOG_PERSP_LIN:
        case GX_FOG_ORTHO_LIN: emit(s, "    float fogZ = fogF;\n"); break;
        case GX_FOG_PERSP_EXP:
        case GX_FOG_ORTHO_EXP: emit(s, "    float fogZ = 1.0 - exp2(-8.0 * fogF);\n"); break;
        case GX_FOG_PERSP_EXP2:
        case GX_FOG_ORTHO_EXP2: emit(s, "    float fogZ = 1.0 - exp2(-8.0 * fogF * fogF);\n"); break;
        case GX_FOG_PERSP_REVEXP:
        case GX_FOG_ORTHO_REVEXP: emit(s, "    float fogZ = exp2(-8.0 * (1.0 - fogF));\n"); break;
        case GX_FOG_PERSP_REVEXP2:
        case GX_FOG_ORTHO_REVEXP2:
            emit(s, "    fogF = 1.0 - fogF;\n    float fogZ = exp2(-8.0 * fogF * fogF);\n");
            break;
        default: emit(s, "    float fogZ = 0.0;\n"); break;
        }
        emit(s, "    prev.rgb = lerp(prev.rgb, uFogColor.rgb, clamp(fogZ, 0.0, 1.0));\n");
    }
    if (writes_depth) {
        const u32 stage_index = key->stage_count - 1u;
        const GxrStage* stage = &key->stages[stage_index];
        char texel[32], swapped_texel[128];
        snprintf(texel, sizeof(texel), "s%u", stage_index);
        swapped(swapped_texel, sizeof(swapped_texel), texel, key, stage->swap_tex, false);
        emit(s, "    float3 zt = floor(%s * 255.0 + 0.5);\n", swapped_texel);
        emit(s, "    float z24 = dot(zt, float3(65536.0, 256.0, 1.0)) + uZBias;\n");
        if (z_tex_op == GX_ZT_ADD)
            emit(s, "    z24 += floor(clamp(vPosition.z, 0.0, 1.0) * 16777215.0 + 0.5);\n");
        emit(s, "    z24 -= floor(z24 / 16777216.0) * 16777216.0;\n");
        emit(s, "    gl_FragDepth = z24 / 16777215.0;\n");
    }
    emit(s, "    oColor = clamp(prev, 0.0, 1.0);\n}\n");
    return !s->overflow;
}

/* --------------------------------------------------- vertex source (GPU) */

static const char* tex_attr_name(u8 source)
{
    static const char* names[] = { "aT0", "aT1", "aT2", "aT3" };
    const u32 k = (u32) source - (u32) GX_TG_TEX0;
    return names[k < GXR_GPU_TEX ? k : 0u];
}

static void emit_light_channel(Source* s, const GxrVtxKey* key, u32 index, bool alpha,
                               const char* vc)
{
    const GxrVtxChan* cc = &key->chan[alpha ? 2u + index : index];
    const char* sw = alpha ? ".a" : ".rgb";
    char mat[32], amb[32];
    if (cc->mat_src == GX_SRC_VTX) snprintf(mat, sizeof(mat), "%s", vc);
    else snprintf(mat, sizeof(mat), "uMat[%u]", index);
    if (cc->amb_src == GX_SRC_VTX) snprintf(amb, sizeof(amb), "%s", vc);
    else snprintf(amb, sizeof(amb), "uAmb[%u]", index);
    if (!cc->enabled) {
        emit(s, "    vColor%u%s = %s%s;\n", index, sw, mat, sw);
        return;
    }
    emit(s, "    {\n        float4 lit = %s;\n", amb);
    for (u32 l = 0; l < 8u; ++l) {
        const u32 b = l * 5u;
        if ((cc->lights & (1u << l)) == 0u) continue;
        emit(s, "        {\n");
        emit(s, "            float3 ldir = uLight[%u].xyz - eye;\n", b + 1u);
        emit(s, "            float dist2 = dot(ldir, ldir);\n");
        emit(s, "            float dist = sqrt(dist2);\n");
        emit(s, "            ldir = ldir / max(dist, 1e-8);\n");
        if (cc->atten == GX_AF_SPOT) {
            emit(s, "            float cosine = max(0.0, dot(ldir, uLight[%u].xyz));\n", b + 2u);
            emit(s, "            float cos_attn = dot(uLight[%u].xyz, float3(1.0, cosine, cosine * cosine));\n", b + 3u);
            emit(s, "            float dist_attn = dot(uLight[%u].xyz, float3(1.0, dist, dist2));\n", b + 4u);
            emit(s, "            float attn = max(0.0, cos_attn / dist_attn);\n");
        } else if (cc->atten == GX_AF_SPEC) {
            emit(s, "            float attn = (dot(nrm, ldir) >= 0.0) ? max(0.0, dot(nrm, uLight[%u].xyz)) : 0.0;\n", b + 2u);
            emit(s, "            float cos_attn = dot(uLight[%u].xyz, float3(1.0, attn, attn * attn));\n", b + 3u);
            if (cc->diffuse != GX_DF_NONE)
                emit(s, "            float dist_attn = max(0.0, dot(normalize(uLight[%u].xyz), float3(1.0, attn, attn * attn)));\n", b + 4u);
            else
                emit(s, "            float dist_attn = max(0.0, dot(uLight[%u].xyz, float3(1.0, attn, attn * attn)));\n", b + 4u);
            emit(s, "            attn = max(0.0, cos_attn / dist_attn);\n");
        } else {
            emit(s, "            float attn = 1.0;\n");
        }
        if (cc->diffuse == GX_DF_SIGN) emit(s, "            float diff = dot(ldir, nrm);\n");
        else if (cc->diffuse == GX_DF_CLAMP) emit(s, "            float diff = max(0.0, dot(ldir, nrm));\n");
        else emit(s, "            float diff = 1.0;\n");
        emit(s, "            lit = lit + attn * diff * uLight[%u];\n", b);
        emit(s, "        }\n");
    }
    emit(s, "        vColor%u%s = (%s * clamp(lit, 0.0, 1.0))%s;\n    }\n", index, sw, mat, sw);
}

static bool key_uses_tex_palette(const GxrVtxKey* key)
{
    if (!key->has_mtxidx) return false;
    for (u32 i = 0; i < key->texgen_count && i < GXR_MAX_TEXCOORDS; ++i)
        if (key->tg[i].palette == 2u) return true;
    return false;
}

static void emit_vertex_header(Source* s, const GxrVtxKey* key, bool bump, bool point)
{
    emit(s, "#version 460 core\n");
    emit(s, "layout(location=0) in vec3 aPos;\nlayout(location=1) in float aMtx;\n"
            "layout(location=2) in vec3 aNrm;\nlayout(location=3) in vec4 aC0;\n"
            "layout(location=4) in vec4 aC1;\nlayout(location=5) in vec2 aT0;\n"
            "layout(location=6) in vec2 aT1;\nlayout(location=7) in vec2 aT2;\n"
            "layout(location=8) in vec2 aT3;\n");
    if (bump)
        emit(s, "layout(location=9) in vec3 aBinormal;\nlayout(location=10) in vec3 aTangent;\n");
    emit(s, "%s", k_varyings_out);
    emit(s, "uniform vec4 uPos[30];\nuniform vec4 uNrm[30];\nuniform vec4 uProj[4];\n");
    emit(s, "uniform vec4 uTex[%u];\nuniform vec4 uPost[24];\nuniform vec4 uLight[40];\n",
         !bump && key_uses_tex_palette(key) ? GXR_TEX_PALETTE_ROW + 30u : 24u);
    emit(s, "uniform vec4 uMat[2];\nuniform vec4 uAmb[2];\n");
    if (point) emit(s, "uniform vec4 uPoint;\n");
    emit(s, "void main()\n{\n");
    if (key->has_mtxidx) emit(s, "    int m = int(floor(aMtx / 3.0 + 0.01)) * 3;\n");
    else emit(s, "    int m = 0;\n");
}

static void emit_transform(Source* s, const GxrVtxKey* key, bool point)
{
    if (point) {
        emit(s, "    float2 pointCorner = (aMtx < 0.5) ? float2(-1.0, 1.0) :\n");
        emit(s, "        ((aMtx < 1.5) ? float2(1.0, 1.0) : ((aMtx < 2.5) ? float2(1.0, -1.0) : float2(-1.0, -1.0)));\n");
        emit(s, "    float2 pointTex = (aMtx < 0.5) ? float2(0.0, 0.0) :\n");
        emit(s, "        ((aMtx < 1.5) ? float2(1.0, 0.0) : ((aMtx < 2.5) ? float2(1.0, 1.0) : float2(0.0, 1.0)));\n");
    }
    emit(s, "    float4 p = float4(aPos, 1.0);\n");
    emit(s, "    float3 eye = float3(dot(uPos[m], p), dot(uPos[m + 1], p), dot(uPos[m + 2], p));\n");
    emit(s, "    float3 nrm = float3(dot(uNrm[m].xyz, aNrm), dot(uNrm[m + 1].xyz, aNrm), dot(uNrm[m + 2].xyz, aNrm));\n");
    emit(s, "    float nl2 = dot(nrm, nrm);\n");
    emit(s, "    nrm = (nl2 > 1e-16) ? nrm * (1.0 / sqrt(nl2)) : nrm;\n");
    if (key->perspective) {
        emit(s, "    float xc = eye.x * uProj[0].x + eye.z * uProj[0].y;\n");
        emit(s, "    float yc = eye.y * uProj[0].z + eye.z * uProj[0].w;\n");
        emit(s, "    float zc = uProj[1].y + eye.z * uProj[1].x;\n");
        emit(s, "    float wc = -eye.z;\n");
    } else {
        emit(s, "    float xc = uProj[0].y + eye.x * uProj[0].x;\n");
        emit(s, "    float yc = uProj[0].w + eye.y * uProj[0].z;\n");
        emit(s, "    float zc = uProj[1].y + eye.z * uProj[1].x;\n");
        emit(s, "    float wc = 1.0;\n");
    }
    emit(s, "    float4 vPos = float4(uProj[1].z * wc + uProj[1].w * xc, uProj[2].x * wc + uProj[2].y * yc,\n");
    emit(s, "                         uProj[2].z * wc + zc * uProj[2].w, wc);\n");
    if (point) emit(s, "    vPos.xy = vPos.xy + pointCorner * uPoint.xy * wc;\n");
    /* Vita clip space has +Y at the top row; ours rasterises -Y there. */
    emit(s, "    gl_Position = float4(vPos.x, -vPos.y, vPos.z, vPos.w);\n");
    emit(s, "    vColor0 = float4(0.0, 0.0, 0.0, 0.0);\n    vColor1 = float4(0.0, 0.0, 0.0, 0.0);\n");
    for (u32 i = 0; i < key->channel_count && i < 2u; ++i) {
        const char* vc = i == 0 ? "aC0" : "aC1";
        emit_light_channel(s, key, i, false, vc);
        emit_light_channel(s, key, i, true, vc);
    }
}

static void emit_texgen(Source* s, const GxrVtxKey* key, u32 i, bool allow_palette)
{
    const GxrVtxTexGen* tg = &key->tg[i];
    const u32 r = i * 3u;
    if (tg->source >= GX_TG_TEX0 && tg->source <= GX_TG_TEX7)
        emit(s, "        t.xy = %s;\n", tex_attr_name(tg->source));
    else if (tg->source == GX_TG_POS) emit(s, "        t = aPos;\n");
    else if (tg->source == GX_TG_NRM) emit(s, "        t = aNrm;\n");
    else if (tg->source == GX_TG_COLOR0) emit(s, "        t.xy = vColor0.xy;\n");
    else if (tg->source == GX_TG_COLOR1) emit(s, "        t.xy = vColor1.xy;\n");
    if (tg->has_matrix) {
        const bool pn = tg->source == GX_TG_POS || tg->source == GX_TG_NRM;
        const char* w = tg->source == GX_TG_NRM ? "0.0" : "1.0";
        char r0[32], r1[32], r2[32];
        if (allow_palette && tg->palette == 1u && key->has_mtxidx) {
            snprintf(r0, sizeof(r0), "uPos[m]");
            snprintf(r1, sizeof(r1), "uPos[m + 1]");
            snprintf(r2, sizeof(r2), "uPos[m + 2]");
        } else if (allow_palette && tg->palette == 2u && key->has_mtxidx) {
            snprintf(r0, sizeof(r0), "uTex[%u + m]", GXR_TEX_PALETTE_ROW);
            snprintf(r1, sizeof(r1), "uTex[%u + m]", GXR_TEX_PALETTE_ROW + 1u);
            snprintf(r2, sizeof(r2), "uTex[%u + m]", GXR_TEX_PALETTE_ROW + 2u);
        } else {
            snprintf(r0, sizeof(r0), "uTex[%u]", r);
            snprintf(r1, sizeof(r1), "uTex[%u]", r + 1u);
            snprintf(r2, sizeof(r2), "uTex[%u]", r + 2u);
        }
        emit(s, "        float3 src = float3(t.xy, %s);\n", pn ? "t.z" : "1.0");
        if (tg->type == GX_TG_MTX2x4)
            emit(s, "        t = float3(dot(%s.xyz, src) + %s.w * %s, dot(%s.xyz, src) + %s.w * %s, 1.0);\n",
                 r0, r0, w, r1, r1, w);
        else
            emit(s, "        t = float3(dot(%s.xyz, src) + %s.w * %s, dot(%s.xyz, src) + %s.w * %s, dot(%s.xyz, src) + %s.w * %s);\n",
                 r0, r0, w, r1, r1, w, r2, r2, w);
    }
    if (tg->type == GX_TG_MTX3x4 && !tg->has_post)
        emit(s, "        t.xy = (t.z != 0.0) ? t.xy / t.z : t.xy;\n");
    if (tg->has_post) {
        if (tg->normalize)
            emit(s, "        { float tl = sqrt(dot(t, t)); t = (tl > 1e-8) ? t / tl : t; }\n");
        emit(s, "        float3 pr = float3(dot(uPost[%u].xyz, t) + uPost[%u].w, dot(uPost[%u].xyz, t) + uPost[%u].w, dot(uPost[%u].xyz, t) + uPost[%u].w);\n",
             r, r, r + 1u, r + 1u, r + 2u, r + 2u);
        emit(s, "        t = pr;\n        t.xy = (pr.z != 0.0) ? pr.xy / pr.z : pr.xy;\n");
    }
}

static bool build_vertex_source(const GxrVtxKey* key, bool point, u8 point_tex_mask, Source* s)
{
    emit_vertex_header(s, key, false, point);
    emit_transform(s, key, point);
    for (u32 i = 0; i < GXR_MAX_TEXCOORDS; ++i) {
        if (i >= key->texgen_count) { emit(s, "    vTex%u = float2(0.0, 0.0);\n", i); continue; }
        emit(s, "    {\n        float3 t = float3(0.0, 0.0, 1.0);\n");
        emit_texgen(s, key, i, true);
        emit(s, "        vTex%u = t.xy;\n", i);
        if (point && (point_tex_mask & (1u << i)) != 0u)
            emit(s, "        vTex%u = vTex%u + pointTex * uPoint.z;\n", i, i);
        emit(s, "    }\n");
    }
    emit(s, "}\n");
    return !s->overflow;
}

static bool build_bump_vertex_source(const GxrBumpVtxKey* bump_key, Source* s)
{
    const GxrVtxKey* key = &bump_key->legacy;
    const struct melee_vita_bump_plan* plan = &bump_key->plan;
    emit_vertex_header(s, key, true, false);
    emit_transform(s, key, false);
    emit(s, "    float3 binormal = float3(dot(uNrm[m].xyz, aBinormal), dot(uNrm[m + 1].xyz, aBinormal), dot(uNrm[m + 2].xyz, aBinormal));\n");
    emit(s, "    float3 tangent = float3(dot(uNrm[m].xyz, aTangent), dot(uNrm[m + 1].xyz, aTangent), dot(uNrm[m + 2].xyz, aTangent));\n");
    for (u32 i = 0; i < GXR_MAX_TEXCOORDS; ++i) {
        u8 bump_source, bump_light;
        if (i >= key->texgen_count) { emit(s, "    vTex%u = float2(0.0, 0.0);\n", i); continue; }
        emit(s, "    {\n        float3 t = float3(0.0, 0.0, 1.0);\n");
        if (melee_vita_bump_stage(plan, (u8) i, GX_TG_BUMP0, GX_TG_TEXCOORD0, &bump_source,
                                  &bump_light)) {
            emit(s, "        t.xy = vTex%u;\n", bump_source);
            emit(s, "        float3 ldir = uLight[%u].xyz - eye;\n", (u32) bump_light * 5u + 1u);
            emit(s, "        float ll2 = dot(ldir, ldir);\n");
            emit(s, "        if (ll2 > 1e-16) {\n");
            emit(s, "            ldir = ldir * (1.0 / sqrt(ll2));\n");
            emit(s, "            t.x += dot(ldir, tangent);\n");
            emit(s, "            t.y += dot(ldir, binormal);\n");
            emit(s, "        }\n");
            emit(s, "        vTex%u = t.xy;\n    }\n", i);
            continue;
        }
        emit_texgen(s, key, i, false);
        emit(s, "        vTex%u = t.xy;\n    }\n", i);
    }
    emit(s, "}\n");
    return !s->overflow;
}

static char s_cpu_vs_source[] =
    "#version 460 core\n"
    "layout(location=0) in vec4 aPosition;\n"
    "layout(location=1) in vec4 aColor0;\n"
    "layout(location=2) in vec4 aColor1;\n"
    "layout(location=3) in vec2 aTex0;\nlayout(location=4) in vec2 aTex1;\n"
    "layout(location=5) in vec2 aTex2;\nlayout(location=6) in vec2 aTex3;\n"
    "layout(location=7) in vec2 aTex4;\nlayout(location=8) in vec2 aTex5;\n"
    "layout(location=9) in vec2 aTex6;\nlayout(location=10) in vec2 aTex7;\n"
    "out vec4 vColor0;\nout vec4 vColor1;\n"
    "out vec2 vTex0;\nout vec2 vTex1;\nout vec2 vTex2;\nout vec2 vTex3;\n"
    "out vec2 vTex4;\nout vec2 vTex5;\nout vec2 vTex6;\nout vec2 vTex7;\n"
    "uniform float uPointSize;\n"
    "void main()\n{\n"
    "    gl_Position = vec4(aPosition.x, -aPosition.y, aPosition.z, aPosition.w);\n"
    "    gl_PointSize = uPointSize;\n"
    "    vColor0 = aColor0; vColor1 = aColor1;\n"
    "    vTex0 = aTex0; vTex1 = aTex1; vTex2 = aTex2; vTex3 = aTex3;\n"
    "    vTex4 = aTex4; vTex5 = aTex5; vTex6 = aTex6; vTex7 = aTex7;\n"
    "}\n";

/* --------------------------------------------------- program management */

static GxrProgram* find_fragment_program(const GxrShaderKey* key)
{
    const u64 hash = fnv1a(key, sizeof(*key), UINT64_C(1469598103934665603));
    GxrProgram** bucket = &s_programs[hash % GXR_PROGRAM_BUCKETS];
    GxrProgram* p;
    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    for (p = *bucket; p != NULL; p = p->next)
        if (p->hash == hash && memcmp(&p->key, key, sizeof(*key)) == 0) return p;
    p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->hash = hash;
    p->key = *key;
    p->next = *bucket;
    *bucket = p;
    buffer[0] = '\0';
    if (!build_fragment_source(key, &source)) {
        melee_ps5_log("[GXR] fragment source overflow");
        p->failed = true;
        return p;
    }
    p->shader = compile_glsl(GL_FRAGMENT_SHADER, buffer, "fragment");
    p->failed = p->shader == 0;
    return p;
}

/* RAM front cache: only the stages in use matter. */
#define GXR_KEY_CACHE_SIZE 1024u
typedef struct GxrKeyCacheEntry {
    u32 length;
    GxrProgram* program;
    GxrShaderKey key;
} GxrKeyCacheEntry;
static GxrKeyCacheEntry s_key_cache[GXR_KEY_CACHE_SIZE];
static GxrShaderKey s_last_key;
static GxrProgram* s_last_program;
static u32 s_last_length;

static u32 shader_key_used_length(const GxrShaderKey* key)
{
    const u32 stages = key->stage_count < GXR_MAX_STAGES ? key->stage_count : GXR_MAX_STAGES;
    return (u32) offsetof(GxrShaderKey, stages) + stages * (u32) sizeof(GxrStage);
}

static GxrProgram* lookup_program(const GxrShaderKey* key)
{
    GxrShaderKey normalized;
    GxrKeyCacheEntry* slot;
    GxrProgram* program;
    u32 length, hash = 2166136261u;
    memset(&normalized, 0, sizeof(normalized));
    length = shader_key_used_length(key);
    memcpy(&normalized, key, length);
    melee_vita_normalize_alpha_shader_refs(normalized.alpha_ref);
    if (s_last_program != NULL && s_last_length == length &&
        memcmp(&s_last_key, &normalized, length) == 0)
        return s_last_program;
    for (u32 i = 0; i < length; ++i) hash = (hash ^ ((const u8*) &normalized)[i]) * 16777619u;
    slot = &s_key_cache[(hash ^ (hash >> 15)) & (GXR_KEY_CACHE_SIZE - 1u)];
    if (slot->program != NULL && slot->length == length &&
        memcmp(&slot->key, &normalized, length) == 0) {
        program = slot->program;
    } else {
        program = find_fragment_program(&normalized);
        if (program != NULL && !program->failed) {
            slot->length = length;
            slot->program = program;
            memcpy(&slot->key, &normalized, length);
        }
    }
    if (program != NULL && !program->failed) {
        memcpy(&s_last_key, &normalized, length);
        s_last_length = length;
        s_last_program = program;
    }
    return program;
}

static GxrVtxProgram* find_vertex_program(const GxrVtxKey* key, bool point, u8 point_tex_mask)
{
    const u64 hash = fnv1a(key, sizeof(*key), UINT64_C(1469598103934665603) ^ (point ? 0x100u | point_tex_mask : 0u));
    GxrVtxProgram** bucket = &s_vtx_programs[hash % GXR_PROGRAM_BUCKETS];
    GxrVtxProgram* p;
    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    for (p = *bucket; p != NULL; p = p->next)
        if (p->hash == hash && p->point == point && p->point_tex_mask == point_tex_mask &&
            memcmp(&p->key, key, sizeof(*key)) == 0)
            return p;
    p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->hash = hash;
    p->key = *key;
    p->point = point;
    p->point_tex_mask = point_tex_mask;
    p->next = *bucket;
    *bucket = p;
    buffer[0] = '\0';
    if (!build_vertex_source(key, point, point_tex_mask, &source)) {
        melee_ps5_log("[GXR] vertex source overflow");
        p->failed = true;
        return p;
    }
    p->shader = compile_glsl(GL_VERTEX_SHADER, buffer, "vertex");
    p->failed = p->shader == 0;
    return p;
}

static GxrBumpVtxProgram* find_bump_program(const GxrBumpVtxKey* key)
{
    const u64 hash = fnv1a(key, sizeof(*key), UINT64_C(0xcbf29ce484222325) ^ 0xb0b0u);
    GxrBumpVtxProgram** bucket = &s_bump_programs[hash % GXR_PROGRAM_BUCKETS];
    GxrBumpVtxProgram* p;
    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    for (p = *bucket; p != NULL; p = p->next)
        if (p->hash == hash && memcmp(&p->key, key, sizeof(*key)) == 0) return p;
    p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->hash = hash;
    p->key = *key;
    p->next = *bucket;
    *bucket = p;
    buffer[0] = '\0';
    if (!build_bump_vertex_source(key, &source)) {
        p->failed = true;
        return p;
    }
    p->shader = compile_glsl(GL_VERTEX_SHADER, buffer, "bump vertex");
    p->failed = p->shader == 0;
    return p;
}

static GxrLinked* find_linked(GLuint vs, GLuint fs)
{
    const u32 slot = (u32) ((vs * 2654435761u) ^ (fs * 40503u)) & (GXR_LINK_BUCKETS - 1u);
    GxrLinked* l;
    GLint ok = GL_FALSE;
    for (l = s_linked[slot]; l != NULL; l = l->next)
        if (l->vs == vs && l->fs == fs) return l;
    l = calloc(1, sizeof(*l));
    if (l == NULL) return NULL;
    l->vs = vs;
    l->fs = fs;
    l->next = s_linked[slot];
    s_linked[slot] = l;
    l->program = glCreateProgram();
    glAttachShader(l->program, vs);
    glAttachShader(l->program, fs);
    glLinkProgram(l->program);
    glGetProgramiv(l->program, GL_LINK_STATUS, &ok);
    ++s_stats.links;
    if (!ok) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(l->program, sizeof(log), &length, log);
        if (s_stats.compile_failed++ < 16u)
            melee_ps5_log("[GXR] link failed: %.*s", (int) length, log);
        glDeleteProgram(l->program);
        l->program = 0;
        l->failed = true;
        return l;
    }
    glUseProgram(l->program);
    for (u32 i = 0; i < GXR_MAX_TEXMAPS; ++i) {
        char name[16];
        GLint loc;
        snprintf(name, sizeof(name), "uMap%u", i);
        loc = glGetUniformLocation(l->program, name);
        if (loc >= 0) glUniform1i(loc, (GLint) i);
    }
    l->u_prev = glGetUniformLocation(l->program, "uPrev");
    l->u_reg[0] = glGetUniformLocation(l->program, "uReg0");
    l->u_reg[1] = glGetUniformLocation(l->program, "uReg1");
    l->u_reg[2] = glGetUniformLocation(l->program, "uReg2");
    l->u_k[0] = glGetUniformLocation(l->program, "uK0");
    l->u_k[1] = glGetUniformLocation(l->program, "uK1");
    l->u_k[2] = glGetUniformLocation(l->program, "uK2");
    l->u_k[3] = glGetUniformLocation(l->program, "uK3");
    l->u_alpha_ref[0] = glGetUniformLocation(l->program, "uAlphaRef0");
    l->u_alpha_ref[1] = glGetUniformLocation(l->program, "uAlphaRef1");
    l->u_z_bias = glGetUniformLocation(l->program, "uZBias");
    l->u_fog_color = glGetUniformLocation(l->program, "uFogColor");
    l->u_fog_params = glGetUniformLocation(l->program, "uFogParams");
    l->u_ind_mtx[0] = glGetUniformLocation(l->program, "uIndMtx0");
    l->u_ind_mtx[1] = glGetUniformLocation(l->program, "uIndMtx1");
    l->u_pos = glGetUniformLocation(l->program, "uPos");
    l->u_nrm = glGetUniformLocation(l->program, "uNrm");
    l->u_proj = glGetUniformLocation(l->program, "uProj");
    l->u_tex = glGetUniformLocation(l->program, "uTex");
    l->u_post = glGetUniformLocation(l->program, "uPost");
    l->u_light = glGetUniformLocation(l->program, "uLight");
    l->u_mat = glGetUniformLocation(l->program, "uMat");
    l->u_amb = glGetUniformLocation(l->program, "uAmb");
    l->u_point = glGetUniformLocation(l->program, "uPoint");
    l->u_point_size = glGetUniformLocation(l->program, "uPointSize");
    return l;
}

/* --------------------------------------------------------- vertex layouts */

static void setup_layouts(void)
{
    glGenVertexArrays(LAYOUT_COUNT, s_vao);
    /* CPU path: GxrVertex (position[4], color[2][4], tex[8][2]). */
    glBindVertexArray(s_vao[LAYOUT_CPU]);
    glVertexAttribFormat(0, 4, GL_FLOAT, GL_FALSE, 0);
    glVertexAttribFormat(1, 4, GL_FLOAT, GL_FALSE, 16);
    glVertexAttribFormat(2, 4, GL_FLOAT, GL_FALSE, 32);
    for (u32 i = 0; i < 8u; ++i) glVertexAttribFormat(3 + i, 2, GL_FLOAT, GL_FALSE, 48 + i * 8);
    for (u32 i = 0; i < 11u; ++i) {
        glVertexAttribBinding(i, 0);
        glEnableVertexAttribArray(i);
    }
    /* GPU path: GxrGpuVertex (68 bytes); bump adds binormal and tangent. */
    for (u32 layout = LAYOUT_GPU; layout <= LAYOUT_BUMP; ++layout) {
        const u32 attributes = layout == LAYOUT_BUMP ? 11u : 9u;
        glBindVertexArray(s_vao[layout]);
        glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribFormat(1, 1, GL_FLOAT, GL_FALSE, 12);
        glVertexAttribFormat(2, 3, GL_FLOAT, GL_FALSE, 16);
        glVertexAttribFormat(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 28);
        glVertexAttribFormat(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, 32);
        for (u32 i = 0; i < 4u; ++i) glVertexAttribFormat(5 + i, 2, GL_FLOAT, GL_FALSE, 36 + i * 8);
        if (layout == LAYOUT_BUMP) {
            glVertexAttribFormat(9, 3, GL_FLOAT, GL_FALSE, 68);
            glVertexAttribFormat(10, 3, GL_FLOAT, GL_FALSE, 80);
        }
        for (u32 i = 0; i < attributes; ++i) {
            glVertexAttribBinding(i, 0);
            glEnableVertexAttribArray(i);
        }
    }
    glBindVertexArray(0);
}

/* ------------------------------------------------------------- streaming */


static void ring_begin_frame(void)
{
    const u32 frame = melee_ps5_frame_counter();
    if (frame == s_ring_frame) return;
    if (s_ring_frame != UINT32_MAX) {
        /* Everything recorded so far read the previous segment. */
        if (s_ring[s_ring_index].fence != NULL) glDeleteSync(s_ring[s_ring_index].fence);
        s_ring[s_ring_index].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        s_ring_index = (s_ring_index + 1u) % GXR_RING_FRAMES;
    }
    s_ring_frame = frame;
    if (s_ring[s_ring_index].fence != NULL) {
        const GLenum r = glClientWaitSync(s_ring[s_ring_index].fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                                          1000000000ull);
        if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) {
            static u32 logged;
            if (logged++ < 4u) melee_ps5_log("[GXR] ring fence wait failed (0x%x)", (unsigned) r);
        }
        glDeleteSync(s_ring[s_ring_index].fence);
        s_ring[s_ring_index].fence = NULL;
    }
    s_vbo = s_ring[s_ring_index].vbo;
    s_ibo = s_ring[s_ring_index].ibo;
    s_vbo_ptr = s_ring[s_ring_index].vbo_ptr;
    s_ibo_ptr = s_ring[s_ring_index].ibo_ptr;
    s_vbo_used = s_ibo_used = 0;
}

static bool ring_create(void)
{
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    for (u32 i = 0; i < GXR_RING_FRAMES; ++i) {
        glGenBuffers(1, &s_ring[i].vbo);
        glGenBuffers(1, &s_ring[i].ibo);
        glBindBuffer(GL_ARRAY_BUFFER, s_ring[i].vbo);
        glBufferStorage(GL_ARRAY_BUFFER, GXR_VERTEX_RING, NULL, flags);
        s_ring[i].vbo_ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, GXR_VERTEX_RING, flags);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ring[i].ibo);
        glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, GXR_INDEX_RING, NULL, flags);
        s_ring[i].ibo_ptr = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, GXR_INDEX_RING, flags);
        if (s_ring[i].vbo_ptr == NULL || s_ring[i].ibo_ptr == NULL) {
            melee_ps5_log("[GXR] persistent ring map failed (0x%04x)", (unsigned) glGetError());
            return false;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_vbo = s_ring[0].vbo;
    s_ibo = s_ring[0].ibo;
    s_vbo_ptr = s_ring[0].vbo_ptr;
    s_ibo_ptr = s_ring[0].ibo_ptr;
    melee_ps5_log("[GXR] persistent streaming ring: %u x (%u + %u) MB", GXR_RING_FRAMES,
                  GXR_VERTEX_RING >> 20, GXR_INDEX_RING >> 20);
    return true;
}

static bool ring_upload(u8* base, u32* used, u32 capacity, const void* data,
                        u32 bytes, u32 align, u32* offset)
{
    u32 at = (*used + align - 1u) / align * align;
    if (at + bytes > capacity) {
        static u32 logged;
        if (logged++ < 4u) melee_ps5_log("[GXR] streaming ring full (%u bytes)", bytes);
        return false;
    }
    memcpy(base + at, data, bytes);
    *used = at + bytes;
    *offset = at;
    return true;
}

/* -------------------------------------------------------- draw recording */

typedef struct RqDraw {
    GxrLinked* linked;
    u8 layout;
    u8 primitive;
    u8 cull;
    u8 depth_func;
    u8 depth_write;
    u8 color_mask;
    u8 blend_enable;
    u8 texture_mask;
    GLenum blend_eq, blend_src, blend_dst;
    const void* vertices;
    u32 vertex_bytes;
    const u16* indices;
    u32 count;
    f32 point_size;
    vita2d_texture* textures[GXR_MAX_TEXMAPS];
    f32 registers[4][4];
    f32 konst[4][4];
    f32 ind_mtx[2][4];
    f32 alpha_ref[2];
    f32 z_bias;
    f32 fog_color[4];
    f32 fog_params[4];
    u16 mtx_comps, tex_comps, tg_comps, light_comps, point_comps;
    f32 uniforms[]; /* pos, nrm, proj, tex, post, light, mat, amb, point */
} RqDraw;

static GLenum blend_factor(u8 gx, bool source)
{
    switch (gx) {
    case GX_BL_ZERO: return GL_ZERO;
    case GX_BL_ONE: return GL_ONE;
    case GX_BL_SRCCLR: return source ? GL_DST_COLOR : GL_SRC_COLOR;
    case GX_BL_INVSRCCLR: return source ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case GX_BL_SRCALPHA: return GL_SRC_ALPHA;
    case GX_BL_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case GX_BL_DSTALPHA: return GL_DST_ALPHA;
    case GX_BL_INVDSTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_ONE;
    }
}

static void fill_state(RqDraw* d, const GxrDraw* draw)
{
    d->color_mask = (u8) ((draw->color_update ? 7u : 0u) | (draw->alpha_update ? 8u : 0u));
    d->blend_enable = 0;
    d->blend_eq = GL_FUNC_ADD;
    d->blend_src = GL_ONE;
    d->blend_dst = GL_ZERO;
    switch (draw->blend_mode) {
    case GX_BM_BLEND:
        d->blend_enable = 1;
        d->blend_src = blend_factor(draw->blend_src, true);
        d->blend_dst = blend_factor(draw->blend_dst, false);
        break;
    case GX_BM_SUBTRACT:
        d->blend_enable = 1;
        d->blend_eq = GL_FUNC_REVERSE_SUBTRACT;
        d->blend_src = d->blend_dst = GL_ONE;
        break;
    case GX_BM_LOGIC:
        if (draw->logic_op == GX_LO_CLEAR) {
            d->blend_enable = 1;
            d->blend_src = d->blend_dst = GL_ZERO;
        } else if (draw->logic_op == GX_LO_NOOP) {
            d->blend_enable = 1;
            d->blend_src = GL_ZERO;
            d->blend_dst = GL_ONE;
        }
        break;
    default:
        break;
    }
    d->depth_func = draw->depth_compare ? (draw->depth_function & 7u) : 7u /* always */;
    d->depth_write = draw->depth_write ? 1u : 0u;
    d->primitive = draw->primitive;
    d->point_size = draw->line_width < 1.0f ? 1.0f : draw->line_width;
    memcpy(d->registers, draw->registers, sizeof(d->registers));
    memcpy(d->konst, draw->konst, sizeof(d->konst));
    memcpy(d->ind_mtx, draw->ind_mtx, sizeof(d->ind_mtx));
    d->alpha_ref[0] = (f32) draw->key.alpha_ref[0];
    d->alpha_ref[1] = (f32) draw->key.alpha_ref[1];
    d->z_bias = (f32) draw->z_tex_bias;
    memcpy(d->fog_color, draw->fog_color, sizeof(d->fog_color));
    memcpy(d->fog_params, draw->fog_params, sizeof(d->fog_params));
}

static struct {
    u32 epoch;
    MeleeVitaTextureSource source;
    vita2d_texture* texture;
} s_texture_memo[GXR_MAX_TEXMAPS];

static vita2d_texture* resolve_texture_memo(u32 map, const MeleeVitaTextureSource* source)
{
    vita2d_texture* texture;
    if (s_texture_memo[map].epoch == g_melee_vita_texture_memo_epoch &&
        memcmp(&s_texture_memo[map].source, source, sizeof(*source)) == 0)
        return s_texture_memo[map].texture;
    texture = melee_vita_gxm_texture(source);
    if (texture != NULL) {
        s_texture_memo[map].epoch = g_melee_vita_texture_memo_epoch;
        s_texture_memo[map].source = *source;
        s_texture_memo[map].texture = texture;
    }
    return texture;
}

static void resolve_textures(const GxrDraw* draw, RqDraw* d)
{
    for (u32 map = 0; map < GXR_MAX_TEXMAPS; ++map) {
        bool used = false;
        vita2d_texture* texture;
        for (u32 i = 0; i < draw->key.stage_count; ++i) {
            if (draw->key.stages[i].tex_map == map ||
                ((draw->key.stages[i].mirror & 0x80u) != 0u &&
                 ((draw->key.stages[i].mirror >> 5) & 3u) == map)) {
                used = true;
                break;
            }
        }
        if (!used) continue;
        texture = draw->texture_valid[map] ? resolve_texture_memo(map, &draw->textures[map]) : NULL;
        d->textures[map] = texture != NULL ? texture : s_white;
        d->texture_mask |= (u8) (1u << map);
    }
}

static void exec_draw(const void* payload);

static u32 max_index(const u16* indices, u32 count)
{
    u32 top = 0;
    for (u32 i = 0; i < count; ++i)
        if (indices[i] > top) top = indices[i];
    return top;
}

static RqDraw* push_draw(u32 uniform_floats)
{
    RqDraw* d = melee_vita_rq_push_direct(exec_draw, (u32) sizeof(RqDraw) + uniform_floats * 4u);
    if (d != NULL) memset(d, 0, sizeof(*d));
    return d;
}

static u32 tex_uniform_comps(const GxrVtxKey* key)
{
    return key_uses_tex_palette(key) ? (GXR_TEX_PALETTE_ROW + 30u) * 4u : key->texgen_count * 12u;
}

static u32 lights_top(u32 lights_used)
{
    u32 top = 8u;
    while (top > 0u && (lights_used & (1u << (top - 1u))) == 0u) --top;
    return top;
}

/* ------------------------------------------------------------- public API */

void* gxr_arena_alloc(u32 size) { return malloc((size + 15u) & ~15u); }
void gxr_arena_free(void* block) { free(block); }

GxrVertex* gxr_alloc_vertices(u32 count)
{
    if (!s_ready || count == 0) return NULL;
    return melee_vita_rq_alloc_gpu(count * sizeof(GxrVertex), 16);
}

u16* gxr_alloc_indices(u32 count)
{
    if (!s_ready || count == 0) return NULL;
    return melee_vita_rq_alloc_gpu(count * sizeof(u16), 16);
}

bool gxr_available(void) { return s_ready; }
void gxr_set_runtime_shader_compilation_enabled(bool enabled) { (void) enabled; }
bool gxr_runtime_shader_compilation_enabled(void) { return true; }
void gxr_flush_warm_cache(void) {}

bool gxr_draw(const GxrDraw* draw, GxrVertex* vertices, const u16* indices, u32 count)
{
    GxrProgram* program;
    GxrLinked* linked;
    RqDraw* d;
    if (!s_ready || draw == NULL || vertices == NULL || count == 0) return false;
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) { ++s_stats.fallback; return false; }
    linked = find_linked(s_cpu_vs, program->shader);
    if (linked == NULL || linked->failed) { ++s_stats.fallback; return false; }
    d = push_draw(0);
    if (d == NULL) return false;
    fill_state(d, draw);
    d->linked = linked;
    d->layout = LAYOUT_CPU;
    d->cull = GXR_CULL_NONE;
    d->vertices = vertices;
    d->indices = indices;
    d->count = count;
    d->vertex_bytes = (indices != NULL ? max_index(indices, count) + 1u : count) * sizeof(GxrVertex);
    resolve_textures(draw, d);
    ++s_stats.draws;
    return true;
}

static bool draw_gpu(const GxrDraw* draw, const GxrVtxKey* vkey, const GxrVtxUniforms* u,
                     const GxrPointParams* point, const GxrGpuVertex* vertices,
                     const u16* indices, u32 count, u8 cull)
{
    GxrVtxProgram* vp;
    GxrProgram* program;
    GxrLinked* linked;
    RqDraw* d;
    u32 mtx_comps, tex_comps, tg_comps, light_comps, total;
    f32* out;
    if (!s_ready || draw == NULL || vkey == NULL || u == NULL || vertices == NULL ||
        indices == NULL || count == 0)
        return false;
    if (point != NULL && vkey->has_mtxidx) return false;
    if (cull == GXR_CULL_ALL) return true;
    vp = find_vertex_program(vkey, point != NULL, point != NULL ? point->tex_offset_mask : 0u);
    if (vp == NULL || vp->failed) { ++s_stats.fallback; return false; }
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) { ++s_stats.fallback; return false; }
    linked = find_linked(vp->shader, program->shader);
    if (linked == NULL || linked->failed) { ++s_stats.fallback; return false; }

    mtx_comps = vkey->has_mtxidx ? 120u : 12u;
    tex_comps = tex_uniform_comps(vkey);
    tg_comps = vkey->texgen_count * 12u;
    light_comps = lights_top((u32) (vkey->chan[0].lights | vkey->chan[1].lights |
                                    vkey->chan[2].lights | vkey->chan[3].lights)) * 20u;
    total = 2u * mtx_comps + 16u + tex_comps + tg_comps + light_comps + 16u + (point ? 4u : 0u);
    d = push_draw(total);
    if (d == NULL) return false;
    fill_state(d, draw);
    d->linked = linked;
    d->layout = LAYOUT_GPU;
    d->cull = cull;
    d->vertices = vertices;
    d->indices = indices;
    d->count = count;
    d->vertex_bytes = (max_index(indices, count) + 1u) * (u32) sizeof(GxrGpuVertex);
    d->mtx_comps = (u16) mtx_comps;
    d->tex_comps = (u16) tex_comps;
    d->tg_comps = (u16) tg_comps;
    d->light_comps = (u16) light_comps;
    d->point_comps = point ? 4u : 0u;
    out = d->uniforms;
    memcpy(out, u->pos, mtx_comps * 4u); out += mtx_comps;
    memcpy(out, u->nrm, mtx_comps * 4u); out += mtx_comps;
    memcpy(out, u->proj, 64u); out += 16u;
    memcpy(out, u->tex, tex_comps * 4u); out += tex_comps;
    memcpy(out, u->post, tg_comps * 4u); out += tg_comps;
    memcpy(out, u->light, light_comps * 4u); out += light_comps;
    memcpy(out, u->mat, 32u); out += 8u;
    memcpy(out, u->amb, 32u); out += 8u;
    if (point != NULL) {
        out[0] = point->clip_half_x;
        out[1] = point->clip_half_y;
        out[2] = point->tex_span;
        out[3] = 0.0f;
    }
    resolve_textures(draw, d);
    ++s_stats.draws;
    return true;
}

bool gxr_draw_gpu(const GxrDraw* draw, const GxrVtxKey* vkey, const GxrVtxUniforms* uniforms,
                  const GxrGpuVertex* vertices, const u16* indices, u32 count, u8 cull)
{
    return draw_gpu(draw, vkey, uniforms, NULL, vertices, indices, count, cull);
}

bool gxr_draw_gpu_points(const GxrDraw* draw, const GxrVtxKey* vkey,
                         const GxrVtxUniforms* uniforms, const GxrPointParams* point,
                         const GxrGpuVertex* vertices, const u16* indices, u32 count)
{
    if (point == NULL) return false;
    return draw_gpu(draw, vkey, uniforms, point, vertices, indices, count, GXR_CULL_NONE);
}

bool gxr_draw_bump_gpu(const GxrDraw* draw, const GxrBumpVtxKey* vkey,
                       const GxrVtxUniforms* u, const GxrGpuBumpVertex* vertices,
                       const u16* indices, u32 count, u8 cull)
{
    GxrBumpVtxProgram* vp;
    GxrProgram* program;
    GxrLinked* linked;
    RqDraw* d;
    u32 mtx_comps, tg_comps, light_comps, lights_used, total;
    f32* out;
    if (!s_ready || draw == NULL || vkey == NULL || u == NULL || vertices == NULL ||
        indices == NULL || count == 0u || vkey->plan.mask == 0u)
        return false;
    if (cull == GXR_CULL_ALL) return true;
    vp = find_bump_program(vkey);
    if (vp == NULL || vp->failed) { ++s_stats.fallback; return false; }
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) { ++s_stats.fallback; return false; }
    linked = find_linked(vp->shader, program->shader);
    if (linked == NULL || linked->failed) { ++s_stats.fallback; return false; }
    mtx_comps = vkey->legacy.has_mtxidx ? 120u : 12u;
    tg_comps = vkey->legacy.texgen_count * 12u;
    lights_used = (u32) (vkey->legacy.chan[0].lights | vkey->legacy.chan[1].lights |
                         vkey->legacy.chan[2].lights | vkey->legacy.chan[3].lights);
    for (u32 i = 0; i < vkey->plan.count; ++i)
        if ((vkey->plan.mask & (1u << i)) != 0u)
            lights_used |= 1u << (vkey->plan.type[i] - (u32) GX_TG_BUMP0);
    light_comps = lights_top(lights_used) * 20u;
    total = 2u * mtx_comps + 16u + 2u * tg_comps + light_comps + 16u;
    d = push_draw(total);
    if (d == NULL) return false;
    fill_state(d, draw);
    d->linked = linked;
    d->layout = LAYOUT_BUMP;
    d->cull = cull;
    d->vertices = vertices;
    d->indices = indices;
    d->count = count;
    d->vertex_bytes = (max_index(indices, count) + 1u) * (u32) sizeof(GxrGpuBumpVertex);
    d->mtx_comps = (u16) mtx_comps;
    d->tex_comps = (u16) tg_comps;
    d->tg_comps = (u16) tg_comps;
    d->light_comps = (u16) light_comps;
    out = d->uniforms;
    memcpy(out, u->pos, mtx_comps * 4u); out += mtx_comps;
    memcpy(out, u->nrm, mtx_comps * 4u); out += mtx_comps;
    memcpy(out, u->proj, 64u); out += 16u;
    memcpy(out, u->tex, tg_comps * 4u); out += tg_comps;
    memcpy(out, u->post, tg_comps * 4u); out += tg_comps;
    memcpy(out, u->light, light_comps * 4u); out += light_comps;
    memcpy(out, u->mat, 32u); out += 8u;
    memcpy(out, u->amb, 32u);
    resolve_textures(draw, d);
    ++s_stats.draws;
    return true;
}

/* ------------------------------------------------------------- execution */

static const GLenum k_depth_funcs[8] = {
    GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS,
};

static struct {
    u32 epoch;
    GLuint program;
    u8 layout;
    u8 cull;
    u8 depth_func;
    u8 depth_write;
    u8 color_mask;
    u8 blend_enable;
    GLenum blend_eq, blend_src, blend_dst;
} s_rt;

static void apply_cull(u8 cull)
{
    /* Pixel rows (and so window-space winding) match the Vita's; the Vita
     * renderer verified GX back faces as GXM's CCW cull. */
    if (cull == GXR_CULL_NONE) {
        glDisable(GL_CULL_FACE);
        return;
    }
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW);
#ifdef MELEE_PS5_FLIP_CULL
    glCullFace(cull == GXR_CULL_BACK ? GL_FRONT : GL_BACK);
#else
    glCullFace(cull == GXR_CULL_BACK ? GL_BACK : GL_FRONT);
#endif
}

static u64 s_t_upload, s_t_state, s_t_tex, s_t_uni, s_t_draw;
static u32 s_t_batches;
static u32 s_t_draws, s_t_cpu, s_t_merge_cpu, s_t_merge_gpu, s_t_same_state, s_t_unindexed;
extern bool g_ps5_rq_follows_same;

/* How many draws could join the previous one (same program, state,
 * textures and uniforms, adjacent in the queue): the payoff of batching. */
static void merge_stats(const RqDraw* d)
{
    static const RqDraw* prev;
    static u8 prev_copy[sizeof(RqDraw) + 256 * 4];
    static u32 prev_uniform_bytes;
    const RqDraw* p = (const RqDraw*) prev_copy;
    const u32 uniform_bytes = d->layout == LAYOUT_CPU ? 0u
        : (u32) (2u * d->mtx_comps + 16u + d->tex_comps + d->tg_comps + d->light_comps + 16u +
                 d->point_comps) * 4u;
    if (d->layout == LAYOUT_CPU) ++s_t_cpu;
    if (d->indices == NULL) ++s_t_unindexed;
    if (prev != NULL && g_ps5_rq_follows_same && p->linked == d->linked && p->layout == d->layout &&
        p->primitive == d->primitive && p->cull == d->cull && p->depth_func == d->depth_func &&
        p->depth_write == d->depth_write && p->color_mask == d->color_mask &&
        p->blend_enable == d->blend_enable && p->blend_eq == d->blend_eq &&
        p->blend_src == d->blend_src && p->blend_dst == d->blend_dst &&
        p->texture_mask == d->texture_mask &&
        memcmp(p->textures, d->textures, sizeof(d->textures)) == 0) {
        ++s_t_same_state;
        if (memcmp(p->registers, d->registers,
                   (size_t) ((const u8*) &d->mtx_comps - (const u8*) d->registers)) == 0 &&
            p->point_size == d->point_size) {
            if (d->layout == LAYOUT_CPU) ++s_t_merge_cpu;
            else if (prev_uniform_bytes == uniform_bytes &&
                     memcmp(p->uniforms, d->uniforms, uniform_bytes) == 0) ++s_t_merge_gpu;
        }
    }
    prev = d;
    if (sizeof(RqDraw) + uniform_bytes <= sizeof(prev_copy)) {
        memcpy(prev_copy, d, sizeof(RqDraw) + uniform_bytes);
        prev_uniform_bytes = uniform_bytes;
    } else {
        prev = NULL;
    }
}

void melee_ps5_gxr_log_timing(void)
{
    if (s_t_draws == 0) return;
    melee_ps5_log("[GXRTIME] per frame: draws=%u upload=%.2fms state=%.2fms tex=%.2fms "
                  "uniform=%.2fms draw=%.2fms (%.1fus/draw call)",
                  s_t_draws / 120u, s_t_upload / 120000.0, s_t_state / 120000.0,
                  s_t_tex / 120000.0, s_t_uni / 120000.0, s_t_draw / 120000.0,
                  (double) s_t_draw / s_t_draws);
    melee_ps5_log("[GXRMERGE] per frame: cpu=%u unindexed=%u same_state=%u mergeable_cpu=%u "
                  "mergeable_gpu=%u -> GL draws=%u", s_t_cpu / 120u, s_t_unindexed / 120u,
                  s_t_same_state / 120u, s_t_merge_cpu / 120u, s_t_merge_gpu / 120u,
                  s_t_batches / 120u);
    s_t_batches = 0;
    s_t_upload = s_t_state = s_t_tex = s_t_uni = s_t_draw = 0;
    s_t_draws = s_t_cpu = s_t_merge_cpu = s_t_merge_gpu = s_t_same_state = s_t_unindexed = 0;
}

static void reset_state_cache(void)
{
    if (s_rt.epoch != g_melee_vita_gxm_state_epoch) {
        s_rt.epoch = g_melee_vita_gxm_state_epoch;
        s_rt.program = 0;
        s_rt.layout = 0xff;
        s_rt.cull = 0xff;
        s_rt.depth_func = 0xff;
        s_rt.depth_write = 0xff;
        s_rt.color_mask = 0xff;
        s_rt.blend_enable = 0xff;
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_PROGRAM_POINT_SIZE);
    }
}

/* Issues one draw whose vertices and indices are already in the ring. */
static void submit_draw(const RqDraw* d, u32 voffset, u32 ioffset, u32 count, bool indexed)
{
    static const u32 strides[LAYOUT_COUNT] = { sizeof(GxrVertex), sizeof(GxrGpuVertex),
                                               sizeof(GxrGpuBumpVertex) };
    const GxrLinked* l = d->linked;
    u64 t0 = sceKernelGetProcessTimeWide(), t1;
    reset_state_cache();

    if (s_rt.program != l->program) {
        glUseProgram(l->program);
        s_rt.program = l->program;
    }
    if (s_rt.layout != d->layout) {
        glBindVertexArray(s_vao[d->layout]);
        s_rt.layout = d->layout;
    }
    glBindVertexBuffer(0, s_vbo, voffset, (GLsizei) strides[d->layout]);
    if (indexed) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
    if (s_rt.cull != d->cull) {
        apply_cull(d->cull);
        s_rt.cull = d->cull;
    }
    if (s_rt.depth_func != d->depth_func) {
        glDepthFunc(k_depth_funcs[d->depth_func & 7u]);
        s_rt.depth_func = d->depth_func;
    }
    if (s_rt.depth_write != d->depth_write) {
        glDepthMask(d->depth_write ? GL_TRUE : GL_FALSE);
        s_rt.depth_write = d->depth_write;
    }
    if (s_rt.color_mask != d->color_mask) {
        glColorMask((d->color_mask & 1u) != 0, (d->color_mask & 2u) != 0,
                    (d->color_mask & 4u) != 0, (d->color_mask & 8u) != 0);
        s_rt.color_mask = d->color_mask;
    }
    if (s_rt.blend_enable != d->blend_enable || s_rt.blend_eq != d->blend_eq ||
        s_rt.blend_src != d->blend_src || s_rt.blend_dst != d->blend_dst) {
        if (d->blend_enable) {
            glEnable(GL_BLEND);
            glBlendEquation(d->blend_eq);
            glBlendFunc(d->blend_src, d->blend_dst);
        } else {
            glDisable(GL_BLEND);
        }
        s_rt.blend_enable = d->blend_enable;
        s_rt.blend_eq = d->blend_eq;
        s_rt.blend_src = d->blend_src;
        s_rt.blend_dst = d->blend_dst;
    }
    t1 = sceKernelGetProcessTimeWide(); s_t_state += t1 - t0; t0 = t1;
    for (u32 map = 0; map < GXR_MAX_TEXMAPS; ++map) {
        if (!(d->texture_mask & (1u << map))) continue;
        glActiveTexture(GL_TEXTURE0 + map);
        glBindTexture(GL_TEXTURE_2D, melee_ps5_texture_name(d->textures[map]));
    }
    t1 = sceKernelGetProcessTimeWide(); s_t_tex += t1 - t0; t0 = t1;

    /* Fragment uniforms. */
    if (l->u_prev >= 0) glUniform4fv(l->u_prev, 1, d->registers[0]);
    for (u32 i = 0; i < 3u; ++i)
        if (l->u_reg[i] >= 0) glUniform4fv(l->u_reg[i], 1, d->registers[i + 1]);
    for (u32 i = 0; i < 4u; ++i)
        if (l->u_k[i] >= 0) glUniform4fv(l->u_k[i], 1, d->konst[i]);
    for (u32 i = 0; i < 2u; ++i) {
        if (l->u_alpha_ref[i] >= 0) glUniform1f(l->u_alpha_ref[i], d->alpha_ref[i]);
        if (l->u_ind_mtx[i] >= 0) glUniform4fv(l->u_ind_mtx[i], 1, d->ind_mtx[i]);
    }
    if (l->u_z_bias >= 0) glUniform1f(l->u_z_bias, d->z_bias);
    if (l->u_fog_color >= 0) glUniform4fv(l->u_fog_color, 1, d->fog_color);
    if (l->u_fog_params >= 0) glUniform4fv(l->u_fog_params, 1, d->fog_params);

    /* Vertex uniforms. */
    if (d->layout == LAYOUT_CPU) {
        if (l->u_point_size >= 0)
            glUniform1f(l->u_point_size, d->point_size * melee_ps5_target_pixel_scale());
    } else {
        const f32* in = d->uniforms;
        if (l->u_pos >= 0) glUniform4fv(l->u_pos, d->mtx_comps / 4u, in);
        in += d->mtx_comps;
        if (l->u_nrm >= 0) glUniform4fv(l->u_nrm, d->mtx_comps / 4u, in);
        in += d->mtx_comps;
        if (l->u_proj >= 0) glUniform4fv(l->u_proj, 4, in);
        in += 16u;
        if (l->u_tex >= 0 && d->tex_comps) glUniform4fv(l->u_tex, d->tex_comps / 4u, in);
        in += d->tex_comps;
        if (l->u_post >= 0 && d->tg_comps) glUniform4fv(l->u_post, d->tg_comps / 4u, in);
        in += d->tg_comps;
        if (l->u_light >= 0 && d->light_comps) glUniform4fv(l->u_light, d->light_comps / 4u, in);
        in += d->light_comps;
        if (l->u_mat >= 0) glUniform4fv(l->u_mat, 2, in);
        in += 8u;
        if (l->u_amb >= 0) glUniform4fv(l->u_amb, 2, in);
        in += 8u;
        if (l->u_point >= 0 && d->point_comps) glUniform4fv(l->u_point, 1, in);
    }

    t1 = sceKernelGetProcessTimeWide(); s_t_uni += t1 - t0; t0 = t1;
    {
        const GLenum mode = d->primitive == GXR_PRIM_LINES ? GL_LINES
                          : d->primitive == GXR_PRIM_POINTS && d->layout == LAYOUT_CPU ? GL_POINTS
                          : GL_TRIANGLES;
        if (indexed)
            glDrawElements(mode, (GLsizei) count, GL_UNSIGNED_SHORT, (const void*) (uintptr_t) ioffset);
        else
            glDrawArrays(mode, 0, (GLsizei) count);
    }
    s_t_draw += sceKernelGetProcessTimeWide() - t0;
    ++s_t_draws;
}

/* ------------------------------------------------------------- batching */

/* The CPU path issues many small draws that differ only in their vertices
 * (2,000 per frame in a match, ~78% sharing all state with the previous
 * one), and every draw costs ~36 us in the PS5 driver.  Consecutive
 * compatible draws are concatenated in the ring (indices rebased) and
 * issued as one. */
static struct {
    const RqDraw* first; /* state source; lives in the frame arena */
    u32 voffset, vcount; /* vertex region in the ring */
    u32 ioffset, icount;
    bool active;
} s_pend;

static bool draws_compatible(const RqDraw* a, const RqDraw* b)
{
    return a->linked == b->linked && a->layout == b->layout && a->primitive == b->primitive &&
           a->cull == b->cull && a->depth_func == b->depth_func &&
           a->depth_write == b->depth_write && a->color_mask == b->color_mask &&
           a->blend_enable == b->blend_enable && a->blend_eq == b->blend_eq &&
           a->blend_src == b->blend_src && a->blend_dst == b->blend_dst &&
           a->texture_mask == b->texture_mask && a->point_size == b->point_size &&
           memcmp(a->textures, b->textures, sizeof(a->textures)) == 0 &&
           memcmp(a->registers, b->registers,
                  (size_t) ((const u8*) &a->mtx_comps - (const u8*) a->registers)) == 0;
}

void gxr_flush_pending(void)
{
    if (!s_pend.active) return;
    s_pend.active = false;
    ++s_t_batches;
    submit_draw(s_pend.first, s_pend.voffset, s_pend.ioffset, s_pend.icount, true);
}

static bool pend_append(const RqDraw* d)
{
    const u32 vcount = d->vertex_bytes / (u32) sizeof(GxrVertex);
    const u32 base = s_pend.vcount;
    u16* out;
    if (base + vcount > 65536u || s_vbo_used + d->vertex_bytes > GXR_VERTEX_RING ||
        s_ibo_used + d->count * 2u > GXR_INDEX_RING)
        return false;
    /* Both regions end exactly where this draw's data goes. */
    memcpy(s_vbo_ptr + s_vbo_used, d->vertices, d->vertex_bytes);
    s_vbo_used += d->vertex_bytes;
    out = (u16*) (s_ibo_ptr + s_ibo_used);
    for (u32 i = 0; i < d->count; ++i) out[i] = (u16) (d->indices[i] + base);
    s_ibo_used += d->count * 2u;
    s_pend.vcount += vcount;
    s_pend.icount += d->count;
    return true;
}

static void exec_draw(const void* payload)
{
    const RqDraw* d = payload;
    u64 t0;
    u32 voffset, ioffset = 0;
    merge_stats(payload);
    ring_begin_frame();
    t0 = sceKernelGetProcessTimeWide();
    if (d->layout == LAYOUT_CPU && d->indices != NULL) {
        if (s_pend.active && draws_compatible(s_pend.first, d) && pend_append(d)) {
            s_t_upload += sceKernelGetProcessTimeWide() - t0;
            return;
        }
        gxr_flush_pending();
        /* Start a batch: stride-aligned vertices so later ones can follow. */
        if (!ring_upload(s_vbo_ptr, &s_vbo_used, GXR_VERTEX_RING, d->vertices, d->vertex_bytes,
                         64u, &voffset) ||
            !ring_upload(s_ibo_ptr, &s_ibo_used, GXR_INDEX_RING, d->indices, d->count * 2u, 4u,
                         &ioffset))
            return;
        s_pend.first = d;
        s_pend.voffset = voffset;
        s_pend.vcount = d->vertex_bytes / (u32) sizeof(GxrVertex);
        s_pend.ioffset = ioffset;
        s_pend.icount = d->count;
        s_pend.active = true;
        s_t_upload += sceKernelGetProcessTimeWide() - t0;
        return;
    }
    gxr_flush_pending();
    if (!ring_upload(s_vbo_ptr, &s_vbo_used, GXR_VERTEX_RING, d->vertices,
                     d->vertex_bytes, 64u, &voffset))
        return;
    if (d->indices != NULL &&
        !ring_upload(s_ibo_ptr, &s_ibo_used, GXR_INDEX_RING, d->indices,
                     d->count * 2u, 4u, &ioffset))
        return;
    s_t_upload += sceKernelGetProcessTimeWide() - t0;
    ++s_t_batches;
    submit_draw(d, voffset, ioffset, d->count, d->indices != NULL);
}

void (*const g_gxr_exec_draw)(const void*) = exec_draw;

/* ------------------------------------------------------------ copies */

static const char s_depth_copy_vs[] =
    "#version 460 core\n"
    "uniform vec4 uRect;\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "  vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "  vUv = mix(uRect.xy, uRect.zw, c);\n"
    "  gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char s_depth_copy_fs[] =
    "#version 460 core\n"
    "in vec2 vUv;\n"
    "uniform sampler2D uDepth;\n"
    "layout(location=0) out vec4 oColor;\n"
    "void main() {\n"
    "  float depth = clamp(texture(uDepth, vUv).r, 0.0, 1.0);\n"
    "  float z24 = floor(depth * 16777215.0 + 0.5);\n"
    "  float high = floor(z24 / 65536.0);\n"
    "  float middle = floor(z24 / 256.0) - high * 256.0;\n"
    "  float low = z24 - floor(z24 / 256.0) * 256.0;\n"
    "  oColor = vec4(high, middle, low, 255.0) / 255.0;\n"
    "}\n";

bool melee_ps5_gl_copy_depth(unsigned depth_texture, int x0, int y0, int x1, int y1,
                             unsigned source_width, unsigned source_height,
                             vita2d_texture* target)
{
    if (s_depth_copy_prog == 0 || target == NULL) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, melee_ps5_texture_fbo(target));
    glViewport(0, 0, (GLsizei) melee_ps5_texture_width(target),
               (GLsizei) melee_ps5_texture_height(target));
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(s_depth_copy_prog);
    glUniform4f(s_depth_copy_rect, (f32) x0 / (f32) source_width, (f32) y0 / (f32) source_height,
                (f32) x1 / (f32) source_width, (f32) y1 / (f32) source_height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth_texture);
    glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    ++g_melee_vita_gxm_state_epoch;
    return true;
}

/* GXM-texture copy entry points are unused on PS5 (gl_game.c copies). */
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

/* --------------------------------------------------------------- init */

int gxr_init(void)
{
    if (s_ready || s_failed) return s_ready ? 0 : -1;
    s_failed = true;
#ifdef MELEE_PS5_LEGACY_RENDERER
    return -1;
#endif
    s_cpu_vs = compile_glsl(GL_VERTEX_SHADER, s_cpu_vs_source, "cpu vertex");
    if (s_cpu_vs == 0) return -1;
    setup_layouts();
    glGenVertexArrays(1, &s_empty_vao);
    if (!ring_create()) return -1;
    s_white = melee_ps5_texture_create_white();
    s_depth_copy_prog = melee_ps5_gl_program(s_depth_copy_vs, s_depth_copy_fs);
    if (s_depth_copy_prog != 0) {
        s_depth_copy_rect = glGetUniformLocation(s_depth_copy_prog, "uRect");
        glUseProgram(s_depth_copy_prog);
        glUniform1i(glGetUniformLocation(s_depth_copy_prog, "uDepth"), 0);
    }
    s_ready = true;
    s_failed = false;
    melee_ps5_log("[GXR] GLSL shader renderer ready");
    return 0;
}

void gxr_log_stats(void)
{
    melee_vita_log_info("[GXR] draws=%u fallback=%u compiled=%u failed=%u links=%u compile_ms=%llu",
                        s_stats.draws, s_stats.fallback, s_stats.compiled, s_stats.compile_failed,
                        s_stats.links, (unsigned long long) (s_stats.compile_us / 1000u));
    s_stats.draws = 0;
    s_stats.fallback = 0;
}
