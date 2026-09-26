/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gx_render.h"
#include "gx_submit.h"
#include "profiler_live.h"
#include "fragment_alpha_key.h"
#include "gxr_shader_cache.h"
#include "point_sprite.h"
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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MELEE_VITA_TEV_HALF
#define GXR_CACHE_VERSION 4u
#define TEVT "half"
#define GXR_WARM_CACHE GXR_CACHE_DIR "/warm4.bin"
#define GXR_BUILTIN_WARM_CACHE "app0:/shadercache/warm4.bin"
#else
#define GXR_CACHE_VERSION 5u
#define TEVT "float"
#define GXR_WARM_CACHE GXR_CACHE_DIR "/warm5.bin"
#define GXR_BUILTIN_WARM_CACHE "app0:/shadercache/warm5.bin"
#endif
#define GXR_CACHE_DIR "ux0:data/melee/shadercache"
#define GXR_PROGRAM_BUCKETS 256u
#define GXR_MAX_INDEX 63000u
#define GXR_SOURCE_CAPACITY 32768u

_Static_assert(
    sizeof(GxrGpuVertex) == MELEE_VITA_GPU_VERTEX_BYTES,
    "legacy GPU vertex layout changed");
_Static_assert(
    sizeof(GxrVtxKey) == MELEE_VITA_GXR_LEGACY_KEY_BYTES,
    "legacy GPU vertex key layout changed");
_Static_assert(
    sizeof(GxrGpuBumpVertex) == MELEE_VITA_GPU_BUMP_VERTEX_BYTES,
    "bump GPU vertex layout changed");
_Static_assert(
    offsetof(GxrGpuBumpVertex, binormal) ==
        MELEE_VITA_GPU_BUMP_BINORMAL_OFFSET,
    "bump binormal offset changed");
_Static_assert(
    offsetof(GxrGpuBumpVertex, tangent) ==
        MELEE_VITA_GPU_BUMP_TANGENT_OFFSET,
    "bump tangent offset changed");

vita2d_texture* melee_vita_gxm_texture(const MeleeVitaTextureSource* source);

typedef struct GxrProgram {
    u64 hash;
    GxrShaderKey key;
    SceGxmShaderPatcherId id;
    const SceGxmProgram* program;
    const SceGxmProgramParameter* registers[4];
    const SceGxmProgramParameter* konst[4];
    const SceGxmProgramParameter* alpha_ref[2];
    const SceGxmProgramParameter* z_bias;
    const SceGxmProgramParameter* fog_color;
    const SceGxmProgramParameter* fog_params;
    const SceGxmProgramParameter* ind_mtx[2];
    bool failed;
    bool blocked;
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
static SceGxmShaderPatcherId s_depth_copy_id;
static const SceGxmProgram* s_depth_copy_gxp;
static SceGxmFragmentProgram* s_depth_copy_fragment;
static SceGxmShaderPatcherId s_color_copy_id;
static const SceGxmProgram* s_color_copy_gxp;
static SceGxmFragmentProgram* s_color_copy_fragment;
static const SceGxmProgramParameter* s_point_size_param;
static GxrProgram* s_programs[GXR_PROGRAM_BUCKETS];
static u16* s_indices;
static SceUID s_index_block = -1;
static vita2d_texture* s_white;
static bool arena_init(void);

static struct {
    u32 draws, compiled, cache_loaded, compile_failed, fallback;
    u32 ram_hits, warm_writable_hits, warm_packaged_hits, disk_hits;
    u32 compile_attempts, compile_success;
    u64 compile_us, cache_io_us, source_us, register_us, vertex_patch_us;
} s_stats;

static struct {
    u32 draws, compiled, cache_loaded, compile_failed, fallback;
    u32 program_limit, uniform_reject;
    u64 compile_us, cache_io_us, source_us, register_us, vertex_patch_us;
} s_bump_stats;

typedef struct GxrWarmPending {
    u64 hash;
    const SceGxmProgram* program;
    u32 size;
    bool vertex;
    struct GxrWarmPending* next;
} GxrWarmPending;

#define GXR_WARM_MAX_SIZE (16u * 1024u * 1024u)
static u8* s_warm_cache;
static u32 s_warm_cache_size;
static u8* s_builtin_warm_cache;
static u32 s_builtin_warm_cache_size;
static GxrWarmPending* s_warm_pending;
static GxrWarmIndexEntry s_warm_index_entries[GXR_WARM_INDEX_CAPACITY];
static GxrWarmIndex s_warm_index;
static GxrShaderCompilePolicy s_compile_policy;

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

static bool validate_warm_program(
    const void* program, u32 size, void* user)
{
    (void) size;
    (void) user;
    return sceGxmProgramCheck((const SceGxmProgram*) program) >= 0;
}

static const SceGxmProgram* find_warm_cached(
    u64 hash, bool vertex, GxrWarmSource* source)
{
    const SceGxmProgram* program = gxr_warm_index_lookup(
        &s_warm_index, hash, vertex, NULL, source);
    if (program != NULL || s_warm_index.unindexed == 0u) return program;
    program = gxr_warm_cache_scan(
        s_warm_cache, s_warm_cache_size, hash, vertex, NULL);
    if (program != NULL) {
        if (source != NULL) *source = GXR_WARM_SOURCE_WRITABLE;
        return program;
    }
    program = gxr_warm_cache_scan(
        s_builtin_warm_cache, s_builtin_warm_cache_size,
        hash, vertex, NULL);
    if (program != NULL && source != NULL)
        *source = GXR_WARM_SOURCE_PACKAGED;
    return program;
}

static void load_warm_cache_file(
    const char* path, GxrWarmSource source, u8** cache, u32* cache_size)
{
    GxrWarmError error;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    if (fd < 0) return;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size > 0 && size <= GXR_WARM_MAX_SIZE) {
        *cache = malloc((size_t) size);
        if (*cache != NULL &&
            sceIoRead(fd, *cache, (SceSize) size) == size) {
            *cache_size = (u32) size;
        } else {
            free(*cache);
            *cache = NULL;
        }
    } else if (size != 0) {
        melee_vita_log_info(
            "[GXR] warm cache rejected path=%s bytes=%lld",
            path, (long long) size);
    }
    sceIoClose(fd);
    if (*cache_size != 0u &&
        gxr_warm_index_add_buffer(
            &s_warm_index, *cache, *cache_size, source,
            validate_warm_program, NULL, &error) != GXR_WARM_OK) {
        melee_vita_log_info(
            "[GXR] malformed warm cache path=%s offset=%u reason=%s",
            path, error.offset, gxr_warm_status_name(error.status));
        free(*cache);
        *cache = NULL;
        *cache_size = 0u;
    }
}

static void load_warm_cache(void)
{
gxr_warm_index_init(
    &s_warm_index, s_warm_index_entries, GXR_WARM_INDEX_CAPACITY);
load_warm_cache_file(
    GXR_WARM_CACHE, GXR_WARM_SOURCE_WRITABLE,
    &s_warm_cache, &s_warm_cache_size);
load_warm_cache_file(
    GXR_BUILTIN_WARM_CACHE, GXR_WARM_SOURCE_PACKAGED,
    &s_builtin_warm_cache, &s_builtin_warm_cache_size);
#ifndef MELEE_VITA_RELEASE
if (s_warm_cache_size != 0u || s_builtin_warm_cache_size != 0u)
    melee_vita_log_info(
                        "[GXR] warm shader cache loaded data=%uKB builtin=%uKB "
                        "indexed=%u duplicates=%u unindexed=%u",
                        s_warm_cache_size / 1024u,
                        s_builtin_warm_cache_size / 1024u,
                        s_warm_index.count, s_warm_index.duplicates,
                        s_warm_index.unindexed);
#endif
}

static void note_warm_cache(u64 hash, bool vertex,
                        const SceGxmProgram* program, u32 size)
{
GxrWarmPending* pending;
if (find_warm_cached(hash, vertex, NULL) != NULL) return;
for (pending = s_warm_pending; pending != NULL; pending = pending->next)
    if (pending->hash == hash && pending->vertex == vertex) return;
pending = malloc(sizeof(*pending));
if (pending == NULL) return;
pending->hash = hash;
pending->program = program;
pending->size = size;
pending->vertex = vertex;
pending->next = s_warm_pending;
s_warm_pending = pending;
}

