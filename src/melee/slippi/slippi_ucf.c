/* UCF 0.84 as Slippi runs it (part of its General Codes), and UCF 0.8 for
 * older replays.  Copied from the 3DS Slippi port (melee-3ds
 * slippi-beta-v0.1.0, native-3ds/port/engine/ucf.c).  Gated by the Slippi
 * rules (slippi_rules.h), so it only runs in the replay test and online
 * matches. */
#ifdef MELEE_VITA_SLIPPI /* only in -EnableSlippi builds */
#include "slippi_ucf.h"
#include "slippi_ucf_math.h"
#include <string.h>
#include <melee/ft/types.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/kinds/ftCommon/ftCo_0A01.h>
#include <melee/pl/player.h>
#include <melee/mp/mpcoll.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include "slippi_rules.h"

static MPUcfPad pads[4];
/* The four raw pads the last HSD_PadRenewMasterStatus consumed
 * (tools/slippi_edits/online.py); the Slippi replay playback writes the
 * recorded raw sticks here too. */
PADStatus mp_pad_consumed[4];
volatile unsigned mp_ucf_enabled=1;
unsigned mp_ucf_checks[8],mp_ucf_applied[8];
void mp_ucf_reset(void){memset(pads,0,sizeof(pads));}
/* UCF 0.84 is part of Slippi's General Codes (mp_slippi_rules_mask bit MP_SR_UCF). */
static int active(Fighter*f){return mp_ucf_enabled&&mp_slippi_rule(MP_SR_UCF)&&f->x618_player_id<4;}

/* UCF 0.8, for replays from Slippi 3.14 (tools/slippi/vita_replay_pack.py
 * reads which one a replay ran from its code list).  It has only dashback,
 * shield drop and tumble: no 1.0 cardinals, no SDI, no shield SDI, no
 * extended shield drop and no SquatRv fix.  Slippi's sources are in
 * slippi-ssbm-asm, External/UCF 0.8/Logic. */
static int ucf_version = 84;

void mp_ucf_set_version(int version)
{
    ucf_version = version;
}

int mp_ucf_version(void)
{
    return ucf_version;
}

static int is_084(Fighter* f)
{
    return active(f) && ucf_version == 84;
}

static int is_080(Fighter* f)
{
    return active(f) && ucf_version == 80;
}

/* UCF 0.8's stick rounding (the "DoSomething" in UCF SD.asm): |value| * 80,
 * minus 0x37270000 (about 0.00001), cut to a whole number, plus 2, then
 * divided by 80 again.  Each step rounds to float, as the fmuls, fsubs and
 * fdivs there do. */
static float mp_ucf08_round_stick(float value)
{
    float scaled;
    int whole;
    float back;

    if (value < 0) value = -value;
    scaled = value * 80.0f;
    scaled = scaled - 0x1.4ep-17f;
    whole = (int) scaled;
    whole = whole + 2;
    back = (float) whole;
    return back / 80.0f;
}

/* UCF 0.8 shield drop (UCF SD.asm, 800998a4): skips the spot dodge so the
 * shield drop can happen.  Unlike 0.84 it doesn't look at the platform. */
