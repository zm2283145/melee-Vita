/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * OpenGL frame sink for the shared GX compatibility layer (gx.c) on PS5:
 * the PS5 counterpart of platforms/vita/game/gxm_game.c, built on the
 * ps5-opengl (Mesa over AGC) EGL/OpenGL 4.6 stack.
 *
 * Coordinate model.  gx.c works in the Vita's 960x544 screen space.  Here
 * that space is rendered into an offscreen "EFB" framebuffer at a multiple
 * of it (MELEE_PS5_EFB_SCALE) and scaled to the 1920x1080 display at
 * present.  Every framebuffer and texture is stored top row first (row 0 is
 * the top of the picture, as the GameCube, the Vita and every decoded
 * texture have it), so copies are plain blits; only the final presentation
 * flips into OpenGL's bottom-up window.
 *
 * The game thread records a frame as a command list (as on Vita) and the
 * list runs on the same thread at present time, which keeps every GL call
 * on the thread that owns the context.
 */
#include "gl_game.h"
#include "gxm_game.h"
#include "copy_texture_lifetime.h"
#include "heap.h"
#include "../vita/texture_decoder.h"
#include "gx_render.h"
#include "../vita/vita_log.h"
#include "ps5_log.h"

#include <dolphin/gx/GXEnum.h>
#include <psp2/kernel/processmgr.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef MELEE_PS5_EFB_SCALE
#define MELEE_PS5_EFB_SCALE 2u
#endif

#define VITA_W MELEE_VITA_DISPLAY_WIDTH
#define VITA_H MELEE_VITA_DISPLAY_HEIGHT

struct vita2d_texture {
    GLuint tex;
    GLuint fbo;
    GLuint depth;
    u32 width;
    u32 height;
};

static int s_initialized;
static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLint s_window_width = 1920, s_window_height = 1080;

/* The main EFB: colour texture + sampleable depth texture. */
static vita2d_texture s_efb;
static u32 s_efb_scale = MELEE_PS5_EFB_SCALE;

/* Render-target state seen by draws (Vita-space size of the bound target,
 * and the pixel scale from Vita space to it). */
static u32 s_render_width = VITA_W;
static u32 s_render_height = VITA_H;
static u32 s_target_pixel_width = VITA_W * MELEE_PS5_EFB_SCALE;
static u32 s_target_pixel_height = VITA_H * MELEE_PS5_EFB_SCALE;

static u32 s_texture_content_generation = 1;
static u32 s_frame_counter;
static u32 s_texture_uploads;
static u32 s_texture_failures;
u32 g_melee_vita_gxm_state_epoch;

#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
int g_melee_vita_menu_resolution_option = MELEE_VITA_RESOLUTION_NATIVE;
int g_melee_vita_gameplay_resolution_option = MELEE_VITA_RESOLUTION_NATIVE;
#endif

/* ===================================================================
 * GL helpers
 * =================================================================== */

static GLuint compile_shader(GLenum type, const char* source)
{
    GLint ok = GL_FALSE;
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        melee_ps5_log("[GL] shader compile failed: %.*s", (int) length, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint melee_ps5_gl_program(const char* vertex_source, const char* fragment_source)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    GLuint program;
    GLint ok = GL_FALSE;
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        melee_ps5_log("[GL] program link failed: %.*s", (int) length, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void check_gl(const char* where)
{
    static u32 logged;
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR && logged++ < 32u)
        melee_ps5_log("[GL] error 0x%04x at %s", (unsigned) error, where);
}

/* ===================================================================
 * Textures
 * =================================================================== */

static vita2d_texture* texture_create(u32 width, u32 height, bool target, bool depth)
{
    vita2d_texture* t = calloc(1, sizeof(*t));
    if (t == NULL) return NULL;
    t->width = width;
    t->height = height;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei) width, (GLsizei) height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (target) {
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        if (depth) {
            glGenTextures(1, &t->depth);
            glBindTexture(GL_TEXTURE_2D, t->depth);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, (GLsizei) width,
                           (GLsizei) height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                                   t->depth, 0);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            melee_ps5_log("[GL] framebuffer %ux%u incomplete", width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, s_efb.fbo);
    }
    check_gl("texture_create");
    return t;
}

static void texture_free(vita2d_texture* t)
{
    if (t == NULL) return;
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->depth) glDeleteTextures(1, &t->depth);
    if (t->tex) glDeleteTextures(1, &t->tex);
    free(t);
}

unsigned melee_ps5_texture_name(const vita2d_texture* texture)
{
    return texture != NULL ? texture->tex : 0u;
}

unsigned melee_ps5_texture_depth_name(const vita2d_texture* texture)
{
    return texture != NULL ? texture->depth : 0u;
}

unsigned melee_ps5_texture_fbo(const vita2d_texture* texture)
{
    return texture != NULL ? texture->fbo : 0u;
}

unsigned melee_ps5_texture_width(const vita2d_texture* texture)
{
    return texture != NULL ? texture->width : 0u;
}

unsigned melee_ps5_texture_height(const vita2d_texture* texture)
{
    return texture != NULL ? texture->height : 0u;
}

vita2d_texture* melee_ps5_texture_create_white(void)
{
    static const u32 white = 0xffffffffu;
    vita2d_texture* t = texture_create(1, 1, false, false);
    if (t != NULL) {
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    }
    return t;
}

u32 melee_ps5_frame_counter(void) { return s_frame_counter; }

static GLint gl_wrap(u32 mode)
{
    if (mode == 1u) return GL_REPEAT;
    /* Mirrored maps: the GX shaders fold the coordinate into [0, 1] (as on
     * Vita, whose GXM rejected mirror addressing), so the sampler clamps. */
    if (mode == 2u) return gxr_available() ? GL_CLAMP_TO_EDGE : GL_MIRRORED_REPEAT;
    return GL_CLAMP_TO_EDGE;
}

typedef struct TextureCacheEntry {
    MeleeVitaTextureSource source;
    vita2d_texture* texture;
    u32 content_generation;
    u32 sample_hash;
    u32 light_hash;
    u32 last_used_frame;
    u32 hash_frame;
    struct TextureCacheEntry* next;
    struct TextureCacheEntry* hash_next;
} TextureCacheEntry;

static TextureCacheEntry* s_textures;
#define TEXTURE_HASH_SIZE 4096u
static TextureCacheEntry* s_texture_hash[TEXTURE_HASH_SIZE];

#define COPY_TEXTURES 16u
static struct {
    MeleeVitaCopyTextureBinding binding;
    vita2d_texture* texture;
    u32 width, height, frame;
} s_copy_textures[COPY_TEXTURES];

/* Textures may be referenced by the frame being recorded: free them after
 * that frame has been drawn. */
#define GRAVEYARD 64u
static struct { vita2d_texture* texture; u32 frame; } s_graveyard[GRAVEYARD];

