#include "disc_probe.h"
#include "bringup_probe.h"
#include "vita_log.h"
#include "gxm_probe.h"
#include "texture_atlas.h"

#include <debugScreen.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMUS_WAIT_FRAME_COUNT 81u
#define MASTER_HAND_WAIT_FRAME_COUNT 120u
#define WAIT_CACHE_VERSION 1u
#define WAIT_CACHE_DIRECTORY "ux0:data/melee/cache"

struct character_setup {
    const char* name;
    enum melee_vita_character character;
    uint32_t frame_count;
    uint32_t cache_magic;
    const char* cache_path;
    const char* cache_temp_path;
};

static const struct character_setup character_setups[] = {
    {
        "Samus", MELEE_VITA_CHARACTER_SAMUS, SAMUS_WAIT_FRAME_COUNT,
        UINT32_C(0x4d574131),
        WAIT_CACHE_DIRECTORY "/samus_wait_vertices.bin",
        WAIT_CACHE_DIRECTORY "/samus_wait_vertices.tmp",
    },
    {
        "Master Hand", MELEE_VITA_CHARACTER_MASTER_HAND,
        MASTER_HAND_WAIT_FRAME_COUNT, UINT32_C(0x4d484131),
        WAIT_CACHE_DIRECTORY "/masterhand_wait2_vertices.bin",
        WAIT_CACHE_DIRECTORY "/masterhand_wait2_vertices.tmp",
    },
};

struct wait_cache_header {
    uint32_t magic;
    uint32_t version;
    uint32_t vertex_count;
    uint32_t frame_count;
    uint32_t floats_per_frame;
};

static float* load_wait_cache(const struct character_setup* setup,
                              uint32_t vertex_count)
{
    FILE* file = fopen(setup->cache_path, "rb");
    if (file == NULL) return NULL;
    struct wait_cache_header header;
    const size_t floats_per_frame = (size_t) vertex_count * 3u;
    const size_t total_floats = floats_per_frame * setup->frame_count;
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        header.magic != setup->cache_magic ||
        header.version != WAIT_CACHE_VERSION ||
        header.vertex_count != vertex_count ||
        header.frame_count != setup->frame_count ||
        header.floats_per_frame != floats_per_frame) {
        fclose(file);
        return NULL;
    }
    float* positions = malloc(total_floats * sizeof(float));
    if (positions == NULL ||
        fread(positions, sizeof(float), total_floats, file) != total_floats ||
        fgetc(file) != EOF) {
        free(positions);
        fclose(file);
        return NULL;
    }
    fclose(file);
    return positions;
}

static int save_wait_cache(const struct character_setup* setup,
                           const float* positions, uint32_t vertex_count)
{
    sceIoMkdir(WAIT_CACHE_DIRECTORY, 0777);
    FILE* file = fopen(setup->cache_temp_path, "wb");
    if (file == NULL) return 0;
    const size_t floats_per_frame = (size_t) vertex_count * 3u;
    const size_t total_floats = floats_per_frame * setup->frame_count;
    const struct wait_cache_header header = {
        setup->cache_magic, WAIT_CACHE_VERSION, vertex_count,
        setup->frame_count, (uint32_t) floats_per_frame,
    };
    const int written =
        fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
        fwrite(positions, sizeof(float), total_floats, file) == total_floats &&
        fflush(file) == 0;
    fclose(file);
    if (!written) {
        remove(setup->cache_temp_path);
        return 0;
    }
    remove(setup->cache_path);
    if (rename(setup->cache_temp_path, setup->cache_path) != 0) {
        remove(setup->cache_temp_path);
        return 0;
    }
    return 1;
}

int melee_vita_big_endian_storage_order_works(void);

static void wait_for_start(void)
{
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_START) != 0) {
            return;
        }
        sceKernelDelayThread(16 * 1000);
    }
}

