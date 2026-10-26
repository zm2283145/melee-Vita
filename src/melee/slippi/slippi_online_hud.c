/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi Direct online play, the text over the match: each player's name
 * above their damage display, "Delay: Nf", and DISCONNECTED, DESYNC
 * DETECTED or "Desync Risk" (Slippi's InitInGame.asm and StartEngineLoop's
 * status texts).
 *
 * The 3DS Slippi port draws these as SIS text on a copy of the HUD camera
 * (native-3ds/port/engine/slippi/online_hud.c).  Here they are flat shapes
 * in display pixels, handed to the renderer to draw over the finished
 * frame (melee_vita_gxm_set_screen_overlay), like the replay test's panel.
 * Drawing with GX during a match broke the match's own drawing.
 *
 * Positions are Slippi's, in the HUD's 640 x 480 space with the centre at
 * 0, 0 and y going down, then scaled to the display's 544 lines. */
#ifdef MELEE_VITA_SLIPPI /* only in -EnableSlippi builds */
#include "slippi_online.h"

#include "slippi_platform.h"

#include <melee/if/ifall.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/hsd_3915.h>

#include <gxm_game.h>

#include <stdio.h>

#define HUD_MAX_LINE_VERTICES 8192
#define HUD_MAX_TRIANGLE_VERTICES 1536

static MeleeVitaScreenVertex s_lines[HUD_MAX_LINE_VERTICES];
static u32 s_line_count;
static MeleeVitaScreenVertex s_triangles[HUD_MAX_TRIANGLE_VERTICES];
static u32 s_triangle_count;

/* A colour the way the renderer wants it (vita2d's RGBA8). */
static u32 hud_color(u8 r, u8 g, u8 b, u8 a)
{
    return ((u32) a << 24) | ((u32) b << 16) | ((u32) g << 8) | (u32) r;
}

/* From the HUD's 640 x 480 space to display pixels. */
static float display_x(float hud_x)
{
    return 480.0f + hud_x * (544.0f / 480.0f);
}

static float display_y(float hud_y)
{
    return 272.0f + hud_y * (544.0f / 480.0f);
}

static void set_vertex(MeleeVitaScreenVertex* vertex, float x, float y, u32 color)
{
    vertex->x = x;
    vertex->y = y;
    vertex->z = 0.0f;
    vertex->u = 0.0f;
    vertex->v = 0.0f;
    vertex->color = color;
}

/* A small filled square, for the dots of '.' and ':'. */
static void hud_dot(float x, float y, float size, u32 color)
{
    MeleeVitaScreenVertex* v;

    if (s_triangle_count + 6 > HUD_MAX_TRIANGLE_VERTICES) return;
    v = &s_triangles[s_triangle_count];
    set_vertex(&v[0], x, y, color);
    set_vertex(&v[1], x + size, y, color);
    set_vertex(&v[2], x + size, y + size, color);
    set_vertex(&v[3], x, y, color);
    set_vertex(&v[4], x + size, y + size, color);
    set_vertex(&v[5], x, y + size, color);
    s_triangle_count += 6;
}

/* One stroke.  The renderer's lines are one pixel wide, so each stroke
 * goes in twice, the second one pixel to the right. */
static void hud_stroke(float x0, float y0, float x1, float y1, u32 color)
{
    MeleeVitaScreenVertex* v;

    if (s_line_count + 4 > HUD_MAX_LINE_VERTICES) return;
    v = &s_lines[s_line_count];
    set_vertex(&v[0], x0, y0, color);
    set_vertex(&v[1], x1, y1, color);
    set_vertex(&v[2], x0 + 1.0f, y0, color);
    set_vertex(&v[3], x1 + 1.0f, y1, color);
    s_line_count += 4;
}

/* One character of the game's debug font, height pixels tall.  The font
 * has capitals only, so lower case shows as capitals. */
