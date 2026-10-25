/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bench.h"
#include "gfx_settings.h"
#include "gxm_resolution.h"
#include "../vita_log.h"

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <vita2d.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_DIR "ux0:data/melee/benchmark"
#define BENCH_REQUEST BENCH_DIR "/request"

// in game ticks (60 per second of game time)
#define WARMUP_TICKS (5 * 60)
#define MEASURE_TICKS (60 * 60)
#define MAX_ROWS 8000 // room for 60 s at 60 fps plus slowdown

// StKind values (src/melee/gr/forward.h)
static const struct {
    int kind;
    const char* name;
} stages[] = {
    { 0x1F, "Battlefield" },
    { 0x20, "Final Destination" },
    { 0x08, "Yoshi's Story" },
    { 0x03, "Pokemon Stadium" },
    { 0x1C, "Dream Land N64" },
    // Last on purpose: leaving Fountain of Dreams for the next stage crashed
    // once on hardware. Its results are saved before the match ends.
    { 0x02, "Fountain of Dreams" },
};
#define STAGE_COUNT ((int) (sizeof(stages) / sizeof(stages[0])))

typedef struct {
    u32 dt_us;     // time since the previous frame was handed to the GPU
    u32 ticks;     // game logic ticks run for this frame (1 = full speed)
    u32 update_us; // game logic
    u32 render_us; // building the frame on the CPU, includes wait_us
    u32 wait_us;   // waiting for the render thread to finish the last frame
    u32 drain_us;  // waiting for the GX submit worker (part of render_us)
    u32 audio_us;  // audio mixing done on the game thread
    u32 cls_us[6]; // object drawing by class, see cls_names
    u32 cls_draws[6]; // draw calls made by each class
    u32 rt_us;     // render thread time (recording GXM commands)
    u32 gpu_us;    // render thread start -> GPU finished the frame
    u32 counts[5]; // see count_names
} Row;

// object classes (HSD_GOBJ_CLASS_*) grouped for the report
static const char* cls_names[6] = { "fighters", "items", "stage", "effects", "hud", "other" };
static int cls_bucket(unsigned int c)
{
    switch (c) {
    case 4: return 0;               // fighter
    case 6: case 7: return 1;       // item, item link
    case 3: case 13: return 2;      // stage, ground
    case 8: return 3;               // effect
    case 9: case 14: case 17: return 4; // text, ui
    default: return 5;
    }
}
// gx.c g_melee_vita_gx_counts
static const char* count_names[5] = { "gpu draws", "cpu draws", "dl builds", "dl rebuilds",
                                      "dl cpu fallbacks" };

typedef struct {
    int frames;
    float avg_fps, low1_fps, low01_fps, min_fps;
    float avg_ms, stdev_ms, worst_ms, jitter_ms;
    float over17, over25, over34; // % of frames
    float game_speed;             // logic ticks per second, 60 = full speed
    float update_ms, render_ms, wait_ms;
    float drain_ms, audio_ms, cls_ms[6], cls_draws[6], counts[5];
    float rt_ms, gpu_ms, gpu_max_ms; // headroom: compare with 16.7 / 33.3
    int hist[6];
    // what the screen actually showed (from the display callback)
    u32 shown[4];  // frames that stayed up 1, 2, 3, 4+ refreshes
    u32 dropped;   // frames replaced before they were ever shown
    float smooth;  // % of shown frames that stayed up exactly `target` refreshes
    int target;    // 1 = 60 fps, 2 = 30 fps lock
} Stats;

static const char* hist_names[6] = { "<17.5", "17.5-20", "20-25", "25-34", "34-50", ">50" };

extern u32 g_melee_vita_tick_total;       // gmscene.c
extern u64 g_melee_vita_update_total_us;  // gmscene.c
extern u64 g_melee_vita_render_total_us;  // gmscene.c
extern u64 g_melee_vita_audio_mix_total_us; // audio.c
extern u32 g_melee_vita_gx_counts[5];       // gx.c
extern volatile u32 g_melee_vita_rt_busy_us;  // gxm_game.c
extern volatile u32 g_melee_vita_gpu_done_us; // gxm_game.c

