/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi Direct online play, the menus.
 *
 * Like Slippi's "Slippi Online Scene" (Online/Slippi Online Scene/main.asm
 * in slippi-ssbm-asm), this takes over the unused game mode GM_HANYU_CSS:
 *   CSS (1P-style, only this player's door, as Slippi's)
 *   -> SSS (only the loser of the last game picks a stage)
 *   -> VS (the match, from the game info block both players agreed on)
 *   -> back to the CSS, still connected.  Slippi has no results screen.
 * The session (code entry, search, lock-in, chat) is platform code
 * (platforms/vita/slippi/slippi_ui.c).  Each CSS frame this hands it the
 * fighter and the buttons, and it says what to do.
 *
 * Ported from the 3DS Slippi port (melee-3ds slippi-beta-v0.1.0,
 * native-3ds/port/engine/slippi/online_mode.c).  Left out for now: the VS
 * splash (off on the 3DS too) and Slippi's own CSS art (slpCSS.dat). */
#ifdef MELEE_VITA_SLIPPI /* only in -EnableSlippi builds */
#include "slippi_online.h"

#include "slippi_platform.h"

#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1B03.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/sislib.h>

#include <string.h>

enum { ST_CSS, ST_SSS, ST_VS };

static CSSData online_css;
static s8 saved_ckind = CKind_Fox;
static s8 saved_color;
static u8 next_state = ST_VS;
static HSD_Text* css_text;
/* Set when the CSS comes back from the stage pick: no announcer. */
static int css_quiet;

static void css_enter(GameModeState* state);
static void css_exit(GameModeState* state);
static void sss_enter(GameModeState* state);
static void sss_exit(GameModeState* state);
static void vs_enter(GameModeState* state);
static void vs_exit(GameModeState* state);

