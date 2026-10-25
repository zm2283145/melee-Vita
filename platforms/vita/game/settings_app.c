/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Settings screen. The Settings button on the LiveArea starts the game with
 * "-settings" (livearea/template.xml) and main() shows this instead of
 * booting Melee. Same idea as mkwiita's Settings screen.
 * Two pages: the main list and stage cutbacks. */
#include "gfx_settings.h"
#include "gxm_resolution.h"

#include <dolphin/types.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include <stdio.h>
#include <string.h>

enum { PAGE_MAIN, PAGE_STAGES };

enum {
    ROW_PRESET,
    ROW_GAME_RES,
    ROW_FRAMERATE,
    ROW_PARTICLES,
    ROW_LIVE,
    ROW_VSYNC,
    ROW_HEAVY30,
    ROW_STAGES,
    ROW_BOOST, // from here down: not part of a preset
    ROW_ASPECT,
    ROW_FPS,
    ROW_RESET, // buttons
    ROW_START,
    ROW_EXIT,
    ROW_COUNT,
};

static const int particle_steps[] = { 128, 256, 512, 1024, 2048 };
static const int live_steps[] = { 1, 2, 4, 8, 16 };
#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

static const char* row_names[ROW_COUNT] = {
    "PRESET", "GAMEPLAY RESOLUTION", "FRAME RATE",
    "PARTICLES", "LIVE SCREENS", "V-SYNC", "30 FPS IN BIG MATCHES", "STAGE CUTBACKS",
    "CPU/GPU BOOST", "ASPECT RATIO", "SHOW FPS",
    "RESET TO DEFAULT", "SAVE AND START", "SAVE AND EXIT",
};

static const char* help_text[ROW_COUNT] = {
    "Custom: your own values, starts out like before. Ultra/Performance: stage cutbacks. Quality: 30.",
    "Render size during matches. Lower runs faster, higher looks sharper.",
    "For matches. 30 is locked and even, and the game itself still runs at full speed. Menus run at 60.",
    "Most particles drawn at once. Lower helps in big explosions.",
    "How often screens inside stages update (Pokemon Stadium, etc).",
    "Wait for the display refresh before showing each frame.",
    "3-4 players, or Fountain of Dreams without cutbacks: the match runs at a locked 30 FPS.",
    "X: per-stage switches that hide decorative stage effects to save time.",
    "Runs the CPU at 444 MHz and the GPU at 222 MHz with any preset. Uses more battery.",
    "16:9 fills the screen in matches. 4:3 is the original picture, with black bars at the sides.",
    "Shows FPS, worst frame time and clocks in the top left corner.",
    "Go back to the defaults: Custom, set up the way the game ran before this menu.",
    "Save, then start the game.",
    "Save, then close the game.",
};

#define STAGES_BACK GFX_STAGE_COUNT

bool settings_app_requested(void)
{
    char param[1024];
    memset(param, 0, sizeof(param));
    if (sceAppMgrGetAppParam(param) < 0) return false;
    return strstr(param, "settings") != NULL;
}

// step through a list of allowed values, snapping odd values from a hand
// edited file to the closest one
static int step_list(const int* list, int n, int cur, int dir)
{
    int i = 0;
    while (i < n - 1 && list[i] < cur) i++;
    if (list[i] != cur && dir > 0) i--; // cur was between two steps
    i += dir;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return list[i];
}

static void change(GfxSettings* s, int row, int dir)
{
    switch (row) {
    case ROW_PRESET:
        gfx_settings_select((g_gfx_preset + dir + GFX_PRESET_COUNT) % GFX_PRESET_COUNT);
        return;
    // resolution options go 0 = native .. 3 = 50%, so "right" means smaller
    // number = sharper
    case ROW_GAME_RES:
        s->game_res -= dir;
        if (s->game_res < 0) s->game_res = 0;
        if (s->game_res >= MELEE_VITA_RESOLUTION_OPTION_COUNT)
            s->game_res = MELEE_VITA_RESOLUTION_OPTION_COUNT - 1;
        break;
    case ROW_FRAMERATE: s->frame_rate = s->frame_rate == FRAME_RATE_30 ? FRAME_RATE_60 : FRAME_RATE_30; break;
    case ROW_BOOST: s->boost = !s->boost; break;
    case ROW_PARTICLES:
        s->particles = step_list(particle_steps, COUNT(particle_steps), s->particles, dir);
        break;
    case ROW_LIVE:
        // right = updates more often = lower number
        s->live_screens = step_list(live_steps, COUNT(live_steps), s->live_screens, -dir);
        break;
    case ROW_VSYNC: s->vsync = !s->vsync; break;
    case ROW_HEAVY30: s->heavy30 = !s->heavy30; break;
    case ROW_ASPECT: s->widescreen = !s->widescreen; break;
    case ROW_FPS: s->show_fps = !s->show_fps; break;
    default: return;
    }
    gfx_settings_changed(); // anything off the preset's values makes it Custom
}

