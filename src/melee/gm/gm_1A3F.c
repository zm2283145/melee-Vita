#include "gm_1A3F.h"

#include "gm_1A36.h"
#include "gmmain_lib.h"
#include "gmscdata.h"
#include "gmscene.h"
#include "types.h"
#include <dolphin/vi.h>
#include <melee/db/db.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbcardgame.h>
#include <melee/lb/lbcardnew.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/lbheap.h>
#include <melee/lb/lbmthp.h>
#include <melee/lb/lbsnap.h>
#include <melee/lb/types.h>
#include <melee/ty/toy.h>
#include <melee/ty/tydisplay.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/devcom.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/video.h>

struct routingInfo {
    u8 curr_mode;     ///< ::GameModeKind
    u8 pending_mode;  ///< ::GameModeKind
    u8 prev_mode;     ///< ::GameModeKind
    u8 curr_state_id; ///< from ::GameModeState::id
    u8 prev_state_id;
    u8 next_state_id;
};
ASSERT_SIZE(struct routingInfo, 0x6);

struct stateMachine {
    struct routingInfo routing;
    struct routingInfo backup_routing;
    u8 pending_mode_change; ///< ::bool
    u8 (*get_override)(void);
};
ASSERT_SIZE(struct stateMachine, 0x14);

/* 1A3F48 */ static void preloadState(GameModeState*);

/**
 * @brief Runs one game-mode scene transition and invokes the run loop for a
 * given scene
 *
 * Finds the current #GameModeState in @p mode, runs its @c Prep handler, loads
 * the scene via #gm_FindGameSceneHandler / #gm_801A4D34, then advances
 * #GameRouting::curr_scene_idx when the scene loop exits.
 */
/* 1A4014 */ static void gm_801A4014(GameMode*);

/**
 * @brief Loads a game mode, runs it to completion, then unloads it.
 *
 * Loads the data associated with the given #GameModeKind (asset preload and
 * #GameMode::Load), then executes its scene graph via #gm_801A4014 until
 * #GameState::pending is set. When the loop finishes, unloads the mode
 * (#GameMode::Unload) unless the game is resetting, then returns
 * #GameRouting::pending_mode, the next pending #GameModeKind.
 *
 * If #GameState::game_mode_override is defined and returns a mode < #GM_COUNT,
 * a single scene from that game mode will be executed after the current scene
 * exits, skipping typical game mode load/preload routines for the resulting
 * gamemode.  #GameState::pending takes precedence over the override behavior.
 *
 * See also: ::gm_ChangeGameModeAfterCurrentScene, ::gm_SetPendingGameMode,
 * ::gm_SetNewGameModePending
 *
 * @returns The next pending #GameModeKind (#GameRouting::pending_mode).
 */
/* 1A43A0 */ static u8 runGameMode(u8 mode);

/* 479D30 */ static struct stateMachine state_machine;

void preloadState(GameModeState* state)
{
    PreloadedGameModeState* preloaded_state;

    lbDvd_80018CF4(state->preload);
    switch (state->info.scene_kind) {
    case GS_STAFFROLL:
    case GS_RESULTS:
        HSD_SisLib_803A6048(0xC000);
        break;
    case GS_CSS:
        HSD_SisLib_803A6048(0x2400);
        break;
    default:
        HSD_SisLib_803A6048(0x4800);
        break;
    }
    preloaded_state = lbDvd_GetPreloadCacheScene();
    if (lbHeap_80015BB8(2) == 0) {
        preloaded_state->is_heap_persistent[0] = true;
    }
    if (lbHeap_80015BB8(3) == 0) {
        preloaded_state->is_heap_persistent[1] = true;
    }
    lbDvd_80018254();
    lbCardNew_ForgetMemory();
    lbCardGame_Reset();
    lbSnap_8001E27C();
    Toy_803127D4();
    tyDisplay_8031C8B8();
}

static inline u8 firstState(GameModeState* state, u8 next_id)
{
    for (; state->id != (u8) -1; state++) {
        do {
            if (state->id == next_id) {
                break;
            }
        } while (0);
        return state->id;
    }
    return 0;
}