int main(void)
{
    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    /* DebugNet is owned by this long-lived main thread. The current
     * VitaDebugger logger has bounded asynchronous writes and a hardened
     * shutdown path, so it is safe to use for bring-up diagnostics. */
    const int logging = melee_vita_log_start() == 0;

    psvDebugScreenPrintf("Melee Vita bring-up\n\n");
    if (logging) melee_vita_log_info("MELEE_VITA START");
    if (!melee_vita_big_endian_storage_order_works()) {
        psvDebugScreenPrintf("FAIL: GCC big-endian structure support is broken.\n");
        psvDebugScreenPrintf("Press START to exit.\n");
        wait_for_start();
        if (logging) melee_vita_log_stop();
        psvDebugScreenFinish();
        sceKernelExitProcess(2);
        return 2;
    }
    psvDebugScreenPrintf("PASS: big-endian disc structure support\n");

    struct melee_vita_disc_info disc;
    const enum melee_vita_disc_result result =
        melee_vita_probe_disc(MELEE_VITA_DISC_PATH, &disc);

    psvDebugScreenPrintf("Disc: %s\n", MELEE_VITA_DISC_PATH);
    psvDebugScreenPrintf("Size: %llu bytes\n", (unsigned long long) disc.size);
    if (disc.game_id[0] != '\0') {
        psvDebugScreenPrintf("ID: %s  disc=%u revision=%u\n",
                             disc.game_id, disc.disc_number, disc.revision);
    }
    psvDebugScreenPrintf("\n%s\n", melee_vita_disc_result_text(result));
    if (logging) {
        melee_vita_log_info("ABI PASS");
        melee_vita_log_info("DISC path=%s size=%llu id=%s disc=%u revision=%u result=%d",
                            MELEE_VITA_DISC_PATH, (unsigned long long) disc.size,
                            disc.game_id, disc.disc_number, disc.revision, result);
    }

    static struct melee_vita_bringup_info bringup;
    const int bringup_result = result == MELEE_VITA_DISC_OK
        ? melee_vita_run_bringup_probe(MELEE_VITA_DISC_PATH, &bringup)
        : -100;
    psvDebugScreenPrintf("MEM1/FST probe: %s (%d)\n",
                         bringup_result == 0 ? "PASS" : "FAIL", bringup_result);
    if (logging) {
        melee_vita_log_info("MEM1 address=0x%08x size=%u",
                            (unsigned) bringup.mem1_address, bringup.mem1_size);
        melee_vita_log_info("FST offset=0x%08x size=%u entries=%u",
                            bringup.fst_offset, bringup.fst_size, bringup.fst_entries);
        melee_vita_log_info("BANNER offset=0x%08x size=%u result=%d",
                            bringup.banner_offset, bringup.banner_size, bringup_result);
        melee_vita_log_info("ARCHIVE name=%s offset=0x%08x size=%u data=%u reloc=%u public=%u extern=%u",
                            bringup.archive_name, bringup.archive_offset,
                            bringup.archive_size, bringup.archive_data_size,
                            bringup.archive_relocations, bringup.archive_publics,
                            bringup.archive_externs);
        melee_vita_log_info("RELOC applied=%u public_name=%s public_address=0x%08x",
                            bringup.relocations_applied, bringup.public_name,
                            (unsigned) bringup.public_address);
        melee_vita_log_info("EXTERN resolved=%u unresolved=%u nulled=%u slots=%u first_name=%s address=0x%08x",
                            bringup.extern_symbols_resolved,
                            bringup.extern_symbols_unresolved,
                            bringup.extern_symbols_nulled,
                            bringup.extern_slots_patched, bringup.extern_name,
                            (unsigned) bringup.extern_address);
        melee_vita_log_info("PROVIDER name=%s relocations=%u",
                            bringup.provider_name,
                            bringup.provider_relocations);
        melee_vita_log_info("RUNTIME_SYMBOLS registered=%u hits=%u",
                            bringup.runtime_symbols_registered,
                            bringup.runtime_symbol_hits);
        melee_vita_log_info("FIGHTER root_ptrs=%u/%u ext=0x%08x parts=0x%08x joint=0x%08x hurtboxes=%u",
                            bringup.fighter_root_pointers_in_bounds,
                            bringup.fighter_root_pointers,
                            (unsigned) bringup.fighter_ext_attributes,
                            (unsigned) bringup.fighter_parts,
                            (unsigned) bringup.fighter_joint,
                            bringup.fighter_hurtbox_count);
        melee_vita_log_info("FIGHTER parts_idx=%u,%u,%u,%u,%u",
                            bringup.fighter_part_indices[0],
                            bringup.fighter_part_indices[1],
                            bringup.fighter_part_indices[2],
                            bringup.fighter_part_indices[3],
                            bringup.fighter_part_indices[4]);
        melee_vita_log_info("JOINTS count=%u depth=%u named=%u display=%u finite=%u",
                            bringup.joint_count,
                            bringup.joint_max_depth,
                            bringup.joint_named_count,
                            bringup.joint_display_count,
                            bringup.joint_transform_count);
        melee_vita_log_info("DISPLAY dobj=%u material=%u texture=%u image=%u pobj=%u dl_bytes=%u max_tex=%ux%u",
                            bringup.display_object_count,
                            bringup.material_count,
                            bringup.texture_count,
                            bringup.image_count,
                            bringup.polygon_object_count,
                            bringup.display_list_bytes,
                            bringup.max_texture_width,
                            bringup.max_texture_height);
        melee_vita_log_info("GX batches=%u vertices=%u triangles=%u indexed=%u direct=%u arrays=%u max_index=%u",
                            bringup.gx.batches,
                            bringup.gx.vertices,
                            bringup.gx.triangles,
                            bringup.gx.indexed_attributes,
                            bringup.gx.direct_attributes,
                            bringup.gx.vertex_arrays,
                            bringup.gx.max_index);
        melee_vita_log_info("MESH extracted_vertices=%u truncated=%u",
                            bringup.mesh_vertex_count,
                            bringup.mesh_truncated);
        melee_vita_log_info("MESH pobj rigid=%u shared=%u envelope=%u skinned_vertices=%u",
                            bringup.mesh_rigid_pobjs,
                            bringup.mesh_shared_pobjs,
                            bringup.mesh_envelope_pobjs,
                            bringup.mesh_skinned_vertices);
        melee_vita_log_info("MODEL groups=%u variant=%u dobjs_visible=%u hidden=%u",
                            bringup.fighter_model_groups,
                            bringup.fighter_model_variant,
                            bringup.fighter_dobjs_visible,
                            bringup.fighter_dobjs_hidden);
    }
    const uint32_t base_vertex_count = bringup.mesh_vertex_count;
    const size_t frame_floats = (size_t) base_vertex_count * 3u;
    const size_t animation_bytes =
        frame_floats * SAMUS_WAIT_FRAME_COUNT * sizeof(float);
    float* animation_positions = NULL;
    uint32_t animation_frames = 1u;
    if (bringup_result == 0 && base_vertex_count != 0u) {
        psvDebugScreenPrintf("Checking private Wait animation cache...\n");
        animation_positions = load_wait_cache(&character_setups[0],
                                              base_vertex_count);
        if (animation_positions != NULL) {
            animation_frames = SAMUS_WAIT_FRAME_COUNT;
            psvDebugScreenPrintf("Loaded cached Wait animation.\n");
        } else if ((animation_positions = malloc(animation_bytes)) != NULL) {
            memcpy(animation_positions, bringup.mesh_positions,
                   frame_floats * sizeof(float));
            psvDebugScreenPrintf("Preparing %u-frame Wait animation...\n",
                                 SAMUS_WAIT_FRAME_COUNT);
            int animation_ok = 1;
            for (uint32_t frame = 1u; frame < SAMUS_WAIT_FRAME_COUNT;
                 ++frame) {
                const int frame_result = melee_vita_run_bringup_probe_frame(
                    MELEE_VITA_DISC_PATH, &bringup, (float) frame);
                if (frame_result != 0 ||
                    bringup.mesh_vertex_count != base_vertex_count) {
                    psvDebugScreenPrintf(
                        "Animation frame %u failed (%d, vertices=%u).\n",
                        frame, frame_result, bringup.mesh_vertex_count);
                    animation_ok = 0;
                    break;
                }
                memcpy(animation_positions + (size_t) frame * frame_floats,
                       bringup.mesh_positions, frame_floats * sizeof(float));
            }
            if (animation_ok) {
                animation_frames = SAMUS_WAIT_FRAME_COUNT;
                psvDebugScreenPrintf(
                    save_wait_cache(&character_setups[0], animation_positions,
                                    base_vertex_count)
                        ? "Saved private Wait animation cache.\n"
                        : "Could not save Wait animation cache.\n");
            }
        } else {
            psvDebugScreenPrintf("Animation cache allocation failed.\n");
        }
    }
    const float* render_positions = animation_positions != NULL
                                        ? animation_positions
                                        : bringup.mesh_positions;
    struct melee_vita_texture_atlas texture_atlas = { 0 };
    const int texture_result = bringup_result == 0
        ? melee_vita_build_texture_atlas(MELEE_VITA_DISC_PATH, &bringup,
                                         &texture_atlas)
        : -100;
    psvDebugScreenPrintf("Texture atlas: %s (%d), images=%u\n",
                         texture_result == 0 ? "PASS" : "FAIL",
                         texture_result,
                         texture_result == 0 ? texture_atlas.image_count : 0u);

    static struct melee_vita_bringup_info master_hand;
    const int master_hand_result = melee_vita_run_character_probe(
        MELEE_VITA_DISC_PATH, &master_hand,
        MELEE_VITA_CHARACTER_MASTER_HAND);
    psvDebugScreenPrintf("Master Hand probe: %s (%d), vertices=%u\n",
                         master_hand_result == 0 ? "PASS" : "FAIL",
                         master_hand_result, master_hand.mesh_vertex_count);
    if (logging) {
        melee_vita_log_info(
            "MASTER_HAND probe=%d archive=%s public=%s vertices=%u joints=%u tracks=%u visible=%u hidden=%u",
            master_hand_result, master_hand.archive_name,
            master_hand.public_name, master_hand.mesh_vertex_count,
            master_hand.animation_joint_count,
            master_hand.animation_track_count,
            master_hand.fighter_dobjs_visible,
            master_hand.fighter_dobjs_hidden);
    }
    const uint32_t master_hand_vertex_count = master_hand.mesh_vertex_count;
    const size_t master_hand_frame_floats =
        (size_t) master_hand_vertex_count * 3u;
    float* master_hand_positions = NULL;
    uint32_t master_hand_frames = 1u;
    if (master_hand_result == 0 && master_hand_vertex_count != 0u) {
        psvDebugScreenPrintf("Checking private Master Hand idle cache...\n");
        master_hand_positions = load_wait_cache(
            &character_setups[1], master_hand_vertex_count);
        if (master_hand_positions != NULL) {
            master_hand_frames = MASTER_HAND_WAIT_FRAME_COUNT;
            psvDebugScreenPrintf("Loaded cached Master Hand idle.\n");
        } else {
            const size_t master_hand_bytes = master_hand_frame_floats *
                MASTER_HAND_WAIT_FRAME_COUNT * sizeof(float);
            master_hand_positions = malloc(master_hand_bytes);
            if (master_hand_positions != NULL) {
                memcpy(master_hand_positions, master_hand.mesh_positions,
                       master_hand_frame_floats * sizeof(float));
                psvDebugScreenPrintf(
                    "Preparing %u-frame Master Hand idle...\n",
                    MASTER_HAND_WAIT_FRAME_COUNT);
                int animation_ok = 1;
                for (uint32_t frame = 1u;
                     frame < MASTER_HAND_WAIT_FRAME_COUNT; ++frame) {
                    const int frame_result =
                        melee_vita_run_character_probe_frame(
                            MELEE_VITA_DISC_PATH, &master_hand,
                            MELEE_VITA_CHARACTER_MASTER_HAND, (float) frame);
                    if (frame_result != 0 ||
                        master_hand.mesh_vertex_count !=
                            master_hand_vertex_count) {
                        psvDebugScreenPrintf(
                            "Master Hand frame %u failed (%d, vertices=%u).\n",
                            frame, frame_result,
                            master_hand.mesh_vertex_count);
                        animation_ok = 0;
                        break;
                    }
                    memcpy(master_hand_positions +
                               (size_t) frame * master_hand_frame_floats,
                           master_hand.mesh_positions,
                           master_hand_frame_floats * sizeof(float));
                }
                if (animation_ok) {
                    master_hand_frames = MASTER_HAND_WAIT_FRAME_COUNT;
                    psvDebugScreenPrintf(
                        save_wait_cache(&character_setups[1],
                                        master_hand_positions,
                                        master_hand_vertex_count)
                            ? "Saved private Master Hand idle cache.\n"
                            : "Could not save Master Hand idle cache.\n");
                }
            } else {
                psvDebugScreenPrintf(
                    "Master Hand animation cache allocation failed.\n");
            }
        }
    }
    const float* master_hand_render_positions = master_hand_positions != NULL
        ? master_hand_positions : master_hand.mesh_positions;
    struct melee_vita_texture_atlas master_hand_atlas = { 0 };
    const int master_hand_texture_result = master_hand_result == 0
        ? melee_vita_build_texture_atlas(MELEE_VITA_DISC_PATH, &master_hand,
                                         &master_hand_atlas)
        : -100;
    psvDebugScreenPrintf("Master Hand texture: %s (%d), images=%u\n",
                         master_hand_texture_result == 0 ? "PASS" : "FAIL",
                         master_hand_texture_result,
                         master_hand_texture_result == 0
                             ? master_hand_atlas.image_count : 0u);
    if (logging) {
        melee_vita_log_info(
            "MASTER_HAND animation_frames=%u texture=%d images=%u",
            master_hand_frames, master_hand_texture_result,
            master_hand_texture_result == 0 ? master_hand_atlas.image_count
                                            : 0u);
    }

    struct melee_vita_render_character render_characters[2] = {
        {
            render_positions,
            texture_result == 0 ? bringup.mesh_texture_coordinates : NULL,
            bringup.mesh_cull_modes,
            base_vertex_count, animation_frames,
            texture_result == 0 ? texture_atlas.pixels : NULL,
            texture_result == 0 ? texture_atlas.width : 0u,
            texture_result == 0 ? texture_atlas.height : 0u,
        },
        {
            master_hand_render_positions,
            master_hand_texture_result == 0
                ? master_hand.mesh_texture_coordinates : NULL,
            master_hand.mesh_cull_modes,
            master_hand_vertex_count, master_hand_frames,
            master_hand_texture_result == 0 ? master_hand_atlas.pixels : NULL,
            master_hand_texture_result == 0 ? master_hand_atlas.width : 0u,
            master_hand_texture_result == 0 ? master_hand_atlas.height : 0u,
        },
    };
    const unsigned int render_character_count =
        master_hand_result == 0 && master_hand_vertex_count != 0u ? 2u : 1u;
    if (logging)
        melee_vita_log_info("RENDER characters=%u samus_vertices=%u master_hand_vertices=%u",
                            render_character_count, base_vertex_count,
                            master_hand_vertex_count);
    psvDebugScreenPrintf(
        "\nStarting GXM renderer. L/R switches character; START exits.\n");
    psvDebugScreenFinish();
    const int gxm_result = melee_vita_gxm_probe_run(
        render_characters, render_character_count);
    melee_vita_free_texture_atlas(&master_hand_atlas);
    melee_vita_free_texture_atlas(&texture_atlas);
    free(master_hand_positions);
    free(animation_positions);
    if (logging) melee_vita_log_info("GXM triangle result=%d", gxm_result);
    if (logging) melee_vita_log_info("MELEE_VITA EXIT");
    if (logging) melee_vita_log_stop();
    const int exit_code = result == MELEE_VITA_DISC_OK && gxm_result == 0 ? 0 : 3;
    sceKernelExitProcess(exit_code);
    return exit_code;
}
