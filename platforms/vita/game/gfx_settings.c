/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gfx_settings.h"
#include "gxm_resolution.h"
#include "../vita_log.h"

#include <dolphin/types.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <vita2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GFX_PATH "ux0:data/melee/graphics.txt"
#define GFX_TMP_PATH "ux0:data/melee/graphics.tmp"

#ifndef MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION
#define MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION MELEE_VITA_RESOLUTION_NATIVE
#endif

/* these live in gx.c so the PS5 build (which shares gx.c and psdisp.c)
 * keeps the old values */
extern u32 g_melee_vita_particle_limit;
extern u32 g_melee_vita_copy_interval;
extern int g_melee_vita_min_ticks; // gmscene.c, logic ticks per drawn frame

GfxSettings g_gfx;
GfxSettings g_gfx_custom;

// StKind values (src/melee/gr/forward.h)
const struct GfxStage gfx_stages[GFX_STAGE_COUNT] = {
    { 0x1F, "Battlefield", 0 },
    { 0x20, "Final Destination", 0 },
    { 0x08, "Yoshi's Story", 1 },
    { 0x03, "Pokemon Stadium", 0 },
    { 0x02, "Fountain of Dreams", 1 },
    { 0x1C, "Dream Land N64", 0 },
};

extern int g_melee_vita_skip_stage_particles; // gx.c, read by psdisp.c
extern void pc_widescreen_set_mode(int mode);   // widescreen.c: 0 = 4:3, 1 = 16:9

// options that aren't part of any preset: copied across when presets change
static void copy_extras(GfxSettings* dst, const GfxSettings* src)
{
    dst->boost = src->boost;
    dst->widescreen = src->widescreen;
    dst->show_fps = src->show_fps;
}
int g_gfx_preset = GFX_PRESET_CUSTOM;

static const char* preset_names[] = { "ULTRA PERFORMANCE", "PERFORMANCE", "QUALITY", "CUSTOM" };

void gfx_settings_preset(GfxSettings* s, int preset)
{
    memset(s, 0, sizeof(*s));
    // the CPU/GPU boost isn't part of any preset, it's its own switch
    if (preset != GFX_PRESET_CUSTOM) s->heavy30 = 1; // 4 players at 30 in every preset
    if (preset == GFX_PRESET_ULTRA) {
        // Performance at 60% with half the particles, no stage particles
        // anywhere and Fountain of Dreams and Yoshi's Story in potato mode,
        // so every 1v1 runs at 60
        s->stage_lite = (1 << GFX_STAGE_COUNT) - 1;
        s->stage_potato = GFX_POTATO_STAGES;
        s->game_res = MELEE_VITA_RESOLUTION_60;
        s->particles = 128;
        s->live_screens = 8;
        s->frame_rate = FRAME_RATE_60;
    } else if (preset == GFX_PRESET_PERFORMANCE) {
        s->stage_lite = (1 << GFX_STAGE_COUNT) - 1; // no stage particles
        s->game_res = MELEE_VITA_RESOLUTION_75;
        s->particles = 256;
        s->live_screens = 4;
        s->frame_rate = FRAME_RATE_60;
    } else if (preset == GFX_PRESET_QUALITY) {
        // Fountain of Dreams on lite: with the fighters in its water it went
        // over 33 ms. Live screens every 2nd frame for Pokemon Stadium 4P.
        s->stage_lite = 1 << 4; // gfx_stages[4] = Fountain of Dreams
        s->game_res = MELEE_VITA_RESOLUTION_NATIVE;
        s->particles = 2048;
        s->live_screens = 2;
        s->frame_rate = FRAME_RATE_30;
    } else {
        // Custom starts out like the release build before this menu existed:
        // build's gameplay resolution, no cutbacks, no 30 fps lock
        s->game_res = MELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION;
        s->particles = 512;
        s->live_screens = 4;
        s->frame_rate = FRAME_RATE_60;
    }
}

// which preset these values are (the extras don't count), or CUSTOM
int gfx_settings_match(const GfxSettings* s)
{
    for (int p = 0; p < GFX_PRESET_CUSTOM; p++) {
        GfxSettings v;
        gfx_settings_preset(&v, p);
        copy_extras(&v, s); // not part of a preset
        if (memcmp(&v, s, sizeof(v)) == 0) return p;
    }
    return GFX_PRESET_CUSTOM;
}

