/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi Direct online play, the match: a lockstep peer.
 *
 * Slippi's online codes (Online/Core in project-slippi/slippi-ssbm-asm:
 * TriggerSendInput, StartEngineLoop, InitOnlinePlay) as C, without
 * prediction or rollback.  A frame only runs once the opponent's pad for it
 * has arrived.  Until then the game waits inside that frame.  Slippi Dolphin
 * on the other side predicts and rolls back on its own, so it never notices.
 *
 * Ported from the 3DS Slippi port (melee-3ds slippi-beta-v0.1.0,
 * native-3ds/port/engine/slippi/online.c), which follows Melee Unlocked's
 * shim/mu_online.c.  Two things differ from the 3DS:
 * - The desync check has Slippi's two tiers.  When the low 16 bits of the
 *   checksums (a sum of positions and percents) differ by more than 1 the
 *   game ends with DESYNC DETECTED.  When only the high 16 bits (an XOR of
 *   the fighters' state) differ, "Desync Risk" shows and play goes on.  The
 *   3DS ends the game for both.
 * - The game info block is decoded field by field
 *   (mp_slippi_decode_match_block), because the Vita runs little-endian.
 *
 * Frame numbers: online frame N is the pad the engine body consumes while
 * its unpaused frame counter (gm_801A4BB8) is N. */
#ifdef MELEE_VITA_SLIPPI /* only in -EnableSlippi builds */
#include "slippi_online.h"

#include "slippi_match_block.h"
#include "slippi_platform.h"
#include "slippi_rules.h"
#include "slippi_ucf.h"

#include <melee/cm/camera.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/types.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mn/types.h>
#include <melee/pl/player.h>
#include <melee/pl/types.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/jobj.h>

#include <stdio.h>
#include <string.h>

extern u32* HSD_RandSeedPtr;
extern StaticPlayer player_slots[];
/* gmscene.c: nonzero while the scene loop is leaving the scene. */
int mp_slippi_engine_leaving(void);
/* gmvs.c: ends the match as a no contest, like Slippi's FN_END_GAME. */
void mp_slippi_gmvs_end_online(int pauser);
/* lb_0195.c: samples the pads once more, so one more engine body runs. */
void fn_800195FC(void);

#define PAD_SIZE 12
#define MIN_DELAY 1
#define MAX_DELAY 15
/* Slippi's UNFREEZE_INPUTS_FRAME: pads count from here on. */
#define UNFREEZE_FRAME 84
#define START_SYNC_FRAME (UNFREEZE_FRAME - 6)
/* How many of our own checksums are kept (Slippi's DESYNC_ENTRY_COUNT). */
#define DESYNC_ENTRIES 21
#define BLOCK_SIZE 0x138
/* Frame time, in microseconds, at Dolphin's 59.94 Hz. */
#define FRAME_US 16683
/* How far behind the opponent we may run before catching up: 1.6 frames
 * on a LAN, up to 5 on a slow link.  Slippi Dolphin rolls back up to 7
 * frames, so it absorbs us running behind. */
#define LAG_ALLOW_MIN_US (FRAME_US + 10000)
#define LAG_ALLOW_MAX_US (5 * FRAME_US)

typedef struct {
    int frame;
    u32 checksum;
} LocalChecksum;

static struct {
    int pending;         /* a match was agreed, waiting for its scene */
    u8 block[SLIPPI_NET_MATCH_BLOCK_TOTAL];
    int active;
    u8 local_index;
    u8 remote_index;
    u8 delay;
    u32 rng_offset;
    u8 last_local[PAD_SIZE];
    u8 delay_index;
    u8 delay_buffer[MAX_DELAY][PAD_SIZE];
    int finalized;
    int stable_finalized;
    u32 tx_checksum;
    int desync_last_frame;
    u8 desync_write_index;
    LocalChecksum local_checksums[DESYNC_ENTRIES];
    int disconnected;
    int disconnect_shown;
    int desync_shown;
    int risk_shown;
    int game_over;
    int game_end_frame;
    /* counters for slippi.log */
    unsigned frames;
    unsigned waits;
    unsigned wait_ms;
    unsigned longest_wait_ms;
    unsigned no_sample_bodies;
    unsigned advances;
    unsigned dropped;
    /* time sync */
    int advance_left;
    int advance_gap;
    int advancing;
    int drop_samples;
    int last_drop_frame;
    int lag_allow_us;
    unsigned wait_mark;
    unsigned calm_windows;
} on;

static void log_line(const char* format, int a, int b, int c)
{
    char line[200];
    snprintf(line, sizeof(line), format, a, b, c);
    mp_platform_slippi_log(line);
}

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) |
           (u32) p[3];
}

