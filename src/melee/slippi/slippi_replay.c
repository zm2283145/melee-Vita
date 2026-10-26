/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi replay test: the check that this port's game runs bit for bit like
 * the GameCube (and so like Slippi Dolphin).
 *
 * A port of the 3DS Slippi port's port/engine/slippi/replay.c (melee-3ds
 * slippi-beta-v0.1.0), which follows Slippi's playback codes and Melee
 * Unlocked's shim/mu_replay.c (GPL-3.0-or-later).  At boot the game goes
 * straight into the debug VS mode, starts the match from the replay's game
 * info block and seed, forces each frame's start seed, and writes the
 * recorded inputs into the fighters where Slippi's RestoreGameFrame
 * (8006b0dc) does.
 *
 * The 3DS writes every fighter's state to a file and compares it on a PC.
 * Here the replay file (tools/slippi/vita_replay_pack.py) already holds
 * what Slippi recorded, so the Vita compares each fighter's state itself
 * and writes the result to ux0:data/melee/slippi/slippi.log.  Compared bit
 * for bit, like the 3DS's replay_compare.py: before each frame the RNG
 * seed, action, X and Y; after it action, X, Y, facing, percent, shield,
 * stocks and animation frame.
 *
 * The files are replay1.bin, replay2.bin and so on (or just replay.bin),
 * played one after the other, with a table of results at the end.
 *
 * After the first wrong frame the result is FAIL, but the replay keeps
 * going: each frame the fighters get the recorded seed, position, facing
 * and percent back, the way Slippi's playback resyncs (PDB_SHOULD_RESYNC
 * in RestoreGameFrame).  A desync that starts again after a clean stretch
 * is logged as a new one, so one run shows more than the first problem.
 *
 * Without a replay file every function here returns straight away. */
#ifdef MELEE_VITA_SLIPPI /* only in -EnableSlippi builds */
#include "slippi_replay.h"

#include "slippi_match_block.h"
#include "slippi_rules.h"
#include "slippi_ucf.h"

#include <melee/cm/camera.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftcamera.h>
#include <melee/ft/ftparts.h>
#include <melee/ft/types.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/mn/types.h>
#include <melee/mp/mpcoll.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/hsd_3915.h>
#include <sysdolphin/baselib/random.h>

#include <gxm_game.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From the Vita platform layer (platforms/vita/slippi/slippi_plat_vita.c). */
void sp_log(const char* fmt, ...);
const char* sp_data_dir(void);

extern u32* HSD_RandSeedPtr;

/* The raw pads the frame used, read by UCF (slippi_ucf.c). */
extern PADStatus mp_pad_consumed[4];

/* Slippi numbers frames from -123, the first frame of the match intro. */
#define FIRST_FRAME (-123)
#define HEADER_SIZE 0x18C
#define FRAME_HEADER_SIZE 8
#define ENTRY_SIZE 88
#define NAME_OFFSET 0x14C
#define NAME_SIZE 60
#define FLAGS_OFFSET 0x188
#define FLAG_FROZEN_FD 1

/* Mismatch lines written in full for each desync; after that only the
 * counts. */
#define DETAIL_LINES_PER_DESYNC 6
/* Desyncs logged in full for each replay. */
#define MAX_DESYNCS_LOGGED 8
/* A wrong frame this long after the last one starts a new desync. */
#define CLEAN_FRAMES_FOR_NEW_DESYNC 60
/* Replays in a batch (replay1.bin to replay32.bin). */
#define MAX_REPLAYS 32

static int s_loaded;          /* 0 = not tried yet, 1 = a replay is loaded, -1 = none left */
static int s_replay_number;   /* 1 for replay1.bin, 0 for replay.bin */
static int s_replay_total;    /* how many replay files there are */
static int s_batch_done;      /* every replay has played */
static char s_replay_file[32];
static u8* s_blob;
static size_t s_blob_size;
static const u8** s_frames;   /* each replay frame's record in s_blob */
static int s_first_frame;
static unsigned s_frame_count;
static int s_frame_index = FIRST_FRAME;
static int s_terminated;      /* the recording ran out */
static int s_finished;        /* the summary is written */

/* Results. */
static unsigned s_checked;
static unsigned s_mismatched;
static int s_first_bad_frame;
static int s_have_bad_frame;
static unsigned s_detail_lines;
static unsigned s_desyncs;
static int s_last_bad_frame;
static int s_frame_was_bad;   /* this frame already had a wrong record */

/* Who called the RNG this frame, for a frame whose seed comes out wrong.
 * Small numbers are markers: 1 to 4 = that port's pre-frame check. */
#define RNG_TRACE_SIZE 40
static void* s_rng_trace[RNG_TRACE_SIZE];
static unsigned s_rng_trace_count;   /* entries, markers included */
static unsigned s_rng_calls;
static int s_log_rng_trace_again;    /* log the whole frame's trace too */

