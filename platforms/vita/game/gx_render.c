/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gx_render.h"
#include "../vita_log.h"

#include <dolphin/gx/GXEnum.h>

#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <vita2d.h>
#include <vitashark.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GXR_CACHE_VERSION 4u
#define GXR_CACHE_DIR "ux0:data/melee/shadercache"
#define GXR_PROGRAM_BUCKETS 256u
#define GXR_MAX_INDEX 63000u
#define GXR_SOURCE_CAPACITY 32768u

vita2d_texture* melee_vita_gxm_texture(const MeleeVitaTextureSource* source);

typedef struct GxrProgram {
    u64 hash;
    GxrShaderKey key;
    SceGxmShaderPatcherId id;
    const SceGxmProgram* program;
    const SceGxmProgramParameter* registers[4];
    const SceGxmProgramParameter* konst[4];
    bool failed;
    struct GxrFragment* fragments;
    struct GxrProgram* next;
} GxrProgram;

typedef struct GxrFragment {
    SceGxmBlendInfo blend;
    bool has_blend;
    SceGxmFragmentProgram* fragment;
    struct GxrFragment* next;
} GxrFragment;

static bool s_ready;
static bool s_failed;
static SceGxmShaderPatcherId s_vertex_id;
static const SceGxmProgram* s_vertex_gxp;
static SceGxmVertexProgram* s_vertex_program;
static const SceGxmProgramParameter* s_point_size_param;
static GxrProgram* s_programs[GXR_PROGRAM_BUCKETS];
static u16* s_indices;
static SceUID s_index_block = -1;
static vita2d_texture* s_white;
static bool arena_init(void);

static struct {
    u32 draws, compiled, cache_loaded, compile_failed, fallback;
    u64 compile_us;
} s_stats;

/* ---------------------------------------------------------------- utils */

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
    if (s->overflow) return;
    va_start(args, format);
    int written = vsnprintf(s->text + s->length, s->capacity - s->length,
                            format, args);
    va_end(args);
    if (written < 0 || (size_t) written >= s->capacity - s->length) {
        s->overflow = true;
        return;
    }
    s->length += (size_t) written;
}

static void shark_log(const char* message, shark_log_level level, int line)
{
    if (level >= SHARK_LOG_WARNING)
        melee_vita_log_info("[GXR] shacccg %s line %d: %s",
                            level == SHARK_LOG_ERROR ? "error" : "warning",
                            line, message);
}

static void* gpu_alloc(SceKernelMemBlockType type, u32 size, SceUID* uid)
{
    size = (size + 0xfffu) & ~0xfffu;
    *uid = sceKernelAllocMemBlock("gxr", type, size, NULL);
    if (*uid < 0) return NULL;
    void* memory = NULL;
    if (sceKernelGetMemBlockBase(*uid, &memory) < 0 ||
        sceGxmMapMemory(memory, size,
                        SCE_GXM_MEMORY_ATTRIB_READ |
                            SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
        sceKernelFreeMemBlock(*uid);
        *uid = -1;
        return NULL;
    }
    return memory;
}

/* ------------------------------------------------------- source generation */

static const char* channel_name(u8 channel)
{
    static const char* names[] = { "r", "g", "b", "a" };
    return names[channel & 3u];
}

static void swapped(char* out, size_t size, const char* base,
                    const GxrShaderKey* key, u8 table, bool alpha)
{
    const u8* swap = key->swap[table & 3u];
    if (alpha)
        snprintf(out, size, "%s.%s", base, channel_name(swap[3]));
    else
        snprintf(out, size, "%s.%s%s%s", base, channel_name(swap[0]),
                 channel_name(swap[1]), channel_name(swap[2]));
}

static int color_channel_index(u8 channel)
{
    if (channel == GX_COLOR0 || channel == GX_ALPHA0 || channel == GX_COLOR0A0)
        return 0;
    if (channel == GX_COLOR1 || channel == GX_ALPHA1 || channel == GX_COLOR1A1)
        return 1;
    return -1;
}

static const char* konst_fraction(u8 sel)
{
    static const char* values[] = { "1.0", "0.875", "0.75", "0.625",
                                    "0.5", "0.375", "0.25", "0.125" };
    return sel < 8u ? values[sel] : "1.0";
}

static void color_arg(char* out, size_t size, const GxrShaderKey* key,
                      const GxrStage* st, u32 stage_index, u8 arg)
{
    char tmp[64];
    int ras = color_channel_index(st->channel);
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
    case GX_CC_TEXA:
        if (st->tex_map >= GXR_MAX_TEXMAPS) { snprintf(out, size, "float3(1.0,1.0,1.0)"); return; }
        snprintf(tmp, sizeof(tmp), "s%u", stage_index);
        swapped(out, size, tmp, key, st->swap_tex, true);
        strncat(out, "", size);
        { char a[64]; snprintf(a, sizeof(a), "float3(%s,%s,%s)", out, out, out); snprintf(out, size, "%s", a); }
        return;
    case GX_CC_RASC:
        if (ras < 0) { snprintf(out, size, "float3(0.0,0.0,0.0)"); return; }
        snprintf(tmp, sizeof(tmp), "ras%d", ras);
        swapped(out, size, tmp, key, st->swap_ras, false);
        return;
    case GX_CC_RASA:
        if (ras < 0) { snprintf(out, size, "float3(0.0,0.0,0.0)"); return; }
        snprintf(tmp, sizeof(tmp), "ras%d", ras);
        swapped(out, size, tmp, key, st->swap_ras, true);
        { char a[64]; snprintf(a, sizeof(a), "float3(%s,%s,%s)", out, out, out); snprintf(out, size, "%s", a); }
        return;
    case GX_CC_ONE: snprintf(out, size, "float3(1.0,1.0,1.0)"); return;
    case GX_CC_HALF: snprintf(out, size, "float3(0.5,0.5,0.5)"); return;
    case GX_CC_KONST: {
        const u8 sel = st->kcsel;
        if (sel < 8u) { snprintf(out, size, "float3(%s,%s,%s)", konst_fraction(sel), konst_fraction(sel), konst_fraction(sel)); return; }
        if (sel >= 0x0cu && sel <= 0x0fu) { snprintf(out, size, "k%u.rgb", sel - 0x0cu); return; }
        if (sel >= 0x10u && sel <= 0x1fu) {
            snprintf(out, size, "k%u.%s%s%s", (sel - 0x10u) & 3u,
                     channel_name((sel - 0x10u) >> 2), channel_name((sel - 0x10u) >> 2),
                     channel_name((sel - 0x10u) >> 2));
            return;
        }
        snprintf(out, size, "float3(1.0,1.0,1.0)");
        return;
    }
    default: snprintf(out, size, "float3(0.0,0.0,0.0)"); return;
    }
}