static void hud_char(int chr, float x, float y, float height, u32 color)
{
    float width = height * 0.73f;
    float dot = height * 0.13f;
    const u8* strokes;
    int i;

    if (dot < 2.0f) dot = 2.0f;
    if (chr == '.') {
        hud_dot(x + width * 0.3f, y + height * 0.9f - dot, dot, color);
        return;
    }
    if (chr == ':') {
        hud_dot(x + width * 0.3f, y + height * 0.3f - dot * 0.5f, dot, color);
        hud_dot(x + width * 0.3f, y + height * 0.7f - dot * 0.5f, dot, color);
        return;
    }
    if (chr == '#') {
        /* Connect codes have one, and the debug font doesn't. */
        hud_stroke(x + width * 0.25f, y + height * 0.1f, x + width * 0.2f, y + height * 0.9f, color);
        hud_stroke(x + width * 0.55f, y + height * 0.1f, x + width * 0.5f, y + height * 0.9f, color);
        hud_stroke(x + width * 0.0f, y + height * 0.35f, x + width * 0.7f, y + height * 0.35f, color);
        hud_stroke(x + width * 0.0f, y + height * 0.65f, x + width * 0.7f, y + height * 0.65f, color);
        return;
    }

    strokes = HSD_DebugFontStrokes(chr);
    if (strokes == NULL) return;
    i = 0;
    while (i < 0x29 && strokes[i] != 0xFF) {
        u8 p0 = strokes[i];
        u8 p1 = strokes[i + 1];
        i += 2;
        hud_stroke(x + width * (0.11f * (p0 >> 4)), y + height * (0.11f * (p0 & 0xF)),
                   x + width * (0.11f * (p1 >> 4)), y + height * (0.11f * (p1 & 0xF)), color);
    }
}

/* One line of text.  align 0 starts at x, 1 centres on x, 2 ends at x.
 * A dark copy one pixel down and right keeps it readable on any stage. */
static void hud_text(const char* text, float x, float y, float height, int align, u32 color)
{
    float advance = height * 0.73f + height * 0.18f;
    float width = 0.0f;
    u32 shadow = hud_color(0x00, 0x00, 0x00, (u8) (color >> 24));
    float pen;
    int count = 0;
    int i;

    while (text[count] != '\0') count++;
    width = advance * (float) count;
    if (align == 1) {
        x = x - width * 0.5f;
    } else if (align == 2) {
        x = x - width;
    }
    pen = x;
    for (i = 0; i < count; i++) {
        hud_char(text[i], pen + 1.0f, y + 1.0f, height, shadow);
        hud_char(text[i], pen, y, height, color);
        pen += advance;
    }
}

/* Called from DevText_DrawAll on every screen.  Only fills in the shapes;
 * the renderer draws them after the frame. */
void mp_slippi_online_draw_hud(void)
{
    u32 white = hud_color(0xFF, 0xFF, 0xFF, 0xFF);
    u32 faint = hud_color(0xFF, 0xFF, 0xFF, 0x80);
    u32 red = hud_color(0xFF, 0x00, 0x00, 0xFF);
    u32 amber = hud_color(0xFF, 0xB8, 0x00, 0xFF);
    char line[64];
    int status;
    int i;

    if (!mp_slippi_online_active()) return;
    s_line_count = 0;
    s_triangle_count = 0;

    /* The HUD hides while the game is paused, and this text with it. */
    if (!ifAll_IsHUDHidden()) {
        /* Names over the damage displays (InitInGame).  The HUD's
         * positions are in units of 10 of the 640 x 480 space. */
        for (i = 0; i < 2; i++) {
            Vec3* hud;
            if (Player_GetPlayerSlotType(i) == Gm_PKind_NA) continue;
            if (!mp_platform_slippi_ui_name(i == mp_slippi_online_local_index(), line,
                                            sizeof(line)))
            {
                continue;
            }
            hud = ifAll_GetPlayerHUDPosition(i);
            hud_text(line, display_x((hud->x + 0.8f) * 10.0f), display_y(206.4f) - 13.0f, 12.0f,
                     1, white);
        }

        /* "Delay: Nf", bottom right, half transparent. */
        snprintf(line, sizeof(line), "Delay: %df", mp_slippi_online_delay());
        hud_text(line, display_x(270.0f), display_y(207.0f), 13.0f, 2, faint);

        if (mp_slippi_online_desync_risk()) {
            hud_text("Desync Risk", display_x(228.0f), display_y(194.0f) - 16.0f, 14.0f, 1, amber);
        }
    }

    /* DISCONNECTED and DESYNC DETECTED stay up even when paused. */
    status = mp_slippi_online_status();
    if (status == 1) {
        hud_text("DISCONNECTED", display_x(9.0f), display_y(-162.0f), 24.0f, 1, red);
    } else if (status == 2) {
        hud_text("DESYNC DETECTED", display_x(9.0f), display_y(-140.0f), 18.0f, 1, amber);
    }

    melee_vita_gxm_set_screen_overlay(s_triangles, s_triangle_count, s_lines, s_line_count);
}

#endif /* MELEE_VITA_SLIPPI */
