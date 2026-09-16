/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fixed-aspect Vita implementation of the PC widescreen contract. */
#include <pc/widescreen.h>

#include <math.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/tobj.h>

#define VITA_WIDESCREEN_SCALE (320.0f / 219.0f)

static int s_mode = 1;
static bool s_scene_supports_widescreen;

void pc_widescreen_set_mode(int mode)
{
    s_mode = mode >= 0 && mode <= 2 ? mode : 1;
}

void pc_widescreen_set_scene(bool supported)
{
    s_scene_supports_widescreen = supported;
}

void pc_widescreen_update(void)
{
}

float pc_widescreen_scale(void)
{
    return s_mode != 0 && s_scene_supports_widescreen ? VITA_WIDESCREEN_SCALE
                                                      : 1.0f;
}

float pc_widescreen_cobj_scale(struct HSD_CObj* cobj)
{
    if (cobj != NULL && (HSD_CObjGetFlags(cobj) & PC_COBJ_FILL_FRAME)) return 1.0f;
    return pc_widescreen_scale();
}

void pc_widescreen_copy_efb(struct HSD_ImageDesc* image, int origin_x,
                            int origin_y, float center_x, int clear)
{
    float scale;
    float left;
    float right;
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
    HSD_ImageDescCopyFromEFB(image, (u16) lroundf(left), (u16) origin_y,
                             clear, true);
    image->width = width;
}
