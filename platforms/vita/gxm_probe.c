#include "gxm_probe.h"
#include "vita_log.h"

#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct triangle_draw_info {
    uint32_t first_vertex;
    float depth;
    uint32_t color;
};

static int compare_triangle_depth(const void* left, const void* right)
{
    const struct triangle_draw_info* a = left;
    const struct triangle_draw_info* b = right;
    return a->depth < b->depth ? -1 : a->depth > b->depth ? 1 : 0;
}

static uint32_t shaded_orange(float intensity)
{
    return RGBA8((unsigned int) (240.0f * intensity),
                 (unsigned int) (128.0f * intensity),
                 (unsigned int) (64.0f * intensity), 255);
}

static void prepare_view(const float* positions, float* view_positions,
                         unsigned int vertex_count, float angle_sin,
                         float angle_cos)
{
    for (unsigned int i = 0; i < vertex_count; ++i) {
        const float x = positions[i * 3u];
        const float y = positions[i * 3u + 1u];
        const float z = positions[i * 3u + 2u];
        view_positions[i * 3u] = angle_cos * x + angle_sin * z;
        view_positions[i * 3u + 1u] = y;
        view_positions[i * 3u + 2u] = -angle_sin * x + angle_cos * z;
    }
}

static unsigned int prepare_triangles(
    const float* view_positions, struct triangle_draw_info* triangles,
    unsigned int source_triangle_count, const uint8_t* cull_modes)
{
    unsigned int visible_count = 0u;
    for (unsigned int triangle = 0; triangle < source_triangle_count;
         ++triangle) {
        const unsigned int first = triangle * 3u;
        const float* a = view_positions + first * 3u;
        const float* b = a + 3u;
        const float* c = b + 3u;
        const float ab_x = b[0] - a[0];
        const float ab_y = b[1] - a[1];
        const float ab_z = b[2] - a[2];
        const float ac_x = c[0] - a[0];
        const float ac_y = c[1] - a[1];
        const float ac_z = c[2] - a[2];
        const float normal_x = ab_y * ac_z - ab_z * ac_y;
        const float normal_y = ab_z * ac_x - ab_x * ac_z;
        const float normal_z = ab_x * ac_y - ab_y * ac_x;
        /* Match HSD_PObjDisp: bits 14/15 select front/back culling for each
         * polygon object. A zero mode is intentionally two-sided. */
        const uint8_t cull_mode = cull_modes != NULL ? cull_modes[first] : 0u;
        if (cull_mode == 3u ||
            (cull_mode == 1u && normal_z <= 1.0e-8f) ||
            (cull_mode == 2u && normal_z > 1.0e-8f))
            continue;
        const float normal_length = sqrtf(normal_x * normal_x +
                                          normal_y * normal_y +
                                          normal_z * normal_z);
        float light = 0.0f;
        if (normal_length > 1.0e-8f) {
            light = fabsf((-0.30f * normal_x + 0.50f * normal_y +
                           0.81f * normal_z) / normal_length);
        }
        triangles[visible_count].first_vertex = first;
        triangles[visible_count].depth = (a[2] + b[2] + c[2]) / 3.0f;
        triangles[visible_count].color =
            shaded_orange(0.28f + 0.72f * light);
        ++visible_count;
    }
    qsort(triangles, visible_count, sizeof(*triangles),
          compare_triangle_depth);
    return visible_count;
}

static void free_atlases(vita2d_texture** atlases, unsigned int count)
{
    if (atlases == NULL) return;
    for (unsigned int i = 0; i < count; ++i)
        if (atlases[i] != NULL) vita2d_free_texture(atlases[i]);
    free(atlases);
}