void gxr_flush_warm_cache(void)
{
    gxs_drain();
GxrWarmPending* pending;
u32 bytes = 0;
u8* buffer;
u8* out;
SceUID fd;
SceSSize written;
for (pending = s_warm_pending; pending != NULL; pending = pending->next)
    bytes += sizeof(GxrWarmHeader) + ((pending->size + 3u) & ~3u);
if (bytes == 0u) return;
if (bytes > GXR_WARM_MAX_SIZE - s_warm_cache_size) return;
buffer = calloc(1, bytes);
if (buffer == NULL) return;
out = buffer;
for (pending = s_warm_pending; pending != NULL; pending = pending->next) {
    GxrWarmHeader header = {
        GXR_WARM_MAGIC, pending->vertex ? 1u : 0u,
        pending->size, 0u, pending->hash
    };
    memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    memcpy(out, pending->program, pending->size);
    out += (pending->size + 3u) & ~3u;
}
fd = sceIoOpen(GXR_WARM_CACHE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
written = fd >= 0 ? sceIoWrite(fd, buffer, bytes) : -1;
if (fd >= 0) sceIoClose(fd);
free(buffer);
if (written != (SceSSize) bytes) return;
#ifndef MELEE_VITA_RELEASE
melee_vita_log_info("[GXR] warm shader cache appended %uKB", bytes / 1024u);
#endif
while (s_warm_pending != NULL) {
    pending = s_warm_pending;
    s_warm_pending = pending->next;
    free(pending);
}
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

#ifndef MELEE_VITA_RELEASE
static void shark_log(const char* message, shark_log_level level, int line)
{
    if (level >= SHARK_LOG_WARNING)
        melee_vita_log_info("[GXR] shacccg %s line %d: %s",
                            level == SHARK_LOG_ERROR ? "error" : "warning",
                            line, message);
}
#endif

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

static void alpha_compare_expr(Source* s, u8 comp, const char* ref)
{
    const char* ops[] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
    if (comp == GX_NEVER) { emit(s, "false"); return; }
    if (comp == GX_ALWAYS) { emit(s, "true"); return; }
    emit(s, "(ac %s %s)", ops[comp & 7u], ref);
}

static bool alpha_test_trivially_passes(const GxrShaderKey* key)
{
    return melee_vita_alpha_test_trivially_passes(
        key->alpha_op, key->alpha_comp[0], key->alpha_comp[1]);
}

static bool build_fragment_source(const GxrShaderKey* key, Source* s)
{
    const u8 z_tex_op = key->z_tex_op & 3u;
    const u8 fog_type = key->z_tex_op >> 4;
    bool uses_coord[GXR_MAX_TEXCOORDS] = { false };
    bool uses_map[GXR_MAX_TEXMAPS] = { false };
    bool uses_indirect = false;
    for (u32 i = 0; i < key->stage_count; ++i) {
        const GxrStage* st = &key->stages[i];
        if (st->tex_map < GXR_MAX_TEXMAPS) {
            uses_map[st->tex_map] = true;
            if (st->tex_coord < GXR_MAX_TEXCOORDS) uses_coord[st->tex_coord] = true;
            if ((st->mirror & 0x80u) != 0u) {
                uses_indirect = true;
                uses_map[(st->mirror >> 5) & 3u] = true;
                uses_coord[(st->mirror >> 2) & 7u] = true;
            }
        }
    }

    const bool writes_depth =
        z_tex_op != GX_ZT_DISABLE &&
        key->z_tex_format == GX_TF_Z24X8 &&
        key->stage_count > 0u;
    const bool needs_position = fog_type != GX_FOG_NONE ||
                                z_tex_op == GX_ZT_ADD;
    const bool trivially_pass = alpha_test_trivially_passes(key);
    if (writes_depth)
        emit(s, "struct GxrFragmentOut { " TEVT "4 color : COLOR; float depth : DEPTH; };\n");
    emit(s, writes_depth ? "GxrFragmentOut main(\n" : TEVT "4 main(\n");
    emit(s, "    float4 vColor0 : COLOR0,\n    float4 vColor1 : COLOR1");
    for (u32 i = 0; i < GXR_MAX_TEXCOORDS; ++i)
        if (uses_coord[i]) emit(s, ",\n    float2 vTex%u : TEXCOORD%u", i, i);
    for (u32 i = 0; i < GXR_MAX_TEXMAPS; ++i)
        if (uses_map[i]) emit(s, ",\n    uniform sampler2D uMap%u : TEXUNIT%u", i, i);
    emit(s, ",\n    uniform float4 uPrev, uniform float4 uReg0, uniform float4 uReg1, uniform float4 uReg2");
    emit(s, ",\n    uniform float4 uK0, uniform float4 uK1, uniform float4 uK2, uniform float4 uK3");
    if (uses_indirect)
        emit(s, ",\n    uniform float4 uIndMtx0, uniform float4 uIndMtx1");
    if (needs_position)
        emit(s, ",\n    float4 vPosition : WPOS");
    if (fog_type != GX_FOG_NONE)
        emit(s, ",\n    uniform float4 uFogColor, uniform float4 uFogParams");
    for (u32 i = 0; i < 2u; ++i)
        if (key->alpha_comp[i] != GX_NEVER &&
            key->alpha_comp[i] != GX_ALWAYS)
            emit(s, ",\n    uniform float uAlphaRef%u", i);
    if (writes_depth)
        emit(s, ",\n    uniform float uZBias)\n{\n");
    else
        emit(s, ") : COLOR\n{\n");
    emit(s, "    " TEVT "4 prev = uPrev;\n    " TEVT "4 r0 = uReg0;\n    " TEVT "4 r1 = uReg1;\n    " TEVT "4 r2 = uReg2;\n");
    emit(s, "    " TEVT "4 k0 = uK0;\n    " TEVT "4 k1 = uK1;\n    " TEVT "4 k2 = uK2;\n    " TEVT "4 k3 = uK3;\n");
    emit(s, "    " TEVT "4 ras0 = vColor0;\n    " TEVT "4 ras1 = vColor1;\n");

    static const char* reg_names[] = { "prev", "r0", "r1", "r2" };
    for (u32 i = 0; i < key->stage_count; ++i) {
        const GxrStage* st = &key->stages[i];
        char a[96], b[96], c[96], d[96];
        if (st->tex_map < GXR_MAX_TEXMAPS) {
            if (st->tex_coord < GXR_MAX_TEXCOORDS && (st->mirror & 0x80u) != 0u) {
                /* GX indirect texturing: offset this stage's coordinate by
                 * a matrix times the indirect texel (Dolphin reads .abg). */
                emit(s, "    float3 ind%u = float3(tex2D(uMap%u, vTex%u).abg) * 255.0;\n",
                     i, (u32) ((st->mirror >> 5) & 3u), (u32) ((st->mirror >> 2) & 7u));
                emit(s, "    float2 uv%u = vTex%u + float2(dot(uIndMtx0.xyz, ind%u) + uIndMtx0.w, dot(uIndMtx1.xyz, ind%u) + uIndMtx1.w);\n",
                     i, st->tex_coord, i, i);
                if (st->mirror & 1u)
                    emit(s, "    uv%u.x = 1.0 - abs(frac(uv%u.x * 0.5) * 2.0 - 1.0);\n", i, i);
                if (st->mirror & 2u)
                    emit(s, "    uv%u.y = 1.0 - abs(frac(uv%u.y * 0.5) * 2.0 - 1.0);\n", i, i);
                emit(s, "    " TEVT "4 s%u = tex2D(uMap%u, uv%u);\n", i, st->tex_map, i);
            } else if (st->tex_coord < GXR_MAX_TEXCOORDS && (st->mirror & 3u) != 0u) {
                emit(s, "    float2 uv%u = vTex%u;\n", i, st->tex_coord);
                if (st->mirror & 1u)
                    emit(s, "    uv%u.x = 1.0 - abs(frac(uv%u.x * 0.5) * 2.0 - 1.0);\n", i, i);
                if (st->mirror & 2u)
                    emit(s, "    uv%u.y = 1.0 - abs(frac(uv%u.y * 0.5) * 2.0 - 1.0);\n", i, i);
                emit(s, "    " TEVT "4 s%u = tex2D(uMap%u, uv%u);\n", i, st->tex_map, i);
            } else if (st->tex_coord < GXR_MAX_TEXCOORDS)
                emit(s, "    " TEVT "4 s%u = tex2D(uMap%u, vTex%u);\n", i, st->tex_map, st->tex_coord);
            else
                emit(s, "    " TEVT "4 s%u = tex2D(uMap%u, float2(0.0,0.0));\n", i, st->tex_map);
        }
        color_arg(a, sizeof(a), key, st, i, st->color_in[0]);
        color_arg(b, sizeof(b), key, st, i, st->color_in[1]);
        color_arg(c, sizeof(c), key, st, i, st->color_in[2]);
        color_arg(d, sizeof(d), key, st, i, st->color_in[3]);
        emit(s, "    " TEVT "3 c%u = clamp(", i);
        op_expr(s, st->color_op, st->color_bias, st->color_scale, true, a, b, c, d);
        emit(s, st->color_clamp ? ", 0.0, 1.0);\n" : ", -4.0, 4.0);\n");

        alpha_arg(a, sizeof(a), key, st, i, st->alpha_in[0]);
        alpha_arg(b, sizeof(b), key, st, i, st->alpha_in[1]);
        alpha_arg(c, sizeof(c), key, st, i, st->alpha_in[2]);
        alpha_arg(d, sizeof(d), key, st, i, st->alpha_in[3]);
        emit(s, "    " TEVT " a%u = clamp(", i);
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
        case GX_FOG_ORTHO_LIN:
            emit(s, "    float fogZ = fogF;\n");
            break;
        case GX_FOG_PERSP_EXP:
        case GX_FOG_ORTHO_EXP:
            emit(s, "    float fogZ = 1.0 - exp2(-8.0 * fogF);\n");
            break;
        case GX_FOG_PERSP_EXP2:
        case GX_FOG_ORTHO_EXP2:
            emit(s, "    float fogZ = 1.0 - exp2(-8.0 * fogF * fogF);\n");
            break;
        case GX_FOG_PERSP_REVEXP:
        case GX_FOG_ORTHO_REVEXP:
            emit(s, "    float fogZ = exp2(-8.0 * (1.0 - fogF));\n");
            break;
        case GX_FOG_PERSP_REVEXP2:
        case GX_FOG_ORTHO_REVEXP2:
            emit(s, "    fogF = 1.0 - fogF;\n");
            emit(s, "    float fogZ = exp2(-8.0 * fogF * fogF);\n");
            break;
        default:
            emit(s, "    float fogZ = 0.0;\n");
            break;
        }
        emit(s, "    prev.rgb = lerp(prev.rgb, uFogColor.rgb, clamp(fogZ, 0.0, 1.0));\n");
    }
    if (writes_depth) {
        const u32 stage_index = key->stage_count - 1u;
        const GxrStage* stage = &key->stages[stage_index];
        char texel[96];
        snprintf(texel, sizeof(texel), "s%u", stage_index);
        {
            char swapped_texel[128];
            swapped(swapped_texel, sizeof(swapped_texel), texel, key,
                    stage->swap_tex, false);
            emit(s, "    float3 zt = floor(%s * 255.0 + 0.5);\n",
                 swapped_texel);
        }
        emit(s, "    float z24 = dot(zt, float3(65536.0, 256.0, 1.0)) + uZBias;\n");
        if (z_tex_op == GX_ZT_ADD)
            emit(s, "    z24 += floor(clamp(vPosition.z, 0.0, 1.0) * 16777215.0 + 0.5);\n");
        emit(s, "    z24 -= floor(z24 / 16777216.0) * 16777216.0;\n");
        emit(s, "    GxrFragmentOut output;\n");
        emit(s, "    output.color = clamp(prev, 0.0, 1.0);\n");
        emit(s, "    output.depth = z24 / 16777215.0;\n");
        emit(s, "    return output;\n}\n");
    } else {
        emit(s, "    return clamp(prev, 0.0, 1.0);\n}\n");
    }
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

static const char k_depth_copy_source[] =
    "float4 main(float2 vTex0 : TEXCOORD0,\n"
    "    uniform sampler2D uDepth : TEXUNIT0) : COLOR\n"
    "{\n"
    "    float depth = clamp(tex2D(uDepth, vTex0).r, 0.0, 1.0);\n"
    "    float z24 = floor(depth * 16777215.0 + 0.5);\n"
    "    float high = floor(z24 / 65536.0);\n"
    "    float middle = floor(z24 / 256.0) - high * 256.0;\n"
    "    float low = z24 - floor(z24 / 256.0) * 256.0;\n"
    "    return float4(high, middle, low, 255.0) / 255.0;\n"
    "}\n";

static const char k_color_copy_source[] =
    "float4 main(float2 vTex0 : TEXCOORD0,\n"
    "    uniform sampler2D uColor : TEXUNIT0) : COLOR\n"
    "{\n"
    "    return tex2D(uColor, vTex0);\n"
    "}\n";

/* ------------------------------------------------------------ compilation */

static SceGxmProgram* load_cached(u64 hash, bool vertex)
{
    const u64 started = sceKernelGetProcessTimeWide();
    GxrWarmSource warm_source = GXR_WARM_SOURCE_NONE;
    const SceGxmProgram* warm =
        find_warm_cached(hash, vertex, &warm_source);
    char path[128];
    if (warm != NULL) {
        if (warm_source == GXR_WARM_SOURCE_WRITABLE)
            ++s_stats.warm_writable_hits;
        else if (warm_source == GXR_WARM_SOURCE_PACKAGED)
            ++s_stats.warm_packaged_hits;
        s_stats.cache_io_us += sceKernelGetProcessTimeWide() - started;
        return (SceGxmProgram*) warm;
    }
    snprintf(path, sizeof(path), GXR_CACHE_DIR "/%s%016llx.gxp",
             vertex ? "v" : "f", (unsigned long long) hash);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        s_stats.cache_io_us += sceKernelGetProcessTimeWide() - started;
        return NULL;
    }
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
    if (program != NULL) {
        ++s_stats.disk_hits;
        note_warm_cache(hash, vertex, program, (u32) size);
    }
    s_stats.cache_io_us += sceKernelGetProcessTimeWide() - started;
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
    note_warm_cache(hash, vertex, program, size);
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
    if (!gxr_shader_compile_policy_allow(
            &s_compile_policy, GXR_SHADER_COMPILER_NORMAL)) {
        const u32 blocked =
            gxr_shader_compile_policy_blocked(&s_compile_policy);
        if (blocked <= 4u || (blocked & (blocked - 1u)) == 0u)
            melee_vita_log_info(
                "[GXR] sealed shader miss blocked hash=%016llx "
                "vertex=%u blocked=%u",
                (unsigned long long) hash, vertex ? 1u : 0u, blocked);
        return NULL;
    }
    const u64 started = sceKernelGetProcessTimeWide();
    /* vitaShaRK reads the source length from *size and writes the program
     * size back through the same pointer. */
    u32 size = (u32) strlen(source);
    ++s_stats.compile_attempts;
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
    ++s_stats.compile_success;
    store_cached(hash, vertex, copy, size);
    return copy;
}

#define GXR_BUMP_CACHE_PREFIX "bv1-"

static SceGxmProgram* load_bump_cached(u64 hash)
{
    static u32 invalid_logs;
    const u64 started = sceKernelGetProcessTimeWide();
    char path[128];
    SceGxmProgram* program = NULL;
    snprintf(path, sizeof(path), GXR_CACHE_DIR "/" GXR_BUMP_CACHE_PREFIX
             "%016llx.gxp", (unsigned long long) hash);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        if (size > 0 && size < 1024 * 1024) {
            program = malloc((size_t) size);
            if (program != NULL &&
                (sceIoRead(fd, program, (SceSize) size) != size ||
                 sceGxmProgramCheck(program) < 0)) {
                free(program);
                program = NULL;
                if (invalid_logs++ < 4u)
                    melee_vita_log_info(
                        "[GXR/BUMP] invalid disk cache hash=%016llx",
                        (unsigned long long) hash);
            }
            if (program != NULL) ++s_stats.disk_hits;
        }
        sceIoClose(fd);
    }
    s_bump_stats.cache_io_us += sceKernelGetProcessTimeWide() - started;
    return program;
}

