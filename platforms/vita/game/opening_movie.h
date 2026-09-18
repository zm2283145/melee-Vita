#ifndef MELEE_VITA_OPENING_MOVIE_H
#define MELEE_VITA_OPENING_MOVIE_H

#include <stdbool.h>
#include <stdint.h>

struct melee_vita_opening_movie;

enum melee_vita_opening_movie_result {
    MELEE_VITA_OPENING_MOVIE_PLAYING = 0,
    MELEE_VITA_OPENING_MOVIE_FINISHED,
    MELEE_VITA_OPENING_MOVIE_SKIPPED,
    MELEE_VITA_OPENING_MOVIE_FAILED,
};

struct melee_vita_opening_movie* melee_vita_opening_movie_load(void);
void melee_vita_opening_movie_start(struct melee_vita_opening_movie* movie);
enum melee_vita_opening_movie_result melee_vita_opening_movie_update(
    struct melee_vita_opening_movie* movie);
void melee_vita_opening_movie_draw_active(void);
uint32_t melee_vita_opening_movie_elapsed_ticks(
    const struct melee_vita_opening_movie* movie);
void melee_vita_opening_movie_preserve_audio(
    struct melee_vita_opening_movie* movie);
void melee_vita_opening_movie_stop_preserved_audio(void);
void melee_vita_opening_movie_free(struct melee_vita_opening_movie* movie);
bool melee_vita_opening_movie_ready(
    const struct melee_vita_opening_movie* movie);

#endif
