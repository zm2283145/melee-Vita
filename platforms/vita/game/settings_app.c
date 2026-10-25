/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Settings screen. The Settings button on the LiveArea starts the game with
 * "-settings" (livearea/template.xml) and main() shows this instead of
 * booting Melee. Same idea as mkwiita's Settings screen.
 * Four pages: the main list, stage cutbacks, controls and stick calibration. */
#include "gfx_settings.h"
#include "gxm_resolution.h"
#include "pad_vita.h"

#include <dolphin/pad.h>
#include <dolphin/types.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { PAGE_MAIN, PAGE_STAGES, PAGE_CONTROLS, PAGE_STICKS };

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
    ROW_CONTROLS,
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
    "CPU/GPU BOOST", "ASPECT RATIO", "SHOW FPS", "CONTROLS",
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
    "X: change which Vita button does what.",
    "Go back to the defaults: Custom, the way the game ran before (controls are not changed).",
    "Save, then start the game.",
    "Save, then close the game.",
};

// controls page: GameCube buttons, in pad_mapping.h action order
static const char* action_names[MELEE_VITA_ACTION_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "Z", "START",
};
static const char* vita_button_names[MELEE_VITA_BUTTON_COUNT] = {
    "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L", "R", "SELECT", "START",
    "L2 (PSTV)", "R2 (PSTV)", "L3 (PSTV)", "R3 (PSTV)",
};
#define CONTROLS_STICKS MELEE_VITA_ACTION_COUNT
#define CONTROLS_RESET (MELEE_VITA_ACTION_COUNT + 1)
#define CONTROLS_BACK (MELEE_VITA_ACTION_COUNT + 2)
#define STAGES_BACK GFX_STAGE_COUNT

static uint8_t mapping[MELEE_VITA_BUTTON_COUNT];

static void draw_row(vita2d_pgf* font, int y, int selected, const char* name, const char* value);

// stick calibration page
enum { STICK_CAL_LEFT, STICK_CAL_RIGHT, STICK_DEAD_LEFT, STICK_DEAD_RIGHT, STICK_RESET, STICK_BACK };
static const char* stick_rows[] = {
    "CALIBRATE LEFT STICK", "CALIBRATE RIGHT STICK (C-STICK)", "LEFT DEADZONE",
    "RIGHT DEADZONE", "RESET STICKS", "BACK",
};
static MeleeVitaStickCal cals[MELEE_VITA_STICK_COUNT];
static int cal_stick = -1; // stick being calibrated, -1 = none
static int cal_step;       // 0 = let go (centre), 1 = roll around the edge
static float cal_avg_x, cal_avg_y;
static MeleeVitaStickCal cal_new;
static const char* cal_msg = "";

// one stick's raw position from a sample
static void stick_raw(const SceCtrlData* pad, int stick, int* x, int* y)
{
    *x = stick == MELEE_VITA_STICK_LEFT ? pad->lx : pad->rx;
    *y = stick == MELEE_VITA_STICK_LEFT ? pad->ly : pad->ry;
}