static void store_bump_cached(
    u64 hash, const SceGxmProgram* program, u32 size)
{
    static u32 write_logs;
    char path[128];
    snprintf(path, sizeof(path), GXR_CACHE_DIR "/" GXR_BUMP_CACHE_PREFIX
             "%016llx.gxp", (unsigned long long) hash);
    SceUID fd =
        sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        if (write_logs++ < 4u)
            melee_vita_log_info(
                "[GXR/BUMP] disk cache open failed hash=%016llx code=%d",
                (unsigned long long) hash, (int) fd);
        return;
    }
    const SceSSize written = sceIoWrite(fd, program, size);
    sceIoClose(fd);
    if (written != (SceSSize) size && write_logs++ < 4u)
        melee_vita_log_info(
            "[GXR/BUMP] disk cache write failed hash=%016llx "
            "wrote=%d expected=%u",
            (unsigned long long) hash, (int) written, size);
}

static const SceGxmProgram* compile_bump_program(
    const char* source, u64 hash)
{
    if (!gxr_shader_compile_policy_allow(
            &s_compile_policy, GXR_SHADER_COMPILER_BUMP)) {
        const u32 blocked =
            gxr_shader_compile_policy_blocked(&s_compile_policy);
        if (blocked <= 4u || (blocked & (blocked - 1u)) == 0u)
            melee_vita_log_info(
                "[GXR/BUMP] sealed shader miss blocked hash=%016llx "
                "blocked=%u",
                (unsigned long long) hash, blocked);
        return NULL;
    }
    const u64 started = sceKernelGetProcessTimeWide();
    u32 size = (u32) strlen(source);
    ++s_stats.compile_attempts;
    SceGxmProgram* compiled = shark_compile_shader_extended(
        source, &size, SHARK_VERTEX_SHADER, SHARK_OPT_DEFAULT,
        SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
    s_bump_stats.compile_us += sceKernelGetProcessTimeWide() - started;
    if (compiled == NULL || size == 0) {
        const SceShaccCgCompileOutput* output =
            shark_get_internal_compile_output();
        if (output != NULL) {
            for (int i = 0; i < output->diagnosticCount; ++i) {
                const SceShaccCgDiagnosticMessage* d =
                    &output->diagnostics[i];
                melee_vita_log_info(
                    "[GXR/BUMP] compile diag level=%d code=%d "
                    "line=%d: %s",
                    (int) d->level, (int) d->code,
                    d->location != NULL
                        ? (int) d->location->lineNumber : -1,
                    d->message != NULL ? d->message : "(null)");
            }
        }
        melee_vita_log_info(
            "[GXR/BUMP] compile failed hash=%016llx source_bytes=%u",
            (unsigned long long) hash, (unsigned) strlen(source));
        shark_clear_output();
        ++s_bump_stats.compile_failed;
        return NULL;
    }
    SceGxmProgram* copy = malloc(size);
    if (copy != NULL) memcpy(copy, compiled, size);
    shark_clear_output();
    if (copy == NULL) return NULL;
    ++s_bump_stats.compiled;
    ++s_stats.compile_success;
    store_bump_cached(hash, copy, size);
    return copy;
}

/* vita2d's shader patcher has small fixed pools, and the generated GX programs
 * are many: after a few scenes' worth of TEV and vertex variants it runs out
 * and every later patch fails, which shows up as white or shattered geometry
 * until the game is restarted.  These programs get their own patcher with room
 * to grow. */
#define GXR_PATCHER_BUFFER (4u * 1024u * 1024u)
#define GXR_PATCHER_VERTEX_USSE (1u * 1024u * 1024u)
#define GXR_PATCHER_FRAGMENT_USSE (1u * 1024u * 1024u)

static SceGxmShaderPatcher* s_patcher;

static void* patcher_alloc(void* user, unsigned int size) { (void) user; return malloc(size); }
static void patcher_free(void* user, void* mem) { (void) user; free(mem); }

static void* gpu_alloc_mapped(u32 size, u32 attribs, SceUID* uid_out)
{
    void* base = NULL;
    const u32 aligned = (size + 0xfffu) & ~0xfffu;
    SceUID uid = sceKernelAllocMemBlock("melee_gxr_patcher",
                                        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                        aligned, NULL);
    if (uid < 0) return NULL;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 ||
        sceGxmMapMemory(base, aligned, attribs) < 0) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    *uid_out = uid;
    return base;
}

static void* usse_alloc(u32 size, SceUID* uid_out, unsigned int* offset_out, int fragment)
{
    void* memory = NULL;
    const u32 aligned = (size + 0xfffu) & ~0xfffu;
    SceUID uid = sceKernelAllocMemBlock("melee_gxr_usse",
                                        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                        aligned, NULL);
    if (uid < 0) return NULL;
    if (sceKernelGetMemBlockBase(uid, &memory) < 0 ||
        (fragment ? sceGxmMapFragmentUsseMemory(memory, aligned, offset_out)
                  : sceGxmMapVertexUsseMemory(memory, aligned, offset_out)) < 0) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    *uid_out = uid;
    return memory;
}

static SceGxmShaderPatcher* gxr_patcher(void)
{
    static SceUID buffer_uid = -1, vertex_uid = -1, fragment_uid = -1;
    SceGxmShaderPatcherParams params;
    void* buffer;
    void* vertex_usse;
    void* fragment_usse;
    unsigned int vertex_offset = 0, fragment_offset = 0;
    if (s_patcher != NULL) return s_patcher;
    buffer = gpu_alloc_mapped(GXR_PATCHER_BUFFER,
                              SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE,
                              &buffer_uid);
    vertex_usse = usse_alloc(GXR_PATCHER_VERTEX_USSE, &vertex_uid, &vertex_offset, 0);
    fragment_usse = usse_alloc(GXR_PATCHER_FRAGMENT_USSE, &fragment_uid, &fragment_offset, 1);
    if (buffer == NULL || vertex_usse == NULL || fragment_usse == NULL) {
        melee_vita_log_info("[GXR] patcher memory unavailable; using vita2d's");
        return vita2d_get_shader_patcher();
    }
    memset(&params, 0, sizeof(params));
    params.hostAllocCallback = patcher_alloc;
    params.hostFreeCallback = patcher_free;
    params.bufferMem = buffer;
    params.bufferMemSize = GXR_PATCHER_BUFFER;
    params.vertexUsseMem = vertex_usse;
    params.vertexUsseMemSize = GXR_PATCHER_VERTEX_USSE;
    params.vertexUsseOffset = vertex_offset;
    params.fragmentUsseMem = fragment_usse;
    params.fragmentUsseMemSize = GXR_PATCHER_FRAGMENT_USSE;
    params.fragmentUsseOffset = fragment_offset;
    if (sceGxmShaderPatcherCreate(&params, &s_patcher) < 0 || s_patcher == NULL) {
        melee_vita_log_info("[GXR] patcher creation failed; using vita2d's");
        s_patcher = NULL;
        return vita2d_get_shader_patcher();
    }
    melee_vita_log_info("[GXR] shader patcher: %u KiB buffer, %u KiB vertex USSE, %u KiB fragment USSE",
                        GXR_PATCHER_BUFFER / 1024u, GXR_PATCHER_VERTEX_USSE / 1024u,
                        GXR_PATCHER_FRAGMENT_USSE / 1024u);
    return s_patcher;
}

int gxr_init(void)
{
    if (s_ready || s_failed) return s_ready ? 0 : -1;
    s_failed = true;
    gxr_shader_compile_policy_init(&s_compile_policy, true);
    if (shark_init(NULL) < 0) {
        melee_vita_log_info("[GXR] shark_init failed (libshacccg.suprx missing?)");
        return -1;
    }
#ifndef MELEE_VITA_RELEASE
    shark_install_log_cb(shark_log);
#endif
    shark_set_warnings_level(SHARK_WARN_SILENT);
    sceIoMkdir("ux0:data/melee", 0777);
    sceIoMkdir(GXR_CACHE_DIR, 0777);
    load_warm_cache();

    SceGxmShaderPatcher* patcher = gxr_patcher();
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

    {
        const u64 depth_copy_hash =
            fnv1a(k_depth_copy_source, sizeof(k_depth_copy_source),
                  UINT64_C(1469598103934665603) ^ GXR_CACHE_VERSION);
        s_depth_copy_gxp =
            obtain_program(k_depth_copy_source, false, depth_copy_hash);
        if (s_depth_copy_gxp != NULL &&
            sceGxmShaderPatcherRegisterProgram(
                patcher, s_depth_copy_gxp, &s_depth_copy_id) >= 0) {
            sceGxmShaderPatcherCreateFragmentProgram(
                patcher, s_depth_copy_id,
                SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                SCE_GXM_MULTISAMPLE_NONE, NULL, s_vertex_gxp,
                &s_depth_copy_fragment);
        }
        if (s_depth_copy_fragment == NULL)
            melee_vita_log_info("[GXR] depth-copy shader unavailable");
    }
    {
        const u64 color_copy_hash =
            fnv1a(k_color_copy_source, sizeof(k_color_copy_source),
                  UINT64_C(1469598103934665603) ^ GXR_CACHE_VERSION);
        s_color_copy_gxp =
            obtain_program(k_color_copy_source, false, color_copy_hash);
        if (s_color_copy_gxp != NULL &&
            sceGxmShaderPatcherRegisterProgram(
                patcher, s_color_copy_gxp, &s_color_copy_id) >= 0) {
            sceGxmShaderPatcherCreateFragmentProgram(
                patcher, s_color_copy_id,
                SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                SCE_GXM_MULTISAMPLE_NONE, NULL, s_vertex_gxp,
                &s_color_copy_fragment);
        }
        if (s_color_copy_fragment == NULL)
            melee_vita_log_info("[GXR] color-copy shader unavailable");
    }

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
#ifdef MELEE_VITA_SHADER_CACHE_SEAL
    gxr_set_runtime_shader_compilation_enabled(false);
#endif
    melee_vita_log_info("[GXR] shader renderer ready (vertex attrs=%u)",
                        attribute_count);
    return 0;
}

bool gxr_available(void) { return s_ready; }

void gxr_set_runtime_shader_compilation_enabled(bool enabled)
{
    gxr_shader_compile_policy_set(&s_compile_policy, enabled);
    melee_vita_log_info(
        "[GXR] runtime shader compilation %s",
        enabled ? "enabled" : "sealed");
}

bool gxr_runtime_shader_compilation_enabled(void)
{
    return s_compile_policy.enabled;
}