// gobj.c times object draws by class while this is set
int g_melee_vita_bench_prof;
static u32 cur_cls_us[6], cur_cls_draws[6];
// written by the display callback thread while recording
static volatile u32 flip_shown[4], flip_dropped, flip_target, flip_last;
static int players = 2;

static int active;
static int stage_i;
static int recording;
static int warmed_up; // recording started for this stage
static int cancelled;
static int done;      // finished (or cancelled), results on screen
static u64 done_at;
static u32 cur_frame;
static int report_num;

static Row rows[MAX_ROWS];
static int nrows;
static u64 last_present;
static u32 last_ticks;
static u64 last_update, last_render, last_audio;
static u32 last_counts[5];

static Stats results[STAGE_COUNT];

// stage drawing by stage part (map gobj index), summed over the stage
#define MAX_PARTS 64
static u64 part_us[MAX_PARTS];
static u32 part_draws[MAX_PARTS];
static struct { int part; float ms, draws; } top_parts[STAGE_COUNT][6];
static int nresults;

/* ---- stats ---- */

static int cmp_desc(const void* a, const void* b)
{
    u32 x = *(const u32*) a, y = *(const u32*) b;
    return x < y ? 1 : x > y ? -1 : 0;
}

// average fps over the slowest `frac` of frames
static float low_fps(const u32* sorted, int n, float frac)
{
    int k = (int) (n * frac);
    double sum = 0;
    if (k < 1) k = 1;
    for (int i = 0; i < k; i++) sum += sorted[i];
    return sum > 0 ? (float) (1000000.0 * k / sum) : 0.0f;
}

static void compute_stats(const Row* r, int n, Stats* s)
{
    static u32 sorted[MAX_ROWS];
    double total = 0, sq = 0, jit = 0, upd = 0, ren = 0, wai = 0, dra = 0, aud = 0;
    double rt = 0, gpu = 0;
    u32 gpu_max = 0;
    double cls[6] = { 0 }, cls_d[6] = { 0 }, cnt[5] = { 0 };
    u32 ticks = 0;
    int o17 = 0, o25 = 0, o34 = 0;

    memset(s, 0, sizeof(*s));
    s->frames = n;
    if (n < 2) return;

    for (int i = 0; i < n; i++) {
        double ms = r[i].dt_us / 1000.0;
        total += ms;
        sq += ms * ms;
        if (i > 0) jit += fabs(ms - r[i - 1].dt_us / 1000.0);
        ticks += r[i].ticks;
        upd += r[i].update_us;
        ren += r[i].render_us;
        wai += r[i].wait_us;
        dra += r[i].drain_us;
        rt += r[i].rt_us;
        gpu += r[i].gpu_us;
        if (r[i].gpu_us > gpu_max) gpu_max = r[i].gpu_us;
        aud += r[i].audio_us;
        for (int c = 0; c < 6; c++) cls[c] += r[i].cls_us[c];
        for (int c = 0; c < 6; c++) cls_d[c] += r[i].cls_draws[c];
        for (int c = 0; c < 5; c++) cnt[c] += r[i].counts[c];
        if (ms > 17.5) o17++;
        if (ms > 25.0) o25++;
        if (ms > 34.0) o34++;

        if (ms <= 17.5) s->hist[0]++;
        else if (ms <= 20) s->hist[1]++;
        else if (ms <= 25) s->hist[2]++;
        else if (ms <= 34) s->hist[3]++;
        else if (ms <= 50) s->hist[4]++;
        else s->hist[5]++;

        sorted[i] = r[i].dt_us;
    }
    qsort(sorted, n, sizeof(u32), cmp_desc);

    s->avg_ms = (float) (total / n);
    s->avg_fps = (float) (1000.0 * n / total);
    s->stdev_ms = (float) sqrt(fmax(0.0, sq / n - (total / n) * (total / n)));
    s->jitter_ms = (float) (jit / (n - 1));
    s->worst_ms = sorted[0] / 1000.0f;
    s->min_fps = sorted[0] ? 1000000.0f / sorted[0] : 0.0f;
    s->low1_fps = low_fps(sorted, n, 0.01f);
    s->low01_fps = low_fps(sorted, n, 0.001f);
    s->over17 = 100.0f * o17 / n;
    s->over25 = 100.0f * o25 / n;
    s->over34 = 100.0f * o34 / n;
    s->game_speed = (float) (ticks * 1000.0 / total);
    s->update_ms = (float) (upd / n / 1000.0);
    s->render_ms = (float) (fmax(0.0, ren - wai) / n / 1000.0); // without the wait
    s->wait_ms = (float) (wai / n / 1000.0);
    s->drain_ms = (float) (dra / n / 1000.0);
    s->rt_ms = (float) (rt / n / 1000.0);
    s->gpu_ms = (float) (gpu / n / 1000.0);
    s->gpu_max_ms = gpu_max / 1000.0f;
    s->audio_ms = (float) (aud / n / 1000.0);
    for (int c = 0; c < 6; c++) s->cls_ms[c] = (float) (cls[c] / n / 1000.0);
    for (int c = 0; c < 6; c++) s->cls_draws[c] = (float) (cls_d[c] / n);
    for (int c = 0; c < 5; c++) s->counts[c] = (float) (cnt[c] / n);
}