// g_gfx was edited: a value that differs from the preset makes it Custom
void gfx_settings_changed(void)
{
    g_gfx_preset = gfx_settings_match(&g_gfx);
    if (g_gfx_preset == GFX_PRESET_CUSTOM) g_gfx_custom = g_gfx;
    copy_extras(&g_gfx_custom, &g_gfx);
}

// preset row: load that preset's values (Custom brings back the user's own)
void gfx_settings_select(int preset)
{
    copy_extras(&g_gfx_custom, &g_gfx);
    gfx_settings_resolve(&g_gfx, preset, &g_gfx_custom);
    g_gfx_preset = preset;
}

void gfx_settings_resolve(GfxSettings* out, int preset, const GfxSettings* custom)
{
    if (preset == GFX_PRESET_CUSTOM) {
        *out = *custom;
        return;
    }
    gfx_settings_preset(out, preset);
    copy_extras(out, custom); // not part of a preset
}

const char* gfx_settings_preset_name(int preset)
{
    if (preset < 0 || preset >= GFX_PRESET_COUNT)
        preset = GFX_PRESET_CUSTOM;
    return preset_names[preset];
}

static int res_to_percent(int res)
{
    switch (res) {
    case MELEE_VITA_RESOLUTION_75: return 75;
    case MELEE_VITA_RESOLUTION_60: return 60;
    case MELEE_VITA_RESOLUTION_50: return 50;
    default: return 100;
    }
}

static int percent_to_res(int pct)
{
    if (pct >= 100) return MELEE_VITA_RESOLUTION_NATIVE;
    if (pct >= 75) return MELEE_VITA_RESOLUTION_75;
    if (pct >= 60) return MELEE_VITA_RESOLUTION_60;
    return MELEE_VITA_RESOLUTION_50;
}

static void clamp_settings(GfxSettings* s)
{
    if (s->game_res < 0 || s->game_res >= MELEE_VITA_RESOLUTION_OPTION_COUNT)
        s->game_res = MELEE_VITA_RESOLUTION_NATIVE;
    if (s->particles < 16) s->particles = 16;
    if (s->particles > 4096) s->particles = 4096;
    if (s->live_screens < 1) s->live_screens = 1;
    if (s->live_screens > 32) s->live_screens = 32;
    s->boost = s->boost != 0;
    s->vsync = s->vsync != 0;
    s->widescreen = s->widescreen != 0;
    s->show_fps = s->show_fps != 0;
    s->heavy30 = s->heavy30 != 0;
    s->stage_lite &= (1 << GFX_STAGE_COUNT) - 1;
    s->stage_potato &= GFX_POTATO_STAGES;
    if (s->frame_rate != FRAME_RATE_30) s->frame_rate = FRAME_RATE_60;
}

// defaults: Custom with the values the game used before this menu existed
static void set_defaults(void)
{
    gfx_settings_preset(&g_gfx_custom, GFX_PRESET_CUSTOM);
    g_gfx_custom.widescreen = 1;
    g_gfx_preset = GFX_PRESET_CUSTOM;
}

void gfx_settings_reset(void)
{
    set_defaults();
    g_gfx = g_gfx_custom;
}

/* Before graphics.txt the in-game Vita Options panel saved its resolutions
 * here. Only the gameplay one carries over, menus are always native now. */
#define OLD_RES_PATH "ux0:data/melee/resolution-settings.bin"
#define OLD_RES_MAGIC 0x31525356u
#define OLD_RES_VERSION 1u

static void migrate_old_resolution(GfxSettings* c)
{
    struct { u32 magic, version; s32 menu_option, gameplay_option; } old;
    FILE* f = fopen(OLD_RES_PATH, "rb");
    bool ok;
    if (!f) return;
    ok = fread(&old, sizeof(old), 1u, f) == 1u && old.magic == OLD_RES_MAGIC &&
         old.version == OLD_RES_VERSION && old.gameplay_option >= MELEE_VITA_RESOLUTION_NATIVE &&
         old.gameplay_option < MELEE_VITA_RESOLUTION_OPTION_COUNT;
    fclose(f);
    if (!ok) return;
    c->game_res = old.gameplay_option;
    melee_vita_log_info("[GFX] took gameplay resolution %d from resolution-settings.bin",
                        (int) old.gameplay_option);
    // write graphics.txt now so this only happens once; the old file stays
    // in case someone goes back to an older build
    if (gfx_settings_save()) melee_vita_log_info("[GFX] saved " GFX_PATH);
}