static int global_frame(void)
{
    return (int) gm_801A4BB8();
}

/* The online hooks run during an online match, until its scene starts to
 * leave. */
static int hooks_on(void)
{
    return on.active && mp_slippi_engine_leaving() == 0;
}

/* ---------------------------------------------------------------- pads */

/* A pad as the 12 bytes of the console's PAD report. */
static void pad_to_wire(u8* out, const PADStatus* pad)
{
    out[0] = (u8) (pad->button >> 8);
    out[1] = (u8) pad->button;
    out[2] = (u8) pad->stickX;
    out[3] = (u8) pad->stickY;
    out[4] = (u8) pad->substickX;
    out[5] = (u8) pad->substickY;
    out[6] = pad->triggerLeft;
    out[7] = pad->triggerRight;
    out[8] = pad->analogA;
    out[9] = pad->analogB;
    out[10] = (u8) pad->err;
    out[11] = 0;
}

static void wire_to_pad(PADStatus* pad, const u8* in)
{
    pad->button = (u16) ((in[0] << 8) | in[1]);
    pad->stickX = (s8) in[2];
    pad->stickY = (s8) in[3];
    pad->substickX = (s8) in[4];
    pad->substickY = (s8) in[5];
    pad->triggerLeft = in[6];
    pad->triggerRight = in[7];
    pad->analogA = in[8];
    pad->analogB = in[9];
    pad->err = (s8) in[10];
}

/* A stick within 2 of the centre counts as centred, as Slippi sends it. */
static void clamp_stick_at_rest(PADStatus* pad)
{
    if (pad->stickX >= -2 && pad->stickX <= 2 && pad->stickY >= -2 &&
        pad->stickY <= 2)
    {
        pad->stickX = 0;
        pad->stickY = 0;
    }
    if (pad->substickX >= -2 && pad->substickX <= 2 &&
        pad->substickY >= -2 && pad->substickY <= 2)
    {
        pad->substickX = 0;
        pad->substickY = 0;
    }
}

/* ------------------------------------------------- the desync checksum */

static u32 float_bits(float value)
{
    u32 bits;
    memcpy(&bits, &value, 4);
    return bits;
}

/* StartEngineLoop's FN_COMPUTE_CHECKSUM.  The high 16 bits fold an XOR of
 * every fighter's action state, position, percent and spawn number, and
 * each player's stocks.  The low 16 bits are the whole part of the sum of
 * the positions and percents. */
static u32 compute_checksum(void)
{
    u32 acc = 0;
    float sum = 0.0F;
    int i;
    int k;

    for (i = 0; i < 4; i++) {
        for (k = 0; k < 2; k++) {
            HSD_GObj* gobj = player_slots[i].player_entity[k];
            Fighter* fp;
            if (gobj == NULL) continue;
            fp = gobj->user_data;
            acc ^= (u32) fp->motion_id;
            acc ^= float_bits(fp->cur_pos.x);
            acc ^= float_bits(fp->cur_pos.y);
            acc ^= float_bits(fp->dmg.x1830_percent);
            acc ^= (u32) fp->x8_spawnNum;
            sum = sum + fp->cur_pos.x;
            sum = sum + fp->cur_pos.y;
            sum = sum + fp->dmg.x1830_percent;
        }
        acc ^= (u8) player_slots[i].stocks;
    }
    return (((acc >> 16) ^ (acc & 0xFFFF)) << 16) | ((u32) (s32) sum & 0xFFFF);
}

/* Keeps this frame's checksum in the ring, as StartEngineLoop does. */
static void store_checksum(int frame)
{
    int offset = frame - (on.desync_last_frame + 1);
    int index = on.desync_write_index;

    if (offset >= 0) {
        on.desync_write_index = (u8) ((on.desync_write_index + 1) % DESYNC_ENTRIES);
        on.desync_last_frame = frame;
    }
    index = (index + offset) % DESYNC_ENTRIES;
    if (index < 0) index += DESYNC_ENTRIES;
    on.local_checksums[index].frame = frame;
    on.local_checksums[index].checksum = compute_checksum();
}