static const char* on_off(int v) { return v ? "ON" : "OFF"; }

static void value_text(const GfxSettings* s, int row, char* out, int size)
{
    u32 w = 0, h = 0;
    out[0] = 0;
    switch (row) {
    case ROW_PRESET: snprintf(out, size, "%s", gfx_settings_preset_name(g_gfx_preset)); break;
    case ROW_GAME_RES: {
        int r = s->game_res;
        static const int pct[] = { 100, 75, 60, 50 };
        melee_vita_resolution_option_dimensions(r, &w, &h);
        snprintf(out, size, "%d%%  (%ux%u)", pct[r], (unsigned) w, (unsigned) h);
        break;
    }
    case ROW_FRAMERATE: snprintf(out, size, "%s", s->frame_rate == FRAME_RATE_30 ? "30 (LOCKED)" : "60"); break;
    case ROW_BOOST: snprintf(out, size, "%s", on_off(s->boost)); break;
    case ROW_PARTICLES: snprintf(out, size, "%d", s->particles); break;
    case ROW_LIVE:
        if (s->live_screens == 1) snprintf(out, size, "EVERY FRAME");
        else snprintf(out, size, "EVERY %d FRAMES", s->live_screens);
        break;
    case ROW_VSYNC: snprintf(out, size, "%s", on_off(s->vsync)); break;
    case ROW_HEAVY30: snprintf(out, size, "%s", on_off(s->heavy30)); break;
    case ROW_ASPECT: snprintf(out, size, "%s", s->widescreen ? "16:9 (WIDESCREEN)" : "4:3"); break;
    case ROW_FPS: snprintf(out, size, "%s", on_off(s->show_fps)); break;
    case ROW_STAGES: {
        int n = 0;
        for (int i = 0; i < GFX_STAGE_COUNT; i++) n += ((s->stage_lite | s->stage_potato) >> i) & 1;
        snprintf(out, size, "%d ON", n);
        break;
    }
    default: break;
    }
}

// rows with a value get "<  value  >", buttons and page links don't
static void draw_row(vita2d_pgf* font, int y, int selected, const char* name, const char* value)
{
    u32 col = selected ? RGBA8(255, 210, 60, 255) : RGBA8(230, 230, 230, 255);
    if (selected) vita2d_draw_rectangle(50, y - 18, 860, 22, RGBA8(60, 60, 90, 255));
    vita2d_pgf_draw_text(font, 70, y, col, 0.9f, name);
    if (value && value[0]) {
        char tmp[80];
        snprintf(tmp, sizeof(tmp), "<  %s  >", value);
        vita2d_pgf_draw_text(font, 480, y, col, 0.9f, tmp);
    }
}

/* Runs the settings screen. Never returns: it either relaunches the game
 * or exits. */