/* gmscdata.c puts these in GM_HANYU_CSS's place. */
GameModeState mp_slippi_online_states[] = {
    { ST_CSS, lbDvdPreload_3, 0, css_enter, css_exit,
      { GS_CSS, &online_css, &online_css } },
    { ST_SSS, lbDvdPreload_3, 0, sss_enter, sss_exit,
      { GS_SSS, &gmVsMelee_SssData, &gmVsMelee_SssData } },
    { ST_VS, lbDvdPreload_3, 0, vs_enter, vs_exit,
      { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { GM_GAMEMODESTATE_TERMINATE },
};

int mp_slippi_css_online(void)
{
    return gm_GetCurrentGameMode() == GM_HANYU_CSS;
}

int mp_slippi_css_quiet(void)
{
    return mp_slippi_css_online() && css_quiet;
}

int mp_slippi_sss_alt_mode(void)
{
    return mp_platform_slippi_ui_alt(0);
}

/* Z on the online SSS (Slippi's FrozenStadiumToggle). */
void mp_slippi_sss_toggle_alt(void)
{
    if (mp_platform_slippi_ui_alt(1)) {
        lbAudioAx_80024030(1);
    } else {
        lbAudioAx_80024030(0);
    }
}

static void play_sfx(int flags)
{
    if (flags & SLIPPI_UI_SFX_ERROR) {
        lbAudioAx_80024030(3);
    } else if (flags & SLIPPI_UI_SFX_FORWARD) {
        lbAudioAx_80024030(1);
    } else if (flags & SLIPPI_UI_SFX_BACK) {
        lbAudioAx_80024030(0);
    } else if (flags & SLIPPI_UI_SFX_MOVE) {
        lbAudioAx_80024030(2);
    }
}

/* The match's fighters and stage into the preload cache, with their sound
 * banks, as Slippi's game setup does. */
static void prepare_match_files(const u8* info)
{
    PreloadedGameModeState* scene;
    u64 mask = 0;
    int i;

    if (info == NULL) return;
    scene = lbDvd_GetPreloadCacheScene();
    for (i = 0; i < 4; i++) {
        const u8* player = info + 0x60 + 0x24 * i;
        /* Empty slots keep the template's fighter byte, so skip them. */
        if (player[1] >= 3) {
            scene->game_cache.entries[i].char_id = ChKind_None;
        } else {
            scene->game_cache.entries[i].char_id = (s8) player[0];
        }
        scene->game_cache.entries[i].color = player[3];
    }
    lbDvd_80018254();
    lbDvd_80018C2C(199);
    lbDvd_80017700(4);
    lbAudioAx_80026F2C(28);
    for (i = 0; i < 6; i++) {
        const u8* player = info + 0x60 + 0x24 * i;
        if ((s8) player[0] != ChKind_None && player[1] < 3) {
            mask |= lbAudioAx_80026E84((CharacterKind) (s8) player[0]);
        }
    }
    mask |= lbAudioAx_80026EBC((StKind) ((info[0xE] << 8) | info[0xF]));
    lbAudioAx_8002702C(4, mask);
    lbAudioAx_80027168();
    lbAudioAx_80024F6C();
}

/* ------------------------------------------------------------------ CSS */

/* CSSScenePrep: the last fighter again, the 1P-style CSS (match type 14,
 * the event match layout Slippi uses) and an empty preload cache. */
static void css_enter(GameModeState* state)
{
    CSSData* css = gm_GetGameModeStateEnterData(state);
    gm_801B06B0(css, 14, saved_ckind, 4, saved_color, 0x78, 0, 0);
    css->vs.start.players[1].slot_type = Gm_PKind_NA;
    lbDvd_SetupVsPreloadCache();
    next_state = ST_VS;
    /* The CSS's SIS texts are freed with each scene. */
    css_text = NULL;
    mp_platform_slippi_ui_event(SLIPPI_UI_EV_CSS_ENTER, 0);
}

static void css_exit(GameModeState* state)
{
    CSSData* css = gm_GetGameModeStateExitData(state);
    s8 ckind;
    u8 color;

    if (css->pending_scene_change == 2) {
        /* B held: back to the menus, and the connection closes. */
        mp_platform_slippi_ui_event(SLIPPI_UI_EV_LEAVE, 0);
        css_quiet = 0;
        /* The vanilla CSS keeps Zelda. */
        mp_slippi_css_sheik(0);
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        return;
    }
    gm_801B0730(css, &ckind, NULL, &color, NULL, NULL);
    saved_ckind = ckind;
    saved_color = (s8) color;
    if (next_state == ST_SSS) {
        gm_SetNextGameModeStateId(ST_SSS);
        return;
    }
    /* Both locked in: load what the match needs, then straight to it. */
    prepare_match_files(mp_slippi_online_pending_block());
    gm_SetNextGameModeStateId(ST_VS);
}

/* The online CSS text (Slippi's LoadCSSText and UserDisplayFunctions).
 * One SIS text on the CSS's canvas, scaled 0.1 like Slippi's.  The lines
 * come from the session (mp_platform_slippi_ui_text).  The positions,
 * sizes and colours are Slippi's, except the chat lines, which the 3DS
 * made larger. */
#define TEXT_LINES 29
static const struct {
    float x;
    float y;
    float size;
    int fit; /* characters that fit at this size, longer lines get narrower */
} text_layout[TEXT_LINES] = {
    /* header */
    { 70, 23, 0.5f, 0 },
    /* user name and connect code */
    { -112, 20, 0.4f, 0 }, { -112, 40, 0.5f, 0 }, { -112, 65, 0.4f, 0 }, { -112, 85, 0.5f, 0 },
    /* status lines and their spinners */
    { 90, 52, 0.4f, 0 }, { 70, 52, 0.45f, 0 }, { 90, 75, 0.4f, 0 }, { 70, 75, 0.45f, 0 },
    { 90, 98, 0.4f, 0 }, { 70, 98, 0.45f, 0 },
    /* Z hint, chat hint */
    { 70, 132.5f, 0.4f, 0 }, { 70, 152.5f, 0.4f, 0 },
    /* "Playing:" and the opponent */
    { -130, -246, 0.5f, 0 }, { -50, -246, 0.5f, 0 },
    /* error */
    { 90, 52, 0.4f, 0 }, { 90, 70, 0.4f, 0 }, { 90, 88, 0.4f, 0 }, { 90, 106, 0.4f, 0 },
    /* chat messages */
    { 70, 180, 0.55f, 22 }, { 70, 205, 0.55f, 22 }, { 70, 230, 0.55f, 22 },
    { 70, 255, 0.55f, 22 }, { 70, 280, 0.55f, 22 },
    /* the open chat page */
    { 70, 128, 0.5f, 24 }, { 70, 154, 0.5f, 24 }, { 70, 180, 0.5f, 24 },
    { 70, 206, 0.5f, 24 }, { 70, 232, 0.5f, 24 },
};
static const GXColor text_colors[7] = {
    { 0xFF, 0xFF, 0xFF, 0xFF }, /* white */
    { 0x8E, 0x91, 0x96, 0xFF }, /* gray */
    { 0xFF, 0x00, 0x00, 0xFF }, /* red */
    { 0x33, 0xFF, 0x2F, 0xFF }, /* done */
    { 0x3C, 0xBC, 0xFF, 0xFF }, /* waiting */
    { 229, 76, 76, 0xFF },      /* chat from port 1 */
    { 59, 189, 255, 0xFF },     /* chat from port 2 */
};
static int text_ids[TEXT_LINES];
static int text_color_now[TEXT_LINES];
static char text_now[TEXT_LINES][64];

/* How many characters a line shows.  A Shift-JIS pair counts once. */
static int count_characters(const char* text)
{
    int count = 0;
    int k;
    for (k = 0; text[k] != '\0'; k++) {
        u8 c = (u8) text[k];
        if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF)) && text[k + 1] != '\0') {
            k++;
        }
        count++;
    }
    return count;
}

