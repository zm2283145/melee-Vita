#include "gmtitlemode.h"

#include "gm_1A3F.h"
#include "gm_unsplit.h"
#ifdef MELEE_VITA_DIRECT_SNAG
#include "gmmain_lib.h"
#endif
#include "types.h"
#include <melee/db/db.h>
#include <melee/lb/lbdvd.h>
#include <melee/mn/mnvitadebug.h>
#include <sysdolphin/baselib/controller.h>

struct exitData {
    int buttons;
    UNK_T x4;
};

/* 1B089C */ static void onExit(GameModeState*);
/* 4D6878 */ static struct exitData exit_data;

/* 3DD6A0 */ GameModeState gm_Mode_Title_States[] = {
    {
        0,
        lbDvdPreload_3,
        0,
        gmTitleMode_OnEnter,
        onExit,
        {
            GS_TITLE,
            NULL,
            &exit_data,
        },
    },
    { -1 },
};

void gmTitleMode_OnEnter(UNUSED GameModeState* state)
{
#ifdef TARGET_VITA
    mnVitaDebug_End();
#endif
    lbDvd_SetupVsPreloadCache();
}

void onExit(GameModeState* scene)
{
    int* buttons = gm_GetGameModeStateExitData(scene);
#if defined(TARGET_VITA) && !defined(MELEE_VITA_ENABLE_DEBUG_MENU)
    if (*buttons & HSD_PAD_START) {
        gm_80173EEC();
        gm_80172898(0x100);
        if (!gm_80173754(1, 0)) {
            gm_SetPendingGameMode(GM_MENU);
        }
    } else {
        gm_801BF708(1);
        gm_SetPendingGameMode(GM_OPENING_MV);
    }
#else
    if (DbLevel >= DbLKind_DebugRom) {
#ifdef TARGET_VITA
        if (*buttons & (HSD_PAD_START | HSD_PAD_A)) {
            gm_80173EEC();
            gm_80172898(0x100);
            if (!gm_80173754(1, 0)) {
                gm_SetPendingGameMode(GM_MENU);
            }
        } else if (*buttons & HSD_PAD_X) {
            gm_SetPendingGameMode(GM_DEBUG_SOUND_TEST);
        } else if (*buttons & HSD_PAD_Y) {
#ifdef MELEE_VITA_DIRECT_SNAG
            struct gmm_x0_528_t* classic = gmMainLib_8015CDC8();
            classic->x5 = 5;
            gm_SetPendingGameMode(GM_CLASSIC);
#else
            gm_SetPendingGameMode(GM_DEBUG);
#endif
        } else {
            gm_801BF708(1);
            gm_SetPendingGameMode(GM_OPENING_MV);
        }
#else
        if (*buttons & HSD_PAD_A) {
            gm_SetPendingGameMode(GM_DEBUG_VS);
        } else if (*buttons & HSD_PAD_START) {
            gm_80173EEC();
            gm_80172898(0x100);
            if (!gm_80173754(1, 0)) {
                gm_SetPendingGameMode(GM_MENU);
            }
        } else if (*buttons & HSD_PAD_X) {
            gm_SetPendingGameMode(GM_DEBUG_SOUND_TEST);
        } else if (*buttons & HSD_PAD_Y) {
#ifdef MELEE_VITA_DIRECT_SNAG
            struct gmm_x0_528_t* classic = gmMainLib_8015CDC8();
            classic->x5 = 5;
            gm_SetPendingGameMode(GM_CLASSIC);
#else
            gm_SetPendingGameMode(GM_DEBUG);
#endif
        } else {
            gm_801BF708(1);
            gm_SetPendingGameMode(GM_OPENING_MV);
        }
#endif
    } else if (*buttons & HSD_PAD_START) {
        gm_80173EEC();
        gm_80172898(0x100);
        if (!gm_80173754(1, 0)) {
            gm_SetPendingGameMode(GM_MENU);
        }
    } else {
        gm_801BF708(1);
        gm_SetPendingGameMode(GM_OPENING_MV);
    }
#endif
    gm_SetNewGameModePending();
}