/* ---- files ---- */

static void report_path(char* out, int size, const char* ext)
{
    snprintf(out, size, BENCH_DIR "/bench-%03d.%s", report_num, ext);
}

static void write_csv_rows(void)
{
    char path[96];
    FILE* f;
    report_path(path, sizeof(path), "csv");
    f = fopen(path, nresults == 0 ? "w" : "a");
    if (!f) return;
    if (nresults == 0)
        fprintf(f, "stage,frame,frame_ms,ticks,logic_ms,draw_ms,wait_ms,submit_wait_ms,audio_ms,"
                   "fighters_ms,items_ms,stage_ms,effects_ms,hud_ms,other_ms,"
                   "gpu_draws,cpu_draws,dl_builds,dl_rebuilds,dl_fallbacks,render_thread_ms,gpu_ms\n");
    for (int i = 0; i < nrows; i++) {
        const Row* r = &rows[i];
        u32 draw = r->render_us > r->wait_us ? r->render_us - r->wait_us : 0;
        fprintf(f, "%s,%d,%.2f,%u,%.2f,%.2f,%.2f,%.2f,%.2f", stages[stage_i].name, i,
                r->dt_us / 1000.0, (unsigned) r->ticks, r->update_us / 1000.0,
                draw / 1000.0, r->wait_us / 1000.0, r->drain_us / 1000.0, r->audio_us / 1000.0);
        for (int c = 0; c < 6; c++) fprintf(f, ",%.2f", r->cls_us[c] / 1000.0);
        for (int c = 0; c < 5; c++) fprintf(f, ",%u", (unsigned) r->counts[c]);
        fprintf(f, ",%.2f,%.2f\n", r->rt_us / 1000.0, r->gpu_us / 1000.0);
    }
    fclose(f);
}

