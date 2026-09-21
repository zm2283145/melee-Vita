#ifndef MELEE_VITA_OPENING_AUDIO_H
#define MELEE_VITA_OPENING_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

struct melee_vita_opening_audio;

/* Loads an HPS stream from the user's disc image. Playback starts only after
 * start() and is mixed through vita-port's existing AX output thread, avoiding
 * a competing native BGM port. */
struct melee_vita_opening_audio* melee_vita_opening_audio_load(void);
struct melee_vita_opening_audio* melee_vita_opening_audio_load_file(
    const char* filename);
void melee_vita_opening_audio_start(
    struct melee_vita_opening_audio* audio);
void melee_vita_opening_audio_stop(
    struct melee_vita_opening_audio* audio);
void melee_vita_opening_audio_free(
    struct melee_vita_opening_audio* audio);
void melee_vita_opening_audio_handoff(
    struct melee_vita_opening_audio* audio);
void melee_vita_opening_audio_poll(void);
void melee_vita_opening_audio_cancel_handoff(void);
void melee_vita_opening_audio_shutdown(void);
void melee_vita_opening_audio_mix(int16_t* output, uint32_t frames);

bool melee_vita_opening_audio_ready(
    const struct melee_vita_opening_audio* audio);
bool melee_vita_opening_audio_finished(
    const struct melee_vita_opening_audio* audio);
uint64_t melee_vita_opening_audio_played_frames(
    const struct melee_vita_opening_audio* audio);

#endif
