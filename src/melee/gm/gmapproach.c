#include "gmapproach.h"

#include <melee/ft/forward.h>

#include "gm_unsplit.h"
#include "gmscene.h"
#include "giga_bowser_approach_silhouette.inc"
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjobject.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>

static struct {
    HSD_Archive* x0;
    int x4;
    u16 x8;
    u16 xA;
    u16 xC;
    u8 xE;
    bool is_giga_bowser;
} gm_80480D98;

UNK_T gmVsMelee_ApproachData[2];

static HSD_ImageDesc gmApproach_GigaBowserSilhouetteDesc;

static void gmApproach_UseGigaBowserSilhouette(HSD_JObj* jobj)
{
    HSD_JObj* silhouette = jobj == NULL ? NULL : jobj->child;
    HSD_TObj* tobj;

    silhouette = silhouette == NULL ? NULL : silhouette->next;
    silhouette = silhouette == NULL ? NULL : silhouette->next;
    if (silhouette == NULL || silhouette->u.dobj == NULL ||
        silhouette->u.dobj->mobj == NULL)
    {
        return;
    }
    tobj = silhouette->u.dobj->mobj->tobj;
    if (tobj == NULL) {
        return;
    }

    DP_SET(gmApproach_GigaBowserSilhouetteDesc.image_ptr,
           gmApproach_GigaBowserSilhouetteTexture);
    gmApproach_GigaBowserSilhouetteDesc.width = 80;
    gmApproach_GigaBowserSilhouetteDesc.height = 112;
    gmApproach_GigaBowserSilhouetteDesc.format = GX_TF_RGBA8;
    gmApproach_GigaBowserSilhouetteDesc.mipmap = 0;
    gmApproach_GigaBowserSilhouetteDesc.minLOD = 0.0F;
    gmApproach_GigaBowserSilhouetteDesc.maxLOD = 0.0F;
    tobj->imagedesc = &gmApproach_GigaBowserSilhouetteDesc;
    /* Crop the transparent margins of the custom silhouette while retaining
     * the stock warning panel's geometry and animation. */
    tobj->scale.x = 1.45F;
    tobj->scale.y = 1.45F;
    tobj->translate.x = -0.155F;
    /* The silhouette occupies the lower half of its texture; centre that
     * painted region rather than the full transparent canvas. */
    tobj->translate.y = -0.32F;
    tobj->flags |= TEX_MTX_DIRTY;
    /* The stock approach portraits are opaque images selected by a texture
     * animation.  Our standalone RGBA8 image needs its own alpha enabled or
     * the transparent canvas is drawn as a solid rectangle. */
    tobj->flags = (tobj->flags & ~TEX_ALPHAMAP_MASK) | TEX_ALPHAMAP_REPLACE;
    silhouette->u.dobj->mobj->rendermode |= RENDER_XLU | RENDER_NO_ZUPDATE;
}

static void fn_801AD920(HSD_GObj* gobj)
{
    HSD_JObj* jobj = gobj->hsd_obj;
    HSD_JObj* var_r3 = gobj->hsd_obj == NULL ? NULL : GET_JOBJ(gobj)->child;
    HSD_JObj* var_r4 = var_r3 == NULL ? NULL : var_r3->next;
    HSD_JObj* var_r0 = var_r4 == NULL ? NULL : var_r4->next;
    int var_r0_2;

    if (gm_80480D98.x8 <= 0x12C) {
        gm_80480D98.x8++;
    } else {
        gm_80480D98.x8 = 0;
    }
    if (gm_80480D98.xA < 0x12C) {
        gm_80480D98.xA++;
    }
    HSD_JObjReqAnimAll(var_r3, gm_80480D98.x8);
    HSD_JObjReqAnimAll(var_r4, gm_80480D98.xA);
    HSD_JObjReqAnimAll(var_r0, gm_80480D98.xA);
    switch (gm_80480D98.x4) {
    case 3:
        var_r0_2 = 1;
        break;
    case 7:
        var_r0_2 = 2;
        break;
    case 9:
        var_r0_2 = 3;
        break;
    case 10:
        var_r0_2 = 4;
        break;
    case 15:
        var_r0_2 = 5;
        break;
    case 20:
        var_r0_2 = 6;
        break;
    case 21:
        var_r0_2 = 7;
        break;
    case 22:
        var_r0_2 = 8;
        break;
    case 23:
        var_r0_2 = 9;
        break;
    case 24:
        var_r0_2 = 10;
        break;
    case 25:
        var_r0_2 = 11;
        break;
    default:
        var_r0_2 = 0;
        break;
    }
    if (!gm_80480D98.is_giga_bowser) {
        HSD_TObjReqAnimAll(var_r0->u.dobj->mobj->tobj, var_r0_2);
        HSD_AObjSetRate(var_r0->u.dobj->mobj->tobj->aobj, 0.0F);
    }
    HSD_JObjAnimAll(jobj);
    if (gm_80480D98.is_giga_bowser) {
        /* NtAppro has no Giga Bowser frame.  Keep its native model, camera,
         * timing, and material, and replace only the silhouette image. */
        gmApproach_UseGigaBowserSilhouette(jobj);
    }
}