static void retire_texture(vita2d_texture* texture)
{
    if (texture == NULL) return;
    for (u32 i = 0; i < GRAVEYARD; ++i)
        if (s_graveyard[i].texture == NULL) {
            s_graveyard[i].texture = texture;
            s_graveyard[i].frame = s_frame_counter;
            return;
        }
    /* Full: the oldest entries are at least one frame old by now. */
    texture_free(texture);
}

static void collect_graveyard(bool all)
{
    for (u32 i = 0; i < GRAVEYARD; ++i)
        if (s_graveyard[i].texture != NULL &&
            (all || s_frame_counter - s_graveyard[i].frame >= 2u)) {
            texture_free(s_graveyard[i].texture);
            s_graveyard[i].texture = NULL;
        }
}

static inline u32 texture_hash_slot(const MeleeVitaTextureSource* source)
{
    uint64_t key = (uint64_t) (uintptr_t) source->data ^
                   ((uint64_t) (uintptr_t) source->palette * 0x9e3779b97f4a7c15ull) ^
                   ((uint64_t) source->format << 27);
    key ^= key >> 31;
    key *= 0x7feb352dull;
    key ^= key >> 15;
    return (u32) key & (TEXTURE_HASH_SIZE - 1u);
}

static void texture_hash_remove(TextureCacheEntry* entry)
{
    TextureCacheEntry** link = &s_texture_hash[texture_hash_slot(&entry->source)];
    while (*link != NULL) {
        if (*link == entry) {
            *link = entry->hash_next;
            entry->hash_next = NULL;
            return;
        }
        link = &(*link)->hash_next;
    }
}

static u32 texture_sample_hash_n(const MeleeVitaTextureSource* source, u32 samples)
{
    const u8* data = source->data;
    u32 hash = 2166136261u;
    const u32 bytes = (u32) source->width * source->height / 2u;
    const u32 step = bytes > samples ? bytes / samples : 1u;
    if (data == NULL) return 0;
    for (u32 i = 0; i < bytes; i += step) { hash ^= data[i]; hash *= 16777619u; }
    if (source->palette != NULL) {
        const u8* palette = source->palette;
        for (u32 i = 0; i < 32u; ++i) { hash ^= palette[i]; hash *= 16777619u; }
    }
    return hash;
}

static u32 texture_sample_hash(const MeleeVitaTextureSource* source)
{
    return texture_sample_hash_n(source, 256u);
}

static void free_textures(void)
{
    TextureCacheEntry* entry = s_textures;
    while (entry != NULL) {
        TextureCacheEntry* next = entry->next;
        texture_free(entry->texture);
        free(entry);
        entry = next;
    }
    s_textures = NULL;
    memset(s_texture_hash, 0, sizeof(s_texture_hash));
}

static void free_copy_textures(void)
{
    for (u32 i = 0; i < COPY_TEXTURES; ++i) texture_free(s_copy_textures[i].texture);
    memset(s_copy_textures, 0, sizeof(s_copy_textures));
}

void melee_vita_gxm_mark_texture_data_dirty(void)
{
    if (++s_texture_content_generation == 0) s_texture_content_generation = 1;
}

static int same_texture(const MeleeVitaTextureSource* a, const MeleeVitaTextureSource* b)
{
    return a->key == b->key && a->data == b->data && a->width == b->width &&
           a->height == b->height && a->format == b->format && a->wrap_s == b->wrap_s &&
           a->wrap_t == b->wrap_t && a->min_filter == b->min_filter &&
           a->mag_filter == b->mag_filter && a->palette == b->palette &&
           a->palette_format == b->palette_format &&
           a->palette_entries == b->palette_entries && a->chroma_u == b->chroma_u &&
           a->chroma_v == b->chroma_v && a->chroma_width == b->chroma_width &&
           a->chroma_height == b->chroma_height;
}

/* ---- THP movie planes: GX I8 tiles of Y, U, V -> RGBA ---- */

static u8 tiled_i8_sample(const u8* data, u32 width, u32 x, u32 y)
{
    const u32 tiles_per_row = (width + 7u) / 8u;
    const u32 tile = (y / 4u) * tiles_per_row + x / 8u;
    return data[tile * 32u + (y & 3u) * 8u + (x & 7u)];
}

static u8 clamp_color(s32 value) { return value < 0 ? 0 : value > 255 ? 255 : (u8) value; }

static u32 yuv_pixel(u8 luma, u8 chroma_u, u8 chroma_v)
{
    const s32 y = 298 * (luma > 16 ? (s32) luma - 16 : 0) + 128;
    const s32 u = (s32) chroma_u - 128;
    const s32 v = (s32) chroma_v - 128;
    const u8 r = clamp_color((y + 409 * v) >> 8);
    const u8 g = clamp_color((y - 100 * u - 208 * v) >> 8);
    const u8 b = clamp_color((y + 516 * u) >> 8);
    return (u32) r | (u32) g << 8 | (u32) b << 16 | 0xff000000u;
}

static int decode_yuv420(const MeleeVitaTextureSource* source, u32* out)
{
    const u8* y_plane = source->data;
    const u8* u_plane = source->chroma_u;
    const u8* v_plane = source->chroma_v;
    if (y_plane == NULL || u_plane == NULL || v_plane == NULL || source->chroma_width == 0 ||
        source->chroma_height == 0)
        return -1;
    for (u32 y = 0; y < source->height; ++y) {
        u32* row = out + (size_t) y * source->width;
        for (u32 x = 0; x < source->width; ++x) {
            row[x] = yuv_pixel(tiled_i8_sample(y_plane, source->width, x, y),
                               tiled_i8_sample(u_plane, source->chroma_width, x >> 1, y >> 1),
                               tiled_i8_sample(v_plane, source->chroma_width, x >> 1, y >> 1));
        }
    }
    return 0;
}

static int refresh_texture(TextureCacheEntry* entry, const MeleeVitaTextureSource* source)
{
    const size_t count = (size_t) source->width * source->height;
    u32* pixels = malloc(count * sizeof(*pixels));
    int result;
    if (pixels == NULL) return -1;
    if (source->chroma_u != NULL)
        result = decode_yuv420(source, pixels);
    else
        result = melee_vita_decode_texture_raw(
            source->data, source->width, source->height, source->format, source->palette,
            source->palette_format, source->palette_entries, pixels, (u32) count);
    if (result == 0) {
        glBindTexture(GL_TEXTURE_2D, entry->texture->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) source->width,
                        (GLsizei) source->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        check_gl("refresh_texture");
        entry->content_generation = s_texture_content_generation;
    }
    free(pixels);
    return result;
}