/* Compares the opponent's newest checksum with ours for the same frame. */
static void check_desync(void)
{
    int remote_frame = mp_platform_slippi_net_remote_checksum_frame();
    u32 remote = mp_platform_slippi_net_remote_checksum_value();
    u32 local;
    int difference;
    int k;

    if (on.desync_shown) return;
    /* Only frames we have finished, and none before the pads unfreeze. */
    if (remote_frame <= UNFREEZE_FRAME || remote_frame > on.stable_finalized) return;

    for (k = 0; k < DESYNC_ENTRIES; k++) {
        if (on.local_checksums[k].frame == remote_frame) break;
    }
    if (k == DESYNC_ENTRIES) return;
    local = on.local_checksums[k].checksum;

    /* The hard check: the sums may be off by 1 at most. */
    difference = (int) (s16) (local & 0xFFFF) - (int) (s16) (remote & 0xFFFF);
    if (difference < -1 || difference > 1) {
        on.desync_shown = 1;
        log_line("Slippi online: DESYNC at frame %d (ours %08X, theirs %08X)",
                 remote_frame, (int) local, (int) remote);
        /* Error sound, DESYNC DETECTED, and the game ends like a
         * disconnect. */
        lbAudioAx_80024030(3);
        if (!on.game_over) mp_slippi_gmvs_end_online(on.local_index);
        return;
    }

    /* The soft check: the fighters' state differs but the sums agree.
     * Slippi only warns, once. */
    if (!on.risk_shown && (local >> 16) != (remote >> 16)) {
        on.risk_shown = 1;
        log_line("Slippi online: desync risk at frame %d (ours %08X, theirs %08X)",
                 remote_frame, (int) local, (int) remote);
    }
}

/* ------------------------------------------- the pads of each engine body
 * Slippi gives online frame N to the pad sample that the engine body with
 * frame counter N consumes.  So the exchange happens at the top of the
 * body, on the queue entry it is about to read, and the body waits there
 * until the opponent's pad for its frame has arrived. */

/* Right after PADRead.  Nonzero drops the sample, so the engine runs one
 * frame fewer this period.  The time sync (mp_slippi_online_frame_end) uses
 * it when we run ahead of the opponent, as Dolphin skips frames. */
int mp_slippi_online_pad_renew(PADStatus* pad)
{
    (void) pad;
    if (!hooks_on() || on.drop_samples <= 0) return 0;
    on.drop_samples--;
    on.dropped++;
    return 1;
}

static void exchange_pads(int frame)
{
    PadLibData* p = &HSD_PadLibData;
    PADStatus* stat;
    PADStatus* source;
    u8 remote[PAD_SIZE];
    int result;
    int waited = 0;
    int i;

    if (p->qcount == 0 || p->queue == NULL) {
        if (on.no_sample_bodies < 8) {
            log_line("Slippi online: body %d has no pad sample", frame, 0, 0);
        }
        on.no_sample_bodies++;
        return;
    }
    stat = p->queue[p->qread].stat;
    if (frame < 1) {
        memset(stat, 0, 4 * sizeof(*stat));
        return;
    }
    if (frame < START_SYNC_FRAME - on.delay) {
        memset(stat, 0, 4 * sizeof(*stat));
    }

    /* The Vita's own controls are port 1's pad. */
    source = &stat[0];
    clamp_stick_at_rest(source);
    pad_to_wire(on.last_local, source);
    /* The opponent only ever sees the first 8 bytes, so ours run the same
     * way. */
    memset(on.last_local + 8, 0, PAD_SIZE - 8);

    for (i = 0; i < DESYNC_ENTRIES; i++) {
        if (on.local_checksums[i].frame == on.stable_finalized) {
            on.tx_checksum = on.local_checksums[i].checksum;
            break;
        }
    }

    /* Send ours, then wait for theirs, about a millisecond at a time. */
    for (;;) {
        mp_platform_slippi_net_poll();
        result = mp_platform_slippi_net_send_inputs(frame, on.delay,
                                                    on.stable_finalized,
                                                    on.tx_checksum,
                                                    on.last_local, remote);
        if (result != SLIPPI_NET_INPUTS_SKIP || on.game_over) break;
        if (waited == 0) on.waits++;
        waited++;
        mp_platform_slippi_idle();
    }
    on.wait_ms += (unsigned) waited;
    if ((unsigned) waited > on.longest_wait_ms) on.longest_wait_ms = (unsigned) waited;
    if (result != SLIPPI_NET_INPUTS_OK) {
        if (!on.disconnected) {
            log_line("Slippi online: disconnected at frame %d (%d)", frame, result, 0);
        }
        on.disconnected = 1;
        memset(remote, 0, sizeof(remote));
    }

    /* Our player gets our pad from `delay` frames ago. */
    wire_to_pad(&stat[on.local_index], on.delay_buffer[on.delay_index]);
    memcpy(on.delay_buffer[on.delay_index], on.last_local, PAD_SIZE);
    on.delay_index = (u8) ((on.delay_index + 1) % on.delay);
    wire_to_pad(&stat[on.remote_index], remote);

    on.finalized = frame;
    on.frames++;
    if (frame % 600 == 0) {
        log_line("Slippi online: frame %d, waited for the opponent %d times (%d ms)", frame,
                 (int) on.waits, (int) on.wait_ms);
        log_line("Slippi online: ping %d ms, longest wait %d ms",
                 mp_platform_slippi_net_ping_ms(), (int) on.longest_wait_ms, 0);
    }
}