void gfx_settings_load(void)
{
    char line[128];
    FILE* f;

    GfxSettings* c = &g_gfx_custom;

    set_defaults(); // also for files without a preset line

    f = fopen(GFX_PATH, "r");
    if (!f) {
        migrate_old_resolution(c);
        gfx_settings_resolve(&g_gfx, g_gfx_preset, c);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[32], str[32];
        int val;
        if (line[0] == '#') continue;
        if (sscanf(line, " %31[^= ] = %31s", name, str) != 2) continue;
        val = atoi(str);

        if (!strcmp(name, "preset"))
            g_gfx_preset = !strcmp(str, "performance") ? GFX_PRESET_PERFORMANCE
                         : !strcmp(str, "quality") ? GFX_PRESET_QUALITY
                         : !strcmp(str, "ultra") ? GFX_PRESET_ULTRA : GFX_PRESET_CUSTOM;
        else if (!strcmp(name, "gameplay_resolution")) c->game_res = percent_to_res(val);
        else if (!strcmp(name, "cpu_boost")) c->boost = val;
        else if (!strcmp(name, "particles")) c->particles = val;
        else if (!strcmp(name, "live_screens")) c->live_screens = val;
        else if (!strcmp(name, "vsync")) c->vsync = val;
        else if (!strcmp(name, "show_fps")) c->show_fps = val;
        else if (!strcmp(name, "widescreen")) c->widescreen = val;
        else if (!strcmp(name, "frame_rate"))
            c->frame_rate = val > 0 && val <= 30 ? FRAME_RATE_30 : FRAME_RATE_60;
        else if (!strcmp(name, "lock_30_big_matches")) c->heavy30 = val;
        else if (!strncmp(name, "lite_", 5)) {
            // lite_<stage kind in hex>=1
            int kind = (int) strtol(name + 5, NULL, 16);
            for (int i = 0; i < GFX_STAGE_COUNT; i++)
                if (gfx_stages[i].kind == kind) {
                    if (val >= 1) c->stage_lite |= 1 << i;
                    else c->stage_lite &= ~(1 << i);
                    if (val >= 2) c->stage_potato |= 1 << i;
                    else c->stage_potato &= ~(1 << i);
                }
        }
    }
    fclose(f);
    clamp_settings(c);
    gfx_settings_resolve(&g_gfx, g_gfx_preset, c);
    if (g_gfx_preset == GFX_PRESET_CUSTOM) g_gfx_preset = gfx_settings_match(&g_gfx);
}

bool gfx_settings_save(void)
{
    const GfxSettings* c = &g_gfx_custom;
    FILE* f;
    bool ok;

    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/melee", 0777);
    f = fopen(GFX_TMP_PATH, "w");
    if (!f) return false;
    fprintf(f, "# Melee Vita graphics settings\n");
    fprintf(f, "# Change these from the Settings button on the LiveArea screen.\n");
    fprintf(f, "# preset: ultra, performance, quality or custom. The values below are\n");
    fprintf(f, "# the custom ones; the other presets ignore them, apart from cpu_boost,\n");
    fprintf(f, "# widescreen and show_fps, which apply to every preset.\n");
    fprintf(f, "preset=%s\n", g_gfx_preset == GFX_PRESET_ULTRA ? "ultra"
                             : g_gfx_preset == GFX_PRESET_PERFORMANCE ? "performance"
                             : g_gfx_preset == GFX_PRESET_QUALITY ? "quality" : "custom");
    fprintf(f, "gameplay_resolution=%d\n", res_to_percent(c->game_res));
    fprintf(f, "cpu_boost=%d\n", c->boost);
    fprintf(f, "particles=%d\n", c->particles);
    fprintf(f, "live_screens=%d\n", c->live_screens);
    fprintf(f, "vsync=%d\n", c->vsync);
    fprintf(f, "# 1 = 16:9 matches, 0 = 4:3 everywhere\n");
    fprintf(f, "widescreen=%d\n", c->widescreen);
    fprintf(f, "show_fps=%d\n", c->show_fps);
    fprintf(f, "frame_rate=%d\n", c->frame_rate == FRAME_RATE_30 ? 30 : 60);
    fprintf(f, "# 30 fps for 3-4 players, or Fountain of Dreams without cutbacks\n");
    fprintf(f, "lock_30_big_matches=%d\n", c->heavy30);
    fprintf(f, "# stage cutbacks: 0 off, 1 lite (no stage particles), 2 potato\n");
    for (int i = 0; i < GFX_STAGE_COUNT; i++)
        fprintf(f, "lite_%02x=%d   # %s\n", (unsigned) gfx_stages[i].kind,
                (c->stage_potato >> i) & 1 ? 2 : (c->stage_lite >> i) & 1, gfx_stages[i].name);
    ok = fclose(f) == 0;
    if (ok) {
        remove(GFX_PATH);
        ok = rename(GFX_TMP_PATH, GFX_PATH) == 0;
    }
    if (!ok) {
        remove(GFX_TMP_PATH);
        melee_vita_log_info("[GFX] failed to save " GFX_PATH);
    }
    return ok;
}