static vita2d_texture* get_texture(const MeleeVitaTextureSource* source)
{
    TextureCacheEntry* entry;
    u32 sample;
    if (source == NULL || source->data == NULL || source->width == 0 || source->height == 0)
        return NULL;
    for (u32 c = 0; c < COPY_TEXTURES; ++c)
        if (s_copy_textures[c].binding.key == source->data &&
            s_copy_textures[c].texture != NULL) {
            const u32 generation = melee_vita_heap_allocation_generation(source->data);
            if (melee_vita_copy_texture_binding_matches(&s_copy_textures[c].binding,
                                                        source->data, generation))
                return s_copy_textures[c].texture;
            retire_texture(s_copy_textures[c].texture);
            s_copy_textures[c].texture = NULL;
            melee_vita_copy_texture_bind(&s_copy_textures[c].binding, NULL, 0);
        }
    switch (source->format) {
    case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6:
    case 0x8: case 0x9: case 0xa: case 0xe:
        break;
    default:
        return NULL;
    }
    for (entry = s_texture_hash[texture_hash_slot(source)]; entry != NULL;
         entry = entry->hash_next)
        if (same_texture(&entry->source, source)) break;
    if (entry != NULL) {
        entry->last_used_frame = s_frame_counter;
        if (entry->hash_frame == s_frame_counter &&
            entry->content_generation == s_texture_content_generation)
            return entry->texture;
        entry->hash_frame = s_frame_counter;
        {
            const bool full = source->chroma_u != NULL ||
                              entry->content_generation != s_texture_content_generation ||
                              ((s_frame_counter + ((u32) (uintptr_t) entry >> 4)) & 15u) == 0u;
            const u32 light = texture_sample_hash_n(source, 8u);
            if (!full && light == entry->light_hash) return entry->texture;
            sample = texture_sample_hash(source);
            entry->light_hash = light;
        }
        if (entry->content_generation != s_texture_content_generation ||
            entry->sample_hash != sample) {
            ++s_texture_uploads;
            entry->sample_hash = sample;
            if (refresh_texture(entry, source) != 0) {
                ++s_texture_failures;
                return NULL;
            }
        }
        return entry->texture;
    }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) return NULL;
    entry->source = *source;
    entry->sample_hash = texture_sample_hash(source);
    entry->light_hash = texture_sample_hash_n(source, 8u);
    entry->hash_frame = s_frame_counter;
    entry->last_used_frame = s_frame_counter;
    entry->texture = texture_create(source->width, source->height, false, false);
    ++s_texture_uploads;
    if (entry->texture == NULL || refresh_texture(entry, source) != 0) {
        static u32 logged;
        ++s_texture_failures;
        if (logged++ < 16u)
            melee_vita_log_info("[GL] texture decode failed fmt=%u %ux%u pal=%p",
                                (unsigned) source->format, source->width, source->height,
                                source->palette);
        texture_free(entry->texture);
        free(entry);
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, entry->texture->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    source->min_filter == 0u ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    source->mag_filter == 0u ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(source->wrap_s));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(source->wrap_t));
    entry->next = s_textures;
    s_textures = entry;
    {
        const u32 slot = texture_hash_slot(source);
        entry->hash_next = s_texture_hash[slot];
        s_texture_hash[slot] = entry;
    }
    return entry->texture;
}

vita2d_texture* melee_vita_gxm_texture(const MeleeVitaTextureSource* source)
{
    return get_texture(source);
}

/* ===================================================================
 * Command recording
 * =================================================================== */

#define RQ_ARENA_SIZE (32u * 1024u * 1024u)

typedef struct RqCommand {
    MeleeVitaRqExec exec;
    u32 size;
    u32 pad;
} RqCommand;

typedef struct RqFrame {
    u8* cmds;
    u32 size, capacity;
    u8* arena;
    u32 arena_used;
    u32 clear_color;
} RqFrame;

static RqFrame s_frame;
static u32 s_next_clear_color = 0xff000000u;
static u32 s_arena_overflow;

void* melee_vita_rq_push_direct(MeleeVitaRqExec exec, u32 payload_size)
{
    const u32 aligned = (payload_size + 15u) & ~15u;
    const u32 needed = s_frame.size + (u32) sizeof(RqCommand) + aligned;
    RqCommand* command;
    if (!s_initialized) return NULL;
    if (needed > s_frame.capacity) {
        u32 capacity = s_frame.capacity ? s_frame.capacity : 1u << 20;
        u8* grown;
        while (capacity < needed) capacity *= 2u;
        grown = realloc(s_frame.cmds, capacity);
        if (grown == NULL) return NULL;
        s_frame.cmds = grown;
        s_frame.capacity = capacity;
    }
    command = (RqCommand*) (s_frame.cmds + s_frame.size);
    command->exec = exec;
    command->size = aligned;
    s_frame.size = needed;
    return (u8*) command + sizeof(RqCommand);
}

void* melee_vita_rq_push(MeleeVitaRqExec exec, u32 payload_size)
{
    void* payload = melee_vita_rq_push_direct(exec, payload_size);
    if (payload != NULL) memset(payload, 0, payload_size);
    return payload;
}

void* melee_vita_rq_alloc_gpu(u32 size, u32 align)
{
    u32 offset;
    if (!s_initialized || s_frame.arena == NULL || size == 0) return NULL;
    if (align < 16u) align = 16u;
    offset = (s_frame.arena_used + align - 1u) & ~(align - 1u);
    if (offset + size > RQ_ARENA_SIZE) {
        if (s_arena_overflow++ < 4u)
            melee_vita_log_info("[RQ] per-frame arena full (%u bytes requested)", size);
        return NULL;
    }
    s_frame.arena_used = offset + size;
    return s_frame.arena + offset;
}

void melee_vita_gxm_wait_idle(void) {}
void melee_vita_gxm_begin_frame(void) {}
/* Reasons gathered this frame: shadows and copies mark a match. */
static u32 s_frame_reasons;
void melee_vita_gxm_require_full_resolution(u32 reason) { s_frame_reasons |= reason; }
u32 melee_vita_gxm_render_width(void) { return s_render_width; }
u32 melee_vita_gxm_render_height(void) { return s_render_height; }

#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
/* PS5 meaning of the shared resolution options: EFB scale over 960 x 544.
 * The window stays 1920 x 1080; higher scales supersample into it. */
static u32 ps5_scale_for_option(int option)
{
    static const u32 scales[MELEE_VITA_RESOLUTION_OPTION_COUNT] = { 2u, 3u, 4u, 1u };
    return option >= 0 && option < MELEE_VITA_RESOLUTION_OPTION_COUNT ? scales[option] : 2u;
}

#define PS5_RESOLUTION_CONFIG_PATH "/download0/resolution-settings.bin"
#define PS5_RESOLUTION_CONFIG_MAGIC 0x35505352u /* "RSP5" */