// stick page input; returns the help line
static const char* sticks_input(const SceCtrlData* pad, u32 pressed, int dir, int* sub_row, int* page)
{
    const int rows = STICK_BACK + 1;
    if (cal_stick >= 0) {
        int x, y;
        stick_raw(pad, cal_stick, &x, &y);
        if (cal_step == 0) {
            // average while it rests, so one noisy sample doesn't set the centre
            cal_avg_x += ((float) x - cal_avg_x) * 0.1f;
            cal_avg_y += ((float) y - cal_avg_y) * 0.1f;
            if (pressed & SCE_CTRL_CROSS) {
                cal_new.center_x = (int) lroundf(cal_avg_x);
                cal_new.center_y = (int) lroundf(cal_avg_y);
                cal_new.min_x = cal_new.max_x = cal_new.center_x;
                cal_new.min_y = cal_new.max_y = cal_new.center_y;
                cal_step = 1;
                cal_msg = "";
            }
        } else {
            if (x < cal_new.min_x) cal_new.min_x = x;
            if (x > cal_new.max_x) cal_new.max_x = x;
            if (y < cal_new.min_y) cal_new.min_y = y;
            if (y > cal_new.max_y) cal_new.max_y = y;
            if (pressed & SCE_CTRL_CROSS) {
                // each direction needs real travel, or it'd stretch noise to full tilt
                if (cal_new.max_x - cal_new.center_x >= 40 && cal_new.center_x - cal_new.min_x >= 40 &&
                    cal_new.max_y - cal_new.center_y >= 40 && cal_new.center_y - cal_new.min_y >= 40) {
                    cal_new.dead = cals[cal_stick].dead;
                    cals[cal_stick] = cal_new;
                    cal_msg = "Calibrated. It's saved when you leave Settings with a SAVE row.";
                    cal_stick = -1;
                } else {
                    cal_msg = "Not enough movement. Roll it all the way round, then press X.";
                }
            }
        }
        if (pressed & SCE_CTRL_CIRCLE) {
            cal_stick = -1;
            cal_msg = "Cancelled, nothing changed.";
        }
        if (cal_stick < 0) return cal_msg;
        return cal_step == 0 ? "Let go of the stick so it rests in the middle, then press X. O: cancel."
                             : "Roll the stick slowly around the edge 3 times, then press X. O: cancel.";
    }

    if (pressed & SCE_CTRL_UP) *sub_row = (*sub_row + rows - 1) % rows;
    if (pressed & SCE_CTRL_DOWN) *sub_row = (*sub_row + 1) % rows;
    if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) cal_msg = "";
    if ((*sub_row == STICK_CAL_LEFT || *sub_row == STICK_CAL_RIGHT) && (pressed & SCE_CTRL_CROSS)) {
        int x, y;
        cal_stick = *sub_row == STICK_CAL_LEFT ? MELEE_VITA_STICK_LEFT : MELEE_VITA_STICK_RIGHT;
        cal_step = 0;
        stick_raw(pad, cal_stick, &x, &y);
        cal_avg_x = (float) x;
        cal_avg_y = (float) y;
        cal_msg = "";
        return "";
    }
    if ((*sub_row == STICK_DEAD_LEFT || *sub_row == STICK_DEAD_RIGHT) && (dir || (pressed & SCE_CTRL_CROSS))) {
        MeleeVitaStickCal* c = &cals[*sub_row == STICK_DEAD_LEFT ? 0 : 1];
        c->dead += (dir ? dir : 1) * 5;
        if (c->dead > 30) c->dead = 0; // X wraps round
        if (c->dead < 0) c->dead = 0;
    }
    if (*sub_row == STICK_RESET && (pressed & SCE_CTRL_CROSS)) {
        for (int i = 0; i < MELEE_VITA_STICK_COUNT; i++) melee_vita_pad_stick_cal_defaults(&cals[i]);
        cal_msg = "Both sticks back to uncalibrated, no deadzone.";
    }
    if ((pressed & SCE_CTRL_CIRCLE) || (*sub_row == STICK_BACK && (pressed & SCE_CTRL_CROSS))) {
        *page = PAGE_CONTROLS;
        *sub_row = CONTROLS_STICKS;
        cal_msg = "";
    }
    if (cal_msg[0]) return cal_msg;
    switch (*sub_row) {
    case STICK_CAL_LEFT:
    case STICK_CAL_RIGHT: return "X: find this stick's centre and edges. Do this if it drifts or won't reach full tilt.";
    case STICK_DEAD_LEFT:
    case STICK_DEAD_RIGHT: return "Movement smaller than this counts as centred. Raise it if the dot moves on its own.";
    case STICK_RESET: return "Forget the calibration for both sticks.";
    default: return "Back to the controls.";
    }
}

static void draw_circle(float cx, float cy, float r, u32 col)
{
    for (int i = 0; i < 48; i++) {
        float a0 = (float) i * 6.2831853f / 48.0f, a1 = (float) (i + 1) * 6.2831853f / 48.0f;
        vita2d_draw_line(cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), col);
    }
}