/* StartEngineLoop (801a4de4): the top of each engine body. */
void mp_slippi_online_frame_begin(void)
{
    int frame;
    int finished;

    if (!hooks_on()) return;
    frame = global_frame();
    exchange_pads(frame);
    if (on.disconnected && !on.disconnect_shown && !on.game_over) {
        on.disconnect_shown = 1;
        mp_slippi_gmvs_end_online(on.local_index);
    }

    /* The newest frame both of us have finished. */
    finished = frame;
    if (on.finalized < finished) finished = on.finalized;
    if (finished > on.stable_finalized) on.stable_finalized = finished;

    store_checksum(frame);
    if (frame != 0) check_desync();

    /* The game counts as over a few frames after GAME! (or the no contest)
     * so the last pads still go out. */
    if (!on.game_over) {
        if (gmVs_GetSceneController()->state.match_result == 0) {
            on.game_end_frame = 0;
        } else {
            if (on.game_end_frame == 0) on.game_end_frame = frame;
            if (frame - on.game_end_frame > 7) {
                on.game_over = 1;
                log_line("Slippi online: match over at frame %d", frame, 0, 0);
            }
        }
    }
}

/* The end of each engine body: Dolphin's time sync.  A lockstep peer never
 * runs ahead of the opponent's pads, but its clock can.  When we are ahead
 * we drop pad samples, and when we fall too far behind we queue extra ones
 * so the scene loop runs an extra engine body (the drawing just skips a
 * frame). */
