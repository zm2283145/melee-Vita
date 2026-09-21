#include "opening_movie.h"
#include "opening_audio.h"
#include "opening_timeline.h"
#include "gxm_game.h"
#include "jpeg_hw.h"

#include "../disc_probe.h"
#include "../vita_log.h"

#include <psp2/io/fcntl.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/gxm.h>
#include <turbojpeg.h>
#include <vita2d.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Native MTH movie playback.
 *
 * The pipeline mirrors lbmthp.c's split between DVD streaming, decoding and
 * presentation, but uses three Vita threads so that no stage can stall
 * another:
 *
 *   reader thread   sequential, large-chunk ISO reads -> compressed packet ring
 *   decode thread   packet ring -> hardware (or TurboJPEG) decode -> texture pool
 *   main thread     60 Hz rate-table clock -> newest due texture -> vita2d
 *
 * Measurements on retail hardware (see git history) showed the previous
 * single-worker design spent ~340 ms per synchronous 1 MiB refill on the
 * decode thread and discarded late-but-complete frames, which produced visible
 * freezes followed by fast catch-up. A read-ahead ring removes the I/O stalls.
 */

#define DISC_HEADER_FST_OFFSET 0x424u
#define MTH_HEADER_SIZE 0x40u
#define MTH_MAX_COMPRESSED_SIZE (4u * 1024u * 1024u)
/* The MTH header's maximum is 0x20 bytes short for MvOpen.mth. */
#define MTH_CAPACITY_SLACK 4096u
#define PACKET_SLOTS 64u              /* ~2 s of 30 fps video read ahead */
#define READ_CHUNK_SIZE (512u * 1024u)
#define TEXTURE_COUNT 4
#define OPENING_AUDIO_RATE 32000u
#define READER_THREAD_PRIORITY (0x10000100 - 10)
#define DECODE_THREAD_PRIORITY 0x10000100
#define READER_THREAD_STACK (64u * 1024u)
#define DECODE_THREAD_STACK (128u * 1024u)
#define STATS_INTERVAL_TICKS 600u
/* Self-test tolerance between hardware and software decodes (0..255). */
#define HW_SELFTEST_MAX_MEAN_DIFF 12u

enum texture_state {
    TEXTURE_FREE = 0,
    TEXTURE_DECODING,
    TEXTURE_READY,
    TEXTURE_SHOWN,
    TEXTURE_RETIRED,
};

struct movie_packet {
    uint32_t frame;
    uint32_t size;
    unsigned char* data;
};

struct movie_texture {
    vita2d_texture* texture;
    volatile int state;
    volatile uint32_t frame;
    uint32_t width;
    uint32_t height;
};

struct melee_vita_opening_movie {
    /* File layout. */
    const char* movie_filename;
    const uint32_t* rate_table;
    bool force_full_width;
    uint32_t file_offset;
    uint32_t file_size;
    uint32_t header_maximum_frame_size;
    uint32_t packet_capacity;
    uint32_t width;
    uint32_t height;
    uint32_t frame_rate;
    uint32_t frame_count;
    uint32_t first_frame_offset;
    uint32_t first_frame_size;

    /* Reader thread state. */
    SceUID disc_fd;
    unsigned char* chunk;
    uint64_t chunk_disc_offset;
    uint32_t chunk_size;
    unsigned char* packet_memory;
    struct movie_packet packets[PACKET_SLOTS];
    volatile uint32_t packet_write;
    volatile uint32_t packet_read;
    volatile int reader_done;
    uint64_t read_us;
    uint64_t read_bytes;
    uint32_t read_chunks;
    uint32_t reader_waits;

    /* Decode thread state. */
    struct melee_vita_opening_jpeg_hw* hw;
    volatile int use_hw;
    int hw_selftest_done;
    unsigned char* standard_jpeg;
    size_t standard_jpeg_capacity;
    unsigned char* selftest_rgba;
    volatile int decoder_done;
    volatile uint32_t decoded_frames;
    volatile uint32_t dropped_frames;
    uint64_t bridge_us;
    uint64_t decode_us;
    uint32_t decode_warnings;

    /* Shared. */
    struct movie_texture textures[TEXTURE_COUNT];
    struct melee_vita_opening_audio* audio;
    SceUID reader_thread;
    SceUID decode_thread;
    volatile int running;
    volatile int fatal_error;
    volatile uint32_t clock_frame;
    volatile int clock_running;

    /* Main thread state. */
    int current_texture;
    uint32_t current_frame;
    uint32_t presented_frames;
    uint32_t late_presents;
    uint32_t previous_buttons;
    uint64_t start_time;
    uint64_t last_present_time;
    uint64_t next_stats_tick;
    uint64_t elapsed_ticks;
    bool active;
    bool clock_started;
    bool audio_stopped;
    MeleeVitaOpeningPresentation presentation;
};

static struct melee_vita_opening_movie* s_active_movie;

static uint32_t be32(const unsigned char* p)
{
    return (uint32_t) p[0] << 24u | (uint32_t) p[1] << 16u |
           (uint32_t) p[2] << 8u | p[3];
}

static int read_at(FILE* file, uint32_t offset, void* destination, size_t size)
{
    return fseek(file, (long) offset, SEEK_SET) == 0 &&
           fread(destination, 1, size, file) == size;
}