// one stick: grey dot = raw, yellow dot = what Melee gets. The ring is full
// tilt in Melee (radius 80 of 127), the dark disc is the deadzone.
static void draw_stick(vita2d_pgf* font, const SceCtrlData* pad, int stick, float cx, float cy)
{
    const float r = 90.0f; // 127 units
    const MeleeVitaStickCal* cal = cal_stick == stick ? &cal_new : &cals[stick];
    char text[64];
    int x, y;
    int8_t gx, gy;
    float mx, my, len;

    stick_raw(pad, stick, &x, &y);
    vita2d_draw_rectangle(cx - r, cy - r, r * 2, r * 2, RGBA8(30, 30, 44, 255));
    vita2d_draw_fill_circle(cx, cy, (float) cals[stick].dead * r / 100.0f, RGBA8(60, 60, 90, 255));
    draw_circle(cx, cy, r * 80.0f / 127.0f, RGBA8(120, 120, 140, 255));
    vita2d_draw_line(cx - r, cy, cx + r, cy, RGBA8(60, 60, 80, 255));
    vita2d_draw_line(cx, cy - r, cx, cy + r, RGBA8(60, 60, 80, 255));
    if (cal_stick == stick && cal_step == 1) {
        // what's been reached so far, in raw space
        float x0 = cx + (float) (cal_new.min_x - 128) * r / 127.0f, x1 = cx + (float) (cal_new.max_x - 128) * r / 127.0f;
        float y0 = cy + (float) (cal_new.min_y - 128) * r / 127.0f, y1 = cy + (float) (cal_new.max_y - 128) * r / 127.0f;
        u32 col = RGBA8(90, 200, 120, 255);
        vita2d_draw_line(x0, y0, x1, y0, col);
        vita2d_draw_line(x1, y0, x1, y1, col);
        vita2d_draw_line(x1, y1, x0, y1, col);
        vita2d_draw_line(x0, y1, x0, y0, col);
    }
    vita2d_draw_fill_circle(cx + (float) (x - 128) * r / 127.0f, cy + (float) (y - 128) * r / 127.0f, 5.0f,
                            RGBA8(150, 150, 150, 255));
    if (cal_stick == stick) {
        // mid-calibration: the old values would be misleading
        snprintf(text, sizeof(text), "raw %d, %d", x, y);
        vita2d_pgf_draw_text(font, cx - r, cy + r + 22, RGBA8(200, 200, 200, 255), 0.8f, text);
        return;
    }
    melee_vita_pad_stick_apply(cal, x, y, &gx, &gy);
    // what Melee sees: clamped to a radius of 80, then 80 = 1.0
    mx = (float) gx;
    my = (float) gy;
    len = sqrtf(mx * mx + my * my);
    if (len > 80.0f) { mx *= 80.0f / len; my *= 80.0f / len; }
    vita2d_draw_fill_circle(cx + (float) gx * r / 127.0f, cy - (float) gy * r / 127.0f, 6.0f,
                            RGBA8(255, 210, 60, 255));
    snprintf(text, sizeof(text), "raw %d, %d   game %.2f, %.2f", x, y, mx / 80.0f, my / 80.0f);
    vita2d_pgf_draw_text(font, cx - r, cy + r + 22, RGBA8(200, 200, 200, 255), 0.8f, text);
}