static void alpha_arg(char* out, size_t size, const GxrShaderKey* key,
                      const GxrStage* st, u32 stage_index, u8 arg)
{
    char tmp[64];
    int ras = color_channel_index(st->channel);
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
            snprintf(out, size, "k%u.%s", (sel - 0x10u) & 3u,
                     channel_name((sel - 0x10u) >> 2));
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

static void op_expr(Source* s, u8 op, u8 bias, u8 scale, bool is_color,
                    const char* a, const char* b, const char* c, const char* d)
{
    const char* zero = is_color ? "float3(0.0,0.0,0.0)" : "0.0";
    switch (op) {
    case GX_TEV_ADD:
    case GX_TEV_SUB:
        emit(s, "((%slerp(%s, %s, %s) + %s)%s)%s", op == GX_TEV_SUB ? "-" : "",
             a, b, c, d, bias_text(bias), scale_text(scale));
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

static void alpha_compare_expr(Source* s, u8 comp, u8 ref)
{
    const char* ops[] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
    if (comp == GX_NEVER) { emit(s, "false"); return; }
    if (comp == GX_ALWAYS) { emit(s, "true"); return; }
    emit(s, "(ac %s %u.0)", ops[comp & 7u], ref);
}

static bool build_fragment_source(const GxrShaderKey* key, Source* s)
{
    bool uses_coord[GXR_MAX_TEXCOORDS] = { false };
    bool uses_map[GXR_MAX_TEXMAPS] = { false };
    for (u32 i = 0; i < key->stage_count; ++i) {
        const GxrStage* st = &key->stages[i];
        if (st->tex_map < GXR_MAX_TEXMAPS) {
            uses_map[st->tex_map] = true;
            if (st->tex_coord < GXR_MAX_TEXCOORDS) uses_coord[st->tex_coord] = true;
        }
    }

    emit(s, "half4 main(\n");
    emit(s, "    float4 vColor0 : COLOR0,\n    float4 vColor1 : COLOR1");
    for (u32 i = 0; i < GXR_MAX_TEXCOORDS; ++i)
        if (uses_coord[i]) emit(s, ",\n    float2 vTex%u : TEXCOORD%u", i, i);
    for (u32 i = 0; i < GXR_MAX_TEXMAPS; ++i)
        if (uses_map[i]) emit(s, ",\n    uniform sampler2D uMap%u : TEXUNIT%u", i, i);
    emit(s, ",\n    uniform float4 uPrev, uniform float4 uReg0, uniform float4 uReg1, uniform float4 uReg2");
    emit(s, ",\n    uniform float4 uK0, uniform float4 uK1, uniform float4 uK2, uniform float4 uK3) : COLOR\n{\n");
    emit(s, "    half4 prev = uPrev;\n    half4 r0 = uReg0;\n    half4 r1 = uReg1;\n    half4 r2 = uReg2;\n");
    emit(s, "    half4 k0 = uK0;\n    half4 k1 = uK1;\n    half4 k2 = uK2;\n    half4 k3 = uK3;\n");
    emit(s, "    half4 ras0 = vColor0;\n    half4 ras1 = vColor1;\n");

    static const char* reg_names[] = { "prev", "r0", "r1", "r2" };
    for (u32 i = 0; i < key->stage_count; ++i) {
        const GxrStage* st = &key->stages[i];
        char a[96], b[96], c[96], d[96];
        if (st->tex_map < GXR_MAX_TEXMAPS) {
            if (st->tex_coord < GXR_MAX_TEXCOORDS && st->mirror != 0u) {
                emit(s, "    float2 uv%u = vTex%u;\n", i, st->tex_coord);
                if (st->mirror & 1u)
                    emit(s, "    uv%u.x = 1.0 - abs(frac(uv%u.x * 0.5) * 2.0 - 1.0);\n", i, i);
                if (st->mirror & 2u)
                    emit(s, "    uv%u.y = 1.0 - abs(frac(uv%u.y * 0.5) * 2.0 - 1.0);\n", i, i);
                emit(s, "    half4 s%u = tex2D(uMap%u, uv%u);\n", i, st->tex_map, i);
            } else if (st->tex_coord < GXR_MAX_TEXCOORDS)
                emit(s, "    half4 s%u = tex2D(uMap%u, vTex%u);\n", i, st->tex_map, st->tex_coord);
            else
                emit(s, "    half4 s%u = tex2D(uMap%u, float2(0.0,0.0));\n", i, st->tex_map);
        }
        color_arg(a, sizeof(a), key, st, i, st->color_in[0]);
        color_arg(b, sizeof(b), key, st, i, st->color_in[1]);
        color_arg(c, sizeof(c), key, st, i, st->color_in[2]);
        color_arg(d, sizeof(d), key, st, i, st->color_in[3]);
        emit(s, "    half3 c%u = clamp(", i);
        op_expr(s, st->color_op, st->color_bias, st->color_scale, true, a, b, c, d);
        emit(s, st->color_clamp ? ", 0.0, 1.0);\n" : ", -4.0, 4.0);\n");

        alpha_arg(a, sizeof(a), key, st, i, st->alpha_in[0]);
        alpha_arg(b, sizeof(b), key, st, i, st->alpha_in[1]);
        alpha_arg(c, sizeof(c), key, st, i, st->alpha_in[2]);
        alpha_arg(d, sizeof(d), key, st, i, st->alpha_in[3]);
        emit(s, "    half a%u = clamp(", i);
        op_expr(s, st->alpha_op, st->alpha_bias, st->alpha_scale, false, a, b, c, d);
        emit(s, st->alpha_clamp ? ", 0.0, 1.0);\n" : ", -4.0, 4.0);\n");
        emit(s, "    %s.rgb = c%u;\n    %s.a = a%u;\n",
             reg_names[st->color_out & 3u], i, reg_names[st->alpha_out & 3u], i);
    }
    if (key->stage_count > 0) {
        const GxrStage* last = &key->stages[key->stage_count - 1];
        if ((last->color_out & 3u) != 0u)
            emit(s, "    prev.rgb = %s.rgb;\n", reg_names[last->color_out & 3u]);
        if ((last->alpha_out & 3u) != 0u)
            emit(s, "    prev.a = %s.a;\n", reg_names[last->alpha_out & 3u]);
    }

    const bool trivially_pass =
        (key->alpha_op == GX_AOP_AND && key->alpha_comp[0] == GX_ALWAYS &&
         key->alpha_comp[1] == GX_ALWAYS) ||
        (key->alpha_op == GX_AOP_OR &&
         (key->alpha_comp[0] == GX_ALWAYS || key->alpha_comp[1] == GX_ALWAYS));
    if (!trivially_pass) {
        static const char* ops[] = { "&&", "||", "!=", "==" };
        emit(s, "    float ac = floor(clamp(prev.a, 0.0, 1.0) * 255.0 + 0.5);\n");
        emit(s, "    if (!(");
        alpha_compare_expr(s, key->alpha_comp[0], key->alpha_ref[0]);
        emit(s, " %s ", ops[key->alpha_op & 3u]);
        alpha_compare_expr(s, key->alpha_comp[1], key->alpha_ref[1]);
        emit(s, ")) discard;\n");
    }
    emit(s, "    return clamp(prev, 0.0, 1.0);\n}\n");
    return !s->overflow;
}

/* Colour channels use COLOR0/COLOR1 (they are already clamped to 0..1) and
 * texture coordinates TEXCOORD0-7; Sony's compiler rejected TEXCOORD8/9 plus
 * a PSIZE output with a fatal internal error. */
static const char k_vertex_source[] =
    "void main(float4 aPosition, float4 aColor0, float4 aColor1,\n"
    "    float2 aTex0, float2 aTex1, float2 aTex2, float2 aTex3,\n"
    "    float2 aTex4, float2 aTex5, float2 aTex6, float2 aTex7,\n"
    "    out float4 vPosition : POSITION,\n"
    "    out float4 vColor0 : COLOR0, out float4 vColor1 : COLOR1,\n"
    "    out float2 vTex0 : TEXCOORD0, out float2 vTex1 : TEXCOORD1,\n"
    "    out float2 vTex2 : TEXCOORD2, out float2 vTex3 : TEXCOORD3,\n"
    "    out float2 vTex4 : TEXCOORD4, out float2 vTex5 : TEXCOORD5,\n"
    "    out float2 vTex6 : TEXCOORD6, out float2 vTex7 : TEXCOORD7)\n"
    "{\n"
    "    vPosition = aPosition;\n"
    "    vColor0 = aColor0; vColor1 = aColor1;\n"
    "    vTex0 = aTex0; vTex1 = aTex1; vTex2 = aTex2; vTex3 = aTex3;\n"
    "    vTex4 = aTex4; vTex5 = aTex5; vTex6 = aTex6; vTex7 = aTex7;\n"
    "}\n";

/* ------------------------------------------------------------ compilation */

static SceGxmProgram* load_cached(u64 hash, bool vertex)
{
    char path[128];
    snprintf(path, sizeof(path), GXR_CACHE_DIR "/%s%016llx.gxp",
             vertex ? "v" : "f", (unsigned long long) hash);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    SceGxmProgram* program = NULL;
    if (size > 0 && size < 1024 * 1024) {
        program = malloc((size_t) size);
        if (program != NULL &&
            (sceIoRead(fd, program, (SceSize) size) != size ||
             sceGxmProgramCheck(program) < 0)) {
            free(program);
            program = NULL;
        }
    }
    sceIoClose(fd);
    return program;
}

static void store_cached(u64 hash, bool vertex, const SceGxmProgram* program,
                         u32 size)
{
    char path[128];
    snprintf(path, sizeof(path), GXR_CACHE_DIR "/%s%016llx.gxp",
             vertex ? "v" : "f", (unsigned long long) hash);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, program, size);
    sceIoClose(fd);
}

/* Compile (or load) a program.  The returned memory must stay alive while it
 * is registered with the shader patcher, so it is intentionally not freed. */
static const SceGxmProgram* obtain_program(const char* source, bool vertex,
                                           u64 hash)
{
    SceGxmProgram* cached = load_cached(hash, vertex);
    if (cached != NULL) {
        ++s_stats.cache_loaded;
        return cached;
    }
    const u64 started = sceKernelGetProcessTimeWide();
    /* vitaShaRK reads the source length from *size and writes the program
     * size back through the same pointer. */
    u32 size = (u32) strlen(source);
    SceGxmProgram* compiled = shark_compile_shader_extended(
        source, &size, vertex ? SHARK_VERTEX_SHADER : SHARK_FRAGMENT_SHADER,
        SHARK_OPT_DEFAULT, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
    s_stats.compile_us += sceKernelGetProcessTimeWide() - started;
    if (compiled == NULL || size == 0) {
        const SceShaccCgCompileOutput* output = shark_get_internal_compile_output();
        if (output != NULL) {
            for (int i = 0; i < output->diagnosticCount; ++i) {
                const SceShaccCgDiagnosticMessage* d = &output->diagnostics[i];
                melee_vita_log_info("[GXR] diag level=%d code=%d line=%d: %s",
                                    (int) d->level, (int) d->code,
                                    d->location != NULL ? (int) d->location->lineNumber : -1,
                                    d->message != NULL ? d->message : "(null)");
            }
        }
        {
            void* probe = malloc(16u * 1024u * 1024u);
            melee_vita_log_info("[GXR] compile failed; srclen=%u heap16M=%s",
                                (unsigned) strlen(source), probe ? "ok" : "fail");
            free(probe);
        }
        shark_clear_output();
        ++s_stats.compile_failed;
        return NULL;
    }
    SceGxmProgram* copy = malloc(size);
    if (copy != NULL) memcpy(copy, compiled, size);
    shark_clear_output();
    if (copy == NULL) return NULL;
    ++s_stats.compiled;
    store_cached(hash, vertex, copy, size);
    return copy;
}

int gxr_init(void)
{
    if (s_ready || s_failed) return s_ready ? 0 : -1;
    s_failed = true;
    if (shark_init(NULL) < 0) {
        melee_vita_log_info("[GXR] shark_init failed (libshacccg.suprx missing?)");
        return -1;
    }
    shark_install_log_cb(shark_log);
    shark_set_warnings_level(SHARK_WARN_SILENT);
    sceIoMkdir("ux0:data/melee", 0777);
    sceIoMkdir(GXR_CACHE_DIR, 0777);

    SceGxmShaderPatcher* patcher = vita2d_get_shader_patcher();
    const u64 vertex_hash =
        fnv1a(k_vertex_source, sizeof(k_vertex_source),
              UINT64_C(1469598103934665603) ^ GXR_CACHE_VERSION);
    s_vertex_gxp = obtain_program(k_vertex_source, true, vertex_hash);
    if (s_vertex_gxp == NULL ||
        sceGxmShaderPatcherRegisterProgram(patcher, s_vertex_gxp, &s_vertex_id) < 0) {
        melee_vita_log_info("[GXR] vertex program unavailable");
        return -1;
    }

    static const char* names[] = {
        "aPosition", "aColor0", "aColor1", "aTex0", "aTex1", "aTex2", "aTex3",
        "aTex4", "aTex5", "aTex6", "aTex7",
    };
    static const u8 offsets[] = { 0, 4, 8, 12, 14, 16, 18, 20, 22, 24, 26 };
    static const u8 counts[] = { 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2 };
    SceGxmVertexAttribute attributes[11];
    u32 attribute_count = 0;
    for (u32 i = 0; i < 11u; ++i) {
        const SceGxmProgramParameter* param =
            sceGxmProgramFindParameterByName(s_vertex_gxp, names[i]);
        if (param == NULL) continue;
        attributes[attribute_count].streamIndex = 0;
        attributes[attribute_count].offset = (u16) (offsets[i] * sizeof(f32));
        attributes[attribute_count].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
        attributes[attribute_count].componentCount = counts[i];
        attributes[attribute_count].regIndex =
            (u16) sceGxmProgramParameterGetResourceIndex(param);
        ++attribute_count;
    }
    SceGxmVertexStream stream = { sizeof(GxrVertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, s_vertex_id, attributes,
                                               attribute_count, &stream, 1,
                                               &s_vertex_program) < 0) {
        melee_vita_log_info("[GXR] vertex program patch failed");
        return -1;
    }
    s_point_size_param = sceGxmProgramFindParameterByName(s_vertex_gxp, "uPointSize");

    s_indices = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                          GXR_MAX_INDEX * sizeof(u16), &s_index_block);
    if (s_indices == NULL) return -1;
    for (u32 i = 0; i < GXR_MAX_INDEX; ++i) s_indices[i] = (u16) i;

    if (!arena_init()) melee_vita_log_info("[GXR] geometry arena unavailable");

    s_white = vita2d_create_empty_texture(1, 1);
    if (s_white != NULL)
        *(u32*) vita2d_texture_get_datap(s_white) = 0xffffffffu;

    s_ready = true;
    s_failed = false;
    melee_vita_log_info("[GXR] shader renderer ready (vertex attrs=%u)",
                        attribute_count);
    return 0;
}

bool gxr_available(void) { return s_ready; }

static GxrProgram* find_program(const GxrShaderKey* key)
{
    const u64 hash = fnv1a(key, sizeof(*key),
                           UINT64_C(1469598103934665603) ^ GXR_CACHE_VERSION);
    GxrProgram** bucket = &s_programs[hash % GXR_PROGRAM_BUCKETS];
    for (GxrProgram* p = *bucket; p != NULL; p = p->next)
        if (p->hash == hash && memcmp(&p->key, key, sizeof(*key)) == 0)
            return p;

    GxrProgram* p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->hash = hash;
    p->key = *key;
    p->next = *bucket;
    *bucket = p;

    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    buffer[0] = '\0';
    if (!build_fragment_source(key, &source)) {
        p->failed = true;
        melee_vita_log_info("[GXR] fragment source overflow");
        return p;
    }
    p->program = obtain_program(buffer, false, hash);
    if (p->program == NULL ||
        sceGxmShaderPatcherRegisterProgram(vita2d_get_shader_patcher(),
                                           p->program, &p->id) < 0) {
        p->failed = true;
        melee_vita_log_info("[GXR] fragment compile failed hash=%016llx stages=%u",
                            (unsigned long long) hash, key->stage_count);
        /* Log the source once, in chunks that fit a DebugNet datagram. */
        for (size_t off = 0; off < source.length; off += 600) {
            char chunk[601];
            size_t n = source.length - off < 600 ? source.length - off : 600;
            memcpy(chunk, buffer + off, n);
            chunk[n] = '\0';
            melee_vita_log_info("[GXR] src: %s", chunk);
        }
        return p;
    }
    static const char* reg_names[] = { "uPrev", "uReg0", "uReg1", "uReg2" };
    static const char* k_names[] = { "uK0", "uK1", "uK2", "uK3" };
    for (u32 i = 0; i < 4u; ++i) {
        p->registers[i] = sceGxmProgramFindParameterByName(p->program, reg_names[i]);
        p->konst[i] = sceGxmProgramFindParameterByName(p->program, k_names[i]);
    }
    return p;
}

static SceGxmBlendFactor factor(u8 gx, bool source)
{
    switch (gx) {
    case GX_BL_ZERO: return SCE_GXM_BLEND_FACTOR_ZERO;
    case GX_BL_ONE: return SCE_GXM_BLEND_FACTOR_ONE;
    case GX_BL_SRCCLR: return source ? SCE_GXM_BLEND_FACTOR_DST_COLOR : SCE_GXM_BLEND_FACTOR_SRC_COLOR;
    case GX_BL_INVSRCCLR: return source ? SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR : SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GX_BL_SRCALPHA: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case GX_BL_INVSRCALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GX_BL_DSTALPHA: return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case GX_BL_INVDSTALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default: return SCE_GXM_BLEND_FACTOR_ONE;
    }
}

static bool make_blend(const GxrDraw* draw, SceGxmBlendInfo* blend)
{
    u8 mask = 0;
    if (draw->color_update) mask |= SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B;
    if (draw->alpha_update) mask |= SCE_GXM_COLOR_MASK_A;
    memset(blend, 0, sizeof(*blend));
    blend->colorMask = mask;
    blend->colorFunc = SCE_GXM_BLEND_FUNC_NONE;
    blend->alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
    blend->colorSrc = blend->alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    blend->colorDst = blend->alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    switch (draw->blend_mode) {
    case GX_BM_BLEND:
        blend->colorFunc = blend->alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
        blend->colorSrc = blend->alphaSrc = factor(draw->blend_src, true);
        blend->colorDst = blend->alphaDst = factor(draw->blend_dst, false);
        break;
    case GX_BM_SUBTRACT:
        blend->colorFunc = blend->alphaFunc = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
        blend->colorSrc = blend->alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
        blend->colorDst = blend->alphaDst = SCE_GXM_BLEND_FACTOR_ONE;
        break;
    case GX_BM_LOGIC:
        if (draw->logic_op == GX_LO_CLEAR) {
            blend->colorFunc = blend->alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
            blend->colorSrc = blend->alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
            blend->colorDst = blend->alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
        } else if (draw->logic_op == GX_LO_NOOP) {
            blend->colorFunc = blend->alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
            blend->colorSrc = blend->alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
            blend->colorDst = blend->alphaDst = SCE_GXM_BLEND_FACTOR_ONE;
        }
        break;
    default:
        break;
    }
    return !(blend->colorFunc == SCE_GXM_BLEND_FUNC_NONE &&
             mask == SCE_GXM_COLOR_MASK_ALL);
}

static SceGxmFragmentProgram* find_fragment(GxrProgram* program,
                                            const GxrDraw* draw)
{
    SceGxmBlendInfo blend;
    const bool has_blend = make_blend(draw, &blend);
    for (GxrFragment* f = program->fragments; f != NULL; f = f->next)
        if (f->has_blend == has_blend &&
            (!has_blend || memcmp(&f->blend, &blend, sizeof(blend)) == 0))
            return f->fragment;
    GxrFragment* f = calloc(1, sizeof(*f));
    if (f == NULL) return NULL;
    f->blend = blend;
    f->has_blend = has_blend;
    if (sceGxmShaderPatcherCreateFragmentProgram(
            vita2d_get_shader_patcher(), program->id,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
            has_blend ? &blend : NULL, s_vertex_gxp, &f->fragment) < 0) {
        free(f);
        return NULL;
    }
    f->next = program->fragments;
    program->fragments = f;
    return f->fragment;
}

static SceGxmDepthFunc depth_func(u8 function)
{
    static const SceGxmDepthFunc functions[8] = {
        SCE_GXM_DEPTH_FUNC_NEVER, SCE_GXM_DEPTH_FUNC_LESS,
        SCE_GXM_DEPTH_FUNC_EQUAL, SCE_GXM_DEPTH_FUNC_LESS_EQUAL,
        SCE_GXM_DEPTH_FUNC_GREATER, SCE_GXM_DEPTH_FUNC_NOT_EQUAL,
        SCE_GXM_DEPTH_FUNC_GREATER_EQUAL, SCE_GXM_DEPTH_FUNC_ALWAYS,
    };
    return functions[function & 7u];
}

GxrVertex* gxr_alloc_vertices(u32 count)
{
    if (!s_ready || count == 0) return NULL;
    return melee_vita_rq_alloc_gpu(count * sizeof(GxrVertex), sizeof(void*));
}

u16* gxr_alloc_indices(u32 count)
{
    if (!s_ready || count == 0) return NULL;
    return melee_vita_rq_alloc_gpu(count * sizeof(u16), sizeof(u16));
}

/* ------------------------------------------------------------------------
 * Draws are prepared on the game thread (program lookup/compilation and
 * texture resolution) and recorded as a RqDraw; exec_draw issues the GXM
 * calls on the render thread, which also owns the redundant-state cache.
 * ------------------------------------------------------------------------ */
extern u32 g_melee_vita_gxm_state_epoch;

typedef struct RqDraw {
    SceGxmVertexProgram* vertex;
    SceGxmFragmentProgram* fragment;
    const struct GxrProgram* program;
    const struct GxrVtxProgram* vp;
    vita2d_texture* textures[GXR_MAX_TEXMAPS];
    const void* vertices;
    const u16* indices;
    u32 count;
    SceGxmDepthFunc depth_function;
    SceGxmDepthWriteMode depth_write;
    SceGxmCullMode cull;
    SceGxmPrimitiveType primitive;
    u32 line_width;
    f32 point_size;
    u8 texture_mask;
    u8 cpu_path;
    u16 mtx_comps, tg_comps, light_comps, reserved;
    f32 registers[4][4];
    f32 konst[4][4];
    f32 uniforms[]; /* pos, nrm, proj(16), tex, post, light, mat(8), amb(8) */
} RqDraw;

static struct {
    u32 epoch;
    bool valid;
    SceGxmFragmentProgram* fragment;
    SceGxmVertexProgram* vertex;
    SceGxmDepthFunc depth_function;
    SceGxmDepthWriteMode depth_write;
    u32 line_width;
    u8 cull;
} s_rt_state;

static void exec_draw(const void* payload);

static SceGxmPrimitiveType gxm_primitive(u8 primitive)
{
    return primitive == GXR_PRIM_LINES ? SCE_GXM_PRIMITIVE_LINES
         : primitive == GXR_PRIM_POINTS ? SCE_GXM_PRIMITIVE_POINTS
         : SCE_GXM_PRIMITIVE_TRIANGLES;
}

/* Game-thread cache of the last fragment program lookup. */
static GxrShaderKey s_last_key;
static GxrProgram* s_last_program;

static GxrProgram* lookup_program(const GxrShaderKey* key)
{
    GxrProgram* program;
    if (s_last_program != NULL && memcmp(&s_last_key, key, sizeof(*key)) == 0)
        return s_last_program;
    program = find_program(key);
    if (program != NULL && !program->failed) {
        s_last_key = *key;
        s_last_program = program;
    }
    return program;
}

static void resolve_textures(const GxrDraw* draw, RqDraw* d)
{
    for (u32 map = 0; map < GXR_MAX_TEXMAPS; ++map) {
        bool used = false;
        for (u32 i = 0; i < draw->key.stage_count; ++i)
            if (draw->key.stages[i].tex_map == map) { used = true; break; }
        if (!used) continue;
        vita2d_texture* texture = draw->texture_valid[map]
            ? melee_vita_gxm_texture(&draw->textures[map]) : NULL;
        d->textures[map] = texture != NULL ? texture : s_white;
        d->texture_mask |= (u8) (1u << map);
    }
}

static SceGxmDepthFunc draw_depth_function(const GxrDraw* draw)
{
    return draw->depth_compare ? depth_func(draw->depth_function) : SCE_GXM_DEPTH_FUNC_ALWAYS;
}

bool gxr_draw(const GxrDraw* draw, GxrVertex* vertices, const u16* indices,
              u32 count)
{
    GxrProgram* program;
    SceGxmFragmentProgram* fragment;
    RqDraw* d;
    if (!s_ready || draw == NULL || vertices == NULL || count == 0) return false;
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) { ++s_stats.fallback; return false; }
    fragment = find_fragment(program, draw);
    if (fragment == NULL) { ++s_stats.fallback; return false; }
    d = melee_vita_rq_push(exec_draw, sizeof(RqDraw));
    if (d == NULL) return false;
    memset(d, 0, sizeof(*d));
    d->vertex = s_vertex_program;
    d->fragment = fragment;
    d->program = program;
    d->vertices = vertices;
    d->indices = indices;
    d->count = count;
    d->cpu_path = 1;
    d->depth_function = draw_depth_function(draw);
    d->depth_write = draw->depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    d->cull = SCE_GXM_CULL_NONE;
    d->primitive = gxm_primitive(draw->primitive);
    d->line_width = draw->line_width < 1.0f ? 1u : (u32) (draw->line_width + 0.5f);
    d->point_size = draw->line_width < 1.0f ? 1.0f : draw->line_width;
    memcpy(d->registers, draw->registers, sizeof(d->registers));
    memcpy(d->konst, draw->konst, sizeof(d->konst));
    resolve_textures(draw, d);
    ++s_stats.draws;
    return true;
}

/* ===================================================== GPU vertex pipeline */

#define GXR_VTX_VERSION 2u
#define GXR_ARENA_SIZE (48u * 1024u * 1024u)

typedef struct GxrVtxProgram {
    u64 hash;
    GxrVtxKey key;
    bool failed;
    SceGxmShaderPatcherId id;
    const SceGxmProgram* program;
    SceGxmVertexProgram* vertex;
    const SceGxmProgramParameter* u_pos, *u_nrm, *u_proj, *u_tex, *u_post,
        *u_light, *u_mat, *u_amb;
    u32 logged;
    struct GxrVtxProgram* next;
} GxrVtxProgram;

static GxrVtxProgram* s_vtx_programs[GXR_PROGRAM_BUCKETS];

static const char* tex_attr_name(u8 source)
{
    static const char* names[] = { "aT0", "aT1", "aT2", "aT3" };
    const u32 k = (u32) source - (u32) GX_TG_TEX0;
    return names[k < GXR_GPU_TEX ? k : 0u];
}

static void emit_light_channel(Source* s, const GxrVtxKey* key, u32 index,
                               bool alpha, const char* vc)
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
        if ((cc->lights & (1u << l)) == 0u) continue;
        const u32 b = l * 5u;
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
        if (cc->diffuse == GX_DF_SIGN)
            emit(s, "            float diff = dot(ldir, nrm);\n");
        else if (cc->diffuse == GX_DF_CLAMP)
            emit(s, "            float diff = max(0.0, dot(ldir, nrm));\n");
        else
            emit(s, "            float diff = 1.0;\n");
        emit(s, "            lit = lit + attn * diff * uLight[%u];\n", b);
        emit(s, "        }\n");
    }
    emit(s, "        vColor%u%s = (%s * clamp(lit, 0.0, 1.0))%s;\n    }\n", index, sw, mat, sw);
}