static int find_disc_file(FILE* file, const char* wanted,
                          uint32_t* offset_out, uint32_t* size_out)
{
    unsigned char header[12];
    if (!read_at(file, DISC_HEADER_FST_OFFSET, header, sizeof(header)))
        return 0;
    const uint32_t fst_offset = be32(header);
    const uint32_t fst_size = be32(header + 4u);
    unsigned char root[12];
    if (fst_size < sizeof(root) ||
        !read_at(file, fst_offset, root, sizeof(root)) ||
        (be32(root) >> 24u) != 1u)
        return 0;
    const uint32_t entries = be32(root + 8u);
    const uint32_t strings = fst_offset + entries * 12u;
    if (entries == 0u || strings >= fst_offset + fst_size) return 0;

    for (uint32_t index = 1u; index < entries; ++index) {
        unsigned char entry[12];
        if (!read_at(file, fst_offset + index * 12u, entry, sizeof(entry)))
            return 0;
        const uint32_t type_name = be32(entry);
        if ((type_name >> 24u) != 0u) continue;
        const uint32_t name_offset = type_name & UINT32_C(0x00ffffff);
        if (strings + name_offset >= fst_offset + fst_size) continue;
        char name[64] = { 0 };
        if (fseek(file, (long) (strings + name_offset), SEEK_SET) != 0)
            return 0;
        for (uint32_t n = 0u; n + 1u < sizeof(name); ++n) {
            const int c = fgetc(file);
            if (c <= 0) break;
            name[n] = (char) c;
        }
        if (strcmp(name, wanted) == 0) {
            *offset_out = be32(entry + 4u);
            *size_out = be32(entry + 8u);
            return 1;
        }
    }
    return 0;
}

/* THP stores an otherwise ordinary baseline JPEG with the entropy-coded scan
 * already unescaped.  A normal JPEG decoder interprets an unescaped 0xff byte
 * in that scan as a marker, so TurboJPEG stops early and returns a mostly
 * neutral frame.  Restore JPEG byte stuffing (0xff -> 0xff 0x00) between the
 * SOS header and the final EOI marker before handing the frame to a decoder.
 *
 * MvOpen.mth does not use JPEG restart intervals.  Reject one if it appears
 * rather than accidentally converting a restart marker into entropy data. */
static int make_standard_jpeg(const unsigned char* source,
                              size_t source_size,
                              unsigned char* destination,
                              size_t destination_capacity,
                              size_t* destination_size)
{
    if (source == NULL || destination == NULL || destination_size == NULL ||
        source_size < 4u || source[0] != 0xffu || source[1] != 0xd8u)
        return 0;

    size_t position = 2u;
    size_t scan_start = 0u;
    int has_restart_interval = 0;
    while (position + 1u < source_size) {
        if (source[position] != 0xffu) return 0;
        while (position < source_size && source[position] == 0xffu)
            ++position;
        if (position >= source_size) return 0;
        const unsigned char marker = source[position++];
        if (marker == 0xd8u) continue;
        if (marker == 0xd9u) return 0;
        if (marker >= 0xd0u && marker <= 0xd7u) continue;
        if (marker == 0x01u) continue;
        if (position + 2u > source_size) return 0;
        const size_t segment_size =
            ((size_t) source[position] << 8u) | source[position + 1u];
        if (segment_size < 2u || segment_size > source_size - position)
            return 0;
        if (marker == 0xddu) has_restart_interval = 1;
        position += segment_size;
        if (marker == 0xdau) {
            scan_start = position;
            break;
        }
    }
    if (scan_start == 0u || has_restart_interval) return 0;

    size_t eoi = source_size;
    for (size_t i = source_size - 1u; i > scan_start; --i) {
        if (source[i - 1u] == 0xffu && source[i] == 0xd9u) {
            eoi = i - 1u;
            break;
        }
    }
    if (eoi == source_size) return 0;

    if (scan_start > destination_capacity) return 0;
    memcpy(destination, source, scan_start);
    size_t output = scan_start;
    for (size_t i = scan_start; i < eoi; ++i) {
        if (output + 2u > destination_capacity) return 0;
        destination[output++] = source[i];
        if (source[i] == 0xffu) destination[output++] = 0x00u;
    }
    if (destination_capacity - output < 2u) return 0;
    destination[output++] = 0xffu;
    destination[output++] = 0xd9u;
    *destination_size = output;
    return 1;
}

/* ---- Reader thread. ---- */

/* Copy [disc_offset, disc_offset + size) out of a large sequential read
 * window.  Playback only moves forward, so the window only ever advances. */
static int stream_copy(struct melee_vita_opening_movie* movie,
                       uint64_t disc_offset, unsigned char* destination,
                       uint32_t size)
{
    while (size != 0u) {
        const uint64_t window_end =
            movie->chunk_disc_offset + movie->chunk_size;
        if (disc_offset < movie->chunk_disc_offset ||
            disc_offset >= window_end) {
            const uint64_t started = sceKernelGetProcessTimeWide();
            if (sceIoLseek(movie->disc_fd, (SceOff) disc_offset,
                           SCE_SEEK_SET) < 0)
                return 0;
            uint32_t wanted = READ_CHUNK_SIZE;
            const uint64_t file_end =
                (uint64_t) movie->file_offset + movie->file_size;
            if (disc_offset + wanted > file_end)
                wanted = (uint32_t) (file_end - disc_offset);
            int got = 0;
            while ((uint32_t) got < wanted) {
                const int result = sceIoRead(movie->disc_fd,
                                             movie->chunk + got,
                                             wanted - (uint32_t) got);
                if (result <= 0) break;
                got += result;
            }
            if (got <= 0) return 0;
            movie->chunk_disc_offset = disc_offset;
            movie->chunk_size = (uint32_t) got;
            movie->read_us += sceKernelGetProcessTimeWide() - started;
            movie->read_bytes += (uint32_t) got;
            ++movie->read_chunks;
            continue;
        }
        const uint32_t relative =
            (uint32_t) (disc_offset - movie->chunk_disc_offset);
        uint32_t available = movie->chunk_size - relative;
        if (available > size) available = size;
        memcpy(destination, movie->chunk + relative, available);
        destination += available;
        disc_offset += available;
        size -= available;
    }
    return 1;
}