// rewritten after every stage so a crash still leaves the stages so far
static void write_summary(void)
{
    char path[96];
    FILE* f;
    report_path(path, sizeof(path), "txt");
    f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "Melee Vita benchmark %03d%s\n\n", report_num,
            cancelled ? " (cancelled)" : done ? "" : " (incomplete)");
    fprintf(f, "Settings: preset %s, gameplay %s, boost %s, particles %d, "
               "live screens every %d, vsync %s\n",
            gfx_settings_preset_name(g_gfx_preset),
            g_gfx.game_res == 0 ? "100%" : g_gfx.game_res == 1 ? "75%" : g_gfx.game_res == 2 ? "60%" : "50%",
            g_gfx.boost ? "on" : "off", g_gfx.particles, g_gfx.live_screens,
            g_gfx.vsync ? "on" : "off");
    fprintf(f, "Clocks: CPU %d MHz, bus %d MHz, GPU %d MHz, crossbar %d MHz\n",
            scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
            scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    fprintf(f, "Match: %s (all CPU level 9), items off. Frame rate setting: %s.\n",
            players == 2 ? "Fox vs Falco" : "Fox, Falco, Marth, Captain Falcon",
            g_gfx.frame_rate == FRAME_RATE_30 ? "30" : g_gfx.heavy30 ? "60, 30 in big matches" : "60");
    fprintf(f, "Each stage: %d s warm-up, then %d s of game time measured.\n\n",
            WARMUP_TICKS / 60, MEASURE_TICKS / 60);

    fprintf(f, "%-19s %7s %7s %8s %7s %7s %8s %8s %9s %7s %7s %7s %6s\n", "stage", "avg fps",
            "1% low", "0.1% low", "min fps", "avg ms", "stdev ms", "worst ms", "jitter ms",
            ">17.5ms", ">25ms", ">34ms", "speed");
    for (int i = 0; i < nresults; i++) {
        const Stats* s = &results[i];
        fprintf(f, "%-19s %7.1f %7.1f %8.1f %7.1f %7.2f %8.2f %8.1f %9.2f %6.1f%% %6.1f%% %6.1f%% %5.1f%%\n",
                stages[i].name, s->avg_fps, s->low1_fps, s->low01_fps, s->min_fps, s->avg_ms,
                s->stdev_ms, s->worst_ms, s->jitter_ms, s->over17, s->over25, s->over34,
                100.0f * s->game_speed / 60.0f);
    }

    fprintf(f, "\nFrame time histogram (number of frames, ms)\n%-19s", "stage");
    for (int b = 0; b < 6; b++) fprintf(f, " %8s", hist_names[b]);
    fprintf(f, "\n");
    for (int i = 0; i < nresults; i++) {
        fprintf(f, "%-19s", stages[i].name);
        for (int b = 0; b < 6; b++) fprintf(f, " %8d", results[i].hist[b]);
        fprintf(f, "\n");
    }

    fprintf(f, "\nTime per frame (ms)\n%-19s %8s %8s %8s %10s %8s %8s\n", "stage", "logic",
            "draw", "wait", "render thr", "gpu", "gpu max");
    for (int i = 0; i < nresults; i++)
        fprintf(f, "%-19s %8.2f %8.2f %8.2f %10.2f %8.2f %8.2f\n", stages[i].name,
                results[i].update_ms, results[i].render_ms, results[i].wait_ms, results[i].rt_ms,
                results[i].gpu_ms, results[i].gpu_max_ms);

    fprintf(f, "\nScreen pacing (frames counted at the display flip)\n"
               "%-19s %6s %8s %8s %8s %8s %8s %9s\n", "stage", "target", "1 vbl", "2 vbl",
            "3 vbl", "4+ vbl", "dropped", "smooth");
    for (int i = 0; i < nresults; i++) {
        const Stats* s = &results[i];
        fprintf(f, "%-19s %6s %8u %8u %8u %8u %8u %8.1f%%\n", stages[i].name,
                s->target == 2 ? "30" : "60", (unsigned) s->shown[0], (unsigned) s->shown[1],
                (unsigned) s->shown[2], (unsigned) s->shown[3], (unsigned) s->dropped, s->smooth);
    }

    fprintf(f, "\nWhere the draw time goes (ms per frame)\n%-19s %8s %8s", "stage", "audio",
            "submit");
    for (int c = 0; c < 6; c++) fprintf(f, " %8s", cls_names[c]);
    fprintf(f, "\n");
    for (int i = 0; i < nresults; i++) {
        fprintf(f, "%-19s %8.2f %8.2f", stages[i].name, results[i].audio_ms, results[i].drain_ms);
        for (int c = 0; c < 6; c++) fprintf(f, " %8.2f", results[i].cls_ms[c]);
        fprintf(f, "\n");
    }
    fprintf(f, "\nDraw calls per frame by object kind (and us per draw call)\n%-19s", "stage");
    for (int c = 0; c < 6; c++) fprintf(f, " %14s", cls_names[c]);
    fprintf(f, "\n");
    for (int i = 0; i < nresults; i++) {
        fprintf(f, "%-19s", stages[i].name);
        for (int c = 0; c < 6; c++) {
            const float d = results[i].cls_draws[c];
            fprintf(f, " %6.1f (%5.1f)", d, d > 0.5f ? 1000.0f * results[i].cls_ms[c] / d : 0.0f);
        }
        fprintf(f, "\n");
    }

    fprintf(f, "\nMost expensive stage parts (part #: ms per frame / draw calls)\n");
    for (int i = 0; i < nresults; i++) {
        fprintf(f, "%-19s", stages[i].name);
        for (int k = 0; k < 6; k++)
            if (top_parts[i][k].part >= 0)
                fprintf(f, "  #%d: %.2f/%.0f", top_parts[i][k].part, top_parts[i][k].ms,
                        top_parts[i][k].draws);
        fprintf(f, "\n");
    }

    fprintf(f, "\nGX work per frame\n%-19s", "stage");
    for (int c = 0; c < 5; c++) fprintf(f, " %16s", count_names[c]);
    fprintf(f, "\n");
    for (int i = 0; i < nresults; i++) {
        fprintf(f, "%-19s", stages[i].name);
        for (int c = 0; c < 5; c++) fprintf(f, " %16.1f", results[i].counts[c]);
        fprintf(f, "\n");
    }

    fprintf(f, "\nNotes:\n"
               "- 1%% / 0.1%% low: average fps of the slowest 1%% / 0.1%% of frames.\n"
               "- jitter: average change in frame time from one frame to the next.\n"
               "  High jitter feels choppy even when the average fps is fine.\n"
               "- speed: game speed, 100%% means the game logic kept up with real time.\n"
               "- wait: time the game thread waited for the render thread (GPU side)\n"
               "  to finish the previous frame. High wait means GPU-bound, high logic\n"
               "  or draw means CPU-bound.\n"
               "- screen pacing: how many refreshes (vbl, 16.7 ms each) every frame\n"
               "  actually stayed on screen. smooth = %% of frames that stayed up\n"
               "  exactly as long as they should (1 at 60 fps, 2 at 30). Dropped\n"
               "  frames were replaced before they were shown (v-sync off).\n"
               "- render thr: render thread time recording GPU commands for a frame.\n"
               "- gpu: from the render thread starting a frame until the GPU has\n"
               "  finished it. Below 16.7 ms (60 fps) or 33.3 ms (30 fps) there is\n"
               "  room for a higher resolution. Exact at 60 fps with v-sync off; with\n"
               "  v-sync or the 30 fps lock it also counts time waiting to be shown.\n"
               "- audio: mixing sound on the game thread (inside draw).\n"
               "- submit: waiting for the GX submit worker thread at the end of\n"
               "  the frame (inside draw).\n"
               "- fighters/items/...: time drawing each kind of object (inside\n"
               "  draw). Timing them adds about 1.5 us per object.\n"
               "- Per-frame data is in bench-%03d.csv.\n",
            report_num);
    fclose(f);
}