static bool build_vertex_source(const GxrVtxKey* key, Source* s)
{
    emit(s, "void main(float3 aPos, float aMtx, float3 aNrm, float4 aC0, float4 aC1,\n");
    emit(s, "    float2 aT0, float2 aT1, float2 aT2, float2 aT3,\n");
    emit(s, "    uniform float4 uPos[30], uniform float4 uNrm[30], uniform float4 uProj[4],\n");
    emit(s, "    uniform float4 uTex[24], uniform float4 uPost[24], uniform float4 uLight[40],\n");
    emit(s, "    uniform float4 uMat[2], uniform float4 uAmb[2],\n");
    emit(s, "    out float4 vPosition : POSITION,\n");
    emit(s, "    out float4 vColor0 : COLOR0, out float4 vColor1 : COLOR1,\n");
    emit(s, "    out float2 vTex0 : TEXCOORD0, out float2 vTex1 : TEXCOORD1,\n");
    emit(s, "    out float2 vTex2 : TEXCOORD2, out float2 vTex3 : TEXCOORD3,\n");
    emit(s, "    out float2 vTex4 : TEXCOORD4, out float2 vTex5 : TEXCOORD5,\n");
    emit(s, "    out float2 vTex6 : TEXCOORD6, out float2 vTex7 : TEXCOORD7)\n{\n");
    if (key->has_mtxidx)
        emit(s, "    int m = int(floor(aMtx / 3.0 + 0.01)) * 3;\n");
    else
        emit(s, "    int m = 0;\n");
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
    emit(s, "    vPosition = float4(uProj[1].z * wc + uProj[1].w * xc, uProj[2].x * wc + uProj[2].y * yc,\n");
    emit(s, "                       uProj[2].z * wc + zc * uProj[2].w, wc);\n");
    emit(s, "    vColor0 = float4(0.0, 0.0, 0.0, 0.0);\n    vColor1 = float4(0.0, 0.0, 0.0, 0.0);\n");
    for (u32 i = 0; i < key->channel_count && i < 2u; ++i) {
        const char* vc = i == 0 ? "aC0" : "aC1";
        emit_light_channel(s, key, i, false, vc);
        emit_light_channel(s, key, i, true, vc);
    }
    for (u32 i = 0; i < GXR_MAX_TEXCOORDS; ++i) {
        if (i >= key->texgen_count) { emit(s, "    vTex%u = float2(0.0, 0.0);\n", i); continue; }
        const GxrVtxTexGen* tg = &key->tg[i];
        const u32 r = i * 3u;
        emit(s, "    {\n        float3 t = float3(0.0, 0.0, 1.0);\n");
        if (tg->source >= GX_TG_TEX0 && tg->source <= GX_TG_TEX7)
            emit(s, "        t.xy = %s;\n", tex_attr_name(tg->source));
        else if (tg->source == GX_TG_POS) emit(s, "        t = aPos;\n");
        else if (tg->source == GX_TG_NRM) emit(s, "        t = aNrm;\n");
        else if (tg->source == GX_TG_COLOR0) emit(s, "        t.xy = vColor0.xy;\n");
        else if (tg->source == GX_TG_COLOR1) emit(s, "        t.xy = vColor1.xy;\n");
        if (tg->has_matrix) {
            const bool pn = tg->source == GX_TG_POS || tg->source == GX_TG_NRM;
            const char* w = tg->source == GX_TG_NRM ? "0.0" : "1.0";
            emit(s, "        float3 src = float3(t.xy, %s);\n", pn ? "t.z" : "1.0");
            if (tg->type == GX_TG_MTX2x4)
                emit(s, "        t = float3(dot(uTex[%u].xyz, src) + uTex[%u].w * %s, dot(uTex[%u].xyz, src) + uTex[%u].w * %s, 1.0);\n",
                     r, r, w, r + 1u, r + 1u, w);
            else
                emit(s, "        t = float3(dot(uTex[%u].xyz, src) + uTex[%u].w * %s, dot(uTex[%u].xyz, src) + uTex[%u].w * %s, dot(uTex[%u].xyz, src) + uTex[%u].w * %s);\n",
                     r, r, w, r + 1u, r + 1u, w, r + 2u, r + 2u, w);
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
        emit(s, "        vTex%u = t.xy;\n    }\n", i);
    }
    emit(s, "}\n");
    return !s->overflow;
}

static GxrVtxProgram* find_vertex_program(const GxrVtxKey* key)
{
    const u64 hash = fnv1a(key, sizeof(*key),
                           UINT64_C(0x9e3779b97f4a7c15) ^ GXR_VTX_VERSION);
    GxrVtxProgram** bucket = &s_vtx_programs[hash % GXR_PROGRAM_BUCKETS];
    for (GxrVtxProgram* p = *bucket; p != NULL; p = p->next)
        if (p->hash == hash && memcmp(&p->key, key, sizeof(*key)) == 0)
            return p;
    GxrVtxProgram* p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->hash = hash;
    p->key = *key;
    p->next = *bucket;
    *bucket = p;
    p->failed = true;

    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    buffer[0] = '\0';
    if (!build_vertex_source(key, &source)) {
        melee_vita_log_info("[GXR] vertex source overflow");
        return p;
    }
    p->program = obtain_program(buffer, true, hash);
    SceGxmShaderPatcher* patcher = vita2d_get_shader_patcher();
    if (p->program == NULL ||
        sceGxmShaderPatcherRegisterProgram(patcher, p->program, &p->id) < 0) {
        melee_vita_log_info("[GXR] vertex compile failed hash=%016llx", (unsigned long long) hash);
        for (size_t off = 0; off < source.length; off += 600) {
            char chunk[601];
            size_t n = source.length - off < 600 ? source.length - off : 600;
            memcpy(chunk, buffer + off, n);
            chunk[n] = '\0';
            melee_vita_log_info("[GXR] vsrc: %s", chunk);
        }
        return p;
    }
    static const struct { const char* name; u16 offset; u8 format; u8 count; } attrs[] = {
        { "aPos", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3 },
        { "aMtx", 12, SCE_GXM_ATTRIBUTE_FORMAT_F32, 1 },
        { "aNrm", 16, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3 },
        { "aC0", 28, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4 },
        { "aC1", 32, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4 },
        { "aT0", 36, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2 },
        { "aT1", 44, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2 },
        { "aT2", 52, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2 },
        { "aT3", 60, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2 },
    };
    SceGxmVertexAttribute attributes[9];
    u32 count = 0;
    for (u32 i = 0; i < 9u; ++i) {
        const SceGxmProgramParameter* param = sceGxmProgramFindParameterByName(p->program, attrs[i].name);
        if (param == NULL) continue;
        attributes[count].streamIndex = 0;
        attributes[count].offset = attrs[i].offset;
        attributes[count].format = attrs[i].format;
        attributes[count].componentCount = attrs[i].count;
        attributes[count].regIndex = (u16) sceGxmProgramParameterGetResourceIndex(param);
        ++count;
    }
    SceGxmVertexStream stream = { sizeof(GxrGpuVertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, p->id, attributes, count,
                                               &stream, 1, &p->vertex) < 0) {
        melee_vita_log_info("[GXR] vertex program patch failed hash=%016llx", (unsigned long long) hash);
        return p;
    }
    p->u_pos = sceGxmProgramFindParameterByName(p->program, "uPos");
    p->u_nrm = sceGxmProgramFindParameterByName(p->program, "uNrm");
    p->u_proj = sceGxmProgramFindParameterByName(p->program, "uProj");
    p->u_tex = sceGxmProgramFindParameterByName(p->program, "uTex");
    p->u_post = sceGxmProgramFindParameterByName(p->program, "uPost");
    p->u_light = sceGxmProgramFindParameterByName(p->program, "uLight");
    p->u_mat = sceGxmProgramFindParameterByName(p->program, "uMat");
    p->u_amb = sceGxmProgramFindParameterByName(p->program, "uAmb");
    p->failed = false;
    return p;
}

/* ---- persistent GPU arena (first fit, coalescing free list) ---- */

typedef struct ArenaBlock {
    u32 offset, size;
    bool used;
    struct ArenaBlock* next;
} ArenaBlock;

static void* s_arena;
static SceUID s_arena_uid = -1;
static ArenaBlock* s_arena_blocks;

static bool arena_init(void)
{
    s_arena = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, GXR_ARENA_SIZE, &s_arena_uid);
    if (s_arena == NULL) return false;
    s_arena_blocks = calloc(1, sizeof(ArenaBlock));
    if (s_arena_blocks == NULL) return false;
    s_arena_blocks->size = GXR_ARENA_SIZE;
    return true;
}