void settings_app_run(void)
{
    vita2d_pgf* font;
    SceCtrlData pad;
    u32 old_buttons = 0;
    int page = PAGE_MAIN;
    int row = 0, sub_row = 0;
    int held = 0;
    int action = -1; // ROW_START / ROW_EXIT / 0 for quit without saving

    gfx_settings_load();

    vita2d_init();
    vita2d_set_clear_color(RGBA8(16, 16, 24, 255));
    font = vita2d_load_default_pgf();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    while (action < 0) {
        u32 pressed, buttons;
        int dir = 0;
        const char* help = "";

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        buttons = pad.buttons;
        // left stick works like the d-pad
        if (pad.ly < 40) buttons |= SCE_CTRL_UP;
        if (pad.ly > 215) buttons |= SCE_CTRL_DOWN;
        if (pad.lx < 40) buttons |= SCE_CTRL_LEFT;
        if (pad.lx > 215) buttons |= SCE_CTRL_RIGHT;
        pressed = buttons & ~old_buttons;

        // key repeat for held directions
        if (buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
            held++;
            if (held > 20 && held % 5 == 0)
                pressed |= buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        } else {
            held = 0;
        }
        old_buttons = buttons;
        if (pressed & SCE_CTRL_LEFT) dir = -1;
        if (pressed & SCE_CTRL_RIGHT) dir = 1;

        if (page == PAGE_MAIN) {
            if (pressed & SCE_CTRL_UP) row = (row + ROW_COUNT - 1) % ROW_COUNT;
            if (pressed & SCE_CTRL_DOWN) row = (row + 1) % ROW_COUNT;
            if ((pressed & SCE_CTRL_CROSS) && row < ROW_RESET && row != ROW_STAGES)
                dir = 1;
            if (dir) change(&g_gfx, row, dir);
            if (pressed & SCE_CTRL_CROSS) {
                if (row == ROW_STAGES) { page = PAGE_STAGES; sub_row = 0; }
                if (row == ROW_RESET) gfx_settings_reset();
                if (row == ROW_START || row == ROW_EXIT) action = row;
            }
            if (pressed & SCE_CTRL_CIRCLE) action = 0;
            help = help_text[row];
        } else if (page == PAGE_STAGES) {
            const int rows = STAGES_BACK + 1;
            if (pressed & SCE_CTRL_UP) sub_row = (sub_row + rows - 1) % rows;
            if (pressed & SCE_CTRL_DOWN) sub_row = (sub_row + 1) % rows;
            if (sub_row < STAGES_BACK && (dir || (pressed & SCE_CTRL_CROSS))) {
                // OFF -> LITE (-> POTATO, Fountain of Dreams and Yoshi's Story)
                int bit = 1 << sub_row;
                int levels = gfx_stages[sub_row].potato ? 3 : 2;
                int level = (g_gfx.stage_potato & bit) ? 2 : (g_gfx.stage_lite & bit) ? 1 : 0;
                level = (level + (dir < 0 ? levels - 1 : 1)) % levels;
                g_gfx.stage_lite = level >= 1 ? g_gfx.stage_lite | bit : g_gfx.stage_lite & ~bit;
                g_gfx.stage_potato = level == 2 ? g_gfx.stage_potato | bit : g_gfx.stage_potato & ~bit;
                gfx_settings_changed(); // off the preset's cutbacks makes it Custom
            }
            if ((pressed & SCE_CTRL_CIRCLE) || (sub_row == STAGES_BACK && (pressed & SCE_CTRL_CROSS)))
                page = PAGE_MAIN;
            help = sub_row == STAGES_BACK ? "Back to the main settings."
                 : gfx_stages[sub_row].kind == 0x02
                     ? "LITE: no stage particles, stage-only reflection, 60 in 1v1. POTATO: also 50%, no scenery."
                 : gfx_stages[sub_row].kind == 0x08
                     ? "LITE: no stage particles. POTATO: also no background or scenery behind the stage."
                     : "LITE: hides this stage's own particle effects. Fighter and item effects stay.";
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        if (font) {
            char val[64];
            if (page == PAGE_MAIN) {
                vita2d_pgf_draw_text(font, 60, 56, RGBA8(255, 210, 60, 255), 1.3f, "Settings");
                for (int i = 0; i < ROW_COUNT; i++) {
                    int y = 90 + i * 20;
                    if (i >= ROW_BOOST) y += 8; // gap before the options presets don't touch
                    if (i >= ROW_RESET) y += 8; // and before the buttons
                    value_text(&g_gfx, i, val, sizeof(val));
                    if (i == ROW_STAGES) {
                        char name[48];
                        snprintf(name, sizeof(name), "%s  >", row_names[i]);
                        draw_row(font, y, i == row, name, val);
                    } else {
                        draw_row(font, y, i == row, row_names[i], val);
                    }
                }
            } else if (page == PAGE_STAGES) {
                vita2d_pgf_draw_text(font, 60, 56, RGBA8(255, 210, 60, 255), 1.3f, "Stage Cutbacks");
                for (int i = 0; i <= STAGES_BACK; i++) {
                    int y = 110 + i * 30 + (i == STAGES_BACK ? 16 : 0);
                    if (i < STAGES_BACK)
                        draw_row(font, y, i == sub_row, gfx_stages[i].name,
                                 (g_gfx.stage_potato >> i) & 1 ? "POTATO"
                                 : (g_gfx.stage_lite >> i) & 1 ? "LITE" : "OFF");
                    else
                        draw_row(font, y, i == sub_row, "BACK", "");
                }
            }
            vita2d_pgf_draw_text(font, 60, 496, RGBA8(200, 200, 200, 255), 0.85f, help);
            vita2d_pgf_draw_text(font, 60, 526, RGBA8(140, 140, 140, 255), 0.8f,
                                 page == PAGE_MAIN
                                     ? "Up/Down: select   Left/Right or X: change   O: quit without saving"
                                     : "Up/Down: select   Left/Right or X: change   O: back");
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    if (action != 0) gfx_settings_save();
    vita2d_wait_rendering_done();
    if (font) vita2d_free_pgf(font);
    vita2d_fini();

    if (action == ROW_START) {
        // start fresh without the -settings param
        sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    }
    sceKernelExitProcess(0);
}