/* ---- request / setup ---- */

void melee_vita_bench_request(int num_players)
{
    FILE* f;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/melee", 0777);
    sceIoMkdir(BENCH_DIR, 0777);
    f = fopen(BENCH_REQUEST, "w");
    if (f) {
        fprintf(f, "players=%d\n", num_players == 4 ? 4 : 2);
        fclose(f);
    }
}

void melee_vita_bench_init(void)
{
    FILE* f = fopen(BENCH_REQUEST, "r");
    char path[96];
    int n = 0;
    if (!f) return;
    if (fscanf(f, " players = %d", &n) == 1 && n == 4) players = 4;
    fclose(f);
    remove(BENCH_REQUEST); // one run per request, a crash won't loop

    // next free report number
    for (report_num = 1; report_num < 1000; report_num++) {
        report_path(path, sizeof(path), "txt");
        f = fopen(path, "r");
        if (!f) break;
        fclose(f);
    }
    active = 1;
    stage_i = 0;
    melee_vita_log_info("[BENCH] starting benchmark %03d", report_num);
}

int melee_vita_bench_active(void)
{
    return active && !done;
}

int melee_vita_bench_stage(void)
{
    return stages[stage_i < STAGE_COUNT ? stage_i : 0].kind;
}

int melee_vita_bench_players(void)
{
    return players;
}

void melee_vita_bench_flip(u32 vcount, u32 target)
{
    u32 held;
    if (!recording) {
        flip_last = 0;
        return;
    }
    flip_target = target;
    if (flip_last != 0) {
        // the previous frame was on screen from its vblank until this one's
        held = vcount - flip_last;
        if (held == 0) flip_dropped++; // replaced in the same refresh, never shown
        else flip_shown[held > 4 ? 3 : held - 1]++;
    }
    flip_last = vcount;
}

void melee_vita_bench_stage_part(int part, u64 us, u32 draws)
{
    if (!recording || part < 0 || part >= MAX_PARTS) return;
    part_us[part] += us;
    part_draws[part] += draws;
}

