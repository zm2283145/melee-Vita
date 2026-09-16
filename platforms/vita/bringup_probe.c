#include "bringup_probe.h"
#include "figatree.h"
#include "runtime_symbols.h"

#include <psp2/kernel/sysmem.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM1_SIZE (24u * 1024u * 1024u)
#define DISC_HEADER_FST_OFFSET 0x424u
#define MAX_FIGHTER_DOBJS 128u

struct character_config {
    const char* costume_archive;
    const char* fighter_archive;
    const char* animation_archive;
    const char* fighter_public;
    const char* costume_public;
    const char* idle_public;
};

static const struct character_config character_configs[] = {
    {
        "PlSsNr.dat", "PlSs.dat", "PlSsAJ.dat", "ftDataSamus",
        "PlySamus5K_Share_joint",
        "PlySamus5K_Share_ACTION_Wait_figatree",
    },
    {
        "PlMhNr.dat", "PlMh.dat", "PlMhAJ.dat", "ftDataMasterhand",
        "PlyMasterhand_Share_joint",
        "PlyMasterhand_Share_ACTION_Wait2_figatree",
    },
};

struct bringup_disc_layout {
    int valid;
    uint32_t fst_offset, fst_size, fst_entries;
    uint32_t banner_offset, banner_size;
    uint32_t costume_offset, costume_size;
    uint32_t fighter_offset, fighter_size;
    uint32_t animation_offset, animation_size;
};

static struct bringup_disc_layout
    cached_disc_layout[MELEE_VITA_CHARACTER_COUNT];

static uint32_t read_be32(const unsigned char* p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 |
           (uint32_t) p[2] << 8 | (uint32_t) p[3];
}

