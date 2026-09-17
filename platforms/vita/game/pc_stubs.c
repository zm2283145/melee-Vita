/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita stand-ins for PC-port-only features pulled in from upstream (PC
 * preferences, OGG music streaming, the host file cache and widescreen HUD).
 * The Vita build defines TARGET_PC for the shared code, so these hooks are
 * compiled in; each one reports "feature off" so the vanilla path runs. */
#include <stdbool.h>
#include <stddef.h>

bool pc_is_free_camera_enabled(void) { return false; }
bool pc_is_unlock_all_enabled(void) { return false; }
bool pc_is_frozen_stadium_enabled(void) { return false; }
int pc_get_hud_mode(void) { return 0; }

float pc_widescreen_hud_player_x(int player_idx, int total_players,
                                 float original_x)
{
    (void) player_idx;
    (void) total_players;
    return original_x;
}
float pc_widescreen_hud_timer_x(float original_x) { return original_x; }

bool pc_music_stream_open(const char* track_stem)
{
    (void) track_stem;
    return false;
}
void pc_music_stream_stop(void) {}
void pc_music_stream_set_volume(float vol) { (void) vol; }
bool pc_music_stream_is_playing(void) { return false; }

bool pc_file_cache_get(const char* filename, void* dst, size_t* size)
{
    (void) filename;
    (void) dst;
    (void) size;
    return false;
}
bool pc_file_cache_get_size(const char* filename, size_t* size)
{
    (void) filename;
    (void) size;
    return false;
}
void pc_file_cache_put(const char* filename, const void* data, size_t size)
{
    (void) filename;
    (void) data;
    (void) size;
}
void pc_file_cache_start_prewarm(void) {}