void melee_vita_bench_gobj(unsigned int classifier, u64 us, u32 draws)
{
    cur_cls_us[cls_bucket(classifier)] += (u32) us;
    cur_cls_draws[cls_bucket(classifier)] += draws;
}

u32 melee_vita_bench_seed(void)
{
    return 0x4D454C45u + (u32) stage_i; // same match every run
}

/* ---- recording ---- */

static void finish_stage(void)
{
    Stats* s = &results[nresults];
    u32 total = 0;
    compute_stats(rows, nrows, s);
    for (int i = 0; i < 4; i++) {
        s->shown[i] = flip_shown[i];
        total += flip_shown[i];
        flip_shown[i] = 0;
    }
    // the 6 most expensive stage parts
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int p = 0; p < MAX_PARTS; p++)
            if (part_us[p] && (best < 0 || part_us[p] > part_us[best])) best = p;
        top_parts[nresults][k].part = best;
        if (best >= 0 && nrows > 0) {
            top_parts[nresults][k].ms = part_us[best] / 1000.0f / nrows;
            top_parts[nresults][k].draws = (float) part_draws[best] / nrows;
            part_us[best] = 0; // so the next pick is a different part
        }
    }
    memset(part_us, 0, sizeof(part_us));
    memset(part_draws, 0, sizeof(part_draws));
    s->dropped = flip_dropped;
    s->target = (int) flip_target;
    s->smooth = total ? 100.0f * s->shown[s->target == 2 ? 1 : 0] / total : 0.0f;
    flip_dropped = 0;
    flip_last = 0;
    write_csv_rows();
    nresults++;
    melee_vita_log_info("[BENCH] %s: %.1f fps avg, %.1f 1%% low, %.1f ms worst",
                        stages[stage_i].name, results[nresults - 1].avg_fps,
                        results[nresults - 1].low1_fps, results[nresults - 1].worst_ms);
}

int melee_vita_bench_match_tick(u32 frame, int cancel)
{
    if (!melee_vita_bench_active()) return 0;
    cur_frame = frame;

    if (cancel) {
        // only if this stage got past its warm-up (rows still hold the
        // previous stage otherwise)
        int was_recording = recording;
        recording = 0;
        g_melee_vita_bench_prof = 0;
        if (was_recording && nrows > 60) finish_stage();
        cancelled = 1;
        done = 1;
        done_at = sceKernelGetProcessTimeWide();
        write_summary();
        return 1;
    }
    if (frame >= WARMUP_TICKS && !warmed_up) {
        nrows = 0;
        last_present = 0;
        recording = 1;
        warmed_up = 1;
        g_melee_vita_bench_prof = 1;
    }
    if (recording && frame >= WARMUP_TICKS + MEASURE_TICKS) {
        recording = 0;
        warmed_up = 0;
        g_melee_vita_bench_prof = 0;
        finish_stage();
        stage_i++;
        if (stage_i >= STAGE_COUNT) {
            done = 1;
            done_at = sceKernelGetProcessTimeWide();
        }
        write_summary();
        return 1;
    }
    return 0;
}

void melee_vita_bench_frame(u32 sync_wait_us, u32 drain_us)
{
    u64 now, upd, ren, aud;
    u32 ticks;

    if (!recording) return;
    now = sceKernelGetProcessTimeWide();
    ticks = g_melee_vita_tick_total;
    upd = g_melee_vita_update_total_us;
    ren = g_melee_vita_render_total_us;
    aud = g_melee_vita_audio_mix_total_us;

    if (last_present != 0 && nrows < MAX_ROWS) {
        Row* r = &rows[nrows];
        r->dt_us = (u32) (now - last_present);
        r->ticks = ticks - last_ticks;
        r->update_us = (u32) (upd - last_update);
        r->wait_us = sync_wait_us;
        r->drain_us = drain_us;
        r->rt_us = g_melee_vita_rt_busy_us;
        r->gpu_us = g_melee_vita_gpu_done_us;
        r->audio_us = (u32) (aud - last_audio);
        memcpy(r->cls_us, cur_cls_us, sizeof(cur_cls_us));
        memcpy(r->cls_draws, cur_cls_draws, sizeof(cur_cls_draws));
        for (int c = 0; c < 5; c++) r->counts[c] = g_melee_vita_gx_counts[c] - last_counts[c];
        r->render_us = 0;
        // the render total only goes up after present returns, so it
        // belongs to the previous frame
        if (nrows > 0) rows[nrows - 1].render_us = (u32) (ren - last_render);
        nrows++;
    }
    last_present = now;
    last_ticks = ticks;
    last_update = upd;
    last_render = ren;
    last_audio = aud;
    memcpy(last_counts, g_melee_vita_gx_counts, sizeof(last_counts));
    memset(cur_cls_us, 0, sizeof(cur_cls_us));
    memset(cur_cls_draws, 0, sizeof(cur_cls_draws));
}