void mp_slippi_online_frame_end(void)
{
    int frame;
    int offset;
    int count;
    unsigned waits;
    int before;

    if (!hooks_on() || on.game_over || on.disconnected) return;
    frame = global_frame();

    /* Running ahead.  Dolphin halts up to 5 frames when more than 10 ms
     * ahead in the first 120 frames, and later only slows its clock.  We
     * drop whole frames early on, then at most one per 180 frames. */
    if (frame % 30 == 0 && on.drop_samples == 0 && !on.advancing) {
        offset = mp_platform_slippi_net_time_offset_us();
        count = 0;
        if (frame <= 120 && offset > 10000) {
            count = (offset - 10000) / FRAME_US + 1;
            if (count > 5) count = 5;
        } else if (frame > 120 && offset > 8000 && frame - on.last_drop_frame >= 180) {
            count = 1;
        }
        if (count > 0) {
            on.drop_samples = count;
            on.last_drop_frame = frame;
            if (on.dropped < 20 || frame % 600 == 0) {
                log_line("Slippi online: %d us ahead at frame %d, dropping %d frames", offset,
                         frame, count);
            }
        }
    }

    /* Running behind.  On a slow link the opponent's pad arrives after
     * their input delay has run out, so running level with them means
     * waiting on nearly every frame.  Running a little behind costs Dolphin
     * a rollback and costs us nothing.  So while we keep waiting, allow half
     * a frame more lag per busy half second, up to 5 frames, and give it
     * back slowly once the waits stop. */
    if (frame > 120 && frame % 30 == 0) {
        waits = on.waits - on.wait_mark;
        before = on.lag_allow_us;
        on.wait_mark = on.waits;
        if (waits >= 4) {
            on.lag_allow_us += FRAME_US / 2;
            on.calm_windows = 0;
        } else if (waits == 0) {
            on.calm_windows++;
            if (on.calm_windows >= 10) {
                on.lag_allow_us -= FRAME_US / 4;
                on.calm_windows = 0;
            }
        }
        if (on.lag_allow_us > LAG_ALLOW_MAX_US) on.lag_allow_us = LAG_ALLOW_MAX_US;
        if (on.lag_allow_us < LAG_ALLOW_MIN_US) on.lag_allow_us = LAG_ALLOW_MIN_US;
        if (on.lag_allow_us != before && frame % 600 == 0) {
            log_line("Slippi online: frame %d, may run up to %d us behind (%d waits)", frame,
                     on.lag_allow_us, (int) waits);
        }
    }

    /* As Dolphin, no catching up before frame 120: the start-up stalls
     * line the two games up first.  Then up to 3 extra frames, one every
     * 5 frames. */
    if (frame > 120 && frame % 30 == 0 && !on.advancing && on.drop_samples == 0) {
        offset = mp_platform_slippi_net_time_offset_us();
        if (offset < -on.lag_allow_us) {
            count = (-offset - on.lag_allow_us) / FRAME_US + 1;
            if (count > 3) count = 3;
            on.advance_left = count;
            on.advance_gap = 0;
            on.advancing = 1;
            if (on.advances < 20 || frame % 600 == 0) {
                log_line("Slippi online: %d us behind at frame %d, catching up %d frames",
                         -offset, frame, count);
            }
        }
    }
    if (on.advancing) {
        if (on.advance_gap <= 0) {
            fn_800195FC();
            on.advances++;
            on.advance_gap = 4;
            on.advance_left--;
            if (on.advance_left <= 0) on.advancing = 0;
        } else {
            on.advance_gap--;
        }
    }
}

/* What the drawing does that gameplay reads.  The console draws every
 * frame.  An engine body that no drawing follows still needs the camera's
 * matrices (offscreen checks) and the fighters' bone matrices, the way
 * Slippi's rollback runs the camera tasks between frames. */
void mp_slippi_tick_without_draw(void)
{
    HSD_GObj* gobj;
    HSD_GObj* camera;

    if (!hooks_on()) return;
    camera = Camera_80030A50();
    if (camera == NULL) return;
    Camera_8002A4AC(camera);
    HSD_CObjSetCurrent(camera->hsd_obj);
    for (gobj = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; gobj != NULL; gobj = gobj->next) {
        Fighter* fp = gobj->user_data;
        FighterPartsTable* parts;
        u32 i;
        if (fp->x221F_b3 || !ftLib_80086A8C(gobj) || fp->parts == NULL || fp->kind >= 33) {
            continue;
        }
        parts = DP(FighterPartsTable, ftPartsTable[fp->kind].v);
        for (i = 0; i < parts->parts_num; i++) {
            HSD_JObj* joint = fp->parts[i].joint;
            if (joint != NULL) HSD_JObjSetupMatrix(joint);
        }
    }
}

/* -------------------------------------------- starting and ending a match */

int mp_slippi_online_take_match(void)
{
    if (mp_platform_slippi_net_status() != SLIPPI_NET_CONNECTED) return 0;
    if (!mp_platform_slippi_net_match_block(on.block)) return 0;
    on.pending = 1;
    log_line("Slippi online: match ready, stage %d",
             (on.block[0xE] << 8) | on.block[0xF], 0, 0);
    return 1;
}

const u8* mp_slippi_online_pending_block(void)
{
    if (!on.pending) return NULL;
    return on.block;
}

int mp_slippi_online_local_index(void)
{
    return on.local_index;
}

int mp_slippi_online_delay(void)
{
    return on.delay;
}

int mp_slippi_online_active(void)
{
    return on.active;
}

int mp_slippi_online_status(void)
{
    if (!on.active) return 0;
    if (on.desync_shown) return 2;
    if (on.disconnected) return 1;
    return 0;
}

