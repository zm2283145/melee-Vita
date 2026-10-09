/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita features with no PS5 implementation yet.  Callers already handle
 * these being unavailable (they fall back to the portable paths). */
#include "jpeg_hw.h"
#include "opening_movie.h"

#include <stddef.h>

int melee_vita_jpeg_hw_last_error;

struct melee_vita_opening_jpeg_hw* melee_vita_opening_jpeg_hw_create(
    const void* probe_jpeg, size_t probe_jpeg_size, size_t maximum_jpeg_size,
    bool half_scale, struct melee_vita_opening_jpeg_hw_info* info_out,
    struct melee_vita_opening_jpeg_hw_error* error_out)
{
    (void) probe_jpeg; (void) probe_jpeg_size; (void) maximum_jpeg_size; (void) half_scale;
    (void) info_out;
    if (error_out != NULL) {
        error_out->stage = MELEE_VITA_OPENING_JPEG_HW_STAGE_INITIALIZE;
        error_out->code = -1;
    }
    return NULL;
}

int melee_vita_opening_jpeg_hw_decode(struct melee_vita_opening_jpeg_hw* decoder,
                                      const void* standard_jpeg, size_t standard_jpeg_size,
                                      vita2d_texture* target,
                                      struct melee_vita_opening_jpeg_hw_timing* timing_out,
                                      struct melee_vita_opening_jpeg_hw_error* error_out)
{
    (void) decoder; (void) standard_jpeg; (void) standard_jpeg_size; (void) target;
    (void) timing_out; (void) error_out;
    return -1;
}

int melee_vita_jpeg_hw_decode_planes(struct melee_vita_opening_jpeg_hw* decoder,
                                     const void* standard_jpeg, size_t standard_jpeg_size,
                                     const unsigned char** planes_out,
                                     unsigned int* pitch_width_out,
                                     unsigned int* pitch_height_out)
{
    (void) decoder; (void) standard_jpeg; (void) standard_jpeg_size; (void) planes_out;
    (void) pitch_width_out; (void) pitch_height_out;
    return 0;
}

void melee_vita_opening_jpeg_hw_destroy(struct melee_vita_opening_jpeg_hw* decoder)
{
    (void) decoder;
}

/* The Vita plays a pre-converted intro; PS5 uses the game's own THP. */
struct melee_vita_opening_movie* melee_vita_opening_movie_load(void) { return NULL; }
struct melee_vita_opening_movie* melee_vita_opening_movie_load_asset(
    const char* movie_filename, const char* audio_filename, const uint32_t* rate_table,
    bool force_full_width)
{
    (void) movie_filename; (void) audio_filename; (void) rate_table; (void) force_full_width;
    return NULL;
}
void melee_vita_opening_movie_start(struct melee_vita_opening_movie* movie) { (void) movie; }
enum melee_vita_opening_movie_result melee_vita_opening_movie_update(
    struct melee_vita_opening_movie* movie)
{
    (void) movie;
    return MELEE_VITA_OPENING_MOVIE_FAILED;
}
void melee_vita_opening_movie_draw_active(void) {}
void melee_vita_opening_movie_hide_active(void) {}
bool melee_vita_opening_movie_visible(const struct melee_vita_opening_movie* movie)
{
    (void) movie;
    return false;
}
uint32_t melee_vita_opening_movie_total_ticks(const struct melee_vita_opening_movie* movie)
{
    (void) movie;
    return 0;
}
uint32_t melee_vita_opening_movie_elapsed_ticks(const struct melee_vita_opening_movie* movie)
{
    (void) movie;
    return 0;
}
void melee_vita_opening_movie_preserve_audio(struct melee_vita_opening_movie* movie)
{
    (void) movie;
}
void melee_vita_opening_movie_stop_preserved_audio(void) {}
void melee_vita_opening_movie_free(struct melee_vita_opening_movie* movie) { (void) movie; }
bool melee_vita_opening_movie_ready(const struct melee_vita_opening_movie* movie)
{
    (void) movie;
    return false;
}

/* Some shared sources declare the Vita live profiler hooks directly. */
void melee_vita_profiler_record_duration(unsigned int zone, uint64_t elapsed_us)
{
    (void) zone;
    (void) elapsed_us;
}
