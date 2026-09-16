#pragma once

#include <stdint.h>

#include "gx_decoder.h"

struct melee_vita_bringup_info {
    uintptr_t mem1_address;
    uint32_t mem1_size;
    uint32_t fst_offset;
    uint32_t fst_size;
    uint32_t fst_entries;
    uint32_t banner_offset;
    uint32_t banner_size;
    char archive_name[64];
    uint32_t archive_offset;
    uint32_t archive_size;
    uint32_t archive_data_size;
    uint32_t archive_relocations;
    uint32_t archive_publics;
    uint32_t archive_externs;
    uint32_t relocations_applied;
    char public_name[64];
    uintptr_t public_address;
    uint32_t extern_symbols_resolved;
    uint32_t extern_symbols_unresolved;
    uint32_t extern_symbols_nulled;
    uint32_t extern_slots_patched;
    char extern_name[64];
    char provider_name[64];
    uintptr_t extern_address;
    uint32_t provider_relocations;
    uint32_t runtime_symbols_registered;
    uint32_t runtime_symbol_hits;
    uint32_t fighter_root_pointers;
    uint32_t fighter_root_pointers_in_bounds;
    uintptr_t fighter_ext_attributes;
    uintptr_t fighter_parts;
    uintptr_t fighter_joint;
    uint32_t fighter_hurtbox_count;
    uint8_t fighter_part_indices[5];
    uint32_t joint_count;
    uint32_t joint_max_depth;
    uint32_t joint_named_count;
    uint32_t joint_display_count;
    uint32_t joint_transform_count;
    uint32_t display_object_count;
    uint32_t material_count;
    uint32_t texture_count;
    uint32_t image_count;
    uint32_t polygon_object_count;
    uint32_t display_list_bytes;
    uint32_t max_texture_width;
    uint32_t max_texture_height;
    struct melee_vita_gx_stats gx;
#define MELEE_VITA_MESH_VERTEX_CAPACITY 32768u
    uint32_t mesh_vertex_count;
    uint32_t mesh_truncated;
    uint32_t mesh_rigid_pobjs;
    uint32_t mesh_shared_pobjs;
    uint32_t mesh_envelope_pobjs;
    uint32_t mesh_skinned_vertices;
    uint32_t fighter_model_groups;
    uint32_t fighter_model_variant;
    uint32_t fighter_dobjs_visible;
    uint32_t fighter_dobjs_hidden;
    uint32_t animation_joint_count;
    uint32_t animation_track_count;
    float animation_frame;
    uint8_t mesh_matrix_indices[MELEE_VITA_MESH_VERTEX_CAPACITY];
    uint8_t mesh_cull_modes[MELEE_VITA_MESH_VERTEX_CAPACITY];
    float mesh_positions[MELEE_VITA_MESH_VERTEX_CAPACITY * 3u];
    float mesh_texture_coordinates[MELEE_VITA_MESH_VERTEX_CAPACITY * 2u];
    uint32_t mesh_image_offsets[MELEE_VITA_MESH_VERTEX_CAPACITY];
};

enum melee_vita_character {
    MELEE_VITA_CHARACTER_SAMUS = 0,
    MELEE_VITA_CHARACTER_MASTER_HAND = 1,
    MELEE_VITA_CHARACTER_COUNT = 2,
};

int melee_vita_run_bringup_probe(const char* disc_path,
                                 struct melee_vita_bringup_info* info);
int melee_vita_run_bringup_probe_frame(
    const char* disc_path, struct melee_vita_bringup_info* info,
    float animation_frame);
int melee_vita_run_character_probe(
    const char* disc_path, struct melee_vita_bringup_info* info,
    enum melee_vita_character character);
int melee_vita_run_character_probe_frame(
    const char* disc_path, struct melee_vita_bringup_info* info,
    enum melee_vita_character character, float animation_frame);