/* ---- overlay ---- */

int melee_vita_bench_wants_overlay(void)
{
    return active;
}

static const char* res_name(int r)
{
    return r == 0 ? "100%" : r == 1 ? "75%" : r == 2 ? "60%" : "50%";
}

static void draw_results(vita2d_pgf* font)
{
    // table 1: stage, avg, 1% low, worst, jitter, logic, draw, wait, speed
    static const int cols[] = { 40, 230, 315, 400, 490, 575, 660, 745, 840 };
    static const char* heads[] = { "stage", "avg fps", "1% low", "worst ms", "jitter",
                                   "logic ms", "draw ms", "fps lock", "smooth" };
    // table 2: where the draw ms go
    static const int cols2[] = { 40, 230, 315, 400, 490, 575, 660, 745, 840 };
    static const char* heads2[] = { "draw ms ->", "audio", "gpu ms", "fighters", "gpu max",
                                    "stage", "effects", "hud", "draws" };
    char text[160];
    int y;
    u32 white = RGBA8(240, 240, 240, 255), grey = RGBA8(170, 170, 170, 255);

    vita2d_draw_rectangle(20, 20, 920, 504, RGBA8(0, 0, 0, 225));
    snprintf(text, sizeof(text), "Benchmark %03d %s", report_num, cancelled ? "(cancelled)" : "done");
    vita2d_pgf_draw_text(font, 40, 52, RGBA8(255, 210, 60, 255), 1.0f, text);
    // what was tested, so a photo of this screen is enough
    snprintf(text, sizeof(text), "%s  %s  %s fps   gameplay %s   boost %s   particles %d   live %d   vsync %s   CPU %d GPU %d",
             gfx_settings_preset_name(g_gfx_preset),
             players == 2 ? "1v1" : "4 players",
             g_gfx.frame_rate == FRAME_RATE_30 ? "30" : g_gfx.heavy30 ? "60/30" : "60",
             res_name(g_gfx.game_res), g_gfx.boost ? "on" : "off",
             g_gfx.particles, g_gfx.live_screens, g_gfx.vsync ? "on" : "off",
             scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency());
    vita2d_pgf_draw_text(font, 40, 76, grey, 0.7f, text);

    y = 106;
    for (int c = 0; c < 9; c++) vita2d_pgf_draw_text(font, cols[c], y, grey, 0.75f, heads[c]);
    for (int i = 0; i < nresults; i++) {
        const Stats* s = &results[i];
        float v[8] = { s->avg_fps, s->low1_fps, s->worst_ms, s->jitter_ms,
                       s->update_ms, s->render_ms, s->target == 2 ? 30.0f : 60.0f, s->smooth };
        y += 25;
        vita2d_pgf_draw_text(font, cols[0], y, white, 0.8f, stages[i].name);
        for (int c = 0; c < 8; c++) {
            snprintf(text, sizeof(text), c == 7 ? "%.0f%%" : c == 6 ? "%.0f" : "%.1f", v[c]);
            vita2d_pgf_draw_text(font, cols[c + 1], y, white, 0.8f, text);
        }
    }

    y = 284;
    for (int c = 0; c < 9; c++) vita2d_pgf_draw_text(font, cols2[c], y, grey, 0.75f, heads2[c]);
    for (int i = 0; i < nresults; i++) {
        const Stats* s = &results[i];
        y += 25;
        vita2d_pgf_draw_text(font, cols2[0], y, white, 0.8f, stages[i].name);
        snprintf(text, sizeof(text), "%.1f", s->audio_ms);
        vita2d_pgf_draw_text(font, cols2[1], y, white, 0.8f, text);
        snprintf(text, sizeof(text), "%.1f", s->gpu_ms);
        vita2d_pgf_draw_text(font, cols2[2], y, white, 0.8f, text);
        for (int c = 0; c < 5; c++) {
            // items (always ~0) gives its column to the worst gpu time
            snprintf(text, sizeof(text), "%.1f", c == 1 ? s->gpu_max_ms : s->cls_ms[c]);
            vita2d_pgf_draw_text(font, cols2[c + 3], y, white, 0.8f, text);
        }
        // draw calls per frame (gpu + cpu)
        snprintf(text, sizeof(text), "%.0f", s->counts[0] + s->counts[1]);
        vita2d_pgf_draw_text(font, cols2[8], y, white, 0.8f, text);
    }
    vita2d_pgf_draw_text(font, 40, 455, grey, 0.7f,
                         "smooth = frames shown exactly as long as they should; gpu = render start to GPU done (headroom vs 16.7 ms)");
    snprintf(text, sizeof(text), "Report: " BENCH_DIR "/bench-%03d.txt    Square: stage parts    Other: close",
             report_num);
    vita2d_pgf_draw_text(font, 40, 490, grey, 0.75f, text);
}

