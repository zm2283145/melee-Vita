#ifndef MELEE_VITA_PROFILER_LIVE_H
#define MELEE_VITA_PROFILER_LIVE_H

#include <stdint.h>

#ifdef MELEE_VITA_PROFILER
int melee_vita_profiler_start(void);
void melee_vita_profiler_stop(void);
void melee_vita_profiler_record_duration(unsigned int zone,
                                         uint64_t elapsed_us);
void melee_vita_profiler_mark_frame(unsigned int draw_calls,
                                    unsigned int triangles);
void melee_vita_profiler_fragment_metrics(
    unsigned int stages, unsigned int texture_samples,
    unsigned int zero_alpha_discard_draws,
    unsigned int blended_draws, unsigned int alpha_test_draws,
    unsigned int depth_texture_draws);
void melee_vita_profiler_queue_metrics(
    unsigned int command_count, unsigned int command_bytes,
    unsigned int gpu_arena_bytes, unsigned int copy_passes,
    unsigned int scene_begins);
#else
static inline int melee_vita_profiler_start(void) { return 0; }
static inline void melee_vita_profiler_stop(void) {}
static inline void melee_vita_profiler_record_duration(
    unsigned int zone, uint64_t elapsed_us)
{
    (void) zone;
    (void) elapsed_us;
}
static inline void melee_vita_profiler_mark_frame(unsigned int draw_calls,
                                                  unsigned int triangles)
{
    (void) draw_calls;
    (void) triangles;
}
static inline void melee_vita_profiler_fragment_metrics(
    unsigned int stages, unsigned int texture_samples,
    unsigned int zero_alpha_discard_draws,
    unsigned int blended_draws, unsigned int alpha_test_draws,
    unsigned int depth_texture_draws)
{
    (void) stages;
    (void) texture_samples;
    (void) zero_alpha_discard_draws;
    (void) blended_draws;
    (void) alpha_test_draws;
    (void) depth_texture_draws;
}
static inline void melee_vita_profiler_queue_metrics(
    unsigned int command_count, unsigned int command_bytes,
    unsigned int gpu_arena_bytes, unsigned int copy_passes,
    unsigned int scene_begins)
{
    (void) command_count;
    (void) command_bytes;
    (void) gpu_arena_bytes;
    (void) copy_passes;
    (void) scene_begins;
}
#endif

#endif