/* One line per replay for the table at the end. */
typedef struct ReplayResult {
    char name[NAME_SIZE];
    int passed;
    int first_bad_frame;
    unsigned checked;
    unsigned mismatched;
    unsigned desyncs;
    int last_frame;
    int ended_early;
} ReplayResult;

static ReplayResult s_results[MAX_REPLAYS];
static int s_result_count;

/* ---------------------------------------------------------------- reading */

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | (u32) p[3];
}

static u16 be16(const u8* p)
{
    return (u16) ((p[0] << 8) | p[1]);
}

static float bef(const u8* p)
{
    u32 bits = be32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static u32 float_bits(float value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* sp_read_file stops at 1 MiB (it is for user.json and config.ini), and a
 * packed replay is about 170 bytes a frame: 1.4 MB for a 2-minute match.
 * Long matches are under 16 MiB. */
#define MAX_REPLAY_SIZE (16 * 1024 * 1024)

static u8* read_replay_file(const char* path, size_t* size)
{
    FILE* file;
    long length;
    u8* data;

    file = fopen(path, "rb");
    if (file == NULL) return NULL;

    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    if (length <= 0 || length > MAX_REPLAY_SIZE) {
        sp_log("replay: replay.bin is %ld bytes, expected 1 to %d", length, MAX_REPLAY_SIZE);
        fclose(file);
        return NULL;
    }

    data = malloc((size_t) length);
    if (data == NULL) {
        sp_log("replay: no memory to read replay.bin (%ld bytes)", length);
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t) length, file) != (size_t) length) {
        sp_log("replay: could not read all of replay.bin");
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t) length;
    return data;
}

/* Reads one replay file into s_blob and finds its frames.  Returns 1 if
 * it worked. */
static int load_file(const char* name)
{
    char path[256];
    size_t offset;
    unsigned i;

    snprintf(path, sizeof(path), "%s/%s", sp_data_dir(), name);
    s_blob = read_replay_file(path, &s_blob_size);
    if (s_blob == NULL) return 0;

    if (s_blob_size >= 4 && memcmp(s_blob, "VSR1", 4) == 0) {
        sp_log("replay: %s is the old format, pack it again with tools/slippi/vita_replay_pack.py",
               name);
        free(s_blob);
        s_blob = NULL;
        return 0;
    }
    if (s_blob_size < HEADER_SIZE || memcmp(s_blob, "VSR2", 4) != 0) {
        sp_log("replay: %s is not a Vita replay (make it with tools/slippi/vita_replay_pack.py)",
               name);
        free(s_blob);
        s_blob = NULL;
        return 0;
    }
    /* The name field always ends with a NUL; make sure. */
    s_blob[NAME_OFFSET + NAME_SIZE - 1] = 0;

    s_first_frame = (int) be32(s_blob + 0x144);
    s_frame_count = be32(s_blob + 0x148);
    s_frames = malloc(s_frame_count * sizeof(*s_frames));
    if (s_frames == NULL) {
        sp_log("replay: no memory for %u frames", s_frame_count);
        free(s_blob);
        s_blob = NULL;
        return 0;
    }

    /* Find where each frame's record starts. */
    offset = HEADER_SIZE;
    for (i = 0; i < s_frame_count; i++) {
        if (offset + FRAME_HEADER_SIZE > s_blob_size) {
            sp_log("replay: %s ends after %u of %u frames", name, i, s_frame_count);
            s_frame_count = i;
            break;
        }
        s_frames[i] = s_blob + offset;
        offset += FRAME_HEADER_SIZE + s_blob[offset + 4] * ENTRY_SIZE;
    }

    snprintf(s_replay_file, sizeof(s_replay_file), "%s", name);
    sp_log("replay: %s (%s) has %u frames from frame %d, Slippi %u.%u.%u, UCF %s%s", name,
           (const char*) s_blob + NAME_OFFSET, s_frame_count, s_first_frame, s_blob[5], s_blob[6],
           s_blob[7], s_blob[4] == 84 ? "0.84" : s_blob[4] == 80 ? "0.8" : "off",
           (be32(s_blob + FLAGS_OFFSET) & FLAG_FROZEN_FD) ? ", FD background frozen" : "");
    return 1;
}

static void unload_file(void)
{
    free(s_frames);
    s_frames = NULL;
    free(s_blob);
    s_blob = NULL;
    s_blob_size = 0;
    s_frame_count = 0;
}