static GxrProgram* find_program(const GxrShaderKey* key)
{
    u64 hash_seed = UINT64_C(1469598103934665603) ^ GXR_CACHE_VERSION;
    if (!alpha_test_trivially_passes(key))
        hash_seed ^= UINT64_C(0xbbe8f48b562f9d7d);
    const u64 hash = fnv1a(key, sizeof(*key), hash_seed);
    GxrProgram** bucket = &s_programs[hash % GXR_PROGRAM_BUCKETS];
    for (GxrProgram** link = bucket; *link != NULL;) {
        GxrProgram* p = *link;
        if (p->hash == hash && memcmp(&p->key, key, sizeof(*key)) == 0) {
            if (p->blocked) {
                if (s_compile_policy.enabled) {
                    *link = p->next;
                    free(p);
                    break;
                }
                return p;
            }
            ++s_stats.ram_hits;
            return p;
        }
        link = &p->next;
    }

    GxrProgram* p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->hash = hash;
    p->key = *key;
    p->next = *bucket;
    *bucket = p;

    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    p->program = load_cached(hash, false);
    if (p->program != NULL) {
        ++s_stats.cache_loaded;
    } else {
        const u64 started = sceKernelGetProcessTimeWide();
        buffer[0] = '\0';
        if (!build_fragment_source(key, &source)) {
            p->failed = true;
            melee_vita_log_info("[GXR] fragment source overflow");
            return p;
        }
        s_stats.source_us += sceKernelGetProcessTimeWide() - started;
        p->program = obtain_program(buffer, false, hash);
        if (p->program == NULL && !s_compile_policy.enabled) {
            p->blocked = true;
            p->failed = true;
            return p;
        }
    }
    const u64 register_started = sceKernelGetProcessTimeWide();
    if (p->program == NULL ||
        sceGxmShaderPatcherRegisterProgram(gxr_patcher(),
                                           p->program, &p->id) < 0) {
        p->failed = true;
        melee_vita_log_info("[GXR] fragment compile failed hash=%016llx stages=%u",
                            (unsigned long long) hash, key->stage_count);
        if (source.length == 0u) {
            buffer[0] = '\0';
            build_fragment_source(key, &source);
        }
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
    s_stats.register_us += sceKernelGetProcessTimeWide() - register_started;
    static const char* reg_names[] = { "uPrev", "uReg0", "uReg1", "uReg2" };
    static const char* k_names[] = { "uK0", "uK1", "uK2", "uK3" };
    for (u32 i = 0; i < 4u; ++i) {
        p->registers[i] = sceGxmProgramFindParameterByName(p->program, reg_names[i]);
        p->konst[i] = sceGxmProgramFindParameterByName(p->program, k_names[i]);
    }
    p->alpha_ref[0] =
        sceGxmProgramFindParameterByName(p->program, "uAlphaRef0");
    p->alpha_ref[1] =
        sceGxmProgramFindParameterByName(p->program, "uAlphaRef1");
    p->z_bias = sceGxmProgramFindParameterByName(p->program, "uZBias");
    p->fog_color =
        sceGxmProgramFindParameterByName(p->program, "uFogColor");
    p->fog_params =
        sceGxmProgramFindParameterByName(p->program, "uFogParams");
    p->ind_mtx[0] = sceGxmProgramFindParameterByName(p->program, "uIndMtx0");
    p->ind_mtx[1] = sceGxmProgramFindParameterByName(p->program, "uIndMtx1");
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
            gxr_patcher(), program->id,
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
    const struct GxrBumpVtxProgram* bump_vp;
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
    f32 alpha_ref[2];
    f32 z_bias;
    f32 fog_color[4];
    f32 fog_params[4];
    u8 texture_mask;
    u8 cpu_path;
    u8 bump_path;
    u8 reserved8;
    u16 mtx_comps, tg_comps, light_comps, point_comps;
    u16 tex_comps;
    f32 registers[4][4];
    f32 konst[4][4];
    f32 ind_mtx[2][4];
    f32 uniforms[]; /* pos, nrm, proj, tex, post, light, mat, amb, point */
} RqDraw;

static struct {
    u32 epoch;
    bool valid;
    SceGxmFragmentProgram* fragment;
    SceGxmVertexProgram* vertex;
    SceGxmDepthFunc depth_function;
    SceGxmDepthWriteMode depth_write;
    u32 line_width;
    u32 polygon_mode;
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

/* RAM front cache for fragment-program lookups.  find_program() hashes the
 * whole key byte by byte with the persistent (disk/warm-cache) hash, which
 * cost several ms per frame when consecutive draws alternate between
 * materials.  Only the stages in use affect the generated shader, so the
 * front cache hashes and compares just that prefix, a word at a time. */
#define GXR_KEY_CACHE_SIZE 512u
typedef struct GxrKeyCacheEntry {
    u32 length;
    GxrProgram* program;
    GxrShaderKey key;
} GxrKeyCacheEntry;
static GxrKeyCacheEntry s_key_cache[GXR_KEY_CACHE_SIZE];

static u32 shader_key_used_length(const GxrShaderKey* key)
{
    const u32 stages = key->stage_count < GXR_MAX_STAGES
        ? key->stage_count : GXR_MAX_STAGES;
    return (u32) offsetof(GxrShaderKey, stages) +
           stages * (u32) sizeof(GxrStage);
}

static u32 shader_key_quick_hash(const GxrShaderKey* key, u32 length)
{
    const u8* bytes = (const u8*) key;
    u32 hash = 2166136261u;
    u32 i = 0;
    for (; i + 4u <= length; i += 4u) {
        u32 word;
        memcpy(&word, bytes + i, sizeof(word));
        hash = (hash ^ word) * 16777619u;
    }
    for (; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    hash ^= hash >> 15;
    return hash;
}

static GxrProgram* lookup_program(const GxrShaderKey* key)
{
    GxrShaderKey normalized = *key;
    GxrProgram* program;
    GxrKeyCacheEntry* slot;
    u32 length;
    melee_vita_normalize_alpha_shader_refs(normalized.alpha_ref);
    length = shader_key_used_length(&normalized);
    if (s_last_program != NULL &&
        memcmp(&s_last_key, &normalized, length) == 0) {
        return s_last_program;
    }
    slot = &s_key_cache[shader_key_quick_hash(&normalized, length) &
                        (GXR_KEY_CACHE_SIZE - 1u)];
    if (slot->program != NULL && slot->length == length &&
        memcmp(&slot->key, &normalized, length) == 0) {
        program = slot->program;
    } else {
        program = find_program(&normalized);
        if (program != NULL && !program->failed && !program->blocked) {
            slot->length = length;
            slot->program = program;
            memcpy(&slot->key, &normalized, length);
        }
    }
    if (program != NULL && !program->failed) {
        memcpy(&s_last_key, &normalized, length);
        s_last_program = program;
    }
    return program;
}

/* Per-texmap memo of the last resolved texture.  Consecutive draws of one
 * model usually rebind the same GXTexObj, and a full cache lookup (with its
 * content check) per draw added up to several ms per frame.  The memo is
 * dropped every frame and whenever an EFB copy may rebind a texture key. */
u32 g_melee_vita_texture_memo_epoch = 1u;
static struct {
    u32 epoch;
    MeleeVitaTextureSource source;
    vita2d_texture* texture;
} s_texture_memo[GXR_MAX_TEXMAPS];

static vita2d_texture* resolve_texture_memo(u32 map,
                                            const MeleeVitaTextureSource* source)
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
        for (u32 i = 0; i < draw->key.stage_count; ++i) {
            if (draw->key.stages[i].tex_map == map ||
                ((draw->key.stages[i].mirror & 0x80u) != 0u &&
                 ((draw->key.stages[i].mirror >> 5) & 3u) == map)) {
                /* the second test binds an indirect stage's offset map */
                used = true;
                break;
            }
        }
        if (!used) continue;
        vita2d_texture* texture = draw->texture_valid[map]
            ? resolve_texture_memo(map, &draw->textures[map]) : NULL;
        if (texture == NULL) {
            static u32 logged;
            if (logged < 40u) {
                const MeleeVitaTextureSource* t = &draw->textures[map];
                ++logged;
                melee_vita_log_info("[TEXMISS] map=%u valid=%u data=%p %ux%u fmt=%u pal=%p palfmt=%u n=%u",
                                    map, draw->texture_valid[map], t->data, t->width, t->height,
                                    (unsigned) t->format, t->palette, (unsigned) t->palette_format,
                                    (unsigned) t->palette_entries);
            }
        }
        d->textures[map] = texture != NULL ? texture : s_white;
        d->texture_mask |= (u8) (1u << map);
    }
}

static SceGxmDepthFunc draw_depth_function(const GxrDraw* draw)
{
    return draw->depth_compare ? depth_func(draw->depth_function) : SCE_GXM_DEPTH_FUNC_ALWAYS;
}

static bool gxr_draw_now(const GxrDraw* draw, GxrVertex* vertices,
                         const u16* indices, u32 count)
{
    GxrProgram* program;
    SceGxmFragmentProgram* fragment;
    RqDraw* d;
    if (!s_ready || draw == NULL || vertices == NULL || count == 0) return false;
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) { ++s_stats.fallback; return false; }
    fragment = find_fragment(program, draw);
    if (fragment == NULL) { ++s_stats.fallback; return false; }
    d = melee_vita_rq_push_direct(exec_draw, sizeof(RqDraw));
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
    memcpy(d->ind_mtx, draw->ind_mtx, sizeof(d->ind_mtx));
    d->alpha_ref[0] = (f32) draw->key.alpha_ref[0];
    d->alpha_ref[1] = (f32) draw->key.alpha_ref[1];
    d->z_bias = (f32) draw->z_tex_bias;
    memcpy(d->fog_color, draw->fog_color, sizeof(d->fog_color));
    memcpy(d->fog_params, draw->fog_params, sizeof(d->fog_params));
    resolve_textures(draw, d);
    ++s_stats.draws;
    return true;
}

/* ===================================================== GPU vertex pipeline */

#define GXR_VTX_VERSION MELEE_VITA_GXR_LEGACY_KEY_VERSION
#define GXR_ARENA_SIZE (48u * 1024u * 1024u)

_Static_assert(sizeof(GxrGpuVertex) == MELEE_VITA_GPU_POINT_STRIDE,
               "GPU point stride must match payload accounting");
_Static_assert(sizeof(GxrVtxKey) == MELEE_VITA_NONPOINT_VTX_KEY_SIZE,
               "non-point vertex key layout must remain cache-compatible");

typedef struct GxrVtxProgram {
    u64 hash;
    GxrVtxKey key;
    bool failed;
    bool blocked;
    bool point;
    u8 point_tex_mask;
    SceGxmShaderPatcherId id;
    const SceGxmProgram* program;
    SceGxmVertexProgram* vertex;
    const SceGxmProgramParameter* u_pos, *u_nrm, *u_proj, *u_tex, *u_post,
        *u_light, *u_mat, *u_amb, *u_point;
    u32 logged;
    struct GxrVtxProgram* next;
} GxrVtxProgram;

typedef struct GxrBumpVtxProgram {
    u64 hash;
    GxrBumpVtxKey key;
    bool failed;
    bool blocked;
    SceGxmShaderPatcherId id;
    const SceGxmProgram* program;
    SceGxmVertexProgram* vertex;
    const SceGxmProgramParameter* u_pos, *u_nrm, *u_proj, *u_tex, *u_post,
        *u_light, *u_mat, *u_amb;
    struct GxrBumpVtxProgram* next;
} GxrBumpVtxProgram;

static GxrVtxProgram* s_vtx_programs[GXR_PROGRAM_BUCKETS];
#define GXR_BUMP_PROGRAM_LIMIT 8u
static GxrBumpVtxProgram* s_bump_vtx_programs[GXR_PROGRAM_BUCKETS];
static u32 s_bump_vtx_program_count;

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

static bool key_uses_tex_palette(const GxrVtxKey* key);

static bool build_vertex_source(const GxrVtxKey* key, bool point,
                                u8 point_tex_mask, Source* s)
{
    emit(s, "void main(float3 aPos, float aMtx, float3 aNrm, float4 aC0, float4 aC1,\n");
    emit(s, "    float2 aT0, float2 aT1, float2 aT2, float2 aT3,\n");
    emit(s, "    uniform float4 uPos[30], uniform float4 uNrm[30], uniform float4 uProj[4],\n");
    emit(s, "    uniform float4 uTex[%u], uniform float4 uPost[24], uniform float4 uLight[40],\n",
         key_uses_tex_palette(key) ? GXR_TEX_PALETTE_ROW + 30u : 24u);
    emit(s, "    uniform float4 uMat[2], uniform float4 uAmb[2],\n");
    if (point)
        emit(s, "    uniform float4 uPoint,\n");
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
    emit(s, "    vPosition = float4(uProj[1].z * wc + uProj[1].w * xc, uProj[2].x * wc + uProj[2].y * yc,\n");
    emit(s, "                       uProj[2].z * wc + zc * uProj[2].w, wc);\n");
    if (point)
        emit(s, "    vPosition.xy = vPosition.xy + pointCorner * uPoint.xy * wc;\n");
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
            char r0[32], r1[32], r2[32];
            if (tg->palette == 1u && key->has_mtxidx) {
                /* Matrix index per vertex, in position matrix memory. */
                snprintf(r0, sizeof(r0), "uPos[m]");
                snprintf(r1, sizeof(r1), "uPos[m + 1]");
                snprintf(r2, sizeof(r2), "uPos[m + 2]");
            } else if (tg->palette == 2u && key->has_mtxidx) {
                /* Matrix index per vertex, in texture matrix memory. */
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
        emit(s, "        vTex%u = t.xy;\n", i);
        if (point && (point_tex_mask & (1u << i)) != 0u)
            emit(s, "        vTex%u = vTex%u + pointTex * uPoint.z;\n", i, i);
        emit(s, "    }\n");
    }
    emit(s, "}\n");
    return !s->overflow;
}