static void css_text_frame(void)
{
    char line[64];
    int color;
    int i;

    if (css_text == NULL) {
        css_text = HSD_SisLib_803A6754(0, 0);
        if (css_text == NULL) return;
        css_text->default_kerning = 1;
        css_text->default_alignment = 0;
        css_text->pos_z = 0.0f;
        css_text->font_size.x = 0.1f;
        css_text->font_size.y = 0.1f;
        for (i = 0; i < TEXT_LINES; i++) {
            text_ids[i] = HSD_SisLib_803A6B98(css_text, text_layout[i].x, text_layout[i].y, "");
            HSD_SisLib_803A7548(css_text, text_ids[i], text_layout[i].size, text_layout[i].size);
            text_color_now[i] = -1;
            text_now[i][0] = '\0';
        }
    }
    for (i = 0; i < TEXT_LINES; i++) {
        color = mp_platform_slippi_ui_text(i, line, sizeof(line));
        line[sizeof(line) - 1] = '\0';
        if (strcmp(line, text_now[i]) != 0) {
            strcpy(text_now[i], line);
            HSD_SisLib_803A70A0(css_text, text_ids[i], "%s", line);
            if (text_layout[i].fit > 0) {
                /* Narrow a long line to fit, keeping its height. */
                float width = text_layout[i].size;
                int count = count_characters(line);
                if (count > text_layout[i].fit) {
                    width = width * (float) text_layout[i].fit / (float) count;
                }
                HSD_SisLib_803A7548(css_text, text_ids[i], width, text_layout[i].size);
            }
        }
        if (color != text_color_now[i] && color >= 0 && color < 7) {
            text_color_now[i] = color;
            HSD_SisLib_803A74F0(css_text, text_ids[i], (GXColor*) &text_colors[color]);
        }
    }
}