static void resolution_config_load(void)
{
    s32 config[3];
    FILE* file = fopen(PS5_RESOLUTION_CONFIG_PATH, "rb");
    if (file == NULL) return;
    if (fread(config, sizeof(config), 1u, file) == 1u &&
        config[0] == (s32) PS5_RESOLUTION_CONFIG_MAGIC &&
        config[1] >= 0 && config[1] < MELEE_VITA_RESOLUTION_OPTION_COUNT &&
        config[2] >= 0 && config[2] < MELEE_VITA_RESOLUTION_OPTION_COUNT) {
        g_melee_vita_menu_resolution_option = config[1];
        g_melee_vita_gameplay_resolution_option = config[2];
    }
    fclose(file);
}

static void resolution_config_save(void)
{
    const s32 config[3] = { (s32) PS5_RESOLUTION_CONFIG_MAGIC,
                            g_melee_vita_menu_resolution_option,
                            g_melee_vita_gameplay_resolution_option };
    FILE* file = fopen(PS5_RESOLUTION_CONFIG_PATH, "wb");
    if (file == NULL) return;
    fwrite(config, sizeof(config), 1u, file);
    fclose(file);
}

bool melee_vita_gxm_apply_resolution_options(void)
{
    /* Takes effect at the next frame boundary (present). */
    resolution_config_save();
    melee_ps5_log("[GL] resolution options: menu x%u, match x%u",
                  ps5_scale_for_option(g_melee_vita_menu_resolution_option),
                  ps5_scale_for_option(g_melee_vita_gameplay_resolution_option));
    return true;
}
#endif

/* ===================================================================
 * Render state shared with gl_render.c
 * =================================================================== */

/* Binds a render target: the EFB (NULL) or an offscreen texture. */
static void bind_target(vita2d_texture* target)
{
    if (target == NULL) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_efb.fbo);
        s_render_width = VITA_W;
        s_render_height = VITA_H;
        s_target_pixel_width = s_efb.width;
        s_target_pixel_height = s_efb.height;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
        s_render_width = target->width;
        s_render_height = target->height;
        s_target_pixel_width = target->width;
        s_target_pixel_height = target->height;
    }
    glViewport(0, 0, (GLsizei) s_target_pixel_width, (GLsizei) s_target_pixel_height);
    ++g_melee_vita_gxm_state_epoch;
}

f32 melee_ps5_target_pixel_scale(void)
{
    return (f32) s_target_pixel_height / (f32) s_render_height;
}

void melee_ps5_gl_default_depth(void)
{
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_FALSE);
}

/* ===================================================================
 * 2D drawing (vita2d replacement): screen-space Vita pixels
 * =================================================================== */

typedef struct Vertex2D {
    f32 x, y, z;
    f32 u, v;
    u32 color;
} Vertex2D;

static GLuint s_prog2d;
static GLint s_prog2d_textured, s_prog2d_tint, s_prog2d_point;
static GLuint s_vao2d, s_vbo2d;

static const char* s_vs2d =
    "#version 460 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUv;\n"
    "layout(location=2) in vec4 aColor;\n"
    "uniform float uPointSize;\n"
    "out vec2 vUv;\n"
    "out vec4 vColor;\n"
    /* Vita pixels (y down) -> NDC.  Row 0 of every framebuffer is the top,
     * so y=0 maps to NDC -1 (the first row OpenGL rasterises). */
    "void main() {\n"
    "  gl_Position = vec4(aPos.x / 480.0 - 1.0, aPos.y / 272.0 - 1.0, clamp(-aPos.z, 0.0, 1.0), 1.0);\n"
    "  gl_PointSize = uPointSize;\n"
    "  vUv = aUv;\n"
    "  vColor = aColor;\n"
    "}\n";

static const char* s_fs2d =
    "#version 460 core\n"
    "in vec2 vUv;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uTextured;\n"
    "uniform vec4 uTint;\n"
    "layout(location=0) out vec4 oColor;\n"
    "void main() {\n"
    "  oColor = uTextured != 0 ? texture(uTex, vUv) * uTint : vColor;\n"
    "}\n";

static bool init_2d(void)
{
    s_prog2d = melee_ps5_gl_program(s_vs2d, s_fs2d);
    if (s_prog2d == 0) return false;
    s_prog2d_textured = glGetUniformLocation(s_prog2d, "uTextured");
    s_prog2d_tint = glGetUniformLocation(s_prog2d, "uTint");
    s_prog2d_point = glGetUniformLocation(s_prog2d, "uPointSize");
    glUseProgram(s_prog2d);
    glUniform1i(glGetUniformLocation(s_prog2d, "uTex"), 0);
    glGenVertexArrays(1, &s_vao2d);
    glGenBuffers(1, &s_vbo2d);
    glBindVertexArray(s_vao2d);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo2d);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), (void*) 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D),
                          (void*) offsetof(Vertex2D, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex2D),
                          (void*) offsetof(Vertex2D, color));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    check_gl("init_2d");
    return true;
}

/* Textured-quad copy: some framebuffer blits (scaled or flipped) take a CPU
 * fallback in the PS5 driver, a draw never does. */
static GLuint s_copy_prog;
static GLint s_copy_rect;

static const char* s_vs_copy =
    "#version 460 core\n"
    "uniform vec4 uRect;\n" /* source u0 v0 u1 v1 */
    "uniform float uFlip;\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "  vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "  vUv = mix(uRect.xy, uRect.zw, c);\n"
    "  gl_Position = vec4(c.x * 2.0 - 1.0, (uFlip > 0.5 ? 1.0 - c.y * 2.0 : c.y * 2.0 - 1.0), 0.0, 1.0);\n"
    "}\n";
static const char* s_fs_copy =
    "#version 460 core\n"
    "in vec2 vUv;\n"
    "uniform sampler2D uTex;\n"
    "layout(location=0) out vec4 oColor;\n"
    "void main() { oColor = texture(uTex, vUv); }\n";
static GLint s_copy_flip;

static bool init_copy(void)
{
    s_copy_prog = melee_ps5_gl_program(s_vs_copy, s_fs_copy);
    if (s_copy_prog == 0) return false;
    s_copy_rect = glGetUniformLocation(s_copy_prog, "uRect");
    s_copy_flip = glGetUniformLocation(s_copy_prog, "uFlip");
    glUseProgram(s_copy_prog);
    glUniform1i(glGetUniformLocation(s_copy_prog, "uTex"), 0);
    return true;
}

/* Draws texture region [u0,v0]-[u1,v1] over the whole bound viewport. */
static void copy_quad(GLuint texture, f32 u0, f32 v0, f32 u1, f32 v1, bool flip)
{
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(s_copy_prog);
    glUniform4f(s_copy_rect, u0, v0, u1, v1);
    glUniform1f(s_copy_flip, flip ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(s_vao2d);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    ++g_melee_vita_gxm_state_epoch;
}

static void set_blend_add(bool additive)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                        additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
}

static u64 s_acc_draw, s_acc_clear;

