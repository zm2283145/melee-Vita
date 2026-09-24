#include "gmpause.h"

#include "gm_unsplit.h"
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjobject.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/jobj.h>
#ifdef TARGET_VITA
#include <pad_vita.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>
// Packed from the source icons in platforms/vita/assets/buttons.
#include "vita_pause_exit_prompt.inc"
#endif

struct PauseData {
    /* +0 */ HSD_JObj* background;
    /* +4 */ HSD_JObj* analog_stick;
    /* +8 */ HSD_JObj* lras;
    /* +C */ HSD_JObj* z;
    /* +10 */ HSD_JObj* analog_stick_outline;
    /* +14 */ s32 slot;
};

static struct PauseData lbl_80479B10;
static HSD_Archive* lbl_804D6700;
static HSD_GObj* lbl_804D6704;

#ifdef TARGET_VITA
static HSD_ImageDesc gmPause_VitaExitPromptDesc;
static HSD_ImageDesc gmPause_VitaRetryPromptDesc;

static void gmPause_UseVitaPrompt(HSD_JObj* jobj, HSD_ImageDesc* desc,
                                  const u8* pixels, u16 width, u16 height)
{
    HSD_DObj* dobj = HSD_JObjGetDObj(jobj);
    HSD_TObj* tobj;
    if (dobj == NULL || dobj->mobj == NULL ||
        (tobj = dobj->mobj->tobj) == NULL)
    {
        return;
    }
    DP_SET(desc->image_ptr, pixels);
    desc->width = width;
    desc->height = height;
    desc->format = GX_TF_IA4;
    desc->mipmap = 0;
    desc->minLOD = 0.0F;
    desc->maxLOD = 0.0F;
    tobj->imagedesc = desc;
}

static void gmPause_UseVitaPrompts(void)
{
    gmPause_UseVitaPrompt(lbl_80479B10.lras, &gmPause_VitaExitPromptDesc,
                          gmPause_VitaExitPrompt, 124, 36);
    // Select is the default physical binding for Melee's retry (Z) action.
    if (melee_vita_pad_get_mapping(MELEE_VITA_BUTTON_SELECT) ==
        MELEE_VITA_ACTION_Z)
    {
        gmPause_UseVitaPrompt(lbl_80479B10.z, &gmPause_VitaRetryPromptDesc,
                              gmPause_VitaRetryPrompt, 80, 36);
    }
}

static void gmPause_UseNeutralExitColors(void)
{
    HSD_DObj* dobj = HSD_JObjGetDObj(lbl_80479B10.lras);
    HSD_TObj* tobj;
    if (dobj == NULL || dobj->mobj == NULL ||
        (tobj = dobj->mobj->tobj) == NULL)
    {
        return;
    }
    // The stock A-button material tints that part of the texture green.
    // Use the replacement texture's own grayscale colors for this prompt.
    tobj->flags = (tobj->flags & ~TEX_COLORMAP_MASK) | TEX_COLORMAP_REPLACE;
    HSD_MObjCompileTev(dobj->mobj);
}
#endif

void fn_801A0E34(HSD_GObj* arg0)
{
    f32 x;
    f32 y;
    HSD_PadStatus* pad;
    PAD_STACK(8);

    if (lbl_80479B10.slot != 99) {
        x = 10.0F * HSD_PadMasterStatus[(u8) lbl_80479B10.slot].nml_stickX;
        y = 10.0F * HSD_PadMasterStatus[(u8) lbl_80479B10.slot].nml_stickY;
        HSD_JObjSetRotationY(lbl_80479B10.analog_stick, +MTXDegToRad(x));
        HSD_JObjSetRotationX(lbl_80479B10.analog_stick, -MTXDegToRad(y));
    }
}

void gm_801A0FEC(s32 slot, u8 flag)
{
    lbl_80479B10.slot = slot;
    HSD_JObjReqAnimAll(lbl_80479B10.background, (f32) (slot + 1));
    // flag is set by match rules
    if (flag != 0) {
        HSD_JObjClearFlagsAll(lbl_80479B10.background, JOBJ_HIDDEN);
        HSD_JObjSetFlagsAll(lbl_80479B10.lras, JOBJ_HIDDEN);
        HSD_JObjSetFlagsAll(lbl_80479B10.z, JOBJ_HIDDEN);
        HSD_JObjSetFlagsAll(lbl_80479B10.analog_stick, JOBJ_HIDDEN);
        HSD_JObjSetFlagsAll(lbl_80479B10.analog_stick_outline, JOBJ_HIDDEN);
        if (flag & 1) {
            HSD_JObjClearFlagsAll(lbl_80479B10.lras, JOBJ_HIDDEN);
        }
        if (flag & 2) {
            HSD_JObjClearFlagsAll(lbl_80479B10.z, JOBJ_HIDDEN);
        }
        if (flag & 4) {
            HSD_JObjClearFlagsAll(lbl_80479B10.analog_stick, JOBJ_HIDDEN);
            HSD_JObjClearFlagsAll(lbl_80479B10.analog_stick_outline,
                                  JOBJ_HIDDEN);
        }
    } else {
        HSD_JObjSetFlagsAll(lbl_80479B10.background, JOBJ_HIDDEN);
    }
    HSD_JObjAnimAll(lbl_80479B10.background);
#ifdef TARGET_VITA
    gmPause_UseVitaPrompts();
#endif
}

void gm_801A10FC(int slot)
{
    lbl_80479B10.slot = 99;
    HSD_JObjSetFlagsAll(lbl_80479B10.background, JOBJ_HIDDEN);
}

void fn_801A1134(void)
{
    SceneDesc* scene;
    HSD_GObj* gobj;
    HSD_JObj* jobj;

    lbl_804D6700 =
        lbArchive_80016DBC("GmPause", &scene, "ScGamPause_scene_data", 0);
    gobj = GObj_Create(0xEU, 2U, 0U);
    lbl_804D6704 = gobj;
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, GM_SCENE_MODEL(scene, 0)->joint));
    lbl_80479B10.background = jobj;
    lb_80011E24(jobj, &lbl_80479B10.analog_stick, 1, -1);
    lb_80011E24(jobj, &lbl_80479B10.lras, 9, -1);
    lb_80011E24(jobj, &lbl_80479B10.z, 10, -1);
    lb_80011E24(jobj, &lbl_80479B10.analog_stick_outline, 11, -1);
    lbl_80479B10.analog_stick = HSD_JObjGetChild(lbl_80479B10.analog_stick);
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 0xBU, 0U);
    gm_8016895C(jobj, GM_SCENE_MODEL(scene, 0), 0);
    HSD_JObjReqAnimAll(jobj, 1.0f);
    HSD_JObjAnimAll(jobj);
#ifdef TARGET_VITA
    gmPause_UseVitaPrompts();
    gmPause_UseNeutralExitColors();
#endif
    HSD_JObjSetFlagsAll(jobj, JOBJ_HIDDEN);
    HSD_GObj_SetupProc(gobj, fn_801A0E34, 0U);
    lbl_80479B10.slot = 99;
}