int mp_slippi_online_desync_risk(void)
{
    return on.active && on.risk_shown;
}

/* InitOnlinePlay's FN_SyncRNG: each frame starts from a seed both games
 * agree on. */
static void sync_rng_proc(HSD_GObj* gobj)
{
    u32 frame = (u32) global_frame();
    (void) gobj;
    *HSD_RandSeedPtr = ((frame << 16) | (frame >> 16)) + on.rng_offset;
}

/* InitOnlinePlay (8016e748): the agreed match and Slippi's online rules. */
void mp_slippi_online_start_melee(StartMeleeData* data)
{
    HSD_GObj* gobj;
    int delay;
    int i;

    if (!on.pending) return;
    on.pending = 0;

    mp_slippi_decode_match_block(on.block, data);
    on.rng_offset = be32(on.block + BLOCK_SIZE);
    on.local_index = on.block[BLOCK_SIZE + 4] & 1;
    on.remote_index = on.local_index == 0 ? 1 : 0;

    /* Slippi Dolphin always runs its gameplay codes, UCF 0.84 among them
     * (slippi_rules.c), and the online-only ones in an online match. */
    mp_slippi_rules_set_enabled(1);
    mp_ucf_set_version(84);
    mp_slippi_rules_set_frozen_fd(0);
    mp_slippi_rules_set_online(2 /* Direct */, on.local_index);
    mp_slippi_rules_set_frozen_stadium(on.block[BLOCK_SIZE + 6]);

    delay = on.block[BLOCK_SIZE + 5];
    if (delay < MIN_DELAY) delay = 2;
    if (delay > MAX_DELAY) delay = MAX_DELAY;
    on.delay = (u8) delay;

    on.delay_index = 0;
    memset(on.delay_buffer, 0, sizeof(on.delay_buffer));
    memset(on.last_local, 0, sizeof(on.last_local));
    memset(on.local_checksums, 0, sizeof(on.local_checksums));
    on.finalized = 0;
    on.stable_finalized = 0;
    on.tx_checksum = 0;
    on.desync_last_frame = 0;
    on.desync_write_index = 0;
    on.disconnected = 0;
    on.disconnect_shown = 0;
    on.desync_shown = 0;
    on.risk_shown = 0;
    on.game_over = 0;
    on.game_end_frame = 0;
    on.frames = 0;
    on.waits = 0;
    on.wait_ms = 0;
    on.longest_wait_ms = 0;
    on.no_sample_bodies = 0;
    on.advances = 0;
    on.dropped = 0;
    on.advance_left = 0;
    on.advance_gap = 0;
    on.advancing = 0;
    on.drop_samples = 0;
    on.last_drop_frame = 0;
    on.lag_allow_us = LAG_ALLOW_MIN_US;
    on.wait_mark = 0;
    on.calm_windows = 0;

    *HSD_RandSeedPtr = on.rng_offset;
    /* No transformation from a held A (InitOnlinePlay). */
    for (i = 0; i < 4; i++) {
        HSD_PadCopyStatus[i].button = 0;
    }
    /* GObj class 4, p_link 7, priority 0; process s_link 0, as Slippi. */
    gobj = GObj_Create(4, 7, 0);
    HSD_GObj_SetupProc(gobj, sync_rng_proc, 0);
    on.active = 1;
    log_line("Slippi online: match starts, local port %d, delay %d, rng offset %08X",
             on.local_index + 1, on.delay, (int) on.rng_offset);
}

void mp_slippi_online_match_exit(void)
{
    if (!on.active) return;
    mp_slippi_rules_set_online(-1, 0);
    mp_slippi_rules_set_frozen_stadium(0);
    mp_slippi_rules_set_enabled(0);
    log_line("Slippi online: match exit after %d frames, %d waits, desync %d", (int) on.frames,
             (int) on.waits, on.desync_shown);
    log_line("Slippi online: waited %d ms in total, longest %d ms, desync risk %d",
             (int) on.wait_ms, (int) on.longest_wait_ms, on.risk_shown);
    log_line("Slippi online: %d extra frames to catch up, %d dropped", (int) on.advances,
             (int) on.dropped, 0);
    on.active = 0;
}

#endif /* MELEE_VITA_SLIPPI */