static void draw_2d_impl(GLenum primitive, const Vertex2D* vertices, u32 count,
                         const vita2d_texture* texture, u32 tint, f32 point_size);
static void draw_2d(GLenum primitive, const Vertex2D* vertices, u32 count,
                    const vita2d_texture* texture, u32 tint, f32 point_size)
{
    const u64 t0 = sceKernelGetProcessTimeWide();
    draw_2d_impl(primitive, vertices, count, texture, tint, point_size);
    s_acc_draw += sceKernelGetProcessTimeWide() - t0;
}

static void draw_2d_impl(GLenum primitive, const Vertex2D* vertices, u32 count,
                         const vita2d_texture* texture, u32 tint, f32 point_size)
{
    glUseProgram(s_prog2d);
    glBindVertexArray(s_vao2d);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo2d);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (count * sizeof(Vertex2D)), vertices,
                 GL_STREAM_DRAW);
    glUniform1i(s_prog2d_textured, texture != NULL);
    glUniform4f(s_prog2d_tint, (f32) (tint & 0xffu) / 255.0f, (f32) (tint >> 8 & 0xffu) / 255.0f,
                (f32) (tint >> 16 & 0xffu) / 255.0f, (f32) (tint >> 24) / 255.0f);
    glUniform1f(s_prog2d_point, point_size);
    if (texture != NULL) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture->tex);
    }
    glDrawArrays(primitive, 0, (GLsizei) count);
    glBindVertexArray(0);
    ++g_melee_vita_gxm_state_epoch;
}

static void draw_rect(f32 x, f32 y, f32 w, f32 h, u32 color)
{
    const Vertex2D v[6] = {
        { x, y, 0, 0, 0, color },         { x + w, y, 0, 0, 0, color },
        { x + w, y + h, 0, 0, 0, color }, { x + w, y + h, 0, 0, 0, color },
        { x, y + h, 0, 0, 0, color },     { x, y, 0, 0, 0, color },
    };
    draw_2d(GL_TRIANGLES, v, 6, NULL, 0xffffffffu, 1.0f);
}

/* Fills a Vita-space rectangle of the bound target with colour and far-plane
 * depth, as a GX clear does. */
static void clear_region(f32 x, f32 y, f32 w, f32 h, u32 color)
{
    const u64 t0 = sceKernelGetProcessTimeWide();
    const f32 s = melee_ps5_target_pixel_scale();
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint) (x * s), (GLint) (y * s), (GLsizei) (w > 0.0f ? w * s : 1.0f),
              (GLsizei) (h > 0.0f ? h * s : 1.0f));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor((f32) (color & 0xffu) / 255.0f, (f32) (color >> 8 & 0xffu) / 255.0f,
                 (f32) (color >> 16 & 0xffu) / 255.0f, (f32) (color >> 24) / 255.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    melee_ps5_gl_default_depth();
    ++g_melee_vita_gxm_state_epoch;
    s_acc_clear += sceKernelGetProcessTimeWide() - t0;
}

/* ===================================================================
 * Frame begin, copies, offscreen targets
 * =================================================================== */

static void begin_main(u32 clear_color, bool clear)
{
    bind_target(NULL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (clear) clear_region(0.0f, 0.0f, (f32) VITA_W, (f32) VITA_H, clear_color);
    melee_ps5_gl_default_depth();
}

typedef struct RqCopy {
    vita2d_texture* target;
    u32 width, height;
    f32 x0, y0, sx, sy;
    u32 format;
    u32 clear;
} RqCopy;

typedef struct RqCopyClear {
    f32 x, y, width, height;
} RqCopyClear;

static void exec_copy_clear(const void* payload)
{
    const RqCopyClear* c = payload;
    clear_region(c->x, c->y, c->width, c->height, s_frame.clear_color);
}

static void exec_copy(const void* payload)
{
    const RqCopy* c = payload;
    const bool depth_copy = c->format == GX_TF_Z24X8;
    if (c->target != NULL && c->target->fbo != 0u) {
        const f32 s = (f32) s_efb_scale;
        const GLint sx0 = (GLint) (c->x0 * s);
        const GLint sy0 = (GLint) (c->y0 * s);
        const GLint sx1 = (GLint) ((c->x0 + (f32) c->width * c->sx) * s);
        const GLint sy1 = (GLint) ((c->y0 + (f32) c->height * c->sy) * s);
        if (depth_copy) {
            if (!melee_ps5_gl_copy_depth(s_efb.depth, sx0, sy0, sx1, sy1, s_efb.width,
                                         s_efb.height, c->target)) {
                static u32 logged;
                if (logged++ < 4u) melee_vita_log_info("[GXCOPY] depth copy unavailable");
            }
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, c->target->fbo);
            glViewport(0, 0, (GLsizei) c->width, (GLsizei) c->height);
            copy_quad(s_efb.tex, (f32) sx0 / (f32) s_efb.width, (f32) sy0 / (f32) s_efb.height,
                      (f32) sx1 / (f32) s_efb.width, (f32) sy1 / (f32) s_efb.height, false);
        }
        check_gl("exec_copy");
    }
    begin_main(s_frame.clear_color, false);
    if (c->clear)
        clear_region(c->x0, c->y0, (f32) c->width * c->sx, (f32) c->height * c->sy,
                     s_frame.clear_color);
}

typedef struct RqTarget {
    vita2d_texture* target;
    u32 width, height;
    u32 clear_color;
    u32 begin;
} RqTarget;

static void exec_target(const void* payload)
{
    const RqTarget* t = payload;
    if (t->begin) {
        if (t->target == NULL || t->target->fbo == 0u) return;
        bind_target(t->target);
        glDisable(GL_CULL_FACE);
        /* The map starts fully lit; silhouettes darken it. */
        clear_region(0.0f, 0.0f, (f32) t->width, (f32) t->height, t->clear_color);
    } else {
        begin_main(s_frame.clear_color, false);
    }
}