static inline u8 nextState(GameModeState* states)
{
    GameModeState* next = states;
    u8 curr_id = state_machine.routing.curr_state_id;
    int i;
    u8 next_id;
    GameModeState* cur = states;

    for (i = 0; (next_id = next->id) != (u8) -1; i++) {
        if (cur->id > curr_id) {
            return states[i].id;
        }
        cur++;
        next++;
    }

    return firstState(states, next_id);
}

static inline GameModeState* findState(GameModeState* state)
{
    int i, j;
    for (i = state_machine.routing.curr_state_id; i < U8_MAX; i++) {
        for (j = 0; state[j].id != (u8) -1; j++) {
            if (i == state[j].id) {
                return &state[j];
            }
        }
    }
    return NULL;
}

void gm_801A4014(GameMode* mode)
{
    GameScene* scene;
    GameModeState* state;
    struct stateMachine* sm;
    struct GameSceneInfo* info;
    u32 dead; ///< @todo regswap hack
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    u64 phase_started;
    u64 preload_us;
    u64 state_enter_us;
    u64 setup_us;
    u64 scene_exit_us;
    u64 state_exit_us;
    u64 cleanup_us;
#endif
    PAD_STACK(2 * 4);

    sm = &state_machine;
    OSReport("[SCENE] state resolve: mode %u state id %u\n", mode->kind,
             sm->routing.curr_state_id);
    state = findState(mode->states);
    sm->routing.curr_state_id = state->id;
    OSReport("[SCENE] state %u resolved, scene kind %u\n", state->id,
             state->info.scene_kind);

#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    phase_started = OSGetTime();
#endif
    preloadState(state);
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    preload_us = OSTicksToMicroseconds(OSGetTime() - phase_started);
    phase_started = OSGetTime();
#endif
    if (state->on_enter != NULL) {
        OSReport("[SCENE] state %u enter begin\n", state->id);
        state->on_enter(state);
        OSReport("[SCENE] state %u enter complete\n", state->id);
    }
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    state_enter_us = OSTicksToMicroseconds(OSGetTime() - phase_started);
    phase_started = OSGetTime();
#endif
    info = &state->info;
    scene =
        (GameScene*) ((uintptr_t) gm_FindGameSceneHandler(info->scene_kind) |
                      (dead = 0));
    OSReport("[SCENE] scene %u handler resolved at %p\n", info->scene_kind,
             scene);
    gm_801A4BD4();
    OSReport("[SCENE] scene globals reset\n");
    gm_801A4B88(info);
    OSReport("[SCENE] scene info applied\n");
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    setup_us = OSTicksToMicroseconds(OSGetTime() - phase_started);
    phase_started = OSGetTime();
#endif
    if (scene->on_enter != NULL) {
        OSReport("[SCENE] scene %u enter begin\n", info->scene_kind);
        scene->on_enter(info->enter_data);
        OSReport("[SCENE] scene %u enter complete\n", info->scene_kind);
    }
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    OSReport(
        "[TRANSITION] enter mode=%u state=%u scene=%u preload=%lluus "
        "state=%lluus setup=%lluus scene=%lluus\n",
        mode->kind, state->id, info->scene_kind,
        (unsigned long long) preload_us,
        (unsigned long long) state_enter_us,
        (unsigned long long) setup_us,
        (unsigned long long)
            OSTicksToMicroseconds(OSGetTime() - phase_started));
#endif
#ifdef TARGET_VITA
    {
        extern void melee_vita_dvd_log_stats(const char*);
        extern void melee_vita_gxm_log_memory(const char*);
        melee_vita_dvd_log_stats("scene-enter");
        melee_vita_gxm_log_memory("scene-enter");
    }
#endif
    OSReport("[SCENE] scene %u frame loop begin\n", info->scene_kind);
    gm_801A4D34(scene->on_frame, info);
    OSReport("[SCENE] scene %u frame loop complete\n", info->scene_kind);
#ifdef TARGET_VITA
    {
        extern void melee_vita_gxm_log_memory(const char*);
        melee_vita_gxm_log_memory("scene-exit");
    }
#endif
    if (!gmMainLib_8046B0F0.resetting && scene->on_exit != NULL) {
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
        phase_started = OSGetTime();
#endif
        scene->on_exit(info->exit_data);
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
        scene_exit_us = OSTicksToMicroseconds(OSGetTime() - phase_started);
#endif
    }
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    else {
        scene_exit_us = 0;
    }
#endif
    if (!gmMainLib_8046B0F0.resetting) {
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
        phase_started = OSGetTime();
#endif
        if (state->on_exit != NULL) {
            state->on_exit(state);
        }

        state_machine.routing.prev_state_id = sm->routing.curr_state_id;

        if (sm->routing.next_state_id) {
            sm->routing.curr_state_id = sm->routing.next_state_id - 1;
            sm->routing.next_state_id = 0;
        } else {
            sm->routing.curr_state_id = nextState(mode->states);
        }
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
        state_exit_us = OSTicksToMicroseconds(OSGetTime() - phase_started);
#endif
    }
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    else {
        state_exit_us = 0;
    }
    phase_started = OSGetTime();
#endif
    lb_8001CDB4();
    lbCardNew_CompleteAllTasks(11);
    lbMthp_8001F800();
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    cleanup_us = OSTicksToMicroseconds(OSGetTime() - phase_started);
#endif
#ifdef TARGET_VITA
    {
        extern void melee_vita_gxm_invalidate_textures(void);
        extern void melee_vita_gxm_log_memory(const char*);
        melee_vita_gxm_invalidate_textures();
        melee_vita_gxm_log_memory("scene-cache-cleared");
    }
#endif
#if defined(TARGET_VITA) && !defined(MELEE_VITA_RELEASE)
    OSReport(
        "[TRANSITION] exit mode=%u state=%u scene=%u scene=%lluus "
        "state=%lluus cleanup=%lluus\n",
        mode->kind, state->id, info->scene_kind,
        (unsigned long long) scene_exit_us,
        (unsigned long long) state_exit_us,
        (unsigned long long) cleanup_us);
#endif
    if (gmMainLib_8046B0F0.resetting) {
        lbAudioAx_80027DBC();
        HSD_PadReset();
        while (lbCardNew_CompleteNextTask() == 11);
        if (DVDCheckDisk() == 0) {
            OSResetSystem(1, 0, 0);
        }
        lbMthp_8001F800();
        while (HSD_DevComIsBusy(1));
        gmMainLib_8015FBA4();
        gm_GetAllGameModes();
        memzero(&state_machine, sizeof(state_machine));
        gm_801A3EF4();
        gmMainLib_8046B0F0.skip_intro = true;
        gm_ChangeGameModeAfterCurrentScene(GM_BOOT);
        HSD_VISetBlack(0);
    }
}