int g_melee_vita_fps30_active;

static void set_fps30(int on)
{
    g_melee_vita_fps30_active = on;
    g_melee_vita_min_ticks = on ? 2 : 1;
}

// Potato mode draws just the stage parts you play on (map gobj index) and
// caps particles at 128. Everything it hides is scenery with no collision
// or logic.
// Fountain of Dreams keeps the main stage with its water (3) and the side
// platforms (4), and drops to 50%. Gone: the backdrop (0, 1), the
// platforms' reflections (2), the decorations (5-10) and the star points
// (-1, grIzumi_801CCB18).
// Yoshi's Story keeps Randall (2) and the main stage (3, which also spawns
// the Shy Guys; they're items and stay). Gone: the backdrop (0) and the
// animated scenery behind the stage (1). It was CPU-bound (draw 15.0 +
// logic 2.8 ms), so the resolution stays.
static const struct { int kind; int res; signed char keep[4]; } potato_parts[] = {
    { 0x02, MELEE_VITA_RESOLUTION_50, { 3, 4, -1 } }, // Fountain of Dreams
    { 0x08, -1, { 2, 3, -1 } },                       // Yoshi's Story
};
#define POTATO_COUNT ((int) (sizeof(potato_parts) / sizeof(potato_parts[0])))
static int potato_kind; // stage of the current match if it's in potato mode
// grizumi.c: 0 normal, 1 water reflects the stage only (lite), 2 stage only
// and drawn once per match (potato)
int g_melee_vita_potato_reflection;
u32 g_melee_vita_match_count; // grizumi.c: new match -> draw the reflection again
extern void melee_vita_gxm_match_gameplay_res(int option); // gxm_game.c

int melee_vita_stage_part_hidden(int part)
{
    if (!potato_kind) return 0;
    for (int i = 0; i < POTATO_COUNT; i++) {
        if (potato_parts[i].kind != potato_kind) continue;
        for (int k = 0; k < 4 && potato_parts[i].keep[k] >= 0; k++)
            if (potato_parts[i].keep[k] == part) return 0;
        return 1;
    }
    return 0;
}

// decided once when the match starts, so the frame rate never changes mid-match
void melee_vita_match_begin(int players, int stage_kind)
{
    const int fod = stage_kind == 0x02; // Fountain of Dreams
    int lite = 0, potato = 0;
    for (int i = 0; i < GFX_STAGE_COUNT; i++) {
        if (gfx_stages[i].kind != stage_kind) continue;
        lite |= (g_gfx.stage_lite >> i) & 1;
        potato |= (g_gfx.stage_potato >> i) & gfx_stages[i].potato;
    }
    // presets: only potato holds 30 on Fountain of Dreams with 3-4 players
    if (fod && players >= 3 && g_gfx_preset != GFX_PRESET_CUSTOM) potato = 1;
    lite = lite || potato;
    g_melee_vita_skip_stage_particles = lite;
    g_melee_vita_potato_reflection = fod && potato ? 2 : fod && lite ? 1 : 0;
    g_melee_vita_match_count++;
    potato_kind = potato ? stage_kind : 0;
    if (potato) {
        for (int i = 0; i < POTATO_COUNT; i++)
            if (potato_parts[i].kind == stage_kind && potato_parts[i].res >= 0)
                melee_vita_gxm_match_gameplay_res(potato_parts[i].res);
        g_melee_vita_particle_limit = 128;
    }

    if (g_gfx.frame_rate == FRAME_RATE_30) {
        set_fps30(1);
        return;
    }
    if (!g_gfx.heavy30) return;
    // Fountain of Dreams with lite or potato is light enough for 60 in 1v1,
    // except at Performance's 75%: there it stays at 30
    if (fod && lite && players <= 2 && g_gfx_preset != GFX_PRESET_PERFORMANCE) return;
    set_fps30(players >= 3 || fod);
    melee_vita_log_info("[GFX] match: %d players, stage 0x%02x -> %s fps", players, stage_kind,
                        g_melee_vita_fps30_active ? "30" : "60");
}

