#include "texture_atlas.h"

#include "bringup_probe.h"
#include "texture_decoder.h"
#include "vita_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_WIDTH 1024u
#define ATLAS_HEIGHT 1024u
#define MAX_ATLAS_IMAGES 128u
#define NO_IMAGE UINT32_MAX

struct atlas_entry {
    uint32_t image_offset;
    uint32_t width, height, format;
    uint32_t x, y;
};

static uint32_t be32(const unsigned char* p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 |
           (uint32_t) p[2] << 8 | p[3];
}

static uint16_t be16(const unsigned char* p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

static int read_at(FILE* file, uint32_t offset, void* data, size_t size)
{
    return fseek(file, (long) offset, SEEK_SET) == 0 &&
           fread(data, 1, size, file) == size;
}

static int relocate_archive(unsigned char* archive, uint32_t archive_size,
                            unsigned char** data_out, uint32_t* data_size_out)
{
    if (archive_size < 0x20u || be32(archive) != archive_size) return 0;
    const uint32_t data_size = be32(archive + 4u);
    const uint32_t relocation_count = be32(archive + 8u);
    const uint64_t relocation_table = UINT64_C(0x20) + data_size;
    if (relocation_table + (uint64_t) relocation_count * 4u > archive_size)
        return 0;
    unsigned char* data = archive + 0x20u;
    for (uint32_t i = 0; i < relocation_count; ++i) {
        const uint32_t slot =
            be32(archive + (uint32_t) relocation_table + i * 4u);
        if (data_size < 4u || slot > data_size - 4u) return 0;
        const uint32_t target_offset = be32(data + slot);
        if (target_offset >= data_size) return 0;
        void* target = data + target_offset;
        memcpy(data + slot, &target, sizeof(target));
    }
    *data_out = data;
    *data_size_out = data_size;
    return 1;
}

static int compare_height(const void* left, const void* right)
{
    const struct atlas_entry* a = left;
    const struct atlas_entry* b = right;
    if (a->height != b->height) return a->height < b->height ? 1 : -1;
    return a->width < b->width ? 1 : a->width > b->width ? -1 : 0;
}

static struct atlas_entry* find_entry(struct atlas_entry* entries,
                                      uint32_t count, uint32_t offset)
{
    for (uint32_t i = 0; i < count; ++i)
        if (entries[i].image_offset == offset) return &entries[i];
    return NULL;
}

static float wrap_coordinate(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

int melee_vita_build_texture_atlas(
    const char* disc_path, struct melee_vita_bringup_info* mesh,
    struct melee_vita_texture_atlas* atlas)
{
    memset(atlas, 0, sizeof(*atlas));
    FILE* file = fopen(disc_path, "rb");
    if (file == NULL) return -1;
    unsigned char* archive = malloc(mesh->archive_size);
    if (archive == NULL) {
        fclose(file);
        return -2;
    }
    if (!read_at(file, mesh->archive_offset, archive, mesh->archive_size)) {
        free(archive);
        fclose(file);
        return -3;
    }
    fclose(file);
    unsigned char* data = NULL;
    uint32_t data_size = 0;
    if (!relocate_archive(archive, mesh->archive_size, &data, &data_size)) {
        free(archive);
        return -4;
    }

    struct atlas_entry entries[MAX_ATLAS_IMAGES];
    uint32_t entry_count = 0u;
    for (uint32_t vertex = 0; vertex < mesh->mesh_vertex_count; ++vertex) {
        const uint32_t offset = mesh->mesh_image_offsets[vertex];
        if (offset == NO_IMAGE || find_entry(entries, entry_count, offset))
            continue;
        if (entry_count == MAX_ATLAS_IMAGES || offset > data_size - 0x18u) {
            free(archive);
            return -5;
        }
        const unsigned char* image = data + offset;
        struct atlas_entry* entry = &entries[entry_count++];
        memset(entry, 0, sizeof(*entry));
        entry->image_offset = offset;
        entry->width = be16(image + 4u);
        entry->height = be16(image + 6u);
        entry->format = be32(image + 8u);
        if (entry->width == 0u || entry->height == 0u ||
            entry->width > ATLAS_WIDTH - 2u ||
            entry->height > ATLAS_HEIGHT - 2u) {
            free(archive);
            return -6;
        }
    }
    qsort(entries, entry_count, sizeof(entries[0]), compare_height);

    uint32_t cursor_x = 1u, cursor_y = 1u, row_height = 0u;
    for (uint32_t i = 0; i < entry_count; ++i) {
        struct atlas_entry* entry = &entries[i];
        if (cursor_x + entry->width + 1u > ATLAS_WIDTH) {
            cursor_x = 1u;
            cursor_y += row_height + 1u;
            row_height = 0u;
        }
        if (cursor_y + entry->height + 1u > ATLAS_HEIGHT) {
            free(archive);
            return -7;
        }
        entry->x = cursor_x;
        entry->y = cursor_y;
        cursor_x += entry->width + 1u;
        if (entry->height > row_height) row_height = entry->height;
    }

    atlas->pixels = calloc((size_t) ATLAS_WIDTH * ATLAS_HEIGHT,
                           sizeof(*atlas->pixels));
    if (atlas->pixels == NULL) {
        free(archive);
        return -8;
    }
    atlas->pixels[0] = UINT32_C(0xffffffff);
    uint32_t max_pixels = 0u;
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint32_t count = entries[i].width * entries[i].height;
        if (count > max_pixels) max_pixels = count;
    }
    uint32_t* decoded = malloc((size_t) max_pixels * sizeof(*decoded));
    if (decoded == NULL) {
        melee_vita_free_texture_atlas(atlas);
        free(archive);
        return -9;
    }
    for (uint32_t i = 0; i < entry_count; ++i) {
        struct atlas_entry* entry = &entries[i];
        uint32_t width = 0u, height = 0u, format = UINT32_MAX;
        const int result = melee_vita_decode_texture(
            data + entry->image_offset, data, data_size, decoded, max_pixels,
            &width, &height, &format);
        if (result != 0 || width != entry->width || height != entry->height ||
            format != entry->format) {
            melee_vita_log_info(
                "TEXTURE decode_failed result=%d image=0x%08x expected=%ux%u format=%u actual=%ux%u format=%u",
                result, entry->image_offset, entry->width, entry->height,
                entry->format, width, height, format);
            free(decoded);
            melee_vita_free_texture_atlas(atlas);
            free(archive);
            return -10;
        }
        for (uint32_t y = 0; y < height; ++y)
            memcpy(atlas->pixels +
                       (size_t) (entry->y + y) * ATLAS_WIDTH + entry->x,
                   decoded + (size_t) y * width,
                   (size_t) width * sizeof(*decoded));
    }
    free(decoded);
    free(archive);

    for (uint32_t vertex = 0; vertex < mesh->mesh_vertex_count; ++vertex) {
        struct atlas_entry* entry = find_entry(
            entries, entry_count, mesh->mesh_image_offsets[vertex]);
        if (entry == NULL) {
            mesh->mesh_texture_coordinates[vertex * 2u] =
                0.5f / ATLAS_WIDTH;
            mesh->mesh_texture_coordinates[vertex * 2u + 1u] =
                0.5f / ATLAS_HEIGHT;
            continue;
        }
        const float u = wrap_coordinate(
            mesh->mesh_texture_coordinates[vertex * 2u]);
        const float v = wrap_coordinate(
            mesh->mesh_texture_coordinates[vertex * 2u + 1u]);
        mesh->mesh_texture_coordinates[vertex * 2u] =
            (entry->x + u * (entry->width - 1u) + 0.5f) / ATLAS_WIDTH;
        mesh->mesh_texture_coordinates[vertex * 2u + 1u] =
            (entry->y + v * (entry->height - 1u) + 0.5f) / ATLAS_HEIGHT;
    }
    atlas->width = ATLAS_WIDTH;
    atlas->height = ATLAS_HEIGHT;
    atlas->image_count = entry_count;
    return 0;
}

void melee_vita_free_texture_atlas(struct melee_vita_texture_atlas* atlas)
{
    free(atlas->pixels);
    memset(atlas, 0, sizeof(*atlas));
}