vita2d_texture* melee_vita_gxm_copy_texture(const void* key, u32 width, u32 height,
                                            bool* created)
{
    u32 c, free_slot = COPY_TEXTURES;
    if (created != NULL) *created = false;
    for (c = 0; c < COPY_TEXTURES; ++c) {
        if (s_copy_textures[c].binding.key == key) {
            const u32 generation = melee_vita_heap_allocation_generation(key);
            if (s_copy_textures[c].width == width && s_copy_textures[c].height == height &&
                s_copy_textures[c].texture != NULL &&
                melee_vita_copy_texture_binding_matches(&s_copy_textures[c].binding, key,
                                                        generation)) {
                s_copy_textures[c].frame = s_frame_counter;
                return s_copy_textures[c].texture;
            }
            retire_texture(s_copy_textures[c].texture);
            s_copy_textures[c].texture = NULL;
            free_slot = c;
            break;
        }
        if (s_copy_textures[c].binding.key == NULL && free_slot == COPY_TEXTURES)
            free_slot = c;
    }
    if (free_slot == COPY_TEXTURES) {
        free_slot = 0;
        for (c = 1; c < COPY_TEXTURES; ++c)
            if (s_copy_textures[c].frame < s_copy_textures[free_slot].frame) free_slot = c;
        retire_texture(s_copy_textures[free_slot].texture);
        s_copy_textures[free_slot].texture = NULL;
    }
    melee_vita_copy_texture_bind(&s_copy_textures[free_slot].binding, key,
                                 melee_vita_heap_allocation_generation(key));
    s_copy_textures[free_slot].width = width;
    s_copy_textures[free_slot].height = height;
    s_copy_textures[free_slot].frame = s_frame_counter;
    s_copy_textures[free_slot].texture = texture_create(width, height, true, false);
    if (s_copy_textures[free_slot].texture != NULL) {
        if (created != NULL) *created = true;
    } else {
        melee_vita_copy_texture_bind(&s_copy_textures[free_slot].binding, NULL, 0);
    }
    return s_copy_textures[free_slot].texture;
}

void melee_vita_gxm_begin_target(struct vita2d_texture* target, u32 width, u32 height,
                                 u32 clear_color)
{
    RqTarget* t = melee_vita_rq_push(exec_target, sizeof(RqTarget));
    if (t == NULL) return;
    t->target = target;
    t->width = width;
    t->height = height;
    t->clear_color = clear_color;
    t->begin = 1u;
}

void melee_vita_gxm_end_target(void)
{
    RqTarget* t = melee_vita_rq_push(exec_target, sizeof(RqTarget));
    if (t != NULL) t->begin = 0u;
}

void melee_vita_gxm_queue_copy(struct vita2d_texture* target, u32 width, u32 height, f32 x0,
                               f32 y0, f32 sx, f32 sy, u32 format, int clear)
{
    RqCopy* c;
    if (target == NULL || width == 0u || height == 0u || x0 < 0.0f || y0 < 0.0f ||
        sx <= 0.0f || sy <= 0.0f || x0 + (f32) width * sx > (f32) VITA_W + 0.5f ||
        y0 + (f32) height * sy > (f32) VITA_H + 0.5f) {
        static u32 logged;
        if (logged++ < 8u)
            melee_vita_log_info("[GXCOPY] unsupported copy %ux%u at %.1f,%.1f step %.3f,%.3f",
                                width, height, x0, y0, sx, sy);
        target = NULL;
    }
    c = melee_vita_rq_push(exec_copy, sizeof(RqCopy));
    if (c == NULL) return;
    c->target = target;
    c->width = width;
    c->height = height;
    c->x0 = x0;
    c->y0 = y0;
    c->sx = sx;
    c->sy = sy;
    c->format = format;
    c->clear = clear ? 1u : 0u;
}

void melee_vita_gxm_queue_copy_clear(f32 x, f32 y, f32 width, f32 height)
{
    RqCopyClear* clear = melee_vita_rq_push(exec_copy_clear, sizeof(RqCopyClear));
    if (clear == NULL) return;
    clear->x = x;
    clear->y = y;
    clear->width = width;
    clear->height = height;
}

/* ===================================================================
 * Legacy (CPU-transformed) GX draws
 * =================================================================== */

typedef struct RqLegacy {
    MeleeVitaRenderState state;
    u8 has_state;
    GLenum primitive;
    vita2d_texture* texture;
    u32 tint;
    const Vertex2D* vertices;
    u32 count;
} RqLegacy;

static void exec_legacy(const void* payload)
{
    const RqLegacy* d = payload;
    f32 point = 1.0f;
    GLenum depth_func = GL_ALWAYS;
    GLboolean depth_write = GL_FALSE;
    if (d->has_state) {
        if (d->state.depth_write) depth_write = GL_TRUE;
        point = (f32) (d->state.point_size ? d->state.point_size : 1u);
        if (d->state.line_width > d->state.point_size) point = (f32) d->state.line_width;
        set_blend_add(d->state.additive_blend != 0);
    } else {
        set_blend_add(false);
    }
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(depth_func);
    glDepthMask(depth_write);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    draw_2d(d->primitive, d->vertices, d->count, d->texture, d->tint,
            point * melee_ps5_target_pixel_scale());
}

static void push_legacy(GLenum primitive, const MeleeVitaScreenVertex* vertices, u32 count,
                        vita2d_texture* texture, u32 tint, const MeleeVitaRenderState* state)
{
    Vertex2D* output;
    RqLegacy* d;
    if (!s_initialized || vertices == NULL || count == 0) return;
    output = melee_vita_rq_alloc_gpu(count * sizeof(*output), 16);
    if (output == NULL) return;
    for (u32 i = 0; i < count; ++i) {
        output[i].x = vertices[i].x;
        output[i].y = vertices[i].y;
        output[i].z = vertices[i].z;
        output[i].u = vertices[i].u;
        output[i].v = vertices[i].v;
        output[i].color = vertices[i].color;
    }
    d = melee_vita_rq_push(exec_legacy, sizeof(RqLegacy));
    if (d == NULL) return;
    d->primitive = primitive;
    d->vertices = output;
    d->count = count;
    d->texture = texture;
    d->tint = tint;
    if (state != NULL) {
        d->state = *state;
        d->has_state = 1;
    }
}

void melee_vita_gxm_draw_triangles(const MeleeVitaScreenVertex* vertices, u32 count,
                                   const MeleeVitaTextureSource* source, u32 tint,
                                   const MeleeVitaRenderState* state)
{
    if (count < 3) return;
    push_legacy(GL_TRIANGLES, vertices, count, get_texture(source), tint, state);
}

void melee_vita_gxm_draw_lines(const MeleeVitaScreenVertex* vertices, u32 count,
                               const MeleeVitaRenderState* state)
{
    push_legacy(GL_LINES, vertices, count, NULL, 0xffffffffu, state);
}

void melee_vita_gxm_draw_points(const MeleeVitaScreenVertex* vertices, u32 count,
                                const MeleeVitaRenderState* state)
{
    push_legacy(GL_POINTS, vertices, count, NULL, 0xffffffffu, state);
}

/* ===================================================================
 * Full-screen overlays (movies)
 * =================================================================== */

typedef struct RqOverlay {
    vita2d_texture* texture;
    u32 width, height;
    bool fill_width;
} RqOverlay;