static bool build_bump_vertex_source(
    const GxrBumpVtxKey* bump_key, Source* s)
{
    const GxrVtxKey* key = &bump_key->legacy;
    const struct melee_vita_bump_plan* plan = &bump_key->plan;
    emit(s, "void main(float3 aPos, float aMtx, float3 aNrm,\n");
    emit(s, "    float3 aBinormal, float3 aTangent,\n");
    emit(s, "    float4 aC0, float4 aC1,\n");
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
    emit(s, "    float3 binormal = float3(dot(uNrm[m].xyz, aBinormal), dot(uNrm[m + 1].xyz, aBinormal), dot(uNrm[m + 2].xyz, aBinormal));\n");
    emit(s, "    float3 tangent = float3(dot(uNrm[m].xyz, aTangent), dot(uNrm[m + 1].xyz, aTangent), dot(uNrm[m + 2].xyz, aTangent));\n");
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
        if (i >= key->texgen_count) {
            emit(s, "    vTex%u = float2(0.0, 0.0);\n", i);
            continue;
        }
        const GxrVtxTexGen* tg = &key->tg[i];
        const u32 r = i * 3u;
        u8 bump_source, bump_light;
        emit(s, "    {\n        float3 t = float3(0.0, 0.0, 1.0);\n");
        if (melee_vita_bump_stage(
                plan, (u8) i, GX_TG_BUMP0, GX_TG_TEXCOORD0,
                &bump_source, &bump_light)) {
            emit(s, "        t.xy = vTex%u;\n", bump_source);
            emit(s, "        float3 ldir = uLight[%u].xyz - eye;\n",
                 (u32) bump_light * 5u + 1u);
            emit(s, "        float ll2 = dot(ldir, ldir);\n");
            emit(s, "        if (ll2 > 1e-16) {\n");
            emit(s, "            ldir = ldir * (1.0 / sqrt(ll2));\n");
            emit(s, "            t.x += dot(ldir, tangent);\n");
            emit(s, "            t.y += dot(ldir, binormal);\n");
            emit(s, "        }\n");
            emit(s, "        vTex%u = t.xy;\n    }\n", i);
            continue;
        }
        if (tg->source >= GX_TG_TEX0 && tg->source <= GX_TG_TEX7)
            emit(s, "        t.xy = %s;\n", tex_attr_name(tg->source));
        else if (tg->source == GX_TG_POS) emit(s, "        t = aPos;\n");
        else if (tg->source == GX_TG_NRM) emit(s, "        t = aNrm;\n");
        else if (tg->source == GX_TG_COLOR0) emit(s, "        t.xy = vColor0.xy;\n");
        else if (tg->source == GX_TG_COLOR1) emit(s, "        t.xy = vColor1.xy;\n");
        if (tg->has_matrix) {
            const bool pn =
                tg->source == GX_TG_POS || tg->source == GX_TG_NRM;
            const char* w = tg->source == GX_TG_NRM ? "0.0" : "1.0";
            emit(s, "        float3 src = float3(t.xy, %s);\n",
                 pn ? "t.z" : "1.0");
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

static GxrVtxProgram* find_vertex_program_kind(const GxrVtxKey* key,
                                               bool point,
                                               u8 point_tex_mask)
{
    const u64 nonpoint_hash =
        melee_vita_nonpoint_vertex_program_hash(
            key, sizeof(*key), GXR_VTX_VERSION);
    const u64 hash = point
        ? melee_vita_point_gpu_program_hash(nonpoint_hash, point_tex_mask)
        : nonpoint_hash;
    GxrVtxProgram** bucket = &s_vtx_programs[hash % GXR_PROGRAM_BUCKETS];
    for (GxrVtxProgram** link = bucket; *link != NULL;) {
        GxrVtxProgram* p = *link;
        if (p->hash == hash && p->point == point &&
            p->point_tex_mask == point_tex_mask &&
            memcmp(&p->key, key, sizeof(*key)) == 0) {
            if (p->blocked) {
                if (s_compile_policy.enabled) {
                    *link = p->next;
                    free(p);
                    break;
                }
                return p;
            }
            ++s_stats.ram_hits;
            return p;
        }
        link = &p->next;
    }
    GxrVtxProgram* p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    p->hash = hash;
    p->key = *key;
    p->point = point;
    p->point_tex_mask = point_tex_mask;
    p->next = *bucket;
    *bucket = p;
    p->failed = true;

    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    p->program = load_cached(hash, true);
    if (p->program != NULL) {
        ++s_stats.cache_loaded;
    } else {
        const u64 started = sceKernelGetProcessTimeWide();
        buffer[0] = '\0';
        if (!build_vertex_source(key, point, point_tex_mask, &source)) {
            melee_vita_log_info("[GXR] vertex source overflow");
            return p;
        }
        s_stats.source_us += sceKernelGetProcessTimeWide() - started;
        p->program = obtain_program(buffer, true, hash);
        if (p->program == NULL && !s_compile_policy.enabled) {
            p->blocked = true;
            return p;
        }
    }
    SceGxmShaderPatcher* patcher = gxr_patcher();
    const u64 register_started = sceKernelGetProcessTimeWide();
    if (p->program == NULL ||
        sceGxmShaderPatcherRegisterProgram(patcher, p->program, &p->id) < 0) {
        melee_vita_log_info("[GXR] vertex compile failed hash=%016llx", (unsigned long long) hash);
        if (source.length == 0u) {
            buffer[0] = '\0';
            build_vertex_source(key, point, point_tex_mask, &source);
        }
        for (size_t off = 0; off < source.length; off += 600) {
            char chunk[601];
            size_t n = source.length - off < 600 ? source.length - off : 600;
            memcpy(chunk, buffer + off, n);
            chunk[n] = '\0';
            melee_vita_log_info("[GXR] vsrc: %s", chunk);
        }
        return p;
    }
    s_stats.register_us += sceKernelGetProcessTimeWide() - register_started;
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
    const u64 patch_started = sceKernelGetProcessTimeWide();
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, p->id, attributes, count,
                                               &stream, 1, &p->vertex) < 0) {
        melee_vita_log_info("[GXR] vertex program patch failed hash=%016llx", (unsigned long long) hash);
        return p;
    }
    s_stats.vertex_patch_us += sceKernelGetProcessTimeWide() - patch_started;
    p->u_pos = sceGxmProgramFindParameterByName(p->program, "uPos");
    p->u_nrm = sceGxmProgramFindParameterByName(p->program, "uNrm");
    p->u_proj = sceGxmProgramFindParameterByName(p->program, "uProj");
    p->u_tex = sceGxmProgramFindParameterByName(p->program, "uTex");
    p->u_post = sceGxmProgramFindParameterByName(p->program, "uPost");
    p->u_light = sceGxmProgramFindParameterByName(p->program, "uLight");
    p->u_mat = sceGxmProgramFindParameterByName(p->program, "uMat");
    p->u_amb = sceGxmProgramFindParameterByName(p->program, "uAmb");
    p->u_point = sceGxmProgramFindParameterByName(p->program, "uPoint");
    p->failed = false;
    return p;
}