/* 1 if replay<number>.bin is there. */
static int replay_file_exists(int number)
{
    char path[256];
    FILE* file;

    snprintf(path, sizeof(path), "%s/replay%d.bin", sp_data_dir(), number);
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static int load(void)
{
    char name[32];

    if (s_loaded != 0) return s_loaded > 0;
    s_loaded = -1;

    /* replay.bin on its own, or else the batch from replay1.bin. */
    if (load_file("replay.bin")) {
        s_replay_number = 0;
        s_replay_total = 1;
        s_loaded = 1;
        return 1;
    }
    snprintf(name, sizeof(name), "replay%d.bin", 1);
    if (load_file(name)) {
        s_replay_number = 1;
        s_replay_total = 1;
        while (s_replay_total < MAX_REPLAYS && replay_file_exists(s_replay_total + 1)) {
            s_replay_total++;
        }
        sp_log("replay: %d replays to play", s_replay_total);
        s_loaded = 1;
        return 1;
    }
    return 0;
}

/* The table of results, once every replay has played. */
static void log_batch_table(void)
{
    int i;
    int passed = 0;

    sp_log("replay: all %d replays done", s_result_count);
    for (i = 0; i < s_result_count; i++) {
        ReplayResult* r = &s_results[i];
        if (r->passed) {
            passed++;
            sp_log("replay: %2d PASS %s, %u records, to frame %d%s", i + 1, r->name, r->checked,
                   r->last_frame, r->ended_early ? " (match ended early)" : "");
        } else {
            sp_log("replay: %2d FAIL %s, first wrong frame %d, %u of %u records wrong, %u desyncs, "
                   "to frame %d%s",
                   i + 1, r->name, r->first_bad_frame, r->mismatched, r->checked, r->desyncs,
                   r->last_frame, r->ended_early ? " (match ended early)" : "");
        }
    }
    sp_log("replay: %d of %d passed", passed, s_result_count);
}

/* After a replay finishes, the next one in the batch (or the end). */
static void load_next(void)
{
    char name[32];

    if (s_batch_done) return;
    unload_file();
    if (s_replay_number > 0 && s_replay_number < MAX_REPLAYS) {
        s_replay_number++;
        snprintf(name, sizeof(name), "replay%d.bin", s_replay_number);
        if (load_file(name)) {
            s_finished = 0;
            return;
        }
    }

    /* No more: turn the harness and the Slippi codes off. */
    s_loaded = -1;
    s_batch_done = 1;
    log_batch_table();
    mp_slippi_rules_set_enabled(0);
    mp_slippi_rules_set_frozen_fd(0);
    mp_ucf_set_version(84);
}

int mp_slippi_replay_on(void)
{
    return load();
}

static const u8* frame_record(int frame)
{
    int i = frame - s_first_frame;
    if (!mp_slippi_replay_on()) return NULL;
    if (i < 0 || (unsigned) i >= s_frame_count) return NULL;
    return s_frames[i];
}

static int is_follower(Fighter* fp)
{
    return fp->is_sub_fighter && fp->kind == Ft_Kind_Nana;
}

static const u8* entry_for(int frame, int port, int follower)
{
    const u8* record = frame_record(frame);
    int count;
    int i;

    if (record == NULL) return NULL;
    count = record[4];
    for (i = 0; i < count; i++) {
        const u8* entry = record + FRAME_HEADER_SIZE + i * ENTRY_SIZE;
        if (entry[0] == port && entry[1] == follower) return entry;
    }
    return NULL;
}

/* ---------------------------------------------------------------- checking */

static void note_frame_result(int bad)
{
    s_checked++;
    if (!bad) return;
    s_mismatched++;
    if (!s_have_bad_frame) {
        s_have_bad_frame = 1;
        s_first_bad_frame = s_frame_index;
    }
}

/* Called before a wrong field is logged.  The first wrong field after a
 * clean stretch starts a new desync, which gets its own detail lines. */
static void start_desync_if_new(void)
{
    if (s_frame_was_bad) return;
    s_frame_was_bad = 1;
    if (s_desyncs == 0 || s_frame_index - s_last_bad_frame > CLEAN_FRAMES_FOR_NEW_DESYNC) {
        s_desyncs++;
        s_detail_lines = 0;
        if (s_desyncs <= MAX_DESYNCS_LOGGED) {
            sp_log("replay: desync %u starts at frame %d", s_desyncs, s_frame_index);
        }
    }
}

static int detail_line_allowed(void)
{
    start_desync_if_new();
    if (s_desyncs > MAX_DESYNCS_LOGGED) return 0;
    if (s_detail_lines >= DETAIL_LINES_PER_DESYNC) return 0;
    s_detail_lines++;
    return 1;
}

/* One log line per wrong field, a few for each desync. */
static void report_u32(const char* when, Fighter* fp, const char* field, u32 mine, u32 slippi)
{
    if (!detail_line_allowed()) return;
    s_detail_lines++;
    sp_log("replay: frame %d port %d%s %s %s: Vita %u, Slippi %u", s_frame_index, fp->player_id + 1,
           is_follower(fp) ? " nana" : "", when, field, (unsigned) mine, (unsigned) slippi);
}

static void report_float(const char* when, Fighter* fp, const char* field, u32 mine, u32 slippi)
{
    float a;
    float b;
    if (!detail_line_allowed()) return;
    memcpy(&a, &mine, sizeof(a));
    memcpy(&b, &slippi, sizeof(b));
    sp_log("replay: frame %d port %d%s %s %s: Vita %.9g (%08X), Slippi %.9g (%08X)", s_frame_index,
           fp->player_id + 1, is_follower(fp) ? " nana" : "", when, field, (double) a,
           (unsigned) mine, (double) b, (unsigned) slippi);
}

static int check_u32(const char* when, Fighter* fp, const char* field, u32 mine, u32 slippi)
{
    if (mine == slippi) return 0;
    report_u32(when, fp, field, mine, slippi);
    return 1;
}

static int check_float(const char* when, Fighter* fp, const char* field, float mine, const u8* slippi)
{
    u32 mine_bits = float_bits(mine);
    u32 slippi_bits = be32(slippi);
    if (mine_bits == slippi_bits) return 0;
    report_float(when, fp, field, mine_bits, slippi_bits);
    return 1;
}

/* Called by HSD_Rand, HSD_Randf and HSD_Randi (random.c). */
static void add_to_rng_trace(void* value)
{
    if (s_rng_trace_count < RNG_TRACE_SIZE) {
        s_rng_trace[s_rng_trace_count] = value;
        s_rng_trace_count++;
    }
}

void mp_slippi_replay_note_rng(void* caller)
{
    if (s_loaded <= 0) return;
    add_to_rng_trace(caller);
    s_rng_calls++;
}

/* The RNG calls this frame so far, as offsets from HSD_Rand, so they can
 * be found in the ELF (address of HSD_Rand in nm + offset). */
static void log_rng_trace(void)
{
    char line[400];
    size_t used;
    unsigned i;

    used = (size_t) snprintf(line, sizeof(line), "replay: %u RNG calls this frame so far:",
                             s_rng_calls);
    for (i = 0; i < s_rng_trace_count && used < sizeof(line); i++) {
        uintptr_t value = (uintptr_t) s_rng_trace[i];
        if (value >= 1 && value <= 4) {
            used += (size_t) snprintf(line + used, sizeof(line) - used, " [port %u]",
                                      (unsigned) value);
        } else {
            used += (size_t) snprintf(line + used, sizeof(line) - used, " %+ld",
                                      (long) (value - (uintptr_t) &HSD_Rand));
        }
    }
    sp_log("%s", line);
}

/* At the start of each desync: what went into that fighter this frame. */
static void log_inputs(Fighter* fp, const u8* entry)
{
    sp_log("replay: port %d inputs: stick %.4f %.4f, c-stick %.4f %.4f, trigger %.4f, buttons %08X, "
           "raw stick %d %d, raw c-stick %d %d, UCF %d",
           fp->player_id + 1, (double) bef(entry + 8), (double) bef(entry + 12),
           (double) bef(entry + 16), (double) bef(entry + 20), (double) bef(entry + 24),
           (unsigned) be32(entry + 28), (s8) entry[2], (s8) entry[3], (s8) entry[4],
           (s8) entry[5], mp_ucf_version());
}

/* Before the frame runs: what Slippi's pre-frame record holds. */
static void check_pre(Fighter* fp, const u8* entry)
{
    int bad = 0;
    int bad_seed;
    unsigned desyncs_before = s_desyncs;
    if ((entry[32] & 1) == 0) return;
    /* Mark the check in the RNG trace. */
    add_to_rng_trace((void*) (uintptr_t) (fp->player_id + 1));
    bad_seed = check_u32("pre", fp, "seed", *HSD_RandSeedPtr, be32(entry + 36));
    bad |= bad_seed;
    bad |= check_u32("pre", fp, "action", fp->motion_id, be16(entry + 34));
    bad |= check_float("pre", fp, "x", fp->cur_pos.x, entry + 40);
    bad |= check_float("pre", fp, "y", fp->cur_pos.y, entry + 44);
    note_frame_result(bad);
    if (s_desyncs != desyncs_before && s_desyncs <= MAX_DESYNCS_LOGGED) {
        log_inputs(fp, entry);
        if (bad_seed) {
            log_rng_trace();
            s_log_rng_trace_again = 1;
        }
    }
}

/* After the frame: what Slippi's post-frame record holds. */
static void check_post(Fighter* fp, const u8* entry)
{
    int bad = 0;
    unsigned desyncs_before = s_desyncs;
    if ((entry[32] & 2) == 0) return;
    bad |= check_u32("post", fp, "action", fp->motion_id, be16(entry + 48));
    bad |= check_float("post", fp, "x", fp->cur_pos.x, entry + 52);
    bad |= check_float("post", fp, "y", fp->cur_pos.y, entry + 56);
    bad |= check_float("post", fp, "facing", fp->facing_dir, entry + 60);
    bad |= check_float("post", fp, "percent", fp->dmg.x1830_percent, entry + 64);
    bad |= check_float("post", fp, "shield", fp->shield_health, entry + 68);
    bad |= check_float("post", fp, "animation frame", fp->cur_anim_frame, entry + 72);
    bad |= check_u32("post", fp, "stocks", (u32) Player_GetStocks(fp->player_id), entry[33]);
    note_frame_result(bad);
    if (s_desyncs != desyncs_before && s_desyncs <= MAX_DESYNCS_LOGGED) log_inputs(fp, entry);
}

/* Slippi's playback resync (RestoreGameFrame with PDB_SHOULD_RESYNC set):
 * the recorded seed, position and facing, and the percent through
 * Damage_UpdatePercent.  Only after the first wrong frame, so it never
 * hides a desync. */
static void resync(Fighter* fp, const u8* entry)
{
    float percent;
    float change;

    if ((entry[32] & 1) == 0) return;
    *HSD_RandSeedPtr = be32(entry + 36);
    fp->cur_pos.x = bef(entry + 40);
    fp->cur_pos.y = bef(entry + 44);
    fp->facing_dir = bef(entry + 76);

    if ((entry[32] & 4) == 0) return;
    percent = bef(entry + 80);
    change = percent - fp->dmg.x1830_percent;
    if (change != 0.0f) {
        Fighter_TakeDamage_8006CC7C(fp, change);
    }
}

static void log_summary(const char* why)
{
    ReplayResult* r;

    sp_log("replay: %s: %s at frame %d: %u fighter records checked, %u wrong", s_replay_file, why,
           s_frame_index, s_checked, s_mismatched);
    if (s_have_bad_frame) {
        sp_log("replay: first wrong frame %d (Slippi frame number; %d is the first frame of the intro), %u desyncs",
               s_first_bad_frame, FIRST_FRAME, s_desyncs);
        sp_log("replay: RESULT FAIL");
    } else if (s_checked == 0) {
        sp_log("replay: RESULT FAIL (nothing was checked)");
    } else {
        sp_log("replay: RESULT PASS");
    }

    /* Keep a line for the table at the end. */
    if (s_result_count >= MAX_REPLAYS) return;
    r = &s_results[s_result_count];
    s_result_count++;
    snprintf(r->name, sizeof(r->name), "%s", (const char*) s_blob + NAME_OFFSET);
    r->passed = !s_have_bad_frame && s_checked > 0;
    r->first_bad_frame = s_first_bad_frame;
    r->checked = s_checked;
    r->mismatched = s_mismatched;
    r->desyncs = s_desyncs;
    r->last_frame = s_frame_index;
    r->ended_early = !s_terminated;
}

/* ---------------------------------------------------------------- screen */

/* After a replay's match: straight into the next one, without the results
 * screen.  After the last one the results screen shows as usual. */
void mp_slippi_replay_route(u8* next_state_id)
{
    if (s_loaded <= 0 || !s_finished) return;
    if (s_replay_number > 0 && replay_file_exists(s_replay_number + 1)) {
        *next_state_id = 1; /* the debug VS mode's match state */
    } else {
        /* The last one: write the table now, the results screen may wait. */
        load_next();
    }
}

static int count_passed(void)
{
    int i;
    int passed = 0;
    for (i = 0; i < s_result_count; i++) {
        if (s_results[i].passed) passed++;
    }
    return passed;
}

/* The panel is plain shapes in display pixels (960 x 544), handed to the
 * renderer to draw over the finished frame (melee_vita_gxm_set_screen_overlay).
 * It used to be drawn with GX from DevText_DrawAll, and in a match that
 * broke the match's own drawing: a black screen with streaks fanning out
 * from the bottom left corner. */
#define PANEL_MAX_TRIANGLE_VERTICES 64
#define PANEL_MAX_LINE_VERTICES 8192
/* One character of the debug font, in display pixels. */
#define PANEL_CELL_WIDTH 11.0f
#define PANEL_CELL_HEIGHT 15.0f

static MeleeVitaScreenVertex s_panel_triangles[PANEL_MAX_TRIANGLE_VERTICES];
static u32 s_panel_triangle_count;
static MeleeVitaScreenVertex s_panel_lines[PANEL_MAX_LINE_VERTICES];
static u32 s_panel_line_count;

/* A colour the way the renderer wants it (vita2d's RGBA8). */
static u32 panel_color(u8 r, u8 g, u8 b, u8 a)
{
    return ((u32) a << 24) | ((u32) b << 16) | ((u32) g << 8) | (u32) r;
}

static void panel_vertex(MeleeVitaScreenVertex* vertex, float x, float y,
                         u32 color)
{
    vertex->x = x;
    vertex->y = y;
    vertex->z = 0.0f;
    vertex->u = 0.0f;
    vertex->v = 0.0f;
    vertex->color = color;
}

/* A filled rectangle, as two triangles. */
static void panel_rectangle(float x, float y, float w, float h, u32 color)
{
    MeleeVitaScreenVertex* v;

    if (w <= 0.0f || h <= 0.0f) return;
    if (s_panel_triangle_count + 6 > PANEL_MAX_TRIANGLE_VERTICES) return;
    v = &s_panel_triangles[s_panel_triangle_count];
    panel_vertex(&v[0], x, y, color);
    panel_vertex(&v[1], x + w, y, color);
    panel_vertex(&v[2], x + w, y + h, color);
    panel_vertex(&v[3], x, y, color);
    panel_vertex(&v[4], x + w, y + h, color);
    panel_vertex(&v[5], x, y + h, color);
    s_panel_triangle_count += 6;
}

/* One stroke of a character.  The renderer's lines are one pixel wide, so
 * each stroke goes in twice, the second one pixel to the right. */
static void panel_stroke(float x0, float y0, float x1, float y1, u32 color)
{
    MeleeVitaScreenVertex* v;

    if (s_panel_line_count + 4 > PANEL_MAX_LINE_VERTICES) return;
    v = &s_panel_lines[s_panel_line_count];
    panel_vertex(&v[0], x0, y0, color);
    panel_vertex(&v[1], x1, y1, color);
    panel_vertex(&v[2], x0 + 1.0f, y0, color);
    panel_vertex(&v[3], x1 + 1.0f, y1, color);
    s_panel_line_count += 4;
}

/* One character of the debug font, the way DrawASCII draws it. */
static void panel_char(int chr, float x, float y, u32 color)
{
    const u8* strokes;
    int i;
    u8 p0;
    u8 p1;

    /* DrawASCII draws these two as points. */
    if (chr == '.') {
        panel_rectangle(x + PANEL_CELL_WIDTH * 0.3f,
                        y + PANEL_CELL_HEIGHT * 0.9f - 1.0f, 2.0f, 2.0f,
                        color);
        return;
    }
    if (chr == ':') {
        panel_rectangle(x + PANEL_CELL_WIDTH * 0.3f,
                        y + PANEL_CELL_HEIGHT * 0.3f - 1.0f, 2.0f, 2.0f,
                        color);
        panel_rectangle(x + PANEL_CELL_WIDTH * 0.3f,
                        y + PANEL_CELL_HEIGHT * 0.7f - 1.0f, 2.0f, 2.0f,
                        color);
        return;
    }

    strokes = HSD_DebugFontStrokes(chr);
    if (strokes == NULL) return;
    i = 0;
    while (i < 0x29 && strokes[i] != 0xFF) {
        p0 = strokes[i];
        p1 = strokes[i + 1];
        i += 2;
        panel_stroke(x + PANEL_CELL_WIDTH * (0.11f * (p0 >> 4)),
                     y + PANEL_CELL_HEIGHT * (0.11f * (p0 & 0xF)),
                     x + PANEL_CELL_WIDTH * (0.11f * (p1 >> 4)),
                     y + PANEL_CELL_HEIGHT * (0.11f * (p1 & 0xF)), color);
    }
}

/* One line of the panel. */
static void panel_text(const char* text, float x, float y, u32 color)
{
    int i;
    for (i = 0; text[i] != '\0'; i++) {
        panel_char(text[i], x, y, color);
        x += PANEL_CELL_WIDTH;
    }
}

/* The panel in the top left corner, like a benchmark: which replay of how
 * many, how far it is, and the results so far.  Called from
 * DevText_DrawAll (textdraw.c) on every screen.  It only fills in the
 * shapes; the renderer draws them after the frame. */
void mp_slippi_replay_draw_panel(void)
{
    u32 backdrop = panel_color(0x00, 0x00, 0x00, 0xB0);
    u32 white = panel_color(0xFF, 0xFF, 0xFF, 0xFF);
    u32 grey = panel_color(0xA0, 0xB8, 0xD0, 0xFF);
    u32 green = panel_color(0x80, 0xFF, 0x80, 0xFF);
    u32 red = panel_color(0xFF, 0x80, 0x80, 0xFF);
    u32 bar_back = panel_color(0x40, 0x40, 0x40, 0xFF);
    u32 bar_fill = panel_color(0x60, 0xD0, 0xB0, 0xFF);
    char line[80];
    int passed;
    int failed;
    float progress;
    int frames_done;

    if (s_replay_total == 0) return;
    passed = count_passed();
    failed = s_result_count - passed;

    s_panel_triangle_count = 0;
    s_panel_line_count = 0;
    /* Narrow enough to stay clear of the match timer. */
    panel_rectangle(16.0f, 16.0f, 324.0f, 88.0f, backdrop);

    if (s_batch_done) {
        snprintf(line, sizeof(line), "SLIPPI REPLAY TEST DONE");
        panel_text(line, 28.0f, 24.0f, white);
        snprintf(line, sizeof(line), "%d OF %d PASSED", passed, s_result_count);
        panel_text(line, 28.0f, 44.0f, failed == 0 ? green : red);
        panel_text("RESULTS IN SLIPPI.LOG", 28.0f, 64.0f, grey);
    } else {
        snprintf(line, sizeof(line), "SLIPPI REPLAY TEST  %d / %d",
                 s_replay_number > 0 ? s_replay_number : 1, s_replay_total);
        panel_text(line, 28.0f, 24.0f, white);
        if (s_blob != NULL) {
            panel_text((const char*) s_blob + NAME_OFFSET, 28.0f, 44.0f,
                       grey);
        }

        /* This replay's progress. */
        frames_done = s_frame_index - FIRST_FRAME;
        if (frames_done < 0) frames_done = 0;
        progress = 0.0f;
        if (s_frame_count > 0) {
            progress = (float) frames_done / (float) s_frame_count;
        }
        if (progress > 1.0f) progress = 1.0f;
        panel_rectangle(28.0f, 64.0f, 300.0f, 6.0f, bar_back);
        panel_rectangle(28.0f, 64.0f, 300.0f * progress, 6.0f, bar_fill);

        snprintf(line, sizeof(line), "PASS %d  FAIL %d  NOW %s", passed,
                 failed, s_have_bad_frame ? "DESYNC" : "OK");
        panel_text(line, 28.0f, 80.0f, s_have_bad_frame ? red : green);
    }

    melee_vita_gxm_set_screen_overlay(s_panel_triangles,
                                      s_panel_triangle_count, s_panel_lines,
                                      s_panel_line_count);
}

/* ---------------------------------------------------------------- boot */

/* Slippi's "Boot to Playback Scene" (801a45a0). */
void mp_slippi_replay_boot_mode(u8* mode)
{
    if (mp_slippi_replay_on()) {
        sp_log("replay: booting straight into the debug VS mode");
        *mode = GM_DEBUG_VS;
    }
}

/* The replay's fighters and stage into the preload cache, and their sound
 * banks, before the match scene preloads (SceneThink_Playback, 801a6348).
 * The debug VS mode would otherwise preload its own Link and Mario. */
void mp_slippi_replay_prepare_scene(void)
{
    PreloadedGameModeState* scene;
    const u8* info;
    u64 mask = 0;
    int i;

    /* The last replay is done: on to the next one in the batch. */
    if (s_loaded > 0 && s_finished) load_next();

    if (!mp_slippi_replay_on()) return;
    info = s_blob + 8;

    scene = lbDvd_GetPreloadCacheScene();
    for (i = 0; i < 4; i++) {
        const u8* player = info + 0x60 + 0x24 * i;
        /* Empty slots keep the template's fighter byte, so skip them. */
        scene->game_cache.entries[i].char_id = player[1] >= 3 ? ChKind_None : (s8) player[0];
        scene->game_cache.entries[i].color = player[3];
    }
    lbDvd_80018254();
    lbDvd_80018C2C(199);
    lbDvd_80017700(4);
    lbAudioAx_80026F2C(28);
    for (i = 0; i < 6; i++) {
        const u8* player = info + 0x60 + 0x24 * i;
        if ((s8) player[0] != ChKind_None && player[1] < 3) {
            mask |= lbAudioAx_80026E84((CharacterKind) (s8) player[0]);
        }
    }
    mask |= lbAudioAx_80026EBC((StKind) ((info[0xE] << 8) | info[0xF]));
    lbAudioAx_8002702C(4, mask);
    lbAudioAx_80027168();
    lbAudioAx_80024F6C();
}

/* ---------------------------------------------------------------- match */

/* Slippi's RestoreInitialRNG: each frame starts from the recorded seed. */
static void restore_rng_proc(HSD_GObj* gobj)
{
    const u8* record = frame_record(s_frame_index);
    (void) gobj;
    if (record != NULL && !s_terminated) {
        *HSD_RandSeedPtr = be32(record);
    }
    s_rng_trace_count = 0;
    s_rng_calls = 0;
}

/* Slippi's RestoreGameInfo (8016e748): the replay's match data and seed. */
void mp_slippi_replay_start_melee(StartMeleeData* data)
{
    HSD_GObj* gobj;
    int i;

    if (!mp_slippi_replay_on()) return;

    mp_slippi_decode_match_block(s_blob + 8, data);
    *HSD_RandSeedPtr = be32(s_blob + 0x140);

    /* Slippi Dolphin always runs its gameplay codes (neutral spawns, UCF
     * and the rest, slippi_rules.c), so the replay has to as well. */
    mp_slippi_rules_set_enabled(1);
    /* The UCF version the replay ran, and the other codes that changed
     * between Slippi versions (from its code list). */
    mp_ucf_set_version(s_blob[4]);
    mp_slippi_rules_set_frozen_fd((be32(s_blob + FLAGS_OFFSET) & FLAG_FROZEN_FD) != 0);
    s_frame_index = FIRST_FRAME;
    s_terminated = 0;
    s_finished = 0;
    s_checked = 0;
    s_mismatched = 0;
    s_have_bad_frame = 0;
    s_detail_lines = 0;
    s_desyncs = 0;
    s_last_bad_frame = 0;
    s_frame_was_bad = 0;

    /* GObj class 4, p_link 7, priority 0; process s_link 0, as Slippi. */
    gobj = GObj_Create(4, 7, 0);
    HSD_GObj_SetupProc(gobj, restore_rng_proc, 0);

    sp_log("replay: match starts: stage %u, %s mode, %u s timer, seed %08X", data->rules.stkind,
           data->rules.is_stock ? "stock" : "time", (unsigned) data->rules.time_limit,
           (unsigned) *HSD_RandSeedPtr);
    for (i = 0; i < 4; i++) {
        PlayerInitData* player = &data->players[i];
        if (player->slot_type >= 3) continue;
        sp_log("replay: port %d: character %d, color %u, %d stocks", i + 1, player->ckind,
               player->color, player->stocks);
    }
}

/* Slippi's IncrementFrameIndex / FetchGameFrame (8016d294). */
void mp_slippi_replay_scene_think(int match_result)
{
    if (!mp_slippi_replay_on() || s_finished) return;

    if (match_result != 0) {
        /* GAME! (or no contest): the results screen may wait for a button. */
        mp_slippi_replay_match_exit();
        return;
    }

    /* After a wrong seed: the rest of that frame's RNG calls. */
    if (s_log_rng_trace_again) {
        s_log_rng_trace_again = 0;
        log_rng_trace();
    }

    /* The frame that just ran: remember if it had a wrong record. */
    if (s_frame_was_bad) s_last_bad_frame = s_frame_index;
    s_frame_was_bad = 0;

    if (gm_801A4BA8() == 0) s_frame_index = FIRST_FRAME;
    else s_frame_index++;

    /* Every 600 frames (10 s), so a crash still leaves a result. */
    if (s_frame_index % 600 == 0) {
        sp_log("replay: frame %d: %u records checked, %u wrong", s_frame_index, s_checked,
               s_mismatched);
    }
    if (frame_record(s_frame_index) == NULL && !s_terminated) {
        s_terminated = 1;
        sp_log("replay: recording ends after frame %d", s_frame_index - 1);
    }
}

int mp_slippi_replay_terminated(void)
{
    return mp_slippi_replay_on() && s_terminated && !s_finished;
}

/* Slippi's RestoreGameFrame (8006b0dc): the recorded inputs replace the
 * pad's, then the pre-frame check. */
void mp_slippi_replay_input(Fighter* fp)
{
    const u8* entry;
    int follower;
    int qread;
    PADStatus* pad;

    if (!mp_slippi_replay_on() || s_terminated || s_finished) return;

    follower = is_follower(fp);
    entry = entry_for(s_frame_index, fp->player_id, follower);
    if (entry == NULL) return;

    if (!follower) {
        fp->input.lstick[0].x = bef(entry + 8);
        fp->input.lstick[0].y = bef(entry + 12);
        fp->input.cstick[0].x = bef(entry + 16);
        fp->input.cstick[0].y = bef(entry + 20);
        fp->input.triggers[0] = bef(entry + 24);
        fp->input.held_buttons[0] = be32(entry + 28);

        /* UCF reads the raw stick from the pad queue entry this frame's
         * input came from, so put the recorded raw stick there too. */
        qread = HSD_PadLibData.qread - 1;
        if (qread < 0) qread += HSD_PadLibData.qnum;
        pad = &HSD_PadLibData.queue[qread].stat[fp->x618_player_id];
        pad->stickX = (s8) entry[2];
        pad->stickY = (s8) entry[3];
        pad->substickX = (s8) entry[4];
        pad->substickY = (s8) entry[5];

        /* And where the UCF code (slippi_ucf.c) reads them. */
        pad = &mp_pad_consumed[fp->x618_player_id & 3];
        pad->stickX = (s8) entry[2];
        pad->stickY = (s8) entry[3];
        pad->substickX = (s8) entry[4];
        pad->substickY = (s8) entry[5];
    }

    /* Slippi's spawn correction on the first frame. */
    if (s_frame_index == FIRST_FRAME) {
        CmSubject* box;
        ftPartSetRotX(fp, 0, 0.0f);
        fp->coll_data.cur_pos = fp->cur_pos;
        fp->coll_data.last_pos = fp->cur_pos;
        fp->mv.co.entry.x4 = fp->cur_pos.y;
        fp->coll_data.x38 = mpColl_804D64AC;
        Player_80032828(fp->player_id, fp->is_sub_fighter, &fp->cur_pos);
        ftCamera_UpdateCameraBox(fp->gobj);
        box = fp->x890_cameraBox;
        box->ext.h.x = box->target_ext.h.x;
        box->ext.h.y = box->target_ext.h.y;
        Camera_8002F3AC();
    }

    check_pre(fp, entry);
    if (s_have_bad_frame) resync(fp, entry);
}

/* Slippi's SendGamePostFrame point: after the camera callback. */
void mp_slippi_replay_post_frame(Fighter* fp)
{
    const u8* entry;

    if (!mp_slippi_replay_on() || s_terminated || s_finished || fp->x221F_b3) return;
    entry = entry_for(s_frame_index, fp->player_id, is_follower(fp));
    if (entry == NULL) return;
    check_post(fp, entry);
}

void mp_slippi_replay_match_exit(void)
{
    if (!mp_slippi_replay_on() || s_finished) return;
    s_finished = 1;
    log_summary(s_terminated ? "replay finished" : "match ended early");
}

#endif /* MELEE_VITA_SLIPPI */