static void exec_overlay(const void* payload)
{
    const RqOverlay* o = payload;
    const f32 width = o->fill_width ? 960.0f : 544.0f * (73.0f / 60.0f);
    const f32 x = (960.0f - width) * 0.5f;
    melee_ps5_gl_default_depth();
    set_blend_add(false);
    draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0xff000000u);
    if (o->texture != NULL) {
        const Vertex2D v[6] = {
            { x, 0, 0, 0, 0, ~0u },          { x + width, 0, 0, 1, 0, ~0u },
            { x + width, 544, 0, 1, 1, ~0u }, { x + width, 544, 0, 1, 1, ~0u },
            { x, 544, 0, 0, 1, ~0u },         { x, 0, 0, 0, 0, ~0u },
        };
        draw_2d(GL_TRIANGLES, v, 6, o->texture, 0xffffffffu, 1.0f);
    }
}

static void queue_overlay(vita2d_texture* texture, u32 width, u32 height, bool fill_width)
{
    RqOverlay* o;
    if (!s_initialized || width == 0u || height == 0u) return;
    o = melee_vita_rq_push(exec_overlay, sizeof(*o));
    if (o == NULL) return;
    o->texture = texture;
    o->width = width;
    o->height = height;
    o->fill_width = fill_width;
}

void melee_vita_gxm_queue_overlay(vita2d_texture* texture, u32 width, u32 height)
{
    extern int melee_vita_widescreen_active(void);
    queue_overlay(texture, width, height, melee_vita_widescreen_active() != 0);
}

void melee_vita_gxm_queue_overlay_full_width(vita2d_texture* texture, u32 width, u32 height)
{
    queue_overlay(texture, width, height, true);
}

/* ===================================================================
 * Init, present, shutdown
 * =================================================================== */

int melee_vita_gxm_init(void)
{
    static const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint major = 0, minor = 0, count = 0;
    if (s_initialized) return 0;

    /* Compiled shader variants persist across runs (first-use hitches). */
    mkdir("/download0/shadercache", 0777);
    setenv("PS5_SHADER_CACHE_DIR", "/download0/shadercache", 1);
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s_display == EGL_NO_DISPLAY || !eglInitialize(s_display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(s_display, config_attributes, &config, 1, &count) || count != 1) {
        melee_ps5_log("[GL] EGL initialisation failed: 0x%04x", (unsigned) eglGetError());
        return -1;
    }
    s_surface = eglCreateWindowSurface(s_display, config, (EGLNativeWindowType) 0, NULL);
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, context_attributes);
    if (s_surface == EGL_NO_SURFACE || s_context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(s_display, s_surface, s_surface, s_context)) {
        melee_ps5_log("[GL] EGL surface/context failed: 0x%04x", (unsigned) eglGetError());
        return -1;
    }
    eglQuerySurface(s_display, s_surface, EGL_WIDTH, &s_window_width);
    eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &s_window_height);
    eglSwapInterval(s_display, 1);
    melee_ps5_log("[GL] EGL %d.%d, window %dx%d, %s / %s", major, minor, s_window_width,
                  s_window_height, (const char*) glGetString(GL_VERSION),
                  (const char*) glGetString(GL_RENDERER));

    /* Depth in [0, 1] as GXM and GX produce it. */
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
    resolution_config_load();
    s_efb_scale = ps5_scale_for_option(g_melee_vita_menu_resolution_option);
#endif
    s_efb.width = VITA_W * s_efb_scale;
    s_efb.height = VITA_H * s_efb_scale;
    {
        vita2d_texture* efb = texture_create(s_efb.width, s_efb.height, true, true);
        if (efb == NULL) return -1;
        s_efb = *efb;
        free(efb);
    }
    if (!init_2d() || !init_copy()) return -1;
    s_frame.arena = malloc(RQ_ARENA_SIZE);
    if (s_frame.arena == NULL) return -1;
    s_frame.clear_color = 0xff000000u;
    s_initialized = 1;
    melee_ps5_log("[GL] EFB %ux%u (x%u)", s_efb.width, s_efb.height, s_efb_scale);
    if (gxr_init() != 0) melee_vita_log_info("[GXR] using the CPU-transformed GX path");
    bind_target(NULL);
    return 0;
}

bool g_ps5_rq_follows_same; /* the previous command ran the same executor */
extern void (*const g_gxr_exec_draw)(const void*);
extern void gxr_flush_pending(void);

static void run_frame(void)
{
    u32 offset = 0;
    void (*previous)(const void*) = NULL;
    begin_main(s_frame.clear_color, true);
    while (offset < s_frame.size) {
        const RqCommand* command = (const RqCommand*) (s_frame.cmds + offset);
        offset += (u32) sizeof(RqCommand);
        g_ps5_rq_follows_same = previous == command->exec;
        previous = command->exec;
        /* Batched draws must land before anything else touches GL state. */
        if (command->exec != g_gxr_exec_draw) gxr_flush_pending();
        command->exec(s_frame.cmds + offset);
        offset += command->size;
    }
    gxr_flush_pending();
}

/* ---- screenshots (L3 + R3): the EFB, halved, as a 24-bit BMP ---------------- */

static volatile int s_screenshot_requested;
static u32 s_screenshot_index;

void melee_ps5_request_screenshot(void) { s_screenshot_requested = 1; }

static void write_le32(u8* p, u32 v) { p[0] = (u8) v; p[1] = (u8) (v >> 8); p[2] = (u8) (v >> 16); p[3] = (u8) (v >> 24); }

static void take_screenshot(void)
{
    const u32 w = s_efb.width, h = s_efb.height;
    const u32 ow = w / 2u, oh = h / 2u;
    const u32 row_bytes = (ow * 3u + 3u) & ~3u;
    u8* pixels = malloc((size_t) w * h * 4u);
    u8* bmp = malloc(54u + (size_t) row_bytes * oh);
    char path[64];
    FILE* file;
    if (pixels == NULL || bmp == NULL) { free(pixels); free(bmp); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, s_efb.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei) w, (GLsizei) h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    write_le32(bmp + 2, 54u + row_bytes * oh);
    write_le32(bmp + 10, 54u);
    write_le32(bmp + 14, 40u);
    write_le32(bmp + 18, ow);
    write_le32(bmp + 22, (u32) -(s32) oh); /* top-down rows, as the EFB stores them */
    bmp[26] = 1; bmp[28] = 24;
    write_le32(bmp + 34, row_bytes * oh);
    for (u32 y = 0; y < oh; ++y) {
        u8* out = bmp + 54u + (size_t) y * row_bytes;
        for (u32 x = 0; x < ow; ++x) {
            const u8* in = pixels + ((size_t) (y * 2u) * w + x * 2u) * 4u;
            out[x * 3u + 0] = in[2];
            out[x * 3u + 1] = in[1];
            out[x * 3u + 2] = in[0];
        }
    }
    snprintf(path, sizeof(path), "/download0/shot-%03u.bmp", s_screenshot_index++);
    file = fopen(path, "wb");
    if (file != NULL) {
        fwrite(bmp, 1, 54u + (size_t) row_bytes * oh, file);
        fclose(file);
        melee_ps5_log("[SHOT] wrote %s", path);
    }
    free(pixels);
    free(bmp);
}