void* gxr_arena_alloc(u32 size)
{
    size = (size + 15u) & ~15u;
    for (ArenaBlock* b = s_arena_blocks; b != NULL; b = b->next) {
        if (b->used || b->size < size) continue;
        if (b->size > size + 64u) {
            ArenaBlock* rest = malloc(sizeof(*rest));
            if (rest == NULL) return NULL;
            rest->offset = b->offset + size;
            rest->size = b->size - size;
            rest->used = false;
            rest->next = b->next;
            b->next = rest;
            b->size = size;
        }
        b->used = true;
        return (u8*) s_arena + b->offset;
    }
    return NULL;
}

void gxr_arena_free(void* block)
{
    if (block == NULL) return;
    const u32 offset = (u32) ((u8*) block - (u8*) s_arena);
    ArenaBlock* prev = NULL;
    for (ArenaBlock* b = s_arena_blocks; b != NULL; prev = b, b = b->next) {
        if (b->offset != offset) continue;
        b->used = false;
        if (b->next != NULL && !b->next->used) {
            ArenaBlock* n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        }
        if (prev != NULL && !prev->used) {
            prev->size += b->size;
            prev->next = b->next;
            free(b);
        }
        return;
    }
}

static SceGxmCullMode cull_mode_for(u8 cull)
{
    /* Verified on hardware: GX back faces map to GXM's CCW cull with this
     * viewport (the title's PRESS START ring showed mirrored the other way). */
    return cull == GXR_CULL_BACK ? SCE_GXM_CULL_CCW
         : cull == GXR_CULL_FRONT ? SCE_GXM_CULL_CW : SCE_GXM_CULL_NONE;
}