// second results page: which parts of each stage cost the most to draw
static void draw_parts(vita2d_pgf* font)
{
    char text[160];
    u32 white = RGBA8(240, 240, 240, 255), grey = RGBA8(170, 170, 170, 255);
    int y = 110;

    vita2d_draw_rectangle(20, 20, 920, 504, RGBA8(0, 0, 0, 225));
    snprintf(text, sizeof(text), "Benchmark %03d: most expensive stage parts", report_num);
    vita2d_pgf_draw_text(font, 40, 52, RGBA8(255, 210, 60, 255), 1.0f, text);
    vita2d_pgf_draw_text(font, 40, 80, grey, 0.75f,
                         "part #: ms per frame / draw calls      (stage parts = the stage's own models)");
    for (int i = 0; i < nresults; i++) {
        int n = 0;
        vita2d_pgf_draw_text(font, 40, y, white, 0.85f, stages[i].name);
        text[0] = 0;
        for (int k = 0; k < 6; k++) {
            if (top_parts[i][k].part < 0) continue;
            n += snprintf(text + n, sizeof(text) - n, "#%d: %.2f / %.0f    ", top_parts[i][k].part,
                          top_parts[i][k].ms, top_parts[i][k].draws);
        }
        vita2d_pgf_draw_text(font, 60, y + 24, white, 0.8f, text);
        y += 58;
    }
    vita2d_pgf_draw_text(font, 40, 490, grey, 0.75f, "Square: back to the results    Other buttons: close");
}

void melee_vita_bench_draw(void)
{
    vita2d_pgf* font = gfx_font();
    char text[128];

    if (!active || !font) return;

    if (done) {
        static u32 old_buttons;
        static int parts_page;
        SceCtrlData pad;
        u32 pressed;
        if (parts_page) draw_parts(font);
        else draw_results(font);
        // Square flips to the stage parts page; anything else closes, but
        // not before the results were up for 3 s
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~old_buttons;
        old_buttons = pad.buttons;
        if (pressed & SCE_CTRL_SQUARE) parts_page = !parts_page;
        else if (pressed && sceKernelGetProcessTimeWide() - done_at > 3000000u)
            active = 0;
        return;
    }

    if (recording) {
        int left = (int) (WARMUP_TICKS + MEASURE_TICKS - (int) cur_frame) / 60;
        snprintf(text, sizeof(text), "BENCHMARK %d/%d  %s  %d s left   START: cancel", stage_i + 1,
                 STAGE_COUNT, stages[stage_i].name, left < 0 ? 0 : left);
    } else {
        snprintf(text, sizeof(text), "BENCHMARK %d/%d  %s  warming up   START: cancel",
                 stage_i + 1, STAGE_COUNT, stages[stage_i].name);
    }
    vita2d_draw_rectangle(4.0f, 516.0f, (float) vita2d_pgf_text_width(font, 0.8f, text) + 12.0f,
                          24.0f, RGBA8(0, 0, 0, 160));
    vita2d_pgf_draw_text(font, 10, 534, RGBA8(255, 210, 60, 255), 0.8f, text);
}