static int reader_thread(SceSize argument_size, void* argument)
{
    (void) argument_size;
    struct melee_vita_opening_movie* movie =
        *(struct melee_vita_opening_movie**) argument;

    uint32_t offset = movie->first_frame_offset;
    uint32_t size = movie->first_frame_size;
    for (uint32_t frame = 0u; frame < movie->frame_count; ++frame) {
        while (__atomic_load_n(&movie->running, __ATOMIC_ACQUIRE) != 0 &&
               movie->packet_write -
                       __atomic_load_n(&movie->packet_read,
                                       __ATOMIC_ACQUIRE) >= PACKET_SLOTS) {
            ++movie->reader_waits;
            sceKernelDelayThread(2000u);
        }
        if (__atomic_load_n(&movie->running, __ATOMIC_ACQUIRE) == 0) break;

        if (size < 8u || size > movie->packet_capacity ||
            offset > movie->file_size || size > movie->file_size - offset) {
            melee_vita_log_info(
                "FRONTEND movie reader bad frame=%u offset=%u size=%u",
                frame, offset, size);
            __atomic_store_n(&movie->fatal_error, 1, __ATOMIC_RELEASE);
            break;
        }
        struct movie_packet* packet =
            &movie->packets[movie->packet_write % PACKET_SLOTS];
        if (!stream_copy(movie, (uint64_t) movie->file_offset + offset,
                         packet->data, size)) {
            melee_vita_log_info("FRONTEND movie reader I/O failed frame=%u",
                                frame);
            __atomic_store_n(&movie->fatal_error, 1, __ATOMIC_RELEASE);
            break;
        }
        packet->frame = frame;
        packet->size = size;
        const uint32_t next_size = be32(packet->data);
        offset += size;
        size = next_size;
        __atomic_add_fetch(&movie->packet_write, 1u, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&movie->reader_done, 1, __ATOMIC_RELEASE);
    return 0;
}

/* ---- Decode thread. ---- */

static int decode_software(struct melee_vita_opening_movie* movie,
                           tjhandle jpeg, size_t jpeg_size,
                           unsigned char* output, int pitch,
                           uint32_t* width_out, uint32_t* height_out)
{
    const uint32_t width = movie->width / 2u;
    const uint32_t height = movie->height / 2u;
    const int result = tjDecompress2(
        jpeg, movie->standard_jpeg, (unsigned long) jpeg_size, output,
        (int) width, pitch, (int) height, TJPF_RGBA,
        TJFLAG_FASTUPSAMPLE | TJFLAG_FASTDCT);
    if (result != 0) {
        if (tjGetErrorCode(jpeg) == TJERR_FATAL) {
            melee_vita_log_info("FRONTEND movie fatal JPEG error=%s",
                                tjGetErrorStr2(jpeg));
            return 0;
        }
        if (++movie->decode_warnings == 1u)
            melee_vita_log_info("FRONTEND movie first JPEG warning error=%s",
                                tjGetErrorStr2(jpeg));
    }
    *width_out = width;
    *height_out = height;
    return 1;
}

/* Compare one hardware-decoded full-size frame against TurboJPEG's half-size
 * decode of the same data.  This validates the codec ABI and colour
 * conversion on the real console without anyone looking at the screen. */
static void hw_selftest(struct melee_vita_opening_movie* movie,
                        struct movie_texture* target, tjhandle jpeg,
                        size_t jpeg_size)
{
    const uint32_t half_w = movie->width / 2u;
    const uint32_t half_h = movie->height / 2u;
    if (movie->selftest_rgba == NULL ||
        tjDecompress2(jpeg, movie->standard_jpeg,
                      (unsigned long) jpeg_size, movie->selftest_rgba,
                      (int) half_w, (int) (half_w * 4u), (int) half_h,
                      TJPF_RGBA, TJFLAG_FASTUPSAMPLE) != 0)
        return;

    const unsigned char* hw = vita2d_texture_get_datap(target->texture);
    const uint32_t stride = vita2d_texture_get_stride(target->texture);
    uint64_t diff = 0u;
    uint64_t luma = 0u;
    uint32_t samples = 0u;
    for (uint32_t y = 4u; y + 4u < half_h; y += 8u) {
        for (uint32_t x = 4u; x + 4u < half_w; x += 8u) {
            const unsigned char* s = movie->selftest_rgba + (y * half_w + x) * 4u;
            const unsigned char* h = hw + (y * 2u) * stride + (x * 2u) * 4u;
            for (int c = 0; c < 3; ++c)
                diff += (uint32_t) abs((int) s[c] - (int) h[c]);
            luma += s[1];
            ++samples;
        }
    }
    if (samples == 0u) return;
    /* Pick a frame with real content; black frames prove nothing. */
    if (luma / samples < 24u) return;
    movie->hw_selftest_done = 1;
    const uint32_t mean = (uint32_t) (diff / (samples * 3u));
    melee_vita_log_info("FRONTEND movie hw selftest frame=%u mean_diff=%u luma=%u",
                        target->frame, mean, (uint32_t) (luma / samples));
    if (mean > HW_SELFTEST_MAX_MEAN_DIFF) {
        melee_vita_log_info("FRONTEND movie hw output mismatch; using TurboJPEG");
        __atomic_store_n(&movie->use_hw, 0, __ATOMIC_RELEASE);
    }
}

static struct movie_texture* acquire_texture(
    struct melee_vita_opening_movie* movie)
{
    for (int i = 0; i < TEXTURE_COUNT; ++i) {
        struct movie_texture* texture = &movie->textures[i];
        int expected = TEXTURE_FREE;
        if (__atomic_compare_exchange_n(&texture->state, &expected,
                                        TEXTURE_DECODING, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
            return texture;
    }
    return NULL;
}

static int decode_thread(SceSize argument_size, void* argument)
{
    (void) argument_size;
    struct melee_vita_opening_movie* movie =
        *(struct melee_vita_opening_movie**) argument;
    tjhandle jpeg = tjInitDecompress();
    if (jpeg == NULL) {
        melee_vita_log_info("FRONTEND movie TurboJPEG init failed");
        __atomic_store_n(&movie->fatal_error, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&movie->decoder_done, 1, __ATOMIC_RELEASE);
        return 0;
    }

    while (__atomic_load_n(&movie->running, __ATOMIC_ACQUIRE) != 0) {
        const uint32_t read = movie->packet_read;
        const uint32_t write =
            __atomic_load_n(&movie->packet_write, __ATOMIC_ACQUIRE);
        if (read == write) {
            if (__atomic_load_n(&movie->reader_done, __ATOMIC_ACQUIRE) != 0) {
                __atomic_store_n(&movie->decoder_done, 1, __ATOMIC_RELEASE);
                break;
            }
            sceKernelDelayThread(1000u);
            continue;
        }
        const struct movie_packet* packet =
            &movie->packets[read % PACKET_SLOTS];

        /* Frames are intra-coded, so a frame that is already older than the
         * frame due on screen can be skipped.  Only skip when a newer packet
         * is already buffered, so something always reaches the screen. */
        if (__atomic_load_n(&movie->clock_running, __ATOMIC_ACQUIRE) != 0 &&
            write - read > 1u &&
            packet->frame <
                __atomic_load_n(&movie->clock_frame, __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&movie->dropped_frames, 1u, __ATOMIC_RELEASE);
            __atomic_add_fetch(&movie->packet_read, 1u, __ATOMIC_RELEASE);
            continue;
        }

        struct movie_texture* target = acquire_texture(movie);
        if (target == NULL) {
            sceKernelDelayThread(1000u);
            continue;
        }

        uint64_t started = sceKernelGetProcessTimeWide();
        size_t jpeg_size = 0u;
        if (!make_standard_jpeg(packet->data + 4u, packet->size - 4u,
                                movie->standard_jpeg,
                                movie->standard_jpeg_capacity, &jpeg_size)) {
            melee_vita_log_info(
                "FRONTEND movie THP-JPEG bridge failed frame=%u bytes=%u",
                packet->frame, packet->size - 4u);
            __atomic_store_n(&movie->fatal_error, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&target->state, TEXTURE_FREE, __ATOMIC_RELEASE);
            break;
        }
        movie->bridge_us += sceKernelGetProcessTimeWide() - started;

        started = sceKernelGetProcessTimeWide();
        target->frame = packet->frame;
        int ok = 0;
        if (__atomic_load_n(&movie->use_hw, __ATOMIC_ACQUIRE) != 0) {
            struct melee_vita_opening_jpeg_hw_error error;
            ok = melee_vita_opening_jpeg_hw_decode(
                movie->hw, movie->standard_jpeg, jpeg_size, target->texture,
                NULL, &error);
            if (ok) {
                target->width = movie->width;
                target->height = movie->height;
                if (!movie->hw_selftest_done)
                    hw_selftest(movie, target, jpeg, jpeg_size);
            } else {
                melee_vita_log_info(
                    "FRONTEND movie hw decode failed frame=%u stage=%d "
                    "code=0x%08x; using TurboJPEG",
                    packet->frame, (int) error.stage,
                    (unsigned int) error.code);
                __atomic_store_n(&movie->use_hw, 0, __ATOMIC_RELEASE);
            }
        }
        if (!ok) {
            ok = decode_software(
                movie, jpeg, jpeg_size,
                vita2d_texture_get_datap(target->texture),
                (int) vita2d_texture_get_stride(target->texture),
                &target->width, &target->height);
        }
        movie->decode_us += sceKernelGetProcessTimeWide() - started;
        if (!ok) {
            __atomic_store_n(&movie->fatal_error, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&target->state, TEXTURE_FREE, __ATOMIC_RELEASE);
            break;
        }

        __atomic_add_fetch(&movie->decoded_frames, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&target->state, TEXTURE_READY, __ATOMIC_RELEASE);
        __atomic_add_fetch(&movie->packet_read, 1u, __ATOMIC_RELEASE);
    }
    tjDestroy(jpeg);
    return 0;
}

/* ---- Main thread. ---- */

static void stop_opening_audio(struct melee_vita_opening_movie* movie,
                               const char* reason)
{
    if (movie == NULL || movie->audio == NULL || movie->audio_stopped) return;
    movie->audio_stopped = true;
    const uint64_t played =
        melee_vita_opening_audio_played_frames(movie->audio);
    uint64_t wall_frames = 0u;
    if (movie->clock_started) {
        const uint64_t now = sceKernelGetProcessTimeWide();
        wall_frames = (now - movie->start_time) * OPENING_AUDIO_RATE /
                      UINT64_C(1000000);
    }
    melee_vita_log_info(
        "FRONTEND opening audio stop reason=%s played=%llu wall=%llu "
        "delta=%lld finished=%u",
        reason, (unsigned long long) played, (unsigned long long) wall_frames,
        (long long) played - (long long) wall_frames,
        melee_vita_opening_audio_finished(movie->audio) ? 1u : 0u);
    melee_vita_opening_audio_stop(movie->audio);
}

static void log_stats(struct melee_vita_opening_movie* movie,
                      const char* label, uint32_t desired)
{
    const uint32_t decoded =
        __atomic_load_n(&movie->decoded_frames, __ATOMIC_ACQUIRE);
    melee_vita_log_info(
        "FRONTEND movie %s desired=%u shown=%u presented=%u late=%u "
        "decoded=%u dropped=%u queue=%u hw=%d decode_us=%llu bridge_us=%llu "
        "read_chunks=%u read_ms_per_chunk=%llu reader_waits=%u",
        label, desired, movie->current_frame, movie->presented_frames,
        movie->late_presents, decoded,
        __atomic_load_n(&movie->dropped_frames, __ATOMIC_ACQUIRE),
        __atomic_load_n(&movie->packet_write, __ATOMIC_ACQUIRE) -
            __atomic_load_n(&movie->packet_read, __ATOMIC_ACQUIRE),
        __atomic_load_n(&movie->use_hw, __ATOMIC_ACQUIRE),
        decoded ? (unsigned long long) (movie->decode_us / decoded) : 0u,
        decoded ? (unsigned long long) (movie->bridge_us / decoded) : 0u,
        movie->read_chunks,
        movie->read_chunks
            ? (unsigned long long) (movie->read_us / movie->read_chunks / 1000u)
            : 0u,
        movie->reader_waits);
}

/* Present the newest decoded frame that is due, retire everything older and
 * recycle retired textures once the GPU has finished the previous scene. */
static void present_due_frame(struct melee_vita_opening_movie* movie,
                              uint32_t desired, uint64_t now)
{
    int best = -1;
    for (int i = 0; i < TEXTURE_COUNT; ++i) {
        const struct movie_texture* texture = &movie->textures[i];
        if (__atomic_load_n(&texture->state, __ATOMIC_ACQUIRE) !=
                TEXTURE_READY ||
            texture->frame > desired)
            continue;
        if (best < 0 || texture->frame > movie->textures[best].frame)
            best = i;
    }
    if (best >= 0 &&
        (movie->current_texture < 0 ||
         movie->textures[best].frame > movie->current_frame)) {
        if (movie->current_texture >= 0)
            __atomic_store_n(&movie->textures[movie->current_texture].state,
                             TEXTURE_RETIRED, __ATOMIC_RELEASE);
        movie->current_texture = best;
        movie->current_frame = movie->textures[best].frame;
        __atomic_store_n(&movie->textures[best].state, TEXTURE_SHOWN,
                         __ATOMIC_RELEASE);
        if (movie->current_frame + 1u < desired) ++movie->late_presents;
        ++movie->presented_frames;
        movie->last_present_time = now;
    }

    bool retired = false;
    for (int i = 0; i < TEXTURE_COUNT; ++i) {
        struct movie_texture* texture = &movie->textures[i];
        const int state = __atomic_load_n(&texture->state, __ATOMIC_ACQUIRE);
        if (state == TEXTURE_READY && movie->current_texture >= 0 &&
            texture->frame <= movie->current_frame) {
            __atomic_store_n(&texture->state, TEXTURE_RETIRED,
                             __ATOMIC_RELEASE);
            retired = true;
        } else if (state == TEXTURE_RETIRED) {
            retired = true;
        }
    }
    if (retired) {
        /* The retired textures were only referenced by already-submitted
         * scenes.  Wait for those to finish before handing the memory back
         * to the decoder. */
        melee_vita_gxm_wait_idle();
        vita2d_wait_rendering_done();
        for (int i = 0; i < TEXTURE_COUNT; ++i) {
            if (__atomic_load_n(&movie->textures[i].state, __ATOMIC_ACQUIRE) ==
                TEXTURE_RETIRED)
                __atomic_store_n(&movie->textures[i].state, TEXTURE_FREE,
                                 __ATOMIC_RELEASE);
        }
    }
}

static SceUID start_thread(const char* name, SceKernelThreadEntry entry,
                           int priority, SceSize stack, int cpu_mask,
                           struct melee_vita_opening_movie* movie)
{
    SceUID thread = sceKernelCreateThread(name, entry, priority, stack, 0,
                                          cpu_mask, NULL);
    if (thread < 0) return thread;
    int result = sceKernelStartThread(thread, sizeof(movie), &movie);
    if (result < 0) {
        sceKernelDeleteThread(thread);
        return result;
    }
    return thread;
}

static void create_hw_decoder(struct melee_vita_opening_movie* movie)
{
    FILE* disc = fopen(MELEE_VITA_DISC_PATH, "rb");
    unsigned char* first = malloc(movie->first_frame_size);
    size_t jpeg_size = 0u;
    int ok = disc != NULL && first != NULL &&
        movie->first_frame_size >= 8u &&
        movie->first_frame_size <= movie->packet_capacity &&
        read_at(disc, movie->file_offset + movie->first_frame_offset, first,
                movie->first_frame_size) &&
        make_standard_jpeg(first + 4u, movie->first_frame_size - 4u,
                           movie->standard_jpeg,
                           movie->standard_jpeg_capacity, &jpeg_size);
    if (disc != NULL) fclose(disc);
    free(first);
    if (!ok) {
        melee_vita_log_info("FRONTEND movie hw probe frame unavailable");
        return;
    }

    struct melee_vita_opening_jpeg_hw_info info;
    struct melee_vita_opening_jpeg_hw_error error;
    movie->hw = melee_vita_opening_jpeg_hw_create(
        movie->standard_jpeg, jpeg_size, movie->standard_jpeg_capacity, false,
        &info, &error);
    if (movie->hw == NULL) {
        melee_vita_log_info(
            "FRONTEND movie hw decoder unavailable stage=%d code=0x%08x",
            (int) error.stage, (unsigned int) error.code);
        return;
    }
    if (info.decoded_width != movie->width ||
        info.decoded_height != movie->height) {
        melee_vita_log_info("FRONTEND movie hw size mismatch %ux%u",
                            info.decoded_width, info.decoded_height);
        melee_vita_opening_jpeg_hw_destroy(movie->hw);
        movie->hw = NULL;
        return;
    }
    movie->use_hw = 1;
    melee_vita_log_info(
        "FRONTEND movie hw decoder ready %ux%u colorspace=0x%08x yuv=%u coef=%u",
        info.decoded_width, info.decoded_height, info.color_space,
        info.yuv_capacity, info.coefficient_capacity);
}

struct melee_vita_opening_movie* melee_vita_opening_movie_load_asset(
    const char* movie_filename, const char* audio_filename,
    const uint32_t* rate_table, bool force_full_width)
{
    const uint64_t load_started = sceKernelGetProcessTimeWide();
    struct melee_vita_opening_movie* movie = calloc(1, sizeof(*movie));
    if (movie == NULL) return NULL;
    movie->disc_fd = -1;
    movie->reader_thread = -1;
    movie->decode_thread = -1;
    movie->current_texture = -1;
    movie->current_frame = UINT32_MAX;
    movie->movie_filename = movie_filename;
    movie->rate_table = rate_table;
    movie->force_full_width = force_full_width;

    FILE* disc = fopen(MELEE_VITA_DISC_PATH, "rb");
    unsigned char header[MTH_HEADER_SIZE];
    int valid = disc != NULL && movie_filename != NULL &&
        find_disc_file(disc, movie_filename, &movie->file_offset,
                       &movie->file_size) &&
        read_at(disc, movie->file_offset, header, sizeof(header));
    if (disc != NULL) fclose(disc);
    if (valid) {
        movie->header_maximum_frame_size = be32(header + 0x0cu);
        movie->width = be32(header + 0x10u);
        movie->height = be32(header + 0x14u);
        movie->frame_rate = be32(header + 0x18u);
        movie->frame_count = be32(header + 0x1cu);
        movie->first_frame_offset = be32(header + 0x20u);
        movie->first_frame_size = be32(header + 0x28u);
        valid = memcmp(header, "MTHP", 4u) == 0 &&
            be32(header + 8u) <= 2u && movie->width != 0u &&
            movie->height != 0u && movie->width <= 1024u &&
            movie->height <= 1024u && (movie->width & 1u) == 0u &&
            (movie->height & 1u) == 0u && movie->frame_count != 0u &&
            movie->header_maximum_frame_size >= 8u &&
            movie->header_maximum_frame_size <= MTH_MAX_COMPRESSED_SIZE &&
            movie->first_frame_offset >= MTH_HEADER_SIZE;
    }
    if (!valid) {
        melee_vita_log_info("FRONTEND movie header failed file=%s",
                            movie_filename != NULL ? movie_filename : "(null)");
        melee_vita_opening_movie_free(movie);
        return NULL;
    }

    movie->packet_capacity =
        movie->header_maximum_frame_size + MTH_CAPACITY_SLACK;
    movie->standard_jpeg_capacity = (size_t) movie->packet_capacity * 2u;
    movie->standard_jpeg = malloc(movie->standard_jpeg_capacity);
    movie->packet_memory = malloc((size_t) movie->packet_capacity * PACKET_SLOTS);
    movie->chunk = malloc(READ_CHUNK_SIZE);
    movie->selftest_rgba = malloc((movie->width / 2u) * (movie->height / 2u) * 4u);
    movie->disc_fd = sceIoOpen(MELEE_VITA_DISC_PATH, SCE_O_RDONLY, 0);
    bool textures_ok = true;
    for (int i = 0; i < TEXTURE_COUNT; ++i) {
        movie->textures[i].texture =
            vita2d_create_empty_texture(movie->width, movie->height);
        if (movie->textures[i].texture == NULL) {
            textures_ok = false;
            continue;
        }
        vita2d_texture_set_filters(movie->textures[i].texture,
                                   SCE_GXM_TEXTURE_FILTER_LINEAR,
                                   SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    if (movie->standard_jpeg == NULL || movie->packet_memory == NULL ||
        movie->chunk == NULL || movie->disc_fd < 0 ||
        !textures_ok) {
        melee_vita_log_info("FRONTEND movie allocation failed fd=%d",
                            movie->disc_fd);
        melee_vita_opening_movie_free(movie);
        return NULL;
    }
    for (uint32_t i = 0u; i < PACKET_SLOTS; ++i)
        movie->packets[i].data =
            movie->packet_memory + (size_t) i * movie->packet_capacity;

#ifndef MELEE_VITA_OPENING_MOVIE_SOFTWARE_ONLY
    create_hw_decoder(movie);
#endif

    movie->audio = melee_vita_opening_audio_load_file(audio_filename);
    if (!melee_vita_opening_audio_ready(movie->audio)) {
        melee_vita_log_info("FRONTEND movie audio unavailable file=%s",
                            audio_filename != NULL ? audio_filename : "(null)");
        melee_vita_opening_movie_free(movie);
        return NULL;
    }

    movie->running = 1;
    movie->reader_thread = start_thread(
        "melee movie reader", reader_thread, READER_THREAD_PRIORITY,
        READER_THREAD_STACK, SCE_KERNEL_CPU_MASK_USER_0, movie);
    movie->decode_thread = start_thread(
        "melee movie decode", decode_thread, DECODE_THREAD_PRIORITY,
        DECODE_THREAD_STACK, SCE_KERNEL_CPU_MASK_USER_2, movie);
    if (movie->reader_thread < 0 || movie->decode_thread < 0) {
        melee_vita_log_info("FRONTEND movie thread failed reader=%d decode=%d",
                            movie->reader_thread, movie->decode_thread);
        melee_vita_opening_movie_free(movie);
        return NULL;
    }
    melee_vita_log_info(
        "FRONTEND movie loaded file=%s %ux%u frames=%u header_fps=%u bytes=%u "
        "header_max=%u packet_capacity=%u hw=%d load_us=%llu",
        movie->movie_filename, movie->width, movie->height, movie->frame_count,
        movie->frame_rate, movie->file_size, movie->header_maximum_frame_size,
        movie->packet_capacity, movie->use_hw,
        (unsigned long long) (sceKernelGetProcessTimeWide() - load_started));
    return movie;
}

struct melee_vita_opening_movie* melee_vita_opening_movie_load(void)
{
    static const uint32_t opening_rate_table[] = {
        MELEE_VITA_OPENING_FIRST_RATE_FRAMES, 2u,
        MELEE_VITA_OPENING_FAST_RATE_FRAMES, 1u,
        UINT32_MAX, 2u,
    };
    return melee_vita_opening_movie_load_asset(
        "MvOpen.mth", "opening.hps", opening_rate_table, false);
}

void melee_vita_opening_movie_start(struct melee_vita_opening_movie* movie)
{
    SceCtrlData pad = { 0 };
    if (movie == NULL) return;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    movie->active = true;
    movie->previous_buttons = pad.buttons;
    movie->clock_started = false;
    melee_vita_opening_presentation_start(&movie->presentation);
    s_active_movie = movie;
    melee_vita_log_info("FRONTEND movie start queue=%u",
                        movie->packet_write - movie->packet_read);
}

enum melee_vita_opening_movie_result melee_vita_opening_movie_update(
    struct melee_vita_opening_movie* movie)
{
    SceCtrlData pad = { 0 };
    if (movie == NULL) return MELEE_VITA_OPENING_MOVIE_FAILED;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
        return MELEE_VITA_OPENING_MOVIE_PLAYING;
    const uint32_t pressed = pad.buttons & ~movie->previous_buttons;
    movie->previous_buttons = pad.buttons;
    if (!movie->active) return MELEE_VITA_OPENING_MOVIE_PLAYING;
    if ((pressed & (SCE_CTRL_START | SCE_CTRL_CROSS)) != 0u) {
        melee_vita_log_info("FRONTEND movie skipped frame=%u",
                            movie->current_frame);
        stop_opening_audio(movie, "skip");
        return MELEE_VITA_OPENING_MOVIE_SKIPPED;
    }
    if (__atomic_load_n(&movie->fatal_error, __ATOMIC_ACQUIRE) != 0) {
        stop_opening_audio(movie, "video_failure");
        return MELEE_VITA_OPENING_MOVIE_FAILED;
    }

    const uint64_t now = sceKernelGetProcessTimeWide();
    if (!movie->clock_started) {
        present_due_frame(movie, 0u, now);
        if (movie->current_frame != 0u) return MELEE_VITA_OPENING_MOVIE_PLAYING;
        movie->start_time = now;
        movie->clock_started = true;
        __atomic_store_n(&movie->clock_running, 1, __ATOMIC_RELEASE);
        melee_vita_opening_audio_start(movie->audio);
        log_stats(movie, "first_frame", 0u);
        return MELEE_VITA_OPENING_MOVIE_PLAYING;
    }

    const uint64_t elapsed_ticks =
        (now - movie->start_time) * UINT64_C(60) / UINT64_C(1000000);
    movie->elapsed_ticks = elapsed_ticks;
    const uint64_t total_ticks =
        melee_vita_movie_total_ticks(movie->frame_count, movie->rate_table);
    const uint32_t desired =
        melee_vita_movie_frame_for_tick(elapsed_ticks, movie->frame_count,
                                        movie->rate_table);
    __atomic_store_n(&movie->clock_frame, desired, __ATOMIC_RELEASE);
    present_due_frame(movie, desired, now);

    if (elapsed_ticks >= movie->next_stats_tick) {
        movie->next_stats_tick = elapsed_ticks + STATS_INTERVAL_TICKS;
        log_stats(movie, "perf", desired);
    }

    const bool last_shown = movie->current_frame == movie->frame_count - 1u;
    const bool stream_exhausted =
        __atomic_load_n(&movie->decoder_done, __ATOMIC_ACQUIRE) != 0;
    if (elapsed_ticks >= total_ticks && (last_shown || stream_exhausted)) {
        const uint64_t final_duration =
            (uint64_t) melee_vita_movie_frame_ticks(movie->current_frame,
                                                    movie->rate_table) *
            UINT64_C(1000000) / UINT64_C(60);
        if (now - movie->last_present_time >= final_duration) {
            log_stats(movie, "finished", desired);
            movie->active = false;
            if (melee_vita_opening_presentation_finish(
                    &movie->presentation))
                return MELEE_VITA_OPENING_MOVIE_FINISHED;
        }
    }
    return MELEE_VITA_OPENING_MOVIE_PLAYING;
}

void melee_vita_opening_movie_draw_active(void)
{
    const struct melee_vita_opening_movie* movie = s_active_movie;
    if (movie == NULL || !movie->presentation.visible) return;
    if (movie->current_texture < 0) {
        if (movie->force_full_width) {
            melee_vita_gxm_queue_overlay_full_width(
                NULL, movie->width, movie->height);
        } else {
            melee_vita_gxm_queue_overlay(NULL, movie->width, movie->height);
        }
        return;
    }
    const struct movie_texture* texture =
        &movie->textures[movie->current_texture];
    if (movie->force_full_width) {
        melee_vita_gxm_queue_overlay_full_width(
            texture->texture, texture->width, texture->height);
    } else {
        melee_vita_gxm_queue_overlay(texture->texture, texture->width,
                                     texture->height);
    }
}

void melee_vita_opening_movie_hide_active(void)
{
    if (s_active_movie == NULL) return;
    melee_vita_opening_presentation_hide(&s_active_movie->presentation);
}

bool melee_vita_opening_movie_visible(
    const struct melee_vita_opening_movie* movie)
{
    return movie != NULL && movie->presentation.visible;
}

uint32_t melee_vita_opening_movie_total_ticks(
    const struct melee_vita_opening_movie* movie)
{
    if (movie == NULL) return 0u;
    const uint64_t total_ticks =
        melee_vita_movie_total_ticks(movie->frame_count, movie->rate_table);
    return total_ticks <= UINT32_MAX ? (uint32_t) total_ticks : UINT32_MAX;
}

uint32_t melee_vita_opening_movie_elapsed_ticks(
    const struct melee_vita_opening_movie* movie)
{
    return movie != NULL && movie->elapsed_ticks <= UINT32_MAX
        ? (uint32_t) movie->elapsed_ticks : UINT32_MAX;
}

void melee_vita_opening_movie_preserve_audio(
    struct melee_vita_opening_movie* movie)
{
    if (movie == NULL || movie->audio == NULL) return;
    melee_vita_opening_audio_handoff(movie->audio);
    movie->audio = NULL;
}

void melee_vita_opening_movie_stop_preserved_audio(void)
{
    melee_vita_opening_audio_cancel_handoff();
}

void melee_vita_opening_movie_free(struct melee_vita_opening_movie* movie)
{
    if (movie == NULL) return;
    melee_vita_opening_presentation_hide(&movie->presentation);
    if (s_active_movie == movie) s_active_movie = NULL;
    stop_opening_audio(movie, "free");
    __atomic_store_n(&movie->running, 0, __ATOMIC_RELEASE);
    if (movie->reader_thread >= 0) {
        sceKernelWaitThreadEnd(movie->reader_thread, NULL, NULL);
        sceKernelDeleteThread(movie->reader_thread);
    }
    if (movie->decode_thread >= 0) {
        sceKernelWaitThreadEnd(movie->decode_thread, NULL, NULL);
        sceKernelDeleteThread(movie->decode_thread);
    }
    melee_vita_opening_jpeg_hw_destroy(movie->hw);
    melee_vita_gxm_wait_idle();
    vita2d_wait_rendering_done();
    for (int i = 0; i < TEXTURE_COUNT; ++i) {
        if (movie->textures[i].texture != NULL)
            vita2d_free_texture(movie->textures[i].texture);
    }
    if (movie->disc_fd >= 0) sceIoClose(movie->disc_fd);
    free(movie->selftest_rgba);
    free(movie->chunk);
    free(movie->packet_memory);
    free(movie->standard_jpeg);
    melee_vita_opening_audio_free(movie->audio);
    free(movie);
}

bool melee_vita_opening_movie_ready(
    const struct melee_vita_opening_movie* movie)
{
    return movie != NULL &&
           __atomic_load_n(&movie->fatal_error, __ATOMIC_ACQUIRE) == 0;
}