static void set_uniform(void* buffer, const SceGxmProgramParameter* param, u32 count, const f32* data, const char* name)
{
    const int r = sceGxmSetUniformDataF(buffer, param, 0, count, data);
    static u32 logged;
    if (r < 0 && logged++ < 16u)
        melee_vita_log_info("[GXRDBG] set %s count=%u failed %08x (array=%u comps=%u)", name, count, (unsigned) r,
                            (unsigned) sceGxmProgramParameterGetArraySize(param),
                            (unsigned) sceGxmProgramParameterGetComponentCount(param));
}

bool gxr_draw_gpu(const GxrDraw* draw, const GxrVtxKey* vkey,
                  const GxrVtxUniforms* u, const GxrGpuVertex* vertices,
                  const u16* indices, u32 count, u8 cull)
{
    static GxrVtxProgram* last_vp;
    GxrVtxProgram* vp;
    GxrProgram* program;
    SceGxmFragmentProgram* fragment;
    RqDraw* d;
    u32 mtx_comps, tg_comps, light_comps = 0, total;
    f32* out;
    if (!s_ready || draw == NULL || vertices == NULL || indices == NULL || count == 0)
        return false;
    if (cull == GXR_CULL_ALL) return true;
    vp = (last_vp != NULL && memcmp(&last_vp->key, vkey, sizeof(*vkey)) == 0)
        ? last_vp : find_vertex_program(vkey);
    last_vp = vp;
    if (vp == NULL || vp->failed) { ++s_stats.fallback; return false; }
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) { ++s_stats.fallback; return false; }
    fragment = find_fragment(program, draw);
    if (fragment == NULL) { ++s_stats.fallback; return false; }

    mtx_comps = vkey->has_mtxidx ? 120u : 12u;
    tg_comps = vkey->texgen_count * 12u;
    {
        const u32 lights_used = (u32) (vkey->chan[0].lights | vkey->chan[1].lights |
                                       vkey->chan[2].lights | vkey->chan[3].lights);
        u32 top = 8u;
        while (top > 0u && (lights_used & (1u << (top - 1u))) == 0u) --top;
        light_comps = top * 20u;
    }
    total = 2u * mtx_comps + 16u + 2u * tg_comps + light_comps + 16u;
    d = melee_vita_rq_push(exec_draw, sizeof(RqDraw) + total * sizeof(f32));
    if (d == NULL) return false;
    memset(d, 0, sizeof(*d));
    d->vertex = vp->vertex;
    d->fragment = fragment;
    d->program = program;
    d->vp = vp;
    d->vertices = vertices;
    d->indices = indices;
    d->count = count;
    d->depth_function = draw_depth_function(draw);
    d->depth_write = draw->depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    d->cull = cull_mode_for(cull);
    d->primitive = gxm_primitive(draw->primitive);
    d->line_width = draw->line_width < 1.0f ? 1u : (u32) (draw->line_width + 0.5f);
    d->mtx_comps = (u16) mtx_comps;
    d->tg_comps = (u16) tg_comps;
    d->light_comps = (u16) light_comps;
    memcpy(d->registers, draw->registers, sizeof(d->registers));
    memcpy(d->konst, draw->konst, sizeof(d->konst));
    resolve_textures(draw, d);
    out = d->uniforms;
    memcpy(out, u->pos, mtx_comps * sizeof(f32)); out += mtx_comps;
    memcpy(out, u->nrm, mtx_comps * sizeof(f32)); out += mtx_comps;
    memcpy(out, u->proj, 16u * sizeof(f32)); out += 16u;
    memcpy(out, u->tex, tg_comps * sizeof(f32)); out += tg_comps;
    memcpy(out, u->post, tg_comps * sizeof(f32)); out += tg_comps;
    memcpy(out, u->light, light_comps * sizeof(f32)); out += light_comps;
    memcpy(out, u->mat, 8u * sizeof(f32)); out += 8u;
    memcpy(out, u->amb, 8u * sizeof(f32));
    ++s_stats.draws;
    return true;
}