int melee_vita_gxm_probe_run(
    const struct melee_vita_render_character* characters,
    unsigned int character_count)
{
    if (vita2d_init_advanced(1024u * 1024u) == 0) return -1;
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(12, 18, 32, 255));
    if (characters == NULL || character_count == 0u) {
        vita2d_fini();
        return -2;
    }

    unsigned int max_vertex_count = 0u;
    for (unsigned int i = 0; i < character_count; ++i) {
        const struct melee_vita_render_character* c = &characters[i];
        if (c->positions == NULL || c->vertex_count == 0u ||
            c->vertex_count % 3u != 0u || c->frame_count == 0u) {
            vita2d_fini();
            return -2;
        }
        if (c->vertex_count > max_vertex_count)
            max_vertex_count = c->vertex_count;
    }
    float* view_positions = malloc(max_vertex_count * 3u * sizeof(float));
    struct triangle_draw_info* triangles =
        malloc((max_vertex_count / 3u) * sizeof(*triangles));
    unsigned int* animation_frames =
        calloc(character_count, sizeof(*animation_frames));
    unsigned char* triangle_counts_logged =
        calloc(character_count, sizeof(*triangle_counts_logged));
    vita2d_texture** atlases = calloc(character_count, sizeof(*atlases));
    if (view_positions == NULL || triangles == NULL ||
        animation_frames == NULL || triangle_counts_logged == NULL ||
        atlases == NULL) {
        free(view_positions);
        free(triangles);
        free(animation_frames);
        free(triangle_counts_logged);
        free(atlases);
        vita2d_fini();
        return -3;
    }

    for (unsigned int i = 0; i < character_count; ++i) {
        const struct melee_vita_render_character* c = &characters[i];
        if (c->atlas_pixels == NULL || c->texture_coordinates == NULL ||
            c->atlas_width == 0u || c->atlas_height == 0u)
            continue;
        atlases[i] = vita2d_create_empty_texture(c->atlas_width,
                                                 c->atlas_height);
        if (atlases[i] == NULL) {
            free_atlases(atlases, character_count);
            free(view_positions);
            free(triangles);
            free(animation_frames);
            free(triangle_counts_logged);
            vita2d_fini();
            return -6;
        }
        unsigned char* destination = vita2d_texture_get_datap(atlases[i]);
        const unsigned int stride = vita2d_texture_get_stride(atlases[i]);
        for (unsigned int y = 0; y < c->atlas_height; ++y)
            memcpy(destination + (size_t) y * stride,
                   c->atlas_pixels + (size_t) y * c->atlas_width,
                   (size_t) c->atlas_width * sizeof(*c->atlas_pixels));
        vita2d_texture_set_filters(atlases[i], SCE_GXM_TEXTURE_FILTER_LINEAR,
                                   SCE_GXM_TEXTURE_FILTER_LINEAR);
    }

    const float angle = 0.436332313f;
    const float angle_sin = sinf(angle);
    const float angle_cos = cosf(angle);
    unsigned int selected = 0u;
    unsigned int previous_buttons = 0u;
    int result = 0;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_START) != 0u) break;
        const unsigned int pressed = pad.buttons & ~previous_buttons;
        if ((pressed & SCE_CTRL_LTRIGGER) != 0u) {
            selected = (selected + character_count - 1u) % character_count;
            melee_vita_log_info("INPUT L selected=%u count=%u", selected,
                                character_count);
        }
        if ((pressed & SCE_CTRL_RTRIGGER) != 0u) {
            selected = (selected + 1u) % character_count;
            melee_vita_log_info("INPUT R selected=%u count=%u", selected,
                                character_count);
        }
        previous_buttons = pad.buttons;

        const struct melee_vita_render_character* c = &characters[selected];
        const unsigned int vertex_count = c->vertex_count;
        const unsigned int source_triangle_count = vertex_count / 3u;
        const float* frame_positions = c->positions +
            (size_t) animation_frames[selected] * vertex_count * 3u;
        prepare_view(frame_positions, view_positions, vertex_count, angle_sin,
                     angle_cos);
        const unsigned int triangle_count = prepare_triangles(
            view_positions, triangles, source_triangle_count, c->cull_modes);
        if (!triangle_counts_logged[selected]) {
            melee_vita_log_info(
                "RENDER character=%u triangles=%u rendered=%u culled=%u",
                selected, source_triangle_count, triangle_count,
                source_triangle_count - triangle_count);
            triangle_counts_logged[selected] = 1u;
        }
        float min_x = view_positions[0], max_x = view_positions[0];
        float min_y = view_positions[1], max_y = view_positions[1];
        for (unsigned int i = 1; i < vertex_count; ++i) {
            const float x = view_positions[i * 3u];
            const float y = view_positions[i * 3u + 1u];
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
        const float width = max_x - min_x;
        const float height = max_y - min_y;
        if (width <= 0.0001f || height <= 0.0001f) {
            result = -4;
            break;
        }
        const float scale_x = 700.0f / width;
        const float scale_y = 420.0f / height;
        const float screen_scale = scale_x < scale_y ? scale_x : scale_y;
        vita2d_texture* atlas = atlases[selected];

        vita2d_start_drawing();
        vita2d_clear_screen();
        const size_t vertex_size = atlas != NULL
            ? sizeof(vita2d_texture_vertex) : sizeof(vita2d_color_vertex);
        void* vertex_memory =
            vita2d_pool_memalign(vertex_count * vertex_size, 16u);
        if (vertex_memory == NULL) {
            vita2d_end_drawing();
            result = -5;
            break;
        }
        vita2d_color_vertex* vertices = vertex_memory;
        vita2d_texture_vertex* textured_vertices = vertex_memory;
        for (unsigned int triangle = 0; triangle < triangle_count; ++triangle) {
            const struct triangle_draw_info* draw = &triangles[triangle];
            for (unsigned int corner = 0; corner < 3u; ++corner) {
                const unsigned int source = draw->first_vertex + corner;
                const unsigned int destination = triangle * 3u + corner;
                const float screen_x = 480.0f +
                    (view_positions[source * 3u] -
                     (min_x + max_x) * 0.5f) * screen_scale;
                const float screen_y = 272.0f -
                    (view_positions[source * 3u + 1u] -
                     (min_y + max_y) * 0.5f) * screen_scale;
                if (atlas != NULL) {
                    textured_vertices[destination].x = screen_x;
                    textured_vertices[destination].y = screen_y;
                    textured_vertices[destination].z = 0.5f;
                    textured_vertices[destination].u =
                        c->texture_coordinates[source * 2u];
                    textured_vertices[destination].v =
                        c->texture_coordinates[source * 2u + 1u];
                } else {
                    vertices[destination].x = screen_x;
                    vertices[destination].y = screen_y;
                    vertices[destination].z = 0.5f;
                    vertices[destination].color = draw->color;
                }
            }
        }
        if (atlas != NULL)
            vita2d_draw_array_textured(atlas, SCE_GXM_PRIMITIVE_TRIANGLES,
                                       textured_vertices, vertex_count,
                                       UINT32_C(0xffffffff));
        else
            vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, vertices,
                              vertex_count);
        vita2d_end_drawing();
        vita2d_swap_buffers();
        animation_frames[selected] =
            (animation_frames[selected] + 1u) % c->frame_count;
        sceKernelDelayThread(1000);
    }
    vita2d_wait_rendering_done();
    free_atlases(atlases, character_count);
    free(view_positions);
    free(triangles);
    free(animation_frames);
    free(triangle_counts_logged);
    vita2d_fini();
    return result;
}