void* gm_GetGameModeStateEnterData(GameModeState* scene)
{
    return scene->info.enter_data;
}

void* gm_GetGameModeStateExitData(GameModeState* state)
{
    return state->info.exit_data;
}

void gm_SetGameModeStateId(u8 id)
{
    state_machine.routing.curr_state_id = id;
    state_machine.routing.prev_state_id = id;
}

/// @note Actually sets the next scene to the scene following the input
void gm_SetNextGameModeStateId(u8 curr_id)
{
    state_machine.routing.next_state_id = curr_id + 1;
}

u8 gm_GetPreviousSceneIndex(void)
{
    return state_machine.routing.prev_state_id;
}

u8 gm_GetCurrentSceneIndex(void)
{
    return state_machine.routing.curr_state_id;
}

void gm_SetNewGameModePending(void)
{
    state_machine.pending_mode_change = true;
}

void gm_SetPendingGameMode(u8 pending_mode)
{
    state_machine.routing.pending_mode = pending_mode;
}

void gm_ChangeGameModeAfterCurrentScene(int pending_mode)
{
    state_machine.routing.pending_mode = pending_mode;
    state_machine.pending_mode_change = true;
}

u8 gm_GetCurrentGameMode(void)
{
    return state_machine.routing.curr_mode;
}

u8 gm_GetPreviousGameMode(void)
{
    return state_machine.routing.prev_mode;
}

void gm_SetGameModeOverride(u8 (*mode)(void))
{
    state_machine.get_override = mode;
}