static GxrBumpVtxProgram* find_bump_vertex_program(
    const GxrBumpVtxKey* key)
{
    const u64 hash = melee_vita_gxr_bump_vertex_hash(
        &key->legacy, sizeof(key->legacy), &key->plan);
    GxrBumpVtxProgram** bucket =
        &s_bump_vtx_programs[hash % GXR_PROGRAM_BUCKETS];
    for (GxrBumpVtxProgram** link = bucket; *link != NULL;) {
        GxrBumpVtxProgram* p = *link;
        if (p->hash == hash && memcmp(&p->key, key, sizeof(*key)) == 0) {
            if (p->blocked) {
                if (s_compile_policy.enabled) {
                    *link = p->next;
                    free(p);
                    --s_bump_vtx_program_count;
                    break;
                }
                return p;
            }
            ++s_stats.ram_hits;
            return p;
        }
        link = &p->next;
    }
    if (s_bump_vtx_program_count >= GXR_BUMP_PROGRAM_LIMIT) {
        if (s_bump_stats.program_limit++ < 8u)
            melee_vita_log_info(
                "[GXR/BUMP] reject program-limit hash=%016llx limit=%u",
                (unsigned long long) hash, GXR_BUMP_PROGRAM_LIMIT);
        return NULL;
    }
    GxrBumpVtxProgram* p = calloc(1, sizeof(*p));
    if (p == NULL) return NULL;
    ++s_bump_vtx_program_count;
    p->hash = hash;
    p->key = *key;
    p->next = *bucket;
    *bucket = p;
    p->failed = true;

    static char buffer[GXR_SOURCE_CAPACITY];
    Source source = { buffer, 0, sizeof(buffer), false };
    p->program = load_bump_cached(hash);
    if (p->program != NULL) {
        ++s_bump_stats.cache_loaded;
    } else {
        const u64 started = sceKernelGetProcessTimeWide();
        buffer[0] = '\0';
        if (!build_bump_vertex_source(key, &source)) {
            melee_vita_log_info(
                "[GXR/BUMP] reject source-overflow hash=%016llx",
                (unsigned long long) hash);
            return p;
        }
        s_bump_stats.source_us +=
            sceKernelGetProcessTimeWide() - started;
        p->program = compile_bump_program(buffer, hash);
        if (p->program == NULL && !s_compile_policy.enabled) {
            p->blocked = true;
            return p;
        }
    }
    SceGxmShaderPatcher* patcher = gxr_patcher();
    const u64 register_started = sceKernelGetProcessTimeWide();
    if (p->program == NULL ||
        sceGxmShaderPatcherRegisterProgram(
            patcher, p->program, &p->id) < 0) {
        melee_vita_log_info(
            "[GXR/BUMP] reject register hash=%016llx",
            (unsigned long long) hash);
        return p;
    }
    s_bump_stats.register_us +=
        sceKernelGetProcessTimeWide() - register_started;
    static const struct {
        const char* name;
        u16 offset;
        u8 format;
        u8 count;
        bool required;
    } attrs[] = {
        { "aPos", 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, true },
        { "aMtx", 12, SCE_GXM_ATTRIBUTE_FORMAT_F32, 1, false },
        { "aNrm", 16, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, true },
        { "aC0", 28, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, false },
        { "aC1", 32, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, false },
        { "aT0", 36, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, false },
        { "aT1", 44, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, false },
        { "aT2", 52, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, false },
        { "aT3", 60, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, false },
        { "aBinormal", 68, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, true },
        { "aTangent", 80, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, true },
    };
    SceGxmVertexAttribute attributes[11];
    u32 count = 0;
    for (u32 i = 0; i < 11u; ++i) {
        const SceGxmProgramParameter* param =
            sceGxmProgramFindParameterByName(p->program, attrs[i].name);
        if (param == NULL) {
            if (attrs[i].required) {
                melee_vita_log_info(
                    "[GXR/BUMP] reject missing-attribute %s hash=%016llx",
                    attrs[i].name, (unsigned long long) hash);
                return p;
            }
            continue;
        }
        attributes[count].streamIndex = 0;
        attributes[count].offset = attrs[i].offset;
        attributes[count].format = attrs[i].format;
        attributes[count].componentCount = attrs[i].count;
        attributes[count].regIndex =
            (u16) sceGxmProgramParameterGetResourceIndex(param);
        ++count;
    }
    const SceGxmVertexStream stream = {
        sizeof(GxrGpuBumpVertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT,
    };
    const u64 patch_started = sceKernelGetProcessTimeWide();
    if (sceGxmShaderPatcherCreateVertexProgram(
            patcher, p->id, attributes, count, &stream, 1, &p->vertex) < 0) {
        melee_vita_log_info(
            "[GXR/BUMP] reject patch hash=%016llx",
            (unsigned long long) hash);
        return p;
    }
    s_bump_stats.vertex_patch_us +=
        sceKernelGetProcessTimeWide() - patch_started;
    p->u_pos = sceGxmProgramFindParameterByName(p->program, "uPos");
    p->u_nrm = sceGxmProgramFindParameterByName(p->program, "uNrm");
    p->u_proj = sceGxmProgramFindParameterByName(p->program, "uProj");
    p->u_tex = sceGxmProgramFindParameterByName(p->program, "uTex");
    p->u_post = sceGxmProgramFindParameterByName(p->program, "uPost");
    p->u_light = sceGxmProgramFindParameterByName(p->program, "uLight");
    p->u_mat = sceGxmProgramFindParameterByName(p->program, "uMat");
    p->u_amb = sceGxmProgramFindParameterByName(p->program, "uAmb");
    if (p->u_pos == NULL || p->u_nrm == NULL ||
        p->u_proj == NULL || p->u_light == NULL) {
        melee_vita_log_info(
            "[GXR/BUMP] reject missing-uniform hash=%016llx",
            (unsigned long long) hash);
        return p;
    }
    p->failed = false;
    melee_vita_log_info(
        "[GXR/BUMP] program ready hash=%016llx slot=%u source=%s",
        (unsigned long long) hash, s_bump_vtx_program_count,
        source.length == 0u ? "cache" : "compile");
    return p;
}

/* Same idea as the fragment key cache: skip the persistent hash when the
 * vertex key was seen recently (fighters alternate between a few keys). */
#define GXR_VTX_KEY_CACHE_SIZE 256u
static struct {
    GxrVtxKey key;
    GxrVtxProgram* program;
} s_vtx_key_cache[GXR_VTX_KEY_CACHE_SIZE];

static GxrVtxProgram* find_vertex_program(const GxrVtxKey* key)
{
    const u8* bytes = (const u8*) key;
    u32 hash = 2166136261u;
    for (u32 i = 0; i < sizeof(*key); ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    hash ^= hash >> 13;
    {
        const u32 index = hash & (GXR_VTX_KEY_CACHE_SIZE - 1u);
        GxrVtxProgram* program = s_vtx_key_cache[index].program;
        if (program != NULL &&
            memcmp(&s_vtx_key_cache[index].key, key, sizeof(*key)) == 0)
            return program;
        program = find_vertex_program_kind(key, false, 0u);
        if (program != NULL && !program->failed && !program->blocked) {
            s_vtx_key_cache[index].key = *key;
            s_vtx_key_cache[index].program = program;
        }
        return program;
    }
}

static GxrVtxProgram* find_point_vertex_program(const GxrVtxKey* key,
                                                u8 point_tex_mask)
{
    return find_vertex_program_kind(key, true, point_tex_mask);
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

#ifdef MELEE_VITA_PROFILER
void melee_vita_prof_add(int zone, u64 us);
#define GXR_SUBZONE_BEGIN() u64 gxr_zone_t = sceKernelGetProcessTimeWide()
#define GXR_SUBZONE_END(zone) do { \
        const u64 gxr_zone_now = sceKernelGetProcessTimeWide(); \
        melee_vita_prof_add((zone), gxr_zone_now - gxr_zone_t); \
        gxr_zone_t = gxr_zone_now; \
    } while (0)
#else
#define GXR_SUBZONE_BEGIN() do { } while (0)
#define GXR_SUBZONE_END(zone) do { } while (0)
#endif
/* Profiler sub-zones of gpu_submit (see VPZ_* in gx.c). */
#define GXR_ZONE_VERTEX_PROGRAM 12
#define GXR_ZONE_FRAGMENT_PROGRAM 13
#define GXR_ZONE_RQ_PUSH 15
#define GXR_ZONE_RESOLVE_TEXTURES 16
#define GXR_ZONE_UNIFORM_COPY 17

static bool key_uses_tex_palette(const GxrVtxKey* key)
{
    if (!key->has_mtxidx) return false;
    for (u32 i = 0; i < key->texgen_count && i < GXR_MAX_TEXCOORDS; ++i)
        if (key->tg[i].palette == 2u) return true;
    return false;
}

/* uTex components: one 3-row matrix per texgen, or the full array when a
 * texgen picks one of GX_TEXMTX0..9 per vertex. */
static u32 tex_uniform_comps(const GxrVtxKey* key)
{
    return key_uses_tex_palette(key)
        ? (GXR_TEX_PALETTE_ROW + 30u) * 4u : key->texgen_count * 12u;
}

/* Uniform block layout shared by the synchronous path and queued jobs. */
static u32 gpu_uniform_layout(const GxrVtxKey* vkey, bool point_draw,
                              u32* mtx_comps, u32* tg_comps, u32* light_comps)
{
    const u32 lights_used = (u32) (vkey->chan[0].lights | vkey->chan[1].lights |
                                   vkey->chan[2].lights | vkey->chan[3].lights);
    u32 top = 8u;
    while (top > 0u && (lights_used & (1u << (top - 1u))) == 0u) --top;
    *mtx_comps = vkey->has_mtxidx ? 120u : 12u;
    *tg_comps = vkey->texgen_count * 12u;
    *light_comps = top * 20u;
    return 2u * *mtx_comps + 16u + tex_uniform_comps(vkey) + *tg_comps +
           *light_comps + 16u +
           (point_draw ? 4u : 0u);
}

static void pack_gpu_uniforms(f32* out, const GxrVtxUniforms* u,
                              const GxrPointParams* point, u32 mtx_comps,
                              u32 tex_comps, u32 tg_comps, u32 light_comps)
{
    memcpy(out, u->pos, mtx_comps * sizeof(f32)); out += mtx_comps;
    memcpy(out, u->nrm, mtx_comps * sizeof(f32)); out += mtx_comps;
    memcpy(out, u->proj, 16u * sizeof(f32)); out += 16u;
    memcpy(out, u->tex, tex_comps * sizeof(f32)); out += tex_comps;
    memcpy(out, u->post, tg_comps * sizeof(f32)); out += tg_comps;
    memcpy(out, u->light, light_comps * sizeof(f32)); out += light_comps;
    memcpy(out, u->mat, 8u * sizeof(f32)); out += 8u;
    memcpy(out, u->amb, 8u * sizeof(f32)); out += 8u;
    if (point != NULL) {
        out[0] = point->clip_half_x;
        out[1] = point->clip_half_y;
        out[2] = point->tex_span;
        out[3] = 0.0f;
    }
}

/* packed: uniforms already laid out by pack_gpu_uniforms (queued jobs), or
 * NULL to pack from u. */
static bool gxr_draw_gpu_impl(const GxrDraw* draw, const GxrVtxKey* vkey,
                              const GxrVtxUniforms* u,
                              const GxrPointParams* point,
                              const GxrGpuVertex* vertices,
                              const u16* indices, u32 count, u8 cull,
                              const f32* packed)
{
    static GxrVtxProgram* last_vp;
    GxrVtxProgram* vp;
    GxrProgram* program;
    SceGxmFragmentProgram* fragment;
    RqDraw* d;
    u32 mtx_comps, tg_comps, light_comps = 0, total;
    const bool point_draw = point != NULL;
    const u8 point_tex_mask = point_draw ? point->tex_offset_mask : 0u;
    if (!s_ready || draw == NULL || vertices == NULL || indices == NULL || count == 0)
        return false;
    if (point_draw && vkey->has_mtxidx) return false;
    if (cull == GXR_CULL_ALL) return true;
    GXR_SUBZONE_BEGIN();
    vp = (last_vp != NULL && last_vp->point == point_draw &&
          last_vp->point_tex_mask == point_tex_mask &&
          memcmp(&last_vp->key, vkey, sizeof(*vkey)) == 0)
        ? last_vp
        : point_draw ? find_point_vertex_program(vkey, point_tex_mask)
                     : find_vertex_program(vkey);
    last_vp = vp;
    if (vp == NULL || vp->failed) { ++s_stats.fallback; return false; }
    GXR_SUBZONE_END(GXR_ZONE_VERTEX_PROGRAM);
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) { ++s_stats.fallback; return false; }
    fragment = find_fragment(program, draw);
    if (fragment == NULL) { ++s_stats.fallback; return false; }
    GXR_SUBZONE_END(GXR_ZONE_FRAGMENT_PROGRAM);

    total = gpu_uniform_layout(vkey, point_draw, &mtx_comps, &tg_comps,
                               &light_comps);
    d = melee_vita_rq_push_direct(exec_draw, sizeof(RqDraw) + total * sizeof(f32));
    if (d == NULL) return false;
    memset(d, 0, sizeof(*d));
    GXR_SUBZONE_END(GXR_ZONE_RQ_PUSH);
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
    d->tex_comps = (u16) tex_uniform_comps(vkey);
    d->light_comps = (u16) light_comps;
    d->point_comps = point_draw ? 4u : 0u;
    memcpy(d->registers, draw->registers, sizeof(d->registers));
    memcpy(d->konst, draw->konst, sizeof(d->konst));
    memcpy(d->ind_mtx, draw->ind_mtx, sizeof(d->ind_mtx));
    d->alpha_ref[0] = (f32) draw->key.alpha_ref[0];
    d->alpha_ref[1] = (f32) draw->key.alpha_ref[1];
    d->z_bias = (f32) draw->z_tex_bias;
    memcpy(d->fog_color, draw->fog_color, sizeof(d->fog_color));
    memcpy(d->fog_params, draw->fog_params, sizeof(d->fog_params));
    resolve_textures(draw, d);
    GXR_SUBZONE_END(GXR_ZONE_RESOLVE_TEXTURES);
    if (packed != NULL)
        memcpy(d->uniforms, packed, total * sizeof(f32));
    else
        pack_gpu_uniforms(d->uniforms, u, point, mtx_comps,
                          tex_uniform_comps(vkey), tg_comps, light_comps);
    GXR_SUBZONE_END(GXR_ZONE_UNIFORM_COPY);
    melee_vita_profiler_record_duration(39u, 1u);
    ++s_stats.draws;
    return true;
}

static bool bump_uniform_fits(
    const SceGxmProgramParameter* param, u32 components)
{
    if (param == NULL) return components == 0u;
    return (u32) sceGxmProgramParameterGetArraySize(param) *
               (u32) sceGxmProgramParameterGetComponentCount(param) >=
           components;
}

static bool gxr_draw_bump_now(
    const GxrDraw* draw, const GxrBumpVtxKey* vkey,
    const GxrVtxUniforms* u, const GxrGpuBumpVertex* vertices,
    const u16* indices, u32 count, u8 cull)
{
    static GxrBumpVtxProgram* last_vp;
    GxrBumpVtxProgram* vp;
    GxrProgram* program;
    SceGxmFragmentProgram* fragment;
    RqDraw* d;
    u32 mtx_comps, tg_comps, light_comps, total;
    f32* out;
    if (!s_ready || draw == NULL || vkey == NULL || u == NULL ||
        vertices == NULL || indices == NULL || count == 0u ||
        vkey->plan.mask == 0u) {
        ++s_bump_stats.fallback;
        return false;
    }
    if (cull == GXR_CULL_ALL) return true;
    GXR_SUBZONE_BEGIN();
    vp = (last_vp != NULL &&
          memcmp(&last_vp->key, vkey, sizeof(*vkey)) == 0)
        ? last_vp
        : find_bump_vertex_program(vkey);
    last_vp = vp;
    if (vp == NULL || vp->failed) {
        ++s_bump_stats.fallback;
        return false;
    }
    GXR_SUBZONE_END(GXR_ZONE_VERTEX_PROGRAM);
    program = lookup_program(&draw->key);
    if (program == NULL || program->failed) {
        ++s_bump_stats.fallback;
        return false;
    }
    fragment = find_fragment(program, draw);
    if (fragment == NULL) {
        ++s_bump_stats.fallback;
        return false;
    }
    GXR_SUBZONE_END(GXR_ZONE_FRAGMENT_PROGRAM);

    mtx_comps = vkey->legacy.has_mtxidx ? 120u : 12u;
    tg_comps = vkey->legacy.texgen_count * 12u;
    {
        u32 lights_used = (u32) (
            vkey->legacy.chan[0].lights | vkey->legacy.chan[1].lights |
            vkey->legacy.chan[2].lights | vkey->legacy.chan[3].lights);
        for (u32 i = 0; i < vkey->plan.count; ++i) {
            if ((vkey->plan.mask & (1u << i)) != 0u)
                lights_used |= 1u << (
                    vkey->plan.type[i] - (u32) GX_TG_BUMP0);
        }
        u32 top = 8u;
        while (top > 0u &&
               (lights_used & (1u << (top - 1u))) == 0u) {
            --top;
        }
        light_comps = top * 20u;
    }
    if (!bump_uniform_fits(vp->u_pos, mtx_comps) ||
        !bump_uniform_fits(vp->u_nrm, mtx_comps) ||
        !bump_uniform_fits(vp->u_proj, 16u) ||
        !bump_uniform_fits(vp->u_tex, vp->u_tex ? tg_comps : 0u) ||
        !bump_uniform_fits(vp->u_post, vp->u_post ? tg_comps : 0u) ||
        !bump_uniform_fits(vp->u_light, light_comps) ||
        !bump_uniform_fits(vp->u_mat, vp->u_mat ? 8u : 0u) ||
        !bump_uniform_fits(vp->u_amb, vp->u_amb ? 8u : 0u)) {
        if (s_bump_stats.uniform_reject++ < 8u)
            melee_vita_log_info(
                "[GXR/BUMP] reject uniform-capacity hash=%016llx "
                "mtx=%u tex=%u light=%u",
                (unsigned long long) vp->hash,
                mtx_comps, tg_comps, light_comps);
        ++s_bump_stats.fallback;
        return false;
    }

    total =
        2u * mtx_comps + 16u + 2u * tg_comps + light_comps + 16u;
    d = melee_vita_rq_push_direct(
        exec_draw, sizeof(RqDraw) + total * sizeof(f32));
    if (d == NULL) {
        ++s_bump_stats.fallback;
        return false;
    }
    memset(d, 0, sizeof(*d));
    GXR_SUBZONE_END(GXR_ZONE_RQ_PUSH);
    d->vertex = vp->vertex;
    d->fragment = fragment;
    d->program = program;
    d->bump_vp = vp;
    d->bump_path = 1u;
    d->vertices = vertices;
    d->indices = indices;
    d->count = count;
    d->depth_function = draw_depth_function(draw);
    d->depth_write = draw->depth_write
        ? SCE_GXM_DEPTH_WRITE_ENABLED
        : SCE_GXM_DEPTH_WRITE_DISABLED;
    d->cull = cull_mode_for(cull);
    d->primitive = gxm_primitive(draw->primitive);
    d->line_width =
        draw->line_width < 1.0f ? 1u : (u32) (draw->line_width + 0.5f);
    d->mtx_comps = (u16) mtx_comps;
    d->tg_comps = (u16) tg_comps;
    d->tex_comps = (u16) tg_comps;
    d->light_comps = (u16) light_comps;
    memcpy(d->registers, draw->registers, sizeof(d->registers));
    memcpy(d->konst, draw->konst, sizeof(d->konst));
    memcpy(d->ind_mtx, draw->ind_mtx, sizeof(d->ind_mtx));
    d->alpha_ref[0] = (f32) draw->key.alpha_ref[0];
    d->alpha_ref[1] = (f32) draw->key.alpha_ref[1];
    d->z_bias = (f32) draw->z_tex_bias;
    memcpy(d->fog_color, draw->fog_color, sizeof(d->fog_color));
    memcpy(d->fog_params, draw->fog_params, sizeof(d->fog_params));
    resolve_textures(draw, d);
    GXR_SUBZONE_END(GXR_ZONE_RESOLVE_TEXTURES);
    out = d->uniforms;
    memcpy(out, u->pos, mtx_comps * sizeof(f32));
    out += mtx_comps;
    memcpy(out, u->nrm, mtx_comps * sizeof(f32));
    out += mtx_comps;
    memcpy(out, u->proj, 16u * sizeof(f32));
    out += 16u;
    memcpy(out, u->tex, tg_comps * sizeof(f32));
    out += tg_comps;
    memcpy(out, u->post, tg_comps * sizeof(f32));
    out += tg_comps;
    memcpy(out, u->light, light_comps * sizeof(f32));
    out += light_comps;
    memcpy(out, u->mat, 8u * sizeof(f32));
    out += 8u;
    memcpy(out, u->amb, 8u * sizeof(f32));
    GXR_SUBZONE_END(GXR_ZONE_UNIFORM_COPY);
    melee_vita_profiler_record_duration(50u, 1u);
    ++s_bump_stats.draws;
    return true;
}

/* ---- queued submission (see gx_submit.h) --------------------------------
 * A draw is queued only when its shader combination (fragment key, blend
 * state, vertex key) already succeeded once; otherwise it runs synchronously
 * after draining the queue, so a failed shader still reports false and the
 * caller's CPU fallback keeps working. */
#define GXR_KNOWN_SIZE 8192u
static u64 s_known[GXR_KNOWN_SIZE];

static inline void known_mix(u32* a, u32* b, const u8* bytes, u32 length)
{
    u32 i = 0;
    for (; i + 4u <= length; i += 4u) {
        u32 word;
        memcpy(&word, bytes + i, sizeof(word));
        *a = (*a ^ word) * 16777619u;
        *b = (*b + word) * 0x85ebca6bu;
        *b ^= *b >> 13;
    }
    for (; i < length; ++i) {
        *a = (*a ^ bytes[i]) * 16777619u;
        *b = (*b + bytes[i]) * 0x85ebca6bu;
    }
}

static u64 known_hash(const GxrDraw* draw, const void* vkey, u32 vkey_size,
                      u32 salt)
{
    u32 a = 2166136261u ^ salt;
    u32 b = 0x9747b28cu + salt;
    known_mix(&a, &b, (const u8*) &draw->key,
              shader_key_used_length(&draw->key));
    /* blend_mode, blend_src, blend_dst, logic_op, color_update,
     * alpha_update: everything make_blend reads. */
    known_mix(&a, &b, &draw->blend_mode, 6u);
    if (vkey != NULL) known_mix(&a, &b, (const u8*) vkey, vkey_size);
    return ((u64) a << 32 | b) | 1u;
}

static bool known_has(u64 hash)
{
    for (u32 probe = 0; probe < 8u; ++probe) {
        const u64 value = __atomic_load_n(
            &s_known[((u32) hash + probe) & (GXR_KNOWN_SIZE - 1u)],
            __ATOMIC_RELAXED);
        if (value == hash) return true;
        if (value == 0u) return false;
    }
    return false;
}

static void known_add(u64 hash)
{
    for (u32 probe = 0; probe < 8u; ++probe) {
        u64* slot = &s_known[((u32) hash + probe) & (GXR_KNOWN_SIZE - 1u)];
        const u64 value = __atomic_load_n(slot, __ATOMIC_RELAXED);
        if (value == hash) return;
        if (value == 0u) {
            __atomic_store_n(slot, hash, __ATOMIC_RELAXED);
            return;
        }
    }
}

/* Queued draws carry only the parts of GxrDraw that are in use: the used
 * prefix of the shader key (fill_gxr_draw zeroes the rest), the fixed
 * state block, and the texture sources of valid maps.  Copying whole
 * GxrDraw records (~1.1 KB) cost as much as the work being moved. */
#define GXR_DRAW_FIXED_OFFSET offsetof(GxrDraw, blend_mode)
#define GXR_DRAW_FIXED_SIZE \
    (offsetof(GxrDraw, textures) - offsetof(GxrDraw, blend_mode))

typedef struct GxrPackedDraw {
    u16 key_length;
    u8 texture_mask;
    u8 texture_count;
    u8 fixed[GXR_DRAW_FIXED_SIZE];
} GxrPackedDraw;

static u32 packed_draw_extra(const GxrDraw* draw, u8* mask, u8* count)
{
    u8 m = 0, n = 0;
    for (u32 map = 0; map < GXR_MAX_TEXMAPS; ++map)
        if (draw->texture_valid[map]) { m |= (u8) (1u << map); ++n; }
    *mask = m;
    *count = n;
    return ((shader_key_used_length(&draw->key) + 3u) & ~3u) +
           n * (u32) sizeof(MeleeVitaTextureSource);
}

/* Writes the variable part after the header; returns bytes written. */
static u32 pack_draw(GxrPackedDraw* header, u8* out, const GxrDraw* draw)
{
    u8* start = out;
    const u32 key_length = shader_key_used_length(&draw->key);
    header->key_length = (u16) key_length;
    memcpy(header->fixed, (const u8*) draw + GXR_DRAW_FIXED_OFFSET,
           GXR_DRAW_FIXED_SIZE);
    memcpy(out, &draw->key, key_length);
    out += (key_length + 3u) & ~3u;
    for (u32 map = 0; map < GXR_MAX_TEXMAPS; ++map) {
        if (!(header->texture_mask & (1u << map))) continue;
        memcpy(out, &draw->textures[map], sizeof(MeleeVitaTextureSource));
        out += sizeof(MeleeVitaTextureSource);
    }
    return (u32) (out - start);
}

static const u8* unpack_draw(GxrDraw* draw, const GxrPackedDraw* header,
                             const u8* in)
{
    memset(draw, 0, sizeof(*draw));
    memcpy(&draw->key, in, header->key_length);
    in += (header->key_length + 3u) & ~3u;
    memcpy((u8*) draw + GXR_DRAW_FIXED_OFFSET, header->fixed,
           GXR_DRAW_FIXED_SIZE);
    for (u32 map = 0; map < GXR_MAX_TEXMAPS; ++map) {
        if (!(header->texture_mask & (1u << map))) continue;
        memcpy(&draw->textures[map], in, sizeof(MeleeVitaTextureSource));
        draw->texture_valid[map] = 1u;
        in += sizeof(MeleeVitaTextureSource);
    }
    return in;
}

typedef struct GxrGpuJob {
    GxrVtxKey vkey;
    GxrPointParams point;
    const GxrGpuVertex* vertices;
    const u16* indices;
    u32 count;
    u8 has_point;
    u8 cull;
    u16 reserved;
    GxrPackedDraw draw;
    u8 data[]; /* packed draw, then uniforms (4-byte aligned) */
} GxrGpuJob;

static void run_gpu_job(void* payload)
{
    const GxrGpuJob* job = payload;
    GxrDraw draw;
    const u8* uniforms = unpack_draw(&draw, &job->draw, job->data);
    (void) gxr_draw_gpu_impl(&draw, &job->vkey, NULL,
                             job->has_point ? &job->point : NULL,
                             job->vertices, job->indices, job->count,
                             job->cull, (const f32*) uniforms);
}

static bool submit_gpu(const GxrDraw* draw, const GxrVtxKey* vkey,
                       const GxrVtxUniforms* u, const GxrPointParams* point,
                       const GxrGpuVertex* vertices, const u16* indices,
                       u32 count, u8 cull)
{
    u64 hash;
    bool ok;
    if (!gxs_active())
        return gxr_draw_gpu_impl(draw, vkey, u, point, vertices, indices,
                                 count, cull, NULL);
    if (!s_ready || draw == NULL || vkey == NULL || u == NULL ||
        vertices == NULL || indices == NULL || count == 0)
        return false;
    if (point != NULL && vkey->has_mtxidx) return false;
    if (cull == GXR_CULL_ALL) return true;
    hash = known_hash(draw, vkey, sizeof(*vkey),
                      point != NULL ? 0x100u | point->tex_offset_mask : 0u);
    if (known_has(hash)) {
        u32 mtx_comps, tg_comps, light_comps;
        const u32 total = gpu_uniform_layout(vkey, point != NULL, &mtx_comps,
                                             &tg_comps, &light_comps);
        u8 mask, textures;
        const u32 extra = packed_draw_extra(draw, &mask, &textures);
        GxrGpuJob* job = gxs_alloc(
            run_gpu_job, sizeof(GxrGpuJob) + extra + total * sizeof(f32));
        if (job != NULL) {
            job->draw.texture_mask = mask;
            job->draw.texture_count = textures;
            pack_draw(&job->draw, job->data, draw);
            job->vkey = *vkey;
            if (point != NULL) job->point = *point;
            job->has_point = point != NULL;
            job->cull = cull;
            job->vertices = vertices;
            job->indices = indices;
            job->count = count;
            pack_gpu_uniforms((f32*) (job->data + extra), u, point, mtx_comps,
                              tex_uniform_comps(vkey), tg_comps, light_comps);
            return true;
        }
    }
    melee_vita_profiler_record_duration(60u, 1u);
    gxs_drain();
    ok = gxr_draw_gpu_impl(draw, vkey, u, point, vertices, indices, count,
                           cull, NULL);
    if (ok) known_add(hash);
    return ok;
}

bool gxr_draw_gpu(const GxrDraw* draw, const GxrVtxKey* vkey,
                  const GxrVtxUniforms* u, const GxrGpuVertex* vertices,
                  const u16* indices, u32 count, u8 cull)
{
    return submit_gpu(draw, vkey, u, NULL, vertices, indices, count, cull);
}

bool gxr_draw_gpu_points(const GxrDraw* draw, const GxrVtxKey* vkey,
                         const GxrVtxUniforms* u,
                         const GxrPointParams* point,
                         const GxrGpuVertex* vertices, const u16* indices,
                         u32 count)
{
    if (point == NULL) return false;
    return submit_gpu(draw, vkey, u, point, vertices, indices, count,
                      GXR_CULL_NONE);
}

typedef struct GxrCpuJob {
    GxrVertex* vertices;
    const u16* indices;
    u32 count;
    GxrPackedDraw draw;
    u8 data[];
} GxrCpuJob;

static void run_cpu_job(void* payload)
{
    GxrCpuJob* job = payload;
    GxrDraw draw;
    (void) unpack_draw(&draw, &job->draw, job->data);
    (void) gxr_draw_now(&draw, job->vertices, job->indices, job->count);
}

bool gxr_draw(const GxrDraw* draw, GxrVertex* vertices, const u16* indices,
              u32 count)
{
    u64 hash;
    bool ok;
    if (!gxs_active()) return gxr_draw_now(draw, vertices, indices, count);
    if (!s_ready || draw == NULL || vertices == NULL || count == 0)
        return false;
    hash = known_hash(draw, NULL, 0u, 0x200u);
    if (known_has(hash)) {
        u8 mask, textures;
        const u32 extra = packed_draw_extra(draw, &mask, &textures);
        GxrCpuJob* job = gxs_alloc(run_cpu_job, sizeof(GxrCpuJob) + extra);
        if (job != NULL) {
            job->draw.texture_mask = mask;
            job->draw.texture_count = textures;
            pack_draw(&job->draw, job->data, draw);
            job->vertices = vertices;
            job->indices = indices;
            job->count = count;
            return true;
        }
    }
    melee_vita_profiler_record_duration(60u, 1u);
    gxs_drain();
    ok = gxr_draw_now(draw, vertices, indices, count);
    if (ok) known_add(hash);
    return ok;
}

bool gxr_draw_bump_gpu(
    const GxrDraw* draw, const GxrBumpVtxKey* vkey,
    const GxrVtxUniforms* u, const GxrGpuBumpVertex* vertices,
    const u16* indices, u32 count, u8 cull)
{
    /* Bump draws are rare and can reject late (uniform capacity), so they
     * always run synchronously. */
    gxs_drain();
    return gxr_draw_bump_now(draw, vkey, u, vertices, indices, count, cull);
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
        const f32 half_width =
            (f32) melee_vita_gxm_render_width() * 0.5f;
        const f32 half_height =
            (f32) melee_vita_gxm_render_height() * 0.5f;
        sceGxmSetViewport(
            context, half_width, half_width, half_height, -half_height,
            0.0f, 1.0f);
        s_rt_state.fragment = NULL;
        s_rt_state.vertex = NULL;
        s_rt_state.cull = 0xff;
        s_rt_state.polygon_mode = UINT32_MAX;
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
    {
        const SceGxmPolygonMode mode =
            d->primitive == SCE_GXM_PRIMITIVE_LINES
                ? SCE_GXM_POLYGON_MODE_LINE
                : d->primitive == SCE_GXM_PRIMITIVE_POINTS
                ? SCE_GXM_POLYGON_MODE_POINT
                : SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
        if (s_rt_state.polygon_mode != (u32) mode) {
            sceGxmSetFrontPolygonMode(context, mode);
            sceGxmSetBackPolygonMode(context, mode);
            s_rt_state.polygon_mode = (u32) mode;
        }
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
        const GxrBumpVtxProgram* bump_vp = d->bump_vp;
        const SceGxmProgramParameter* u_pos =
            d->bump_path ? bump_vp->u_pos : vp->u_pos;
        const SceGxmProgramParameter* u_nrm =
            d->bump_path ? bump_vp->u_nrm : vp->u_nrm;
        const SceGxmProgramParameter* u_proj =
            d->bump_path ? bump_vp->u_proj : vp->u_proj;
        const SceGxmProgramParameter* u_tex =
            d->bump_path ? bump_vp->u_tex : vp->u_tex;
        const SceGxmProgramParameter* u_post =
            d->bump_path ? bump_vp->u_post : vp->u_post;
        const SceGxmProgramParameter* u_light =
            d->bump_path ? bump_vp->u_light : vp->u_light;
        const SceGxmProgramParameter* u_mat =
            d->bump_path ? bump_vp->u_mat : vp->u_mat;
        const SceGxmProgramParameter* u_amb =
            d->bump_path ? bump_vp->u_amb : vp->u_amb;
        const SceGxmProgramParameter* u_point =
            d->bump_path ? NULL : vp->u_point;
        const f32* in = d->uniforms;
        void* vbuf = NULL;
        sceGxmReserveVertexDefaultUniformBuffer(context, &vbuf);
        if (vbuf != NULL) {
            if (u_pos) set_uniform(vbuf, u_pos, d->mtx_comps, in, "u_pos");
            in += d->mtx_comps;
            if (u_nrm) set_uniform(vbuf, u_nrm, d->mtx_comps, in, "u_nrm");
            in += d->mtx_comps;
            if (u_proj) set_uniform(vbuf, u_proj, 16, in, "u_proj");
            in += 16u;
            if (u_tex && d->tex_comps)
                set_uniform(vbuf, u_tex, d->tex_comps, in, "tex");
            in += d->tex_comps;
            if (u_post && d->tg_comps)
                set_uniform(vbuf, u_post, d->tg_comps, in, "post");
            in += d->tg_comps;
            if (u_light && d->light_comps)
                set_uniform(vbuf, u_light, d->light_comps, in, "light");
            in += d->light_comps;
            if (u_mat) set_uniform(vbuf, u_mat, 8, in, "u_mat");
            in += 8u;
            if (u_amb) set_uniform(vbuf, u_amb, 8, in, "u_amb");
            in += 8u;
            if (u_point && d->point_comps)
                set_uniform(vbuf, u_point, d->point_comps, in, "u_point");
        }
    }
    {
        bool any = false;
        for (u32 i = 0; i < 4u; ++i)
            if (program->registers[i] != NULL || program->konst[i] != NULL) any = true;
        if (program->alpha_ref[0] != NULL ||
            program->alpha_ref[1] != NULL) any = true;
        if (program->z_bias != NULL) any = true;
        if (program->fog_color != NULL || program->fog_params != NULL) any = true;
        if (program->ind_mtx[0] != NULL || program->ind_mtx[1] != NULL) any = true;
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
                for (u32 i = 0; i < 2u; ++i)
                    if (program->alpha_ref[i] != NULL)
                        sceGxmSetUniformDataF(
                            fbuf, program->alpha_ref[i], 0, 1,
                            &d->alpha_ref[i]);
                if (program->z_bias != NULL)
                    sceGxmSetUniformDataF(
                        fbuf, program->z_bias, 0, 1, &d->z_bias);
                if (program->fog_color != NULL)
                    sceGxmSetUniformDataF(
                        fbuf, program->fog_color, 0, 4, d->fog_color);
                if (program->fog_params != NULL)
                    sceGxmSetUniformDataF(
                        fbuf, program->fog_params, 0, 4, d->fog_params);
                for (u32 i = 0; i < 2u; ++i)
                    if (program->ind_mtx[i] != NULL)
                        sceGxmSetUniformDataF(
                            fbuf, program->ind_mtx[i], 0, 4, d->ind_mtx[i]);
            }
        }
    }

    sceGxmSetVertexStream(context, 0, d->vertices);
    if (d->indices != NULL) {
        sceGxmDraw(
            context, d->primitive, SCE_GXM_INDEX_FORMAT_U16, d->indices,
            d->count);
    } else {
        for (u32 first = 0; first < d->count; first += GXR_MAX_INDEX) {
            const u32 n = d->count - first < GXR_MAX_INDEX ? d->count - first : GXR_MAX_INDEX;
            if (first != 0)
                sceGxmSetVertexStream(context, 0, (const GxrVertex*) d->vertices + first);
            sceGxmDraw(
                context, d->primitive, SCE_GXM_INDEX_FORMAT_U16, s_indices,
                n);
        }
    }
}