static int mp_ucf08_suppress_spotdodge(Fighter* f)
{
    float x;
    float y;
    float x_squared;
    float y_squared;
    float length_squared;

    /* The c-stick test comes first in the code. */
    if (f->input.cstick[0].y <= p_ftCommonData->x314) return 0;

    /* The stick has to be at the rim: the rounded x and y, squared and
     * added (fmuls, fmuls, fadds), at least 1. */
    x = mp_ucf08_round_stick(f->input.lstick[0].x);
    y = mp_ucf08_round_stick(f->input.lstick[0].y);
    x_squared = x * x;
    y_squared = y * y;
    length_squared = y_squared + x_squared;
    if (length_squared < 1.0f) return 0;

    /* Held sideways for more than 3 frames. */
    if (f->active_timer.lstick.x <= 3) return 0;

    /* And not pushed down past -0.8. */
    if (-0.8f >= f->input.lstick[0].y) return 0;

    return 1;
}
void mp_ucf_input(Fighter*f){
    if(!active(f)||ftCo_IsCpuControlled(f))return;
    ++mp_ucf_checks[0];
    /* The raw pad this engine body consumed (see mp_pad_consumed). */
    PADStatus*s=&mp_pad_consumed[f->x618_player_id];
    MPUcfPad*p=&pads[f->x618_player_id];mp_ucf_push(p,s->stickX,s->stickY);
    /* UCF 0.8 only needs the raw stick history. */
    if(ucf_version!=84)return;
    if(f->kind!=Ft_Kind_Zelda||f->motion_id!=349){
        mp_ucf_cardinal(s->stickX,s->stickY,&f->input.lstick[0].x,&f->input.lstick[0].y);
        mp_ucf_cardinal(s->substickX,s->substickY,&f->input.cstick[0].x,&f->input.cstick[0].y);
    }
    mp_ucf_drop_history(p,f->input.lstick[0].x,f->input.lstick[0].y,f->active_timer.lstick.y);
}
static int xsmash(Fighter*f){return mp_ucf_delta2(&pads[f->x618_player_id],1)>75*75;}
void mp_ucf_dashback(Fighter*f){
    if(!active(f))return;++mp_ucf_checks[1];
    if(ucf_version==80){
        /* UCF 0.8 (UCF DB.asm) checks how far the stick is pushed either
         * way, not only in the new facing direction. */
        float stick_x=f->input.lstick[0].x;
        if(stick_x<0)stick_x=-stick_x;
        if(f->cur_anim_frame!=2.f)return;
        if(stick_x<p_ftCommonData->dash_smash_stick_threshold)return;
        if(f->active_timer.lstick.x>=2)return;
        if(f->is_sub_fighter)return;
        if(!xsmash(f))return;
    }else{
        if(f->is_sub_fighter||f->cur_anim_frame!=2.f||
           f->input.lstick[0].x*f->facing_dir<p_ftCommonData->dash_smash_stick_threshold||
           f->active_timer.lstick.x>=2||!xsmash(f))return;
    }
    f->mv.co.turn.has_turned=true;f->mv.co.turn.just_turned=true;++mp_ucf_applied[1];
    Fighter_GObj*g=Player_GetEntityAtIndex(f->player_id,1);
    if(g){Fighter*n=g->user_data;if(n->cpu.x444){n->cpu.x444->facing_dir=f->facing_dir;n->cpu.x444->lstick.x=f->facing_dir<0?-128:127;}}
}
int mp_ucf_sdi(Fighter*f){
    if(!is_084(f))return 0;++mp_ucf_checks[2];
    float x=f->input.lstick[1].x,y=f->input.lstick[1].y,t=p_ftCommonData->sdi_min_stick_mag;
    /* fmuls y*y, fmadds x*x+yy, fmuls t*t; continue when t*t > mag. */
    float yy=y*y,mag=mp_ucf_fmadds(x,x,yy),tt=t*t;
    int yes=(f->active_sticky.lstick.x<2||f->active_sticky.lstick.y<2)&&tt>mag&&mp_ucf_delta2(&pads[f->x618_player_id],3)>62*62;
    mp_ucf_applied[2]+=yes;return yes;
}
int mp_ucf_shield_sdi(Fighter*f){
    if(!is_084(f))return 0;++mp_ucf_checks[3];
    /* Preserve UCF 0.84's signed previous-X comparison. */
    int yes=f->active_sticky.lstick.x<2&&f->input.lstick[1].x<p_ftCommonData->sdi_min_stick_mag&&mp_ucf_delta2(&pads[f->x618_player_id],1)>62*62;
    mp_ucf_applied[3]+=yes;return yes;
}
int mp_ucf_tumble(Fighter*f){
    if(!active(f))return 0;++mp_ucf_checks[4];
    float x=f->input.lstick[1].x;if(x<0)x=-x;
    int yes=f->active_timer.lstick.x==1&&x<p_ftCommonData->x210&&xsmash(f);
    mp_ucf_applied[4]+=yes;return yes;
}
int mp_ucf_drop_extended(Fighter*f){
    if(!is_084(f))return 0;++mp_ucf_checks[5];
    int yes=pads[f->x618_player_id].drop_frames>=2;mp_ucf_applied[5]+=yes;return yes;
}
int mp_ucf_suppress_spotdodge(Fighter*f){
    if(!active(f))return 0;++mp_ucf_checks[6];
    if(is_080(f)){
        int yes=mp_ucf08_suppress_spotdodge(f);
        mp_ucf_applied[6]+=yes;return yes;
    }
    int yes=f->input.cstick[0].y>p_ftCommonData->x314&&
        f->active_timer.lstick.x>=p_ftCommonData->x320&&f->input.lstick[0].y>-.8f&&
        f->coll_data.floor.index!=-1&&(f->coll_data.floor.flags&0x100)&&
        mp_ucf_rim(f->input.lstick[0].x,f->input.lstick[0].y);
    mp_ucf_applied[6]+=yes;return yes;
}
float mp_ucf_squat_threshold(Fighter*f){
    if(!is_084(f))return p_ftCommonData->x94;++mp_ucf_checks[7];
    if(f->active_timer.lstick.x<1&&mp_ucf_rim(f->input.lstick[0].x,f->input.lstick[0].y)){++mp_ucf_applied[7];return .59f;}
    return p_ftCommonData->x94;
}
/* Slippi experiment diagnostics: the UCF stick history (replay fighter dumps). */
const void* mp_ucf_history(unsigned* size){*size=sizeof(pads);return pads;}

#endif /* MELEE_VITA_SLIPPI */