static vita2d_texture* texture_create(u32 width, u32 height, bool target, bool depth);
static void texture_free(vita2d_texture* t);

/* Rebuilds the EFB at a new scale between frames. */
static void efb_set_scale(u32 scale)
{
    vita2d_texture* efb;
    if (scale == s_efb_scale || scale == 0u || scale > 4u) return;
    efb = texture_create(VITA_W * scale, VITA_H * scale, true, true);
    if (efb == NULL || efb->fbo == 0u) {
        texture_free(efb);
        return;
    }
    glFinish();
    glDeleteFramebuffers(1, &s_efb.fbo);
    glDeleteTextures(1, &s_efb.depth);
    glDeleteTextures(1, &s_efb.tex);
    s_efb = *efb;
    free(efb);
    s_efb_scale = scale;
    ++g_melee_vita_gxm_state_epoch;
    bind_target(NULL);
    melee_ps5_log("[GL] EFB %ux%u (x%u)", s_efb.width, s_efb.height, s_efb_scale);
}

static void update_efb_scale(void)
{
#ifdef MELEE_VITA_RUNTIME_RESOLUTION_MENU
    /* A match is a run of frames with shadow passes or EFB copies; wait for
     * the scene type to settle before resizing. */
    static bool gameplay;
    static u32 stable;
    const bool now = melee_vita_resolution_is_gameplay_frame(s_frame_reasons);
    s_frame_reasons = 0;
    if (now != gameplay) {
        if (++stable < 20u) return;
        gameplay = now;
    }
    stable = 0;
    efb_set_scale(ps5_scale_for_option(gameplay ? g_melee_vita_gameplay_resolution_option
                                                : g_melee_vita_menu_resolution_option));
#endif
}

void melee_vita_gxm_present(u32 clear_color)
{
    extern int melee_vita_widescreen_active(void);
    static u64 s_acc_run, s_acc_blit, s_acc_swap, s_acc_cmds;
    static u32 s_acc_frames;
    u64 t_start, t_frame, t_blit, t_swap;
    if (!s_initialized) return;
    t_start = sceKernelGetProcessTimeWide();
    run_frame();

    /* 4:3 scenes are pillarboxed inside the 960x544 frame. */
    if (!melee_vita_widescreen_active()) {
        const f32 bar = (960.0f - 544.0f * (73.0f / 60.0f)) * 0.5f;
        bind_target(NULL);
        melee_ps5_gl_default_depth();
        glDisable(GL_CULL_FACE);
        set_blend_add(false);
        draw_rect(0.0f, 0.0f, bar + 1.0f, 544.0f, 0xff000000u);
        draw_rect(960.0f - bar - 1.0f, 0.0f, bar + 1.0f, 544.0f, 0xff000000u);
    }

    /* EFB (top row first) -> window (bottom row first): flip while scaling. */
    if (s_screenshot_requested) {
        s_screenshot_requested = 0;
        take_screenshot();
    }
    t_frame = sceKernelGetProcessTimeWide();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, s_window_width, s_window_height);
    glBindTexture(GL_TEXTURE_2D, s_efb.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    copy_quad(s_efb.tex, 0.0f, 0.0f, 1.0f, 1.0f, true);
    check_gl("present");
    t_blit = sceKernelGetProcessTimeWide();
    if (!eglSwapBuffers(s_display, s_surface)) {
        static u32 logged;
        if (logged++ < 8u) melee_ps5_log("[GL] eglSwapBuffers failed: 0x%04x", (unsigned) eglGetError());
    }
    t_swap = sceKernelGetProcessTimeWide();
    s_acc_run += t_frame - t_start;
    s_acc_blit += t_blit - t_frame;
    s_acc_swap += t_swap - t_blit;
    s_acc_cmds += s_frame.size;
    if (++s_acc_frames == 120u) {
        extern size_t ps5_opengl_heap_live_bytes(void);
        extern void melee_ps5_gxr_log_timing(void);
        melee_ps5_gxr_log_timing();
        melee_ps5_log("[GLPERF] per frame: run=%.2fms (draw=%.2f clear=%.2f) present=%.2fms "
                      "swap=%.2fms cmd=%uB heap=%.1fMB",
                      s_acc_run / 120000.0, s_acc_draw / 120000.0, s_acc_clear / 120000.0,
                      s_acc_blit / 120000.0, s_acc_swap / 120000.0,
                      (unsigned) (s_acc_cmds / 120u),
                      ps5_opengl_heap_live_bytes() / 1048576.0);
        s_acc_run = s_acc_blit = s_acc_swap = s_acc_cmds = 0;
        s_acc_draw = s_acc_clear = 0;
        s_acc_frames = 0;
    }

    s_frame.size = 0;
    s_frame.arena_used = 0;
    s_frame.clear_color = s_next_clear_color;
    s_next_clear_color = clear_color;
    ++s_frame_counter;
    update_efb_scale();
    {
        extern u32 g_melee_vita_texture_memo_epoch;
        ++g_melee_vita_texture_memo_epoch;
    }
    collect_graveyard(false);
    if ((s_frame_counter % 60u) == 0u) {
        TextureCacheEntry** link = &s_textures;
        u32 count = 0;
        while (*link != NULL) {
            TextureCacheEntry* entry = *link;
            if (s_frame_counter - entry->last_used_frame > 180u) {
                *link = entry->next;
                texture_hash_remove(entry);
                retire_texture(entry->texture);
                free(entry);
                continue;
            }
            ++count;
            link = &entry->next;
        }
        if ((s_frame_counter % 600u) == 0u) {
            melee_vita_log_info("[GL] frame %u textures live=%u uploads=%u failures=%u",
                                s_frame_counter, count, s_texture_uploads, s_texture_failures);
            s_texture_uploads = s_texture_failures = 0;
        }
    }
    bind_target(NULL);
}

void melee_vita_gxm_prepare_texture_invalidation(void) {}

void melee_vita_gxm_invalidate_textures(void)
{
    /* Nothing recorded may still reference a texture being freed. */
    if (s_frame.size != 0u) {
        static u32 logged;
        if (logged++ < 4u)
            melee_vita_log_info("[GL] texture invalidation with %u recorded bytes", s_frame.size);
    }
    free_textures();
    free_copy_textures();
    collect_graveyard(true);
    gxr_flush_warm_cache();
}

void melee_vita_gxm_log_memory(const char* phase)
{
    u32 textures = 0;
    for (TextureCacheEntry* e = s_textures; e != NULL; e = e->next) ++textures;
    melee_vita_log_info("[MEM] %s textures=%u", phase, textures);
}

void melee_vita_gxm_shutdown(void)
{
    if (!s_initialized) return;
    free_textures();
    free_copy_textures();
    collect_graveyard(true);
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_display, s_context);
    eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    s_initialized = 0;
}