/* Called from mnCharSel_Scene_OnFrame while the CSS runs normally.
 * Returns 1 to leave the CSS, for the match or the stage pick. */
int mp_slippi_css_frame(int ready, PlayerInitData* local, PlayerInitData* remote, u32 trigger,
                        u32 held)
{
    struct GameCache* cache;
    int packed;
    int flags;
    int opponent;

    (void) remote;
    packed = ready ? 1 : 0;
    packed |= (u8) local->ckind << 8;
    packed |= (u8) local->color << 16;
    flags = mp_platform_slippi_ui_css(packed, (int) trigger, (int) held);
    opponent = mp_platform_slippi_ui_remote();
    play_sfx(flags);
    css_text_frame();
    /* Slippi's CSS art would put MELEE in the title.  Without it the
     * EVENT MATCH title is hidden. */
    mp_slippi_css_hide_title();
    mp_slippi_css_sheik((flags & SLIPPI_UI_SHEIK) != 0);

    /* The opponent's fighter, once known, goes in the preload cache's
     * second slot, so the match loads faster.  It is never shown.  Marking
     * players[1] present would make the one-door CSS read a cursor that
     * this layout never creates. */
    cache = &lbDvd_GetPreloadCacheScene()->game_cache;
    if (opponent >= 0) {
        cache->entries[1].char_id = (s8) (opponent & 0xFF);
        cache->entries[1].color = (u8) (opponent >> 8);
    } else {
        cache->entries[1].char_id = ChKind_None;
    }

    if (flags & SLIPPI_UI_SSS) {
        next_state = ST_SSS;
        return 1;
    }
    if ((flags & SLIPPI_UI_START) && mp_slippi_online_take_match()) {
        next_state = ST_VS;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ SSS */

static void sss_enter(GameModeState* state)
{
    SSSData* sss = gm_GetGameModeStateEnterData(state);
    sss->vs = online_css.vs;
    sss->force_stage_id = -1;
    sss->no_lras = 1;
    sss->start_game = 0;
}

static void sss_exit(GameModeState* state)
{
    SSSData* sss = gm_GetGameModeStateExitData(state);
    if (sss->start_game) {
        int stage = sss->vs.start.rules.stkind;
        mp_platform_slippi_ui_event(SLIPPI_UI_EV_STAGE, stage | (mp_slippi_sss_alt_mode() << 16));
    } else {
        mp_platform_slippi_ui_event(SLIPPI_UI_EV_STAGE, -1);
    }
    css_quiet = 1;
    gm_SetNextGameModeStateId(ST_CSS);
}

/* ------------------------------------------------------------------- VS */

static void vs_enter(GameModeState* state)
{
    (void) state;
    gm_LoadAnnouncer();
    mp_platform_slippi_ui_event(SLIPPI_UI_EV_MATCH, 0);
}

/* VSSceneDecide and CheckIfWonLastGame: back to the CSS, and who picks the
 * next stage. */
static void vs_exit(GameModeState* state)
{
    MatchExitInfo* exit = gm_GetGameModeStateExitData(state);
    MatchEnd* end = &exit->match_end;
    int local = mp_slippi_online_local_index();
    int won = 0;

    if (end->outcome == 7) {
        /* L+R+A+Start, a disconnect or a desync: whoever ended it picks the
         * next stage.  The console keeps who that was in the first byte of
         * the match end data.  Here the pauser in the scene controller has
         * it. */
        won = gmVs_GetSceneController()->state.pauser != local;
    } else if (end->outcome != 0) {
        won = end->player_standings[local].is_big_loser == 0;
    }
    mp_platform_slippi_ui_event(SLIPPI_UI_EV_RESULT, won);
    /* After a game the announcer plays, as in VS mode. */
    css_quiet = 0;
    gm_SetNextGameModeStateId(ST_CSS);
}

#endif /* MELEE_VITA_SLIPPI */