void melee_vita_match_end(void)
{
    g_melee_vita_skip_stage_particles = 0;
    g_melee_vita_potato_reflection = 0;
    if (potato_kind) {
        melee_vita_gxm_match_gameplay_res(-1);
        g_melee_vita_particle_limit = (u32) g_gfx.particles;
    }
    potato_kind = 0;
    set_fps30(0); // menus always run at 60
}

void gfx_settings_apply(void)
{
    g_melee_vita_particle_limit = (u32) g_gfx.particles;
    g_melee_vita_copy_interval = (u32) g_gfx.live_screens;
    pc_widescreen_set_mode(g_gfx.widescreen ? 1 : 0);
    set_fps30(0); // menus run at 60, the frame rate setting is for matches

    // off = leave whatever the system gave us
    if (g_gfx.boost) {
        int a = scePowerSetArmClockFrequency(444);
        int b = scePowerSetBusClockFrequency(222);
        int g = scePowerSetGpuClockFrequency(222);
        int x = scePowerSetGpuXbarClockFrequency(166);
        melee_vita_log_info("[GFX] clock boost results %08x %08x %08x %08x",
                            (u32) a, (u32) b, (u32) g, (u32) x);
    }
    melee_vita_log_info("[GFX] clocks arm=%d bus=%d gpu=%d xbar=%d particles=%d live_screens=%d",
                        scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                        scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(),
                        g_gfx.particles, g_gfx.live_screens);
}

/* ---- fps counter ---- */

extern u32 g_melee_vita_tick_total; // gmscene.c

static vita2d_pgf* font;
// written by the game thread once a second, read by the render thread
static volatile int fps_x10, game_x10, worst_ms_x10, arm_mhz, gpu_mhz;

// shared by the fps counter and the benchmark overlay, render thread only
vita2d_pgf* gfx_font(void)
{
    if (!font) {
        char ascii[96];
        font = vita2d_load_default_pgf();
        if (!font) return NULL;
        // put every glyph in the atlas now so it doesn't change mid-frame later
        for (int i = 0; i < 95; i++) ascii[i] = (char) (32 + i);
        ascii[95] = 0;
        vita2d_pgf_text_width(font, 1.0f, ascii);
    }
    return font;
}

void gfx_fps_tick(void)
{
    static u64 window_start, last;
    static u32 frames, start_ticks, worst_us;
    u64 now;

    if (!g_gfx.show_fps) return;
    now = sceKernelGetProcessTimeWide();
    if (window_start == 0) {
        window_start = last = now;
        start_ticks = g_melee_vita_tick_total;
        return;
    }
    if (now - last > worst_us) worst_us = (u32) (now - last);
    last = now;
    frames++;
    if (now - window_start >= 1000000u) {
        u64 span = now - window_start;
        fps_x10 = (int) (frames * 10000000ull / span);
        game_x10 = (int) ((g_melee_vita_tick_total - start_ticks) * 10000000ull / span);
        worst_ms_x10 = (int) (worst_us / 100u);
        arm_mhz = scePowerGetArmClockFrequency();
        gpu_mhz = scePowerGetGpuClockFrequency();
        window_start = now;
        start_ticks = g_melee_vita_tick_total;
        frames = 0;
        worst_us = 0;
    }
}

void gfx_fps_draw(void)
{
    char text[96];
    int f, g, w;

    if (!g_gfx.show_fps || !gfx_font()) return;
    f = fps_x10;
    g = game_x10;
    w = worst_ms_x10;
    snprintf(text, sizeof(text), "%d.%d FPS  game %d.%d/s  worst %d.%d ms  CPU %d GPU %d",
             f / 10, f % 10, g / 10, g % 10, w / 10, w % 10, arm_mhz, gpu_mhz);
    vita2d_draw_rectangle(4.0f, 4.0f, (float) vita2d_pgf_text_width(font, 0.8f, text) + 12.0f,
                          24.0f, RGBA8(0, 0, 0, 160));
    vita2d_pgf_draw_text(font, 10, 22, RGBA8(255, 255, 0, 255), 0.8f, text);
}