bool gxr_copy_depth(const SceGxmTexture* texture,
                    const GxrVertex* vertices, const u16* indices)
{
    SceGxmContext* context;
    if (!s_ready || s_depth_copy_fragment == NULL || texture == NULL ||
        vertices == NULL || indices == NULL)
        return false;
    context = vita2d_get_context();
    sceGxmSetVertexProgram(context, s_vertex_program);
    sceGxmSetFragmentProgram(context, s_depth_copy_fragment);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFragmentTexture(context, 0, texture);
    sceGxmSetVertexStream(context, 0, vertices);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
               SCE_GXM_INDEX_FORMAT_U16, indices, 6);
    s_rt_state.valid = false;
    return true;
}

bool gxr_copy_color(const SceGxmTexture* texture,
                    const GxrVertex* vertices, const u16* indices)
{
    SceGxmContext* context;
    if (!s_ready || s_color_copy_fragment == NULL || texture == NULL ||
        vertices == NULL || indices == NULL)
        return false;
    context = vita2d_get_context();
    sceGxmSetVertexProgram(context, s_vertex_program);
    sceGxmSetFragmentProgram(context, s_color_copy_fragment);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFragmentTexture(context, 0, texture);
    sceGxmSetVertexStream(context, 0, vertices);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
               SCE_GXM_INDEX_FORMAT_U16, indices, 6);
    s_rt_state.valid = false;
    return true;
}