static uint16_t read_be16(const unsigned char* p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

static int pointer_range_in_data(const void* pointer, size_t size,
                                 const unsigned char* data,
                                 uint32_t data_size);

static float read_be_float(const unsigned char* p)
{
    const uint32_t bits = read_be32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void matrix_identity(float matrix[12])
{
    memset(matrix, 0, 12u * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = 1.0f;
}

static void matrix_concat(const float left[12], const float right[12],
                          float result[12])
{
    float out[12];
    for (uint32_t row = 0; row < 3u; ++row) {
        for (uint32_t column = 0; column < 3u; ++column) {
            out[row * 4u + column] =
                left[row * 4u] * right[column] +
                left[row * 4u + 1u] * right[4u + column] +
                left[row * 4u + 2u] * right[8u + column];
        }
        out[row * 4u + 3u] =
            left[row * 4u] * right[3] +
            left[row * 4u + 1u] * right[7] +
            left[row * 4u + 2u] * right[11] + left[row * 4u + 3u];
    }
    memcpy(result, out, sizeof(out));
}

static void matrix_scaled_add(const float source[12], float destination[12],
                              float weight)
{
    for (uint32_t i = 0; i < 12u; ++i)
        destination[i] += source[i] * weight;
}

static int read_disc_matrix(const void* pointer, const unsigned char* data,
                            uint32_t data_size, float matrix[12])
{
    if (!pointer_range_in_data(pointer, 12u * sizeof(uint32_t), data,
                               data_size))
        return 0;
    const unsigned char* source = (const unsigned char*) pointer;
    for (uint32_t i = 0; i < 12u; ++i)
        matrix[i] = read_be_float(source + i * 4u);
    return 1;
}

/* Matches HSD_MtxSRT from the original engine, including its correction for
 * a non-uniform scale inherited from the parent joint. */
static void matrix_srt(float matrix[12], const float scale[3],
                       const float rotation[3], const float translation[3],
                       const float* parent_scale)
{
    const float sin_x = sinf(rotation[0]);
    const float cos_x = cosf(rotation[0]);
    const float sin_y = sinf(rotation[1]);
    const float cos_y = cosf(rotation[1]);
    const float sin_z = sinf(rotation[2]);
    const float cos_z = cosf(rotation[2]);
    float sx0 = scale[0], sx1 = scale[0], sx2 = scale[0];
    float sy0 = scale[1], sy1 = scale[1], sy2 = scale[1];
    float sz0 = scale[2], sz1 = scale[2], sz2 = scale[2];
    if (parent_scale != NULL &&
        fabsf(parent_scale[0]) > 1.0e-12f &&
        fabsf(parent_scale[1]) > 1.0e-12f &&
        fabsf(parent_scale[2]) > 1.0e-12f) {
        sx0 *= parent_scale[0] / parent_scale[0];
        sy0 *= parent_scale[1] / parent_scale[0];
        sz0 *= parent_scale[2] / parent_scale[0];
        sx1 *= parent_scale[0] / parent_scale[1];
        sy1 *= parent_scale[1] / parent_scale[1];
        sz1 *= parent_scale[2] / parent_scale[1];
        sx2 *= parent_scale[0] / parent_scale[2];
        sy2 *= parent_scale[1] / parent_scale[2];
        sz2 *= parent_scale[2] / parent_scale[2];
    }
    matrix[0] = cos_z * (sx0 * cos_y);
    matrix[4] = sin_z * (sx1 * cos_y);
    matrix[8] = -sx2 * sin_y;
    matrix[1] = sy0 * ((cos_z * (sin_x * sin_y)) - (cos_x * sin_z));
    matrix[5] = sy1 * ((sin_z * (sin_x * sin_y)) + (cos_x * cos_z));
    matrix[9] = cos_y * (sy2 * sin_x);
    matrix[2] = sz0 * ((cos_z * (cos_x * sin_y)) + (sin_x * sin_z));
    matrix[6] = sz1 * ((sin_z * (cos_x * sin_y)) - (sin_x * cos_z));
    matrix[10] = cos_y * (sz2 * cos_x);
    matrix[3] = translation[0];
    matrix[7] = translation[1];
    matrix[11] = translation[2];
}

static void transform_positions(float* positions, uint32_t count,
                                const float matrix[12])
{
    for (uint32_t i = 0; i < count; ++i) {
        const float x = positions[i * 3u];
        const float y = positions[i * 3u + 1u];
        const float z = positions[i * 3u + 2u];
        positions[i * 3u] = matrix[0] * x + matrix[1] * y +
                            matrix[2] * z + matrix[3];
        positions[i * 3u + 1u] = matrix[4] * x + matrix[5] * y +
                                 matrix[6] * z + matrix[7];
        positions[i * 3u + 2u] = matrix[8] * x + matrix[9] * y +
                                 matrix[10] * z + matrix[11];
    }
}

static int read_at(FILE* file, uint32_t offset, void* data, size_t size)
{
    return fseek(file, (long) offset, SEEK_SET) == 0 &&
           fread(data, 1, size, file) == size;
}

static int pointer_range_in_data(const void* pointer, size_t size,
                                 const unsigned char* data,
                                 uint32_t data_size)
{
    if (pointer == NULL) return 1;
    const unsigned char* bytes = (const unsigned char*) pointer;
    return bytes >= data && size <= data_size &&
           bytes <= data + data_size - size;
}

static int be_float_is_finite(const unsigned char* value)
{
    return (read_be32(value) & UINT32_C(0x7f800000)) !=
           UINT32_C(0x7f800000);
}

/* Load a second DAT beside the costume archive. This is deliberately small:
 * the visibility data only contains internal pointers, so extern resolution
 * is neither needed nor desirable here. */
static int load_relocated_archive(FILE* file, uint32_t file_offset,
                                  uint32_t file_size, unsigned char* storage,
                                  uint32_t storage_size,
                                  const char* wanted_public,
                                  unsigned char** data_out,
                                  uint32_t* data_size_out,
                                  void** public_out)
{
    if (file_size < 0x20u || file_size > storage_size ||
        !read_at(file, file_offset, storage, file_size))
        return 0;

    const uint32_t declared_size = read_be32(storage);
    const uint32_t data_size = read_be32(storage + 4u);
    const uint32_t relocations = read_be32(storage + 8u);
    const uint32_t publics = read_be32(storage + 12u);
    const uint32_t externs = read_be32(storage + 16u);
    const uint64_t reloc_table = UINT64_C(0x20) + data_size;
    const uint64_t public_table = reloc_table + (uint64_t) relocations * 4u;
    const uint64_t extern_table = public_table + (uint64_t) publics * 8u;
    const uint64_t symbols = extern_table + (uint64_t) externs * 8u;
    if (declared_size != file_size || symbols > file_size) return 0;

    unsigned char* data = storage + 0x20u;
    for (uint32_t i = 0; i < relocations; ++i) {
        const uint32_t slot = read_be32(storage + (uint32_t) reloc_table + i * 4u);
        if (data_size < 4u || slot > data_size - 4u) return 0;
        const uint32_t target_offset = read_be32(data + slot);
        if (target_offset >= data_size) return 0;
        void* target = data + target_offset;
        memcpy(data + slot, &target, sizeof(target));
    }

    *public_out = NULL;
    for (uint32_t i = 0; i < publics; ++i) {
        const uint32_t entry = (uint32_t) public_table + i * 8u;
        const uint32_t object_offset = read_be32(storage + entry);
        const uint32_t name_offset = read_be32(storage + entry + 4u);
        if (object_offset >= data_size || symbols + name_offset >= file_size)
            return 0;
        const char* name = (const char*) storage + (uint32_t) symbols + name_offset;
        const size_t available = file_size - (uint32_t) symbols - name_offset;
        if (memchr(name, '\0', available) == NULL) return 0;
        if (strcmp(name, wanted_public) == 0)
            *public_out = data + object_offset;
    }
    if (*public_out == NULL) return 0;
    *data_out = data;
    *data_size_out = data_size;
    return 1;
}

static int set_visibility_group(const unsigned char* lookup,
                                uint32_t model_count, int selected_variant,
                                uint8_t visible[MAX_FIGHTER_DOBJS],
                                const unsigned char* data, uint32_t data_size,
                                int reveal_selected)
{
    if (lookup == NULL) return 1;
    if (!pointer_range_in_data(lookup, (size_t) model_count * 8u, data,
                               data_size))
        return 0;
    for (uint32_t model = 0; model < model_count; ++model) {
        const unsigned char* model_lookup = lookup + model * 8u;
        const uint32_t variant_count = read_be32(model_lookup);
        void* variants_pointer = NULL;
        memcpy(&variants_pointer, model_lookup + 4u, sizeof(variants_pointer));
        const unsigned char* variants = variants_pointer;
        if (variant_count > 32u ||
            !pointer_range_in_data(variants, (size_t) variant_count * 8u,
                                   data, data_size))
            return 0;
        for (uint32_t variant = 0; variant < variant_count; ++variant) {
            const unsigned char* desc = variants + variant * 8u;
            const uint32_t count = read_be32(desc);
            void* indices_pointer = NULL;
            memcpy(&indices_pointer, desc + 4u, sizeof(indices_pointer));
            const uint8_t* indices = indices_pointer;
            if (count > MAX_FIGHTER_DOBJS ||
                !pointer_range_in_data(indices, count, data, data_size))
                return 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (indices[i] >= MAX_FIGHTER_DOBJS) return 0;
                visible[indices[i]] =
                    reveal_selected && (int) variant == selected_variant;
            }
        }
    }
    return 1;
}

static int find_animation_segment(FILE* file, uint32_t file_offset,
                                  uint32_t file_size,
                                  const char* wanted_public,
                                  uint32_t* segment_offset,
                                  uint32_t* segment_size)
{
    uint32_t position = 0u;
    while (file_size >= 0x20u && position <= file_size - 0x20u) {
        unsigned char header[0x20u];
        if (!read_at(file, file_offset + position, header, sizeof(header)))
            return 0;
        const uint32_t declared = read_be32(header);
        const uint32_t data_size = read_be32(header + 4u);
        const uint32_t relocations = read_be32(header + 8u);
        const uint32_t publics = read_be32(header + 12u);
        const uint32_t externs = read_be32(header + 16u);
        if (declared < 0x20u || declared > file_size - position) return 0;
        const uint64_t public_table = UINT64_C(0x20) + data_size +
                                      (uint64_t) relocations * 4u;
        const uint64_t symbols = public_table + (uint64_t) publics * 8u +
                                 (uint64_t) externs * 8u;
        if (symbols > declared) return 0;
        unsigned char* archive = malloc(declared);
        if (archive == NULL) return 0;
        const int loaded = read_at(file, file_offset + position, archive,
                                   declared);
        int found = 0;
        if (loaded) {
            for (uint32_t i = 0; i < publics; ++i) {
                const uint32_t entry = (uint32_t) public_table + i * 8u;
                const uint32_t name_offset = read_be32(archive + entry + 4u);
                if (symbols + name_offset >= declared) continue;
                const char* name = (const char*) archive + symbols + name_offset;
                const size_t available = declared - (size_t) symbols - name_offset;
                if (memchr(name, '\0', available) != NULL &&
                    strcmp(name, wanted_public) == 0) {
                    found = 1;
                    break;
                }
            }
        }
        free(archive);
        if (!loaded) return 0;
        if (found) {
            *segment_offset = file_offset + position;
            *segment_size = declared;
            return 1;
        }
        position = (position + declared + 31u) & ~31u;
    }
    return 0;
}

int melee_vita_run_character_probe_frame(
    const char* disc_path, struct melee_vita_bringup_info* info,
    enum melee_vita_character character, float animation_frame)
{
    if ((unsigned int) character >= MELEE_VITA_CHARACTER_COUNT) return -90;
    const struct character_config* config = &character_configs[character];
    struct bringup_disc_layout* layout = &cached_disc_layout[character];
    memset(info, 0, sizeof(*info));
    SceUID block = sceKernelAllocMemBlock(
        "melee_mem1", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MEM1_SIZE, NULL);
    if (block < 0) return -1;
    void* mem1 = NULL;
    if (sceKernelGetMemBlockBase(block, &mem1) < 0 || mem1 == NULL) {
        sceKernelFreeMemBlock(block);
        return -2;
    }
    info->mem1_address = (uintptr_t) mem1;
    info->mem1_size = MEM1_SIZE;

    FILE* file = fopen(disc_path, "rb");
    if (file == NULL) {
        sceKernelFreeMemBlock(block);
        return -3;
    }
    int banner_valid = 0;
    uint32_t fighter_archive_offset = 0;
    uint32_t fighter_archive_size = 0;
    uint32_t animation_archive_offset = 0;
    uint32_t animation_archive_size = 0;
    if (layout->valid) {
        info->fst_offset = layout->fst_offset;
        info->fst_size = layout->fst_size;
        info->fst_entries = layout->fst_entries;
        info->banner_offset = layout->banner_offset;
        info->banner_size = layout->banner_size;
        info->archive_offset = layout->costume_offset;
        info->archive_size = layout->costume_size;
        fighter_archive_offset = layout->fighter_offset;
        fighter_archive_size = layout->fighter_size;
        animation_archive_offset = layout->animation_offset;
        animation_archive_size = layout->animation_size;
        strncpy(info->archive_name, config->costume_archive,
                sizeof(info->archive_name) - 1u);
        banner_valid = 1;
    } else {
        unsigned char header[12];
        if (!read_at(file, DISC_HEADER_FST_OFFSET, header, sizeof(header))) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -4;
        }
        info->fst_offset = read_be32(header);
        info->fst_size = read_be32(header + 4);
        if (info->fst_offset == 0 || info->fst_size < 12) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -5;
        }

        unsigned char root[12];
        if (!read_at(file, info->fst_offset, root, sizeof(root)) ||
            (read_be32(root) >> 24) != 1) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -6;
        }
        info->fst_entries = read_be32(root + 8);
        const uint32_t strings =
            info->fst_offset + info->fst_entries * 12u;
        if (info->fst_entries == 0 ||
            strings >= info->fst_offset + info->fst_size) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -7;
        }

        for (uint32_t i = 1; i < info->fst_entries; ++i) {
        unsigned char entry[12];
        if (!read_at(file, info->fst_offset + i * 12u, entry, sizeof(entry))) break;
        const uint32_t type_name = read_be32(entry);
        if ((type_name >> 24) != 0) continue;
        const uint32_t name_offset = type_name & 0x00ffffffu;
        if (strings + name_offset >= info->fst_offset + info->fst_size) continue;
        char name[64];
        memset(name, 0, sizeof(name));
        if (fseek(file, (long) (strings + name_offset), SEEK_SET) != 0) continue;
        for (unsigned n = 0; n + 1 < sizeof(name); ++n) {
            const int c = fgetc(file);
            if (c <= 0) break;
            name[n] = (char) c;
        }
        if (strcmp(name, "opening.bnr") == 0) {
            info->banner_offset = read_be32(entry + 4);
            info->banner_size = read_be32(entry + 8);
            unsigned char magic[4];
            if (!read_at(file, info->banner_offset, magic, sizeof(magic)) ||
                (memcmp(magic, "BNR1", 4) != 0 && memcmp(magic, "BNR2", 4) != 0)) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -8;
            }
            banner_valid = 1;
        }
        const uint32_t candidate_size = read_be32(entry + 8);
        if (strcmp(name, config->fighter_archive) == 0 &&
            candidate_size >= 0x20u && candidate_size <= MEM1_SIZE) {
            fighter_archive_offset = read_be32(entry + 4u);
            fighter_archive_size = candidate_size;
        }
        if (strcmp(name, config->animation_archive) == 0 &&
            candidate_size >= 0x20u && candidate_size <= MEM1_SIZE) {
            const uint32_t candidate_offset = read_be32(entry + 4u);
            find_animation_segment(file, candidate_offset, candidate_size,
                                   config->idle_public,
                                   &animation_archive_offset,
                                   &animation_archive_size);
        }
        if (strcmp(name, config->costume_archive) == 0 &&
            candidate_size >= 0x20 && candidate_size <= MEM1_SIZE) {
            unsigned char candidate_header[0x20];
            const uint32_t candidate_offset = read_be32(entry + 4);
            if (read_at(file, candidate_offset, candidate_header,
                        sizeof(candidate_header)) &&
                read_be32(candidate_header) == candidate_size) {
                strncpy(info->archive_name, name,
                        sizeof(info->archive_name) - 1);
                info->archive_offset = candidate_offset;
                info->archive_size = candidate_size;
            }
        }
        }
    }
    if (!banner_valid || info->archive_size == 0 ||
        fighter_archive_size == 0 || animation_archive_size == 0) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -9;
    }
    if (!layout->valid) {
        layout->fst_offset = info->fst_offset;
        layout->fst_size = info->fst_size;
        layout->fst_entries = info->fst_entries;
        layout->banner_offset = info->banner_offset;
        layout->banner_size = info->banner_size;
        layout->costume_offset = info->archive_offset;
        layout->costume_size = info->archive_size;
        layout->fighter_offset = fighter_archive_offset;
        layout->fighter_size = fighter_archive_size;
        layout->animation_offset = animation_archive_offset;
        layout->animation_size = animation_archive_size;
        layout->valid = 1;
    }
    if (!read_at(file, info->archive_offset, mem1, info->archive_size)) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -10;
    }
    const unsigned char* archive = (const unsigned char*) mem1;
    const uint32_t declared_size = read_be32(archive);
    info->archive_data_size = read_be32(archive + 4);
    info->archive_relocations = read_be32(archive + 8);
    info->archive_publics = read_be32(archive + 12);
    info->archive_externs = read_be32(archive + 16);
    const uint64_t tables_end = UINT64_C(0x20) + info->archive_data_size +
        (uint64_t) info->archive_relocations * 4u +
        (uint64_t) info->archive_publics * 8u +
        (uint64_t) info->archive_externs * 8u;
    if (declared_size != info->archive_size || tables_end > info->archive_size) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -11;
    }
    unsigned char* data = (unsigned char*) mem1 + 0x20;
    const uint32_t reloc_table = 0x20u + info->archive_data_size;
    for (uint32_t i = 0; i < info->archive_relocations; ++i) {
        const uint32_t slot_offset = read_be32(archive + reloc_table + i * 4u);
        if (info->archive_data_size < 4u ||
            slot_offset > info->archive_data_size - 4u) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -12;
        }
        const uint32_t target_offset = read_be32(data + slot_offset);
        if (target_offset >= info->archive_data_size) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -13;
        }
        void* target = data + target_offset;
        memcpy(data + slot_offset, &target, sizeof(target));
        ++info->relocations_applied;
    }
    uint32_t first_public_offset = UINT32_MAX;
    if (info->archive_publics != 0) {
        const uint32_t public_table = reloc_table + info->archive_relocations * 4u;
        const uint32_t extern_table = public_table + info->archive_publics * 8u;
        const uint32_t symbols = extern_table + info->archive_externs * 8u;
        const uint32_t public_offset = read_be32(archive + public_table);
        const uint32_t symbol_offset = read_be32(archive + public_table + 4u);
        if (public_offset >= info->archive_data_size ||
            symbols + symbol_offset >= info->archive_size) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -14;
        }
        const char* symbol = (const char*) archive + symbols + symbol_offset;
        const size_t available = info->archive_size - symbols - symbol_offset;
        const void* terminator = memchr(symbol, '\0', available);
        if (terminator == NULL) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -15;
        }
        strncpy(info->public_name, symbol, sizeof(info->public_name) - 1);
        info->public_address = (uintptr_t) (data + public_offset);
        first_public_offset = public_offset;
    }
    if (info->archive_externs != 0) {
        melee_vita_runtime_symbols_reset();
        info->runtime_symbols_registered =
            (uint32_t) melee_vita_runtime_symbol_count();
        const uint32_t public_table = reloc_table + info->archive_relocations * 4u;
        const uint32_t extern_table = public_table + info->archive_publics * 8u;
        const uint32_t symbols = extern_table + info->archive_externs * 8u;
        for (uint32_t i = 0; i < info->archive_externs; ++i) {
            uint32_t slot_offset = read_be32(archive + extern_table + i * 8u);
            const uint32_t symbol_offset =
                read_be32(archive + extern_table + i * 8u + 4u);
            if (symbols + symbol_offset >= info->archive_size) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -16;
            }
            const char* symbol = (const char*) archive + symbols + symbol_offset;
            const size_t available = info->archive_size - symbols - symbol_offset;
            if (memchr(symbol, '\0', available) == NULL) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -17;
            }
            if (i == 0) {
                strncpy(info->extern_name, symbol,
                        sizeof(info->extern_name) - 1);
            }
            char provider[64] = { 0 };
            const char* owner = NULL;
            void* target = melee_vita_runtime_symbol_find(symbol, &owner);
            if (target != NULL && owner != NULL) {
                strncpy(provider, owner, sizeof(provider) - 1);
                ++info->runtime_symbol_hits;
            }
            if (target == NULL) {
                ++info->extern_symbols_nulled;
                strncpy(provider, "original:null-default",
                        sizeof(provider) - 1);
            }
            if (info->extern_symbols_resolved == 0) {
                strncpy(info->provider_name, provider,
                        sizeof(info->provider_name) - 1);
                info->extern_address = (uintptr_t) target;
            }
            uint32_t chain_steps = 0;
            while (slot_offset != UINT32_MAX) {
                if (info->archive_data_size < 4u ||
                    slot_offset > info->archive_data_size - 4u ||
                    ++chain_steps > info->archive_data_size / 4u) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -18;
                }
                const uint32_t next = read_be32(data + slot_offset);
                memcpy(data + slot_offset, &target, sizeof(target));
                ++info->extern_slots_patched;
                slot_offset = next;
            }
            ++info->extern_symbols_resolved;
        }
    }

    /* Match the normal fighter draw pass. ftParts_8007487C hides every DObj
     * named by visibility groups 0, 1 and 3. ftDrawCommon then enables group
     * 0, and the Wait action selects its normal (zero) model variant. This is
     * the important difference from drawing every DObj in PlSsNr.dat at once. */
    uint8_t dobj_visible[MAX_FIGHTER_DOBJS];
    memset(dobj_visible, 1, sizeof(dobj_visible));
    const uint32_t fighter_storage_offset =
        (info->archive_size + 31u) & ~31u;
    if (fighter_storage_offset > MEM1_SIZE ||
        fighter_archive_size > MEM1_SIZE - fighter_storage_offset) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -67;
    }
    unsigned char* fighter_data = NULL;
    uint32_t fighter_data_size = 0;
    void* fighter_public = NULL;
    if (!load_relocated_archive(
            file, fighter_archive_offset, fighter_archive_size,
            (unsigned char*) mem1 + fighter_storage_offset,
            MEM1_SIZE - fighter_storage_offset, config->fighter_public, &fighter_data,
            &fighter_data_size, &fighter_public)) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -68;
    }
    void* parts_pointer = NULL;
    if (!pointer_range_in_data(fighter_public, 0x0cu, fighter_data,
                               fighter_data_size)) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -69;
    }
    memcpy(&parts_pointer, (unsigned char*) fighter_public + 8u,
           sizeof(parts_pointer));
    if (!pointer_range_in_data(parts_pointer, 8u, fighter_data,
                               fighter_data_size)) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -70;
    }
    const unsigned char* parts = parts_pointer;
    const uint32_t model_count = read_be32(parts);
    void* table_pointer = NULL;
    memcpy(&table_pointer, parts + 4u, sizeof(table_pointer));
    if (model_count > 11u ||
        !pointer_range_in_data(table_pointer, 16u, fighter_data,
                               fighter_data_size)) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -71;
    }
    info->fighter_model_groups = model_count;
    info->fighter_model_variant = 0u;
    if (model_count != 0u) {
        const unsigned char* visibility_table = table_pointer;
        for (uint32_t category = 0; category < 4u; ++category) {
            if (category == 2u) continue; /* Separate x203C DObj list. */
            void* lookup = NULL;
            memcpy(&lookup, visibility_table + category * 4u,
                   sizeof(lookup));
            if (!set_visibility_group(lookup, model_count, -1, dobj_visible,
                                      fighter_data, fighter_data_size, 0)) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -72;
            }
        }
        void* normal_lookup = NULL;
        memcpy(&normal_lookup, visibility_table, sizeof(normal_lookup));
        if (!set_visibility_group(normal_lookup, model_count, 0, dobj_visible,
                                  fighter_data, fighter_data_size, 1)) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -73;
        }
    }

    const uint32_t animation_storage_offset =
        (fighter_storage_offset + fighter_archive_size + 31u) & ~31u;
    if (animation_storage_offset > MEM1_SIZE ||
        animation_archive_size > MEM1_SIZE - animation_storage_offset) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -75;
    }
    unsigned char* animation_data = NULL;
    uint32_t animation_data_size = 0;
    void* wait_animation = NULL;
    if (!load_relocated_archive(
            file, animation_archive_offset, animation_archive_size,
            (unsigned char*) mem1 + animation_storage_offset,
            MEM1_SIZE - animation_storage_offset, config->idle_public,
            &animation_data, &animation_data_size, &wait_animation)) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -76;
    }
    static struct melee_vita_joint_pose
        joint_poses[MELEE_VITA_FIGATREE_MAX_JOINTS];
    info->animation_frame = animation_frame;
    const int animation_result = melee_vita_figatree_sample(
        wait_animation, animation_data, animation_data_size,
        animation_frame, joint_poses, MELEE_VITA_FIGATREE_MAX_JOINTS,
        &info->animation_joint_count, &info->animation_track_count);
    if (animation_result != 0) {
        fclose(file);
        sceKernelFreeMemBlock(block);
        return -80 + animation_result;
    }
    void* joint_root = NULL;
    if (strcmp(info->public_name, config->fighter_public) == 0) {
        if (first_public_offset == UINT32_MAX ||
            first_public_offset > info->archive_data_size - 0x60u) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -20;
        }
        unsigned char* fighter = data + first_public_offset;
        for (uint32_t offset = 0; offset < 0x60u; offset += 4u) {
            void* pointer = NULL;
            memcpy(&pointer, fighter + offset, sizeof(pointer));
            ++info->fighter_root_pointers;
            if (pointer == NULL ||
                ((unsigned char*) pointer >= data &&
                 (unsigned char*) pointer < data + info->archive_data_size)) {
                ++info->fighter_root_pointers_in_bounds;
            }
        }
        void* ext_attributes = NULL;
        void* parts = NULL;
        void* hurtboxes = NULL;
        void* joint = NULL;
        memcpy(&ext_attributes, fighter + 4u, sizeof(ext_attributes));
        memcpy(&parts, fighter + 8u, sizeof(parts));
        memcpy(&hurtboxes, fighter + 0x30u, sizeof(hurtboxes));
        memcpy(&joint, fighter + 0x5cu, sizeof(joint));
        info->fighter_ext_attributes = (uintptr_t) ext_attributes;
        info->fighter_parts = (uintptr_t) parts;
        info->fighter_joint = (uintptr_t) joint;
        if (info->fighter_root_pointers_in_bounds !=
                info->fighter_root_pointers ||
            parts == NULL || (unsigned char*) parts + 0x18u >
                                 data + info->archive_data_size ||
            joint == NULL) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -21;
        }
        memcpy(info->fighter_part_indices, (unsigned char*) parts + 0x10u,
               sizeof(info->fighter_part_indices));
        if (hurtboxes != NULL) {
            if ((unsigned char*) hurtboxes + 8u >
                data + info->archive_data_size) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -22;
            }
            info->fighter_hurtbox_count = read_be32(hurtboxes);
            if (info->fighter_hurtbox_count > 64u) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -23;
            }
        }
        joint_root = joint;
    } else if (strcmp(info->public_name, config->costume_public) == 0) {
        joint_root = (void*) info->public_address;
    }
    if (joint_root != NULL) {
        struct joint_record {
            unsigned char* joint;
            float world[12];
        };
        struct polygon_task {
            unsigned char* pobj;
            float owner_world[12];
            uint8_t visible;
            uint32_t image_offset;
            float texture_scale[2];
            float texture_translate[2];
        };
        struct joint_walk_entry {
            unsigned char* joint;
            uint32_t depth;
            float parent_world[12];
            float parent_scale[3];
            int parent_scale_valid;
        };
        struct joint_walk_entry stack[512];
        unsigned char* visited[512];
        static struct joint_record joint_records[512];
        static struct polygon_task polygon_tasks[512];
        uint32_t stack_size = 0;
        uint32_t visited_count = 0;
        uint32_t joint_record_count = 0;
        uint32_t polygon_task_count = 0;
        uint32_t fighter_dobj_index = 0;
        struct joint_walk_entry root_entry;
        memset(&root_entry, 0, sizeof(root_entry));
        root_entry.joint = (unsigned char*) joint_root;
        root_entry.depth = 1u;
        matrix_identity(root_entry.parent_world);
        stack[stack_size++] = root_entry;
        while (stack_size != 0) {
            const struct joint_walk_entry entry = stack[--stack_size];
            const uint32_t joint_index = visited_count;
            if (!pointer_range_in_data(entry.joint, 0x40u, data,
                                       info->archive_data_size)) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -24;
            }
            for (uint32_t i = 0; i < visited_count; ++i) {
                if (visited[i] == entry.joint) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -25;
                }
            }
            if (visited_count == 512u) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -26;
            }
            visited[visited_count++] = entry.joint;
            ++info->joint_count;
            if (entry.depth > info->joint_max_depth)
                info->joint_max_depth = entry.depth;

            void* class_name = NULL;
            void* child = NULL;
            void* next = NULL;
            void* display = NULL;
            memcpy(&class_name, entry.joint, sizeof(class_name));
            memcpy(&child, entry.joint + 8u, sizeof(child));
            memcpy(&next, entry.joint + 0x0cu, sizeof(next));
            memcpy(&display, entry.joint + 0x10u, sizeof(display));
            if (!pointer_range_in_data(child, child == NULL ? 0u : 0x40u,
                                       data, info->archive_data_size) ||
                !pointer_range_in_data(next, next == NULL ? 0u : 0x40u,
                                       data, info->archive_data_size) ||
                !pointer_range_in_data(display, display == NULL ? 0u : 1u, data,
                                       info->archive_data_size)) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -27;
            }
            int transforms_finite = 1;
            for (uint32_t offset = 0x14u; offset < 0x38u; offset += 4u) {
                if (!be_float_is_finite(entry.joint + offset)) {
                    transforms_finite = 0;
                    break;
                }
            }
            if (!transforms_finite) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -29;
            }
            const uint32_t joint_flags = read_be32(entry.joint + 4u);
            if ((joint_flags & 0x00020000u) != 0u) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -50;
            }
            float rotation[3], scale[3], translation[3];
            for (uint32_t axis = 0; axis < 3u; ++axis) {
                rotation[axis] = read_be_float(entry.joint + 0x14u + axis * 4u);
                scale[axis] = read_be_float(entry.joint + 0x20u + axis * 4u);
                translation[axis] =
                    read_be_float(entry.joint + 0x2cu + axis * 4u);
                if (joint_index < info->animation_joint_count) {
                    const struct melee_vita_joint_pose* pose =
                        &joint_poses[joint_index];
                    if ((pose->value_mask & (1u << axis)) != 0u)
                        rotation[axis] = pose->rotation[axis];
                    if ((pose->value_mask & (1u << (4u + axis))) != 0u)
                        translation[axis] = pose->translation[axis];
                    if ((pose->value_mask & (1u << (7u + axis))) != 0u)
                        scale[axis] = pose->scale[axis];
                }
            }
            float local_matrix[12], world_matrix[12];
            matrix_srt(local_matrix, scale, rotation, translation,
                       entry.parent_scale_valid ? entry.parent_scale : NULL);
            matrix_concat(entry.parent_world, local_matrix, world_matrix);
            joint_records[joint_record_count].joint = entry.joint;
            memcpy(joint_records[joint_record_count].world, world_matrix,
                   sizeof(world_matrix));
            ++joint_record_count;
            float world_scale[3];
            int world_scale_valid;
            if ((joint_flags & 0x00000008u) != 0u) {
                memcpy(world_scale, entry.parent_scale, sizeof(world_scale));
                world_scale_valid = entry.parent_scale_valid;
            } else {
                for (uint32_t axis = 0; axis < 3u; ++axis) {
                    world_scale[axis] = scale[axis] *
                        (entry.parent_scale_valid ? entry.parent_scale[axis]
                                                  : 1.0f);
                }
                world_scale_valid = 1;
            }
            ++info->joint_transform_count;
            if (class_name != NULL) {
                if (!pointer_range_in_data(class_name, 1u, data,
                                           info->archive_data_size) ||
                    memchr(class_name, '\0',
                           (size_t) (data + info->archive_data_size -
                                    (unsigned char*) class_name)) == NULL) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -28;
                }
                ++info->joint_named_count;
            }
            if (display != NULL) {
                ++info->joint_display_count;
                unsigned char* dobj = (unsigned char*) display;
                uint32_t dobj_steps = 0;
                while (dobj != NULL) {
                    if (!pointer_range_in_data(dobj, 0x10u, data,
                                               info->archive_data_size) ||
                        ++dobj_steps > 512u) {
                        fclose(file);
                        sceKernelFreeMemBlock(block);
                        return -31;
                    }
                    ++info->display_object_count;
                    if (fighter_dobj_index >= MAX_FIGHTER_DOBJS) {
                        fclose(file);
                        sceKernelFreeMemBlock(block);
                        return -74;
                    }
                    const uint8_t this_dobj_visible =
                        dobj_visible[fighter_dobj_index++];
                    if (this_dobj_visible)
                        ++info->fighter_dobjs_visible;
                    else
                        ++info->fighter_dobjs_hidden;
                    void* next_dobj = NULL;
                    void* material = NULL;
                    void* polygon = NULL;
                    memcpy(&next_dobj, dobj + 4u, sizeof(next_dobj));
                    memcpy(&material, dobj + 8u, sizeof(material));
                    memcpy(&polygon, dobj + 0x0cu, sizeof(polygon));
                    uint32_t primary_image_offset = UINT32_MAX;
                    float primary_texture_scale[2] = { 1.0f, 1.0f };
                    float primary_texture_translate[2] = { 0.0f, 0.0f };
                    if (material != NULL) {
                        if (!pointer_range_in_data(material, 0x18u, data,
                                                   info->archive_data_size)) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -32;
                        }
                        ++info->material_count;
                        void* texture = NULL;
                        memcpy(&texture, (unsigned char*) material + 8u,
                               sizeof(texture));
                        uint32_t texture_steps = 0;
                        while (texture != NULL) {
                            unsigned char* tobj = (unsigned char*) texture;
                            if (!pointer_range_in_data(tobj, 0x5cu, data,
                                                       info->archive_data_size) ||
                                ++texture_steps > 64u) {
                                fclose(file);
                                sceKernelFreeMemBlock(block);
                                return -33;
                            }
                            ++info->texture_count;
                            if (texture_steps == 1u) {
                                primary_texture_scale[0] =
                                    read_be_float(tobj + 0x1cu);
                                primary_texture_scale[1] =
                                    read_be_float(tobj + 0x20u);
                                primary_texture_translate[0] =
                                    read_be_float(tobj + 0x28u);
                                primary_texture_translate[1] =
                                    read_be_float(tobj + 0x2cu);
                            }
                            void* next_texture = NULL;
                            void* image = NULL;
                            memcpy(&next_texture, tobj + 4u,
                                   sizeof(next_texture));
                            memcpy(&image, tobj + 0x4cu, sizeof(image));
                            if (image != NULL) {
                                unsigned char* image_desc =
                                    (unsigned char*) image;
                                if (!pointer_range_in_data(
                                        image_desc, 0x18u, data,
                                        info->archive_data_size)) {
                                    fclose(file);
                                    sceKernelFreeMemBlock(block);
                                    return -34;
                                }
                                if (primary_image_offset == UINT32_MAX)
                                    primary_image_offset =
                                        (uint32_t) (image_desc - data);
                                void* pixels = NULL;
                                memcpy(&pixels, image_desc, sizeof(pixels));
                                const uint32_t width = read_be16(image_desc + 4u);
                                const uint32_t height = read_be16(image_desc + 6u);
                                if (width == 0 || height == 0 || width > 4096u ||
                                    height > 4096u ||
                                    !pointer_range_in_data(
                                        pixels, pixels == NULL ? 0u : 1u,
                                        data, info->archive_data_size)) {
                                    fclose(file);
                                    sceKernelFreeMemBlock(block);
                                    return -35;
                                }
                                ++info->image_count;
                                if (width > info->max_texture_width)
                                    info->max_texture_width = width;
                                if (height > info->max_texture_height)
                                    info->max_texture_height = height;
                            }
                            texture = next_texture;
                        }
                    }
                    uint32_t polygon_steps = 0;
                    while (polygon != NULL) {
                        unsigned char* pobj = (unsigned char*) polygon;
                        if (!pointer_range_in_data(pobj, 0x18u, data,
                                                   info->archive_data_size) ||
                            ++polygon_steps > 512u) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -36;
                        }
                        ++info->polygon_object_count;
                        void* next_polygon = NULL;
                        void* display_list = NULL;
                        memcpy(&next_polygon, pobj + 4u, sizeof(next_polygon));
                        memcpy(&display_list, pobj + 0x10u, sizeof(display_list));
                        const uint32_t display_size =
                            (uint32_t) read_be16(pobj + 0x0eu) << 5;
                        if (display_size != 0 &&
                            !pointer_range_in_data(display_list, display_size,
                                                   data,
                                                   info->archive_data_size)) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -37;
                        }
                        info->display_list_bytes += display_size;
                        const int gx_result = melee_vita_gx_decode(
                            pobj, data, info->archive_data_size, &info->gx);
                        if (gx_result != 0) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -40 + gx_result;
                        }
                        if (polygon_task_count == 512u) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -52;
                        }
                        polygon_tasks[polygon_task_count].pobj = pobj;
                        memcpy(polygon_tasks[polygon_task_count].owner_world,
                               world_matrix, sizeof(world_matrix));
                        polygon_tasks[polygon_task_count].visible =
                            this_dobj_visible;
                        polygon_tasks[polygon_task_count].image_offset =
                            primary_image_offset;
                        memcpy(polygon_tasks[polygon_task_count].texture_scale,
                               primary_texture_scale,
                               sizeof(primary_texture_scale));
                        memcpy(
                            polygon_tasks[polygon_task_count].texture_translate,
                            primary_texture_translate,
                            sizeof(primary_texture_translate));
                        ++polygon_task_count;
                        polygon = next_polygon;
                    }
                    dobj = (unsigned char*) next_dobj;
                }
            }
            if (next != NULL) {
                if (stack_size == 512u) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -30;
                }
                struct joint_walk_entry next_entry = entry;
                next_entry.joint = (unsigned char*) next;
                next_entry.depth = entry.depth;
                stack[stack_size++] = next_entry;
            }
            if (child != NULL) {
                if (stack_size == 512u) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -30;
                }
                struct joint_walk_entry child_entry;
                memset(&child_entry, 0, sizeof(child_entry));
                child_entry.joint = (unsigned char*) child;
                child_entry.depth = entry.depth + 1u;
                memcpy(child_entry.parent_world, world_matrix,
                       sizeof(child_entry.parent_world));
                memcpy(child_entry.parent_scale, world_scale,
                       sizeof(child_entry.parent_scale));
                child_entry.parent_scale_valid = world_scale_valid;
                stack[stack_size++] = child_entry;
            }
        }
        for (uint32_t task_index = 0; task_index < polygon_task_count;
             ++task_index) {
            if (info->mesh_vertex_count == MELEE_VITA_MESH_VERTEX_CAPACITY) {
                info->mesh_truncated = 1u;
                break;
            }
            struct polygon_task* task = &polygon_tasks[task_index];
            if (!task->visible) continue;
            const uint32_t first_vertex = info->mesh_vertex_count;
            uint32_t extracted = 0;
            const int extract_result = melee_vita_gx_extract_positions(
                task->pobj, data, info->archive_data_size,
                info->mesh_positions + first_vertex * 3u,
                info->mesh_texture_coordinates + first_vertex * 2u,
                info->mesh_matrix_indices + first_vertex,
                MELEE_VITA_MESH_VERTEX_CAPACITY - first_vertex, &extracted);
            if (extract_result != 0 && extract_result != -7) {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -53;
            }
            if (extract_result == -7) info->mesh_truncated = 1u;
            for (uint32_t i = 0; i < extracted; ++i)
                info->mesh_image_offsets[first_vertex + i] =
                    task->image_offset;
            const uint16_t pobj_flags = read_be16(task->pobj + 0x0cu);
            const uint8_t cull_mode = (uint8_t) (pobj_flags >> 14u);
            for (uint32_t i = 0; i < extracted; ++i)
                info->mesh_cull_modes[first_vertex + i] = cull_mode;
            for (uint32_t i = 0; i < extracted; ++i) {
                float* uv = info->mesh_texture_coordinates +
                            (first_vertex + i) * 2u;
                uv[0] = uv[0] * task->texture_scale[0] +
                        task->texture_translate[0];
                uv[1] = uv[1] * task->texture_scale[1] +
                        task->texture_translate[1];
            }

            const uint32_t pobj_type = read_be16(task->pobj + 0x0cu) & 0x3000u;
            void* pobj_binding = NULL;
            memcpy(&pobj_binding, task->pobj + 0x14u, sizeof(pobj_binding));
            if (pobj_type == 0x2000u) {
                ++info->mesh_envelope_pobjs;
                if (pobj_binding == NULL ||
                    !pointer_range_in_data(pobj_binding, 4u, data,
                                           info->archive_data_size)) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -54;
                }
                float envelope_matrices[10][12];
                uint32_t envelope_count = 0;
                unsigned char* envelope_array = pobj_binding;
                while (envelope_count < 10u) {
                    if (!pointer_range_in_data(envelope_array +
                                                   envelope_count * 4u,
                                               4u, data,
                                               info->archive_data_size)) {
                        fclose(file);
                        sceKernelFreeMemBlock(block);
                        return -55;
                    }
                    void* envelope_desc = NULL;
                    memcpy(&envelope_desc,
                           envelope_array + envelope_count * 4u,
                           sizeof(envelope_desc));
                    if (envelope_desc == NULL) break;
                    if (!pointer_range_in_data(envelope_desc, 8u, data,
                                               info->archive_data_size)) {
                        fclose(file);
                        sceKernelFreeMemBlock(block);
                        return -56;
                    }
                    unsigned char* desc = envelope_desc;
                    void* first_joint = NULL;
                    memcpy(&first_joint, desc, sizeof(first_joint));
                    const float first_weight = read_be_float(desc + 4u);
                    void* second_joint = NULL;
                    if (pointer_range_in_data(desc + 8u, 4u, data,
                                              info->archive_data_size))
                        memcpy(&second_joint, desc + 8u, sizeof(second_joint));
                    if (first_joint == NULL) {
                        fclose(file);
                        sceKernelFreeMemBlock(block);
                        return -57;
                    }
                    memset(envelope_matrices[envelope_count], 0,
                           sizeof(envelope_matrices[envelope_count]));
                    uint32_t envelope_steps = 0;
                    while (first_joint != NULL) {
                        if (!pointer_range_in_data(desc, 8u, data,
                                                   info->archive_data_size) ||
                            ++envelope_steps > 64u) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -58;
                        }
                        void* joint = NULL;
                        memcpy(&joint, desc, sizeof(joint));
                        const float weight = read_be_float(desc + 4u);
                        const float* joint_world = NULL;
                        for (uint32_t record = 0;
                             record < joint_record_count; ++record) {
                            if (joint_records[record].joint == joint) {
                                joint_world = joint_records[record].world;
                                break;
                            }
                        }
                        if (joint_world == NULL || !isfinite(weight)) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -59;
                        }
                        if (envelope_steps == 1u && second_joint == NULL &&
                            first_weight >= 0.999999f) {
                            memcpy(envelope_matrices[envelope_count],
                                   joint_world,
                                   sizeof(envelope_matrices[envelope_count]));
                        } else {
                            void* inverse_pointer = NULL;
                            memcpy(&inverse_pointer,
                                   (unsigned char*) joint + 0x38u,
                                   sizeof(inverse_pointer));
                            float inverse_bind[12], weighted[12];
                            if (!read_disc_matrix(inverse_pointer, data,
                                                  info->archive_data_size,
                                                  inverse_bind)) {
                                fclose(file);
                                sceKernelFreeMemBlock(block);
                                return -60;
                            }
                            matrix_concat(joint_world, inverse_bind, weighted);
                            matrix_scaled_add(
                                weighted,
                                envelope_matrices[envelope_count], weight);
                        }
                        desc += 8u;
                        if (!pointer_range_in_data(desc, 4u, data,
                                                   info->archive_data_size)) {
                            fclose(file);
                            sceKernelFreeMemBlock(block);
                            return -61;
                        }
                        memcpy(&first_joint, desc, sizeof(first_joint));
                    }
                    ++envelope_count;
                }
                if (envelope_count == 0u) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -62;
                }
                for (uint32_t i = 0; i < extracted; ++i) {
                    const uint32_t raw_index =
                        info->mesh_matrix_indices[first_vertex + i];
                    if (raw_index % 3u != 0u ||
                        raw_index / 3u >= envelope_count) {
                        fclose(file);
                        sceKernelFreeMemBlock(block);
                        return -63;
                    }
                    transform_positions(
                        info->mesh_positions + (first_vertex + i) * 3u, 1u,
                        envelope_matrices[raw_index / 3u]);
                }
                info->mesh_skinned_vertices += extracted;
            } else if (pobj_type == 0x0000u && pobj_binding != NULL) {
                ++info->mesh_shared_pobjs;
                const float* shared_world = NULL;
                for (uint32_t record = 0; record < joint_record_count;
                     ++record) {
                    if (joint_records[record].joint == pobj_binding) {
                        shared_world = joint_records[record].world;
                        break;
                    }
                }
                if (shared_world == NULL) {
                    fclose(file);
                    sceKernelFreeMemBlock(block);
                    return -64;
                }
                for (uint32_t i = 0; i < extracted; ++i) {
                    const uint32_t raw_index =
                        info->mesh_matrix_indices[first_vertex + i];
                    if (raw_index != 0u && raw_index != 3u) {
                        fclose(file);
                        sceKernelFreeMemBlock(block);
                        return -65;
                    }
                    transform_positions(
                        info->mesh_positions + (first_vertex + i) * 3u, 1u,
                        raw_index == 0u ? task->owner_world : shared_world);
                }
                info->mesh_skinned_vertices += extracted;
            } else if (pobj_type == 0x0000u || pobj_type == 0x1000u) {
                ++info->mesh_rigid_pobjs;
                transform_positions(info->mesh_positions + first_vertex * 3u,
                                    extracted, task->owner_world);
            } else {
                fclose(file);
                sceKernelFreeMemBlock(block);
                return -66;
            }
            info->mesh_vertex_count += extracted;
        }
        if (info->mesh_truncated != 0u) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -51;
        }
        if (info->mesh_vertex_count == 0) {
            fclose(file);
            sceKernelFreeMemBlock(block);
            return -49;
        }
    }
    fclose(file);
    sceKernelFreeMemBlock(block);
    return 0;
}

int melee_vita_run_bringup_probe(const char* disc_path,
                                 struct melee_vita_bringup_info* info)
{
    return melee_vita_run_character_probe_frame(
        disc_path, info, MELEE_VITA_CHARACTER_SAMUS, 0.0f);
}

int melee_vita_run_bringup_probe_frame(
    const char* disc_path, struct melee_vita_bringup_info* info,
    float animation_frame)
{
    return melee_vita_run_character_probe_frame(
        disc_path, info, MELEE_VITA_CHARACTER_SAMUS, animation_frame);
}

int melee_vita_run_character_probe(
    const char* disc_path, struct melee_vita_bringup_info* info,
    enum melee_vita_character character)
{
    return melee_vita_run_character_probe_frame(
        disc_path, info, character, 0.0f);
}