static void gm_801ADB04(void)
{
    SceneDesc* spC;
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    HSD_CObj* cobj;
    HSD_LObj* lobj;
    PAD_STACK(4);

    gm_80480D98.x0 =
        lbArchive_80016DBC("NtAppro", &spC, "ScNtcApproach_scene_data", 0);
    gobj = GObj_Create(0x13, 0x14, 0);
    cobj = HSD_CObjLoadDesc(DP(HSD_CObjDesc, GM_SCENE_CAMERA(spC)[0].desc));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_CameraKind, cobj);
    GObj_SetupGXLinkMax(gobj, HSD_GObj_803910D8, 0);
    gobj->gxlink_prios = 0x4801;

    gobj = GObj_Create(0xB, 0xF, 0);
    lobj = lb_80011AC4(GM_SCENE_LIGHTS(spC));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_LightKind, lobj);
    GObj_SetupGXLink(gobj, HSD_GObj_LObjCallback, 0, 0);

    gobj = GObj_Create(0xE, 0xF, 0);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, GM_SCENE_MODEL(spC, 0)->joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 0xB, 0);
    gm_8016895C(jobj, GM_SCENE_MODEL(spC, 0), 0);
    HSD_JObjReqAnimAll(jobj, 0.0F);
    HSD_JObjAnimAll(jobj);
    if (gm_80480D98.is_giga_bowser) {
        gmApproach_UseGigaBowserSilhouette(jobj);
    }
    HSD_GObj_SetupProc(gobj, fn_801AD920, 1);
}

void gm_Scene_Approach_OnFrame(void)
{
    if (gm_80480D98.xC++ >= 0xB4 &&
        (HSD_PadCopyStatus[gm_80480D98.xE].trigger &
         (HSD_PAD_A | HSD_PAD_START)))
    {
        gm_801A4B60();
    }
}

void gm_Scene_Approach_OnEnter(void* arg0_)
{
    s8* arg0 = arg0_;
    int var_r0;

    var_r0 = arg0[0];
    gm_80480D98.is_giga_bowser = var_r0 == CKind_GKoops;
    gm_801ADB04();
    if (var_r0 != 3 && var_r0 != 7 && var_r0 != 9 && var_r0 != 10 &&
        var_r0 != 15 && var_r0 != 20 && var_r0 != 21 && var_r0 != 22 &&
        var_r0 != 23 && var_r0 != 24 && var_r0 != 25)
    {
        var_r0 = 25;
    }

    gm_80480D98.x4 = var_r0;
    gm_80480D98.xE = arg0[1];
    gm_80480D98.x8 = 0;
    gm_80480D98.xA = 0;
    gm_80480D98.xC = 0;
    lbAudioAx_80023F28(0x48);
}

void gm_Scene_Approach_OnExit(UNUSED void* exit_data)
{
    lbArchive_80016EFC(gm_80480D98.x0);
    lbAudioAx_800236DC();
}