/* Render thread. */
static void exec_draw(const void* payload)
{
    const RqDraw* d = payload;
    const GxrProgram* program = d->program;
    SceGxmContext* context = vita2d_get_context();
    if (s_rt_state.epoch != g_melee_vita_gxm_state_epoch) {
        s_rt_state.valid = false;
        s_rt_state.epoch = g_melee_vita_gxm_state_epoch;
    }
    if (!s_rt_state.valid) {
        sceGxmSetViewport(context, 480.0f, 480.0f, 272.0f, -272.0f, 0.0f, 1.0f);
        s_rt_state.fragment = NULL;
        s_rt_state.vertex = NULL;
        s_rt_state.cull = 0xff;
    }
    if (s_rt_state.vertex != d->vertex) {
        sceGxmSetVertexProgram(context, d->vertex);
        s_rt_state.vertex = d->vertex;
    }
    if (s_rt_state.fragment != d->fragment) {
        sceGxmSetFragmentProgram(context, d->fragment);
        s_rt_state.fragment = d->fragment;
    }
    if (s_rt_state.cull != (u8) d->cull + 1u) {
        sceGxmSetCullMode(context, d->cull);
        s_rt_state.cull = (u8) d->cull + 1u;
    }
    if (!s_rt_state.valid || s_rt_state.depth_function != d->depth_function) {
        sceGxmSetFrontDepthFunc(context, d->depth_function);
        sceGxmSetBackDepthFunc(context, d->depth_function);
        s_rt_state.depth_function = d->depth_function;
    }
    if (!s_rt_state.valid || s_rt_state.depth_write != d->depth_write) {
        sceGxmSetFrontDepthWriteEnable(context, d->depth_write);
        sceGxmSetBackDepthWriteEnable(context, d->depth_write);
        s_rt_state.depth_write = d->depth_write;
    }
    if (!s_rt_state.valid || s_rt_state.line_width != d->line_width) {
        sceGxmSetFrontPointLineWidth(context, d->line_width);
        sceGxmSetBackPointLineWidth(context, d->line_width);
        s_rt_state.line_width = d->line_width;
    }
    s_rt_state.valid = true;

    for (u32 map = 0; map < GXR_MAX_TEXMAPS; ++map)
        if (d->texture_mask & (1u << map))
            sceGxmSetFragmentTexture(context, map, &d->textures[map]->gxm_tex);

    if (d->cpu_path) {
        if (s_point_size_param != NULL) {
            void* vbuf = NULL;
            sceGxmReserveVertexDefaultUniformBuffer(context, &vbuf);
            if (vbuf != NULL) sceGxmSetUniformDataF(vbuf, s_point_size_param, 0, 1, &d->point_size);
        }
    } else {
        const GxrVtxProgram* vp = d->vp;
        const f32* in = d->uniforms;
        void* vbuf = NULL;
        sceGxmReserveVertexDefaultUniformBuffer(context, &vbuf);
        if (vbuf != NULL) {
            if (vp->u_pos) set_uniform(vbuf, vp->u_pos, d->mtx_comps, in, "u_pos");
            in += d->mtx_comps;
            if (vp->u_nrm) set_uniform(vbuf, vp->u_nrm, d->mtx_comps, in, "u_nrm");
            in += d->mtx_comps;
            if (vp->u_proj) set_uniform(vbuf, vp->u_proj, 16, in, "u_proj");
            in += 16u;
            if (vp->u_tex && d->tg_comps) set_uniform(vbuf, vp->u_tex, d->tg_comps, in, "tex");
            in += d->tg_comps;
            if (vp->u_post && d->tg_comps) set_uniform(vbuf, vp->u_post, d->tg_comps, in, "post");
            in += d->tg_comps;
            if (vp->u_light && d->light_comps) set_uniform(vbuf, vp->u_light, d->light_comps, in, "light");
            in += d->light_comps;
            if (vp->u_mat) set_uniform(vbuf, vp->u_mat, 8, in, "u_mat");
            in += 8u;
            if (vp->u_amb) set_uniform(vbuf, vp->u_amb, 8, in, "u_amb");
        }
    }
    {
        bool any = false;
        for (u32 i = 0; i < 4u; ++i)
            if (program->registers[i] != NULL || program->konst[i] != NULL) any = true;
        if (any) {
            void* fbuf = NULL;
            sceGxmReserveFragmentDefaultUniformBuffer(context, &fbuf);
            if (fbuf != NULL) {
                for (u32 i = 0; i < 4u; ++i) {
                    if (program->registers[i] != NULL)
                        sceGxmSetUniformDataF(fbuf, program->registers[i], 0, 4, d->registers[i]);
                    if (program->konst[i] != NULL)
                        sceGxmSetUniformDataF(fbuf, program->konst[i], 0, 4, d->konst[i]);
                }
            }
        }
    }
    sceGxmSetVertexStream(context, 0, d->vertices);
    if (d->indices != NULL) {
        sceGxmDraw(context, d->primitive, SCE_GXM_INDEX_FORMAT_U16, d->indices, d->count);
    } else {
        for (u32 first = 0; first < d->count; first += GXR_MAX_INDEX) {
            const u32 n = d->count - first < GXR_MAX_INDEX ? d->count - first : GXR_MAX_INDEX;
            if (first != 0)
                sceGxmSetVertexStream(context, 0, (const GxrVertex*) d->vertices + first);
            sceGxmDraw(context, d->primitive, SCE_GXM_INDEX_FORMAT_U16, s_indices, n);
        }
    }
}

void gxr_log_stats(void)
{
    melee_vita_log_info(
        "[GXR] draws=%u compiled=%u cached=%u failed=%u fallback=%u compile_ms=%llu",
        s_stats.draws, s_stats.compiled, s_stats.cache_loaded,
        s_stats.compile_failed, s_stats.fallback,
        (unsigned long long) (s_stats.compile_us / 1000u));
    s_stats.draws = 0;
    s_stats.fallback = 0;
}