bool gm_Is1PMode(u8 mode)
{
    switch (mode) {
    case GM_CLASSIC:
    case GM_ADVENTURE:
    case GM_ALLSTAR:
    case GM_TARGET_TEST:
    case GM_TRAINING:
    case GM_HOME_RUN_CONTEST:
    case GM_10MAN_VS:
    case GM_100MAN_VS:
    case GM_3MIN_VS:
    case GM_15MIN_VS:
    case GM_ENDLESS_VS:
    case GM_CRUEL_VS:
    case GM_EVENT:
        return true;
    }
    return false;
}

static inline GameMode* findMode(u8 kind)
{
    GameMode* cur;
    for (cur = gm_GetAllGameModes(); cur->kind != GM_COUNT; cur++) {
        if (cur->kind == kind) {
            return cur;
        }
    }
    return NULL;
}

u8 runGameMode(u8 mode_kind)
{
    u8 override;
    GameMode* mode;
    struct stateMachine* sm = &state_machine;
    PAD_STACK(2 * 4);

    OSReport("[SCENE] run mode %u begin\n", mode_kind);
    mode = findMode(mode_kind);
    OSReport("[SCENE] mode %u resolved at %p\n", mode_kind, mode);

    state_machine.pending_mode_change = false;
    state_machine.routing.curr_state_id = 0;
    state_machine.routing.prev_state_id = 0;
    state_machine.routing.next_state_id = 0;
    OSReport("[SCENE] mode %u preload begin\n", mode_kind);
    lbDvd_80018F58(mode->preloaded);
    OSReport("[SCENE] mode %u preload complete\n", mode_kind);
    if (mode->on_load != NULL) {
        OSReport("[SCENE] mode %u load callback begin\n", mode_kind);
        mode->on_load();
        OSReport("[SCENE] mode %u load callback complete\n", mode_kind);
    }
    while (!sm->pending_mode_change) {
        if (state_machine.get_override != NULL &&
            (override = state_machine.get_override(), override != GM_COUNT))
        {
            state_machine.backup_routing = state_machine.routing;
            sm->pending_mode_change = false;
            sm->routing.curr_state_id = 0;
            sm->routing.prev_state_id = 0;
            sm->routing.next_state_id = 0;

            gm_801A4014(findMode(override));
            if (!gmMainLib_8046B0F0.resetting) {
                state_machine.routing = state_machine.backup_routing;
            }
        } else {
            gm_801A4014(mode);
        }
    }
    if (!gmMainLib_8046B0F0.resetting && mode->on_unload != NULL) {
        mode->on_unload();
    }
    return state_machine.routing.pending_mode;
}

/// UnclePunch: Scene_Main
void gm_801A4510(void)
{
    GameMode* modes;
    struct stateMachine* gamestate = &state_machine;
    int i;
    PAD_STACK(2 * 4);

    OSReport("[SCENE] mode table initialization begin\n");
    gm_GetAllGameModes();
    memzero(&state_machine, sizeof(struct stateMachine));
    modes = gm_GetAllGameModes();
    for (i = 0; modes[i].kind != GM_COUNT; i++) {
        if (modes[i].on_init != NULL) {
            OSReport("[SCENE] init mode %u begin\n", modes[i].kind);
            modes[i].on_init();
            OSReport("[SCENE] init mode %u complete\n", modes[i].kind);
        }
    }
    OSReport("[SCENE] all mode initialization complete\n");
    if (VIGetDTVStatus() != 0 &&
        (db_gameLaunchButtonState & HSD_PAD_B || OSGetProgressiveMode() == 1))
    {
        state_machine.routing.curr_mode = GM_PROGRESSIVE_SCAN;
    } else {
        state_machine.routing.curr_mode = GM_BOOT;
    }
    state_machine.routing.prev_mode = GM_COUNT;

    OSReport("[SCENE] initial mode is %u\n", state_machine.routing.curr_mode);
    while (true) {
        u8 next_mode = runGameMode(state_machine.routing.curr_mode);
        if (gmMainLib_8046B0F0.resetting) {
            gmMainLib_8046B0F0.resetting = false;
        }
        gamestate->routing.prev_mode = gamestate->routing.curr_mode;
        gamestate->routing.curr_mode = next_mode;
    }
}