void gxr_log_stats(void)
{
    const u32 blocked =
        gxr_shader_compile_policy_blocked(&s_compile_policy);
    melee_vita_log_info(
        "[GXR] draws=%u ram_hits=%u warm_data=%u warm_packaged=%u "
        "disk_hits=%u compile_attempts=%u compile_success=%u blocked=%u "
        "failed=%u fallback=%u compile_ms=%llu cache_io_ms=%llu "
        "source_ms=%llu register_ms=%llu vertex_patch_ms=%llu sealed=%u",
        s_stats.draws, s_stats.ram_hits, s_stats.warm_writable_hits,
        s_stats.warm_packaged_hits, s_stats.disk_hits,
        s_stats.compile_attempts, s_stats.compile_success, blocked,
        s_stats.compile_failed + s_bump_stats.compile_failed,
        s_stats.fallback + s_bump_stats.fallback,
        (unsigned long long)
            ((s_stats.compile_us + s_bump_stats.compile_us) / 1000u),
        (unsigned long long) (s_stats.cache_io_us / 1000u),
        (unsigned long long) (s_stats.source_us / 1000u),
        (unsigned long long) (s_stats.register_us / 1000u),
        (unsigned long long) (s_stats.vertex_patch_us / 1000u),
        s_compile_policy.enabled ? 0u : 1u);
#ifdef MELEE_VITA_RENDER_TRACE
    melee_vita_log_info(
        "[GXR/BUMP/STATS] draws=%u fallback=%u compiled=%u cached=%u compile_failed=%u program_limit=%u uniform_reject=%u",
        s_bump_stats.draws, s_bump_stats.fallback,
        s_bump_stats.compiled, s_bump_stats.cache_loaded,
        s_bump_stats.compile_failed, s_bump_stats.program_limit,
        s_bump_stats.uniform_reject);
    s_bump_stats.draws = 0;
    s_bump_stats.fallback = 0;
#endif
    s_stats.draws = 0;
    s_stats.fallback = 0;
}