static void sticks_draw(vita2d_pgf* font, const SceCtrlData* pad, int sub_row)
{
    char val[16];
    vita2d_pgf_draw_text(font, 60, 56, RGBA8(255, 210, 60, 255), 1.3f, "Stick Calibration");
    vita2d_pgf_draw_text(font, 160, 92, RGBA8(150, 150, 150, 255), 0.8f, "LEFT STICK");
    vita2d_pgf_draw_text(font, 590, 92, RGBA8(150, 150, 150, 255), 0.8f, "RIGHT STICK (C-STICK)");
    draw_stick(font, pad, MELEE_VITA_STICK_LEFT, 250.0f, 200.0f);
    draw_stick(font, pad, MELEE_VITA_STICK_RIGHT, 710.0f, 200.0f);
    for (int i = 0; i <= STICK_BACK; i++) {
        int y = 340 + i * 24 + (i >= STICK_RESET ? 8 : 0);
        val[0] = 0;
        if (i == STICK_DEAD_LEFT || i == STICK_DEAD_RIGHT)
            snprintf(val, sizeof(val), "%d%%", cals[i == STICK_DEAD_LEFT ? 0 : 1].dead);
        draw_row(font, y, cal_stick < 0 && i == sub_row, stick_rows[i], val);
    }
}

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
    // controls live in control-settings.bin (pad.c), shared with the
    // in-game Vita Options panel
    PADInit();
    for (int b = 0; b < MELEE_VITA_BUTTON_COUNT; b++)
        mapping[b] = (uint8_t) melee_vita_pad_get_mapping(b);
    for (int i = 0; i < MELEE_VITA_STICK_COUNT; i++) melee_vita_pad_get_stick_cal(i, &cals[i]);

    vita2d_init();
    vita2d_set_clear_color(RGBA8(16, 16, 24, 255));
    font = vita2d_load_default_pgf();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE); // same as the game, so calibration matches

    while (action < 0) {
        u32 pressed, buttons;
        int dir = 0;
        const char* help = "";

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        buttons = pad.buttons;
        // left stick works like the d-pad, except where the sticks are being tested
        if (page != PAGE_STICKS) {
            if (pad.ly < 40) buttons |= SCE_CTRL_UP;
            if (pad.ly > 215) buttons |= SCE_CTRL_DOWN;
            if (pad.lx < 40) buttons |= SCE_CTRL_LEFT;
            if (pad.lx > 215) buttons |= SCE_CTRL_RIGHT;
        }
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
            if ((pressed & SCE_CTRL_CROSS) && row < ROW_RESET && row != ROW_STAGES && row != ROW_CONTROLS)
                dir = 1;
            if (dir) change(&g_gfx, row, dir);
            if (pressed & SCE_CTRL_CROSS) {
                if (row == ROW_STAGES) { page = PAGE_STAGES; sub_row = 0; }
                if (row == ROW_CONTROLS) { page = PAGE_CONTROLS; sub_row = 0; }
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
        } else if (page == PAGE_STICKS) {
            help = sticks_input(&pad, pressed, dir, &sub_row, &page);
        } else {
            const int rows = CONTROLS_BACK + 1;
            if (pressed & SCE_CTRL_UP) sub_row = (sub_row + rows - 1) % rows;
            if (pressed & SCE_CTRL_DOWN) sub_row = (sub_row + 1) % rows;
            if (sub_row < MELEE_VITA_ACTION_COUNT && (dir || (pressed & SCE_CTRL_CROSS))) {
                // move this GameCube button to the next/previous Vita button;
                // whatever was there swaps over (pad_mapping.h)
                int cur = 0;
                if (!dir) dir = 1;
                while (cur < MELEE_VITA_BUTTON_COUNT && mapping[cur] != sub_row) cur++;
                if (cur < MELEE_VITA_BUTTON_COUNT) {
                    int next = (cur + dir + MELEE_VITA_BUTTON_COUNT) % MELEE_VITA_BUTTON_COUNT;
                    melee_vita_pad_mapping_assign(mapping, next, sub_row);
                }
            }
            if (sub_row == CONTROLS_RESET && (pressed & SCE_CTRL_CROSS))
                melee_vita_pad_mapping_defaults(mapping);
            if ((pressed & SCE_CTRL_CIRCLE) || (sub_row == CONTROLS_BACK && (pressed & SCE_CTRL_CROSS)))
                page = PAGE_MAIN;
            if (sub_row == CONTROLS_STICKS && (pressed & SCE_CTRL_CROSS)) {
                page = PAGE_STICKS;
                sub_row = 0;
            }
            help = sub_row == CONTROLS_BACK ? "Back to the main settings."
                 : sub_row == CONTROLS_RESET ? "Put every button back where it started (sticks are not changed)."
                 : sub_row == CONTROLS_STICKS ? "X: test both sticks, fix drift and range, set a deadzone."
                 : "Left/Right: pick the Vita button. The button that had it before swaps over.";
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
                    if (i == ROW_STAGES || i == ROW_CONTROLS) {
                        char name[48];
                        snprintf(name, sizeof(name), "%s  >", row_names[i]);
                        draw_row(font, y, i == row, name, i == ROW_STAGES ? val : "");
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
            } else if (page == PAGE_STICKS) {
                sticks_draw(font, &pad, sub_row);
            } else {
                vita2d_pgf_draw_text(font, 60, 56, RGBA8(255, 210, 60, 255), 1.3f, "Controls");
                vita2d_pgf_draw_text(font, 70, 88, RGBA8(150, 150, 150, 255), 0.8f, "GAMECUBE BUTTON");
                vita2d_pgf_draw_text(font, 480, 88, RGBA8(150, 150, 150, 255), 0.8f, "VITA BUTTON");
                for (int i = 0; i <= CONTROLS_BACK; i++) {
                    int y = 120 + i * 28 + (i >= CONTROLS_STICKS ? 16 : 0);
                    if (i < MELEE_VITA_ACTION_COUNT) {
                        int b = 0;
                        while (b < MELEE_VITA_BUTTON_COUNT && mapping[b] != i) b++;
                        draw_row(font, y, i == sub_row, action_names[i],
                                 b < MELEE_VITA_BUTTON_COUNT ? vita_button_names[b] : "-");
                    } else {
                        draw_row(font, y, i == sub_row,
                                 i == CONTROLS_STICKS ? "CALIBRATE STICKS  >"
                                 : i == CONTROLS_RESET ? "RESET CONTROLS" : "BACK", "");
                    }
                }
            }
            vita2d_pgf_draw_text(font, 60, 496, RGBA8(200, 200, 200, 255), 0.85f, help);
            vita2d_pgf_draw_text(font, 60, 526, RGBA8(140, 140, 140, 255), 0.8f,
                                 page == PAGE_MAIN
                                     ? "Up/Down: select   Left/Right or X: change   O: quit without saving"
                                 : page == PAGE_STICKS && cal_stick >= 0 ? "X: next step   O: cancel"
                                 : page == PAGE_STICKS ? "D-pad: select   Left/Right or X: change   O: back"
                                     : "Up/Down: select   Left/Right or X: change   O: back");
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    if (action != 0) {
        gfx_settings_save();
        melee_vita_pad_replace_mapping(mapping);
        melee_vita_pad_replace_stick_cal(cals);
    }
    vita2d_wait_rendering_done();
    if (font) vita2d_free_pgf(font);
    vita2d_fini();

    if (action == ROW_START) {
        // start fresh without the -settings param
        sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    }
    sceKernelExitProcess(0);
}
