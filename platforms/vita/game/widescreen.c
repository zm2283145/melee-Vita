/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita implementation of the PC widescreen contract.
 *
 * Mirrors src/pc/widescreen.c: scenes that support widescreen (gameplay) map
 * the logical 640x480 framebuffer onto the whole 960x544 screen and the game
 * widens its SCREEN-pass projections by pc_widescreen_scale(); every other
 * scene is shown at Melee's original 73:60 display aspect, pillarboxed. */
#include <pc/widescreen.h>
#include <pc/pc.h>

#include <math.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/tobj.h>

/* GameCube NTSC pixels are 73:80, so Melee's 640x480 picture is 73:60. */
#define ORIGINAL_ASPECT (73.0f / 60.0f)
#define VITA_ASPECT (960.0f / 544.0f)

static int s_mode = 1;
static bool s_supported;

void pc_widescreen_set_mode(int mode)
{
    s_mode = mode >= 0 && mode <= 2 ? mode : 1;
}

void pc_widescreen_set_scene(bool supported)
{
    s_supported = supported;
}

void pc_widescreen_update(void) {}

/* Whether the logical framebuffer currently spans the full Vita screen. */
int melee_vita_widescreen_active(void)
{
    return s_mode != 0 && s_supported;
}

float pc_widescreen_scale(void)
{
    if (!melee_vita_widescreen_active()) return 1.0f;
    if (HSD_GetCurrentRenderPass() != HSD_RP_SCREEN) return 1.0f;
    return VITA_ASPECT / ORIGINAL_ASPECT;
}

float pc_widescreen_cobj_scale(struct HSD_CObj* cobj)
{
    if (cobj != NULL && (HSD_CObjGetFlags(cobj) & PC_COBJ_FILL_FRAME)) return 1.0f;
    return pc_widescreen_scale();
}

float pc_widescreen_hud_offset(void)
{
    float scale;
    if (pc_get_hud_mode() != 1) return 0.0f;
    scale = pc_widescreen_scale();
    return scale <= 1.0f ? 0.0f : (scale - 1.0f) * 320.0f;
}

#define PC_HUD_WORLD_SCALE 0.09125f

float pc_widescreen_hud_timer_x(float original_x)
{
    const float offset = pc_widescreen_hud_offset();
    return offset <= 0.0f ? original_x : original_x + offset * PC_HUD_WORLD_SCALE;
}

float pc_widescreen_hud_player_x(int player_idx, int total_players, float original_x)
{
    const float offset = pc_widescreen_hud_offset();
    float t;
    if (offset <= 0.0f || total_players <= 1 || player_idx < 0 || player_idx >= total_players)
        return original_x;
    t = -1.0f + 2.0f * (float) player_idx / (float) (total_players - 1);
    return original_x + t * (offset * PC_HUD_WORLD_SCALE);
}

void pc_widescreen_copy_efb(struct HSD_ImageDesc* image, int origin_x,
                            int origin_y, float center_x, int clear)
{
    float scale, left, right;
    u16 width;
    if (image == NULL) return;
    scale = pc_widescreen_scale();
    if (scale <= 1.0f) {
        HSD_ImageDescCopyFromEFB(image, origin_x, origin_y, clear, true);
        return;
    }
    left = (float) origin_x;
    right = origin_x + image->width;
    pc_widescreen_widen(1.0f / scale, center_x, &left, &right);
    width = image->width;
    image->width = (u16) lroundf(right - left);
    HSD_ImageDescCopyFromEFB(image, (u16) lroundf(left), (u16) origin_y, clear, true);
    image->width = width;
}
