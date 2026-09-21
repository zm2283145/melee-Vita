#include "opening_audio.h"

#include "../disc_probe.h"
#include "../vita_log.h"

#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISC_HEADER_FST_OFFSET 0x424u
#define HPS_HEADER_SIZE 0x80u
#define HPS_BLOCK_HEADER_SIZE 0x20u
#define HPS_CHANNEL_HEADER_SIZE 0x38u
#define HPS_CHANNEL_COUNT 2u
#define HPS_MAX_FILE_SIZE (64u * 1024u * 1024u)

static struct melee_vita_opening_audio* s_handoff_audio;
static struct melee_vita_opening_audio* s_output_audio;
static volatile int s_mix_lock;

struct dsp_channel {
    int16_t coefficients[8][2];
    int32_t yn1;
    int32_t yn2;
    const unsigned char* data;
    uint32_t data_size;
    uint32_t frame_offset;
    uint32_t sample_in_frame;
    uint8_t predictor;
    uint8_t scale;
};

struct melee_vita_opening_audio {
    /* Keeping the compressed stream resident deliberately avoids competing ISO
     * seeks with the movie decoder while still decoding PCM a 512-frame grain
     * at a time. */
    unsigned char* file;
    uint32_t file_size;
    uint32_t sample_rate;
    uint32_t total_frames;
    uint32_t block_offset;
    uint32_t block_frames;
    uint32_t block_frame;
    struct dsp_channel channels[HPS_CHANNEL_COUNT];
    volatile int start_requested;
    volatile int stop_requested;
    volatile int ready;
    volatile int finished;
    volatile uint32_t played_frames;
};

static void lock_mixer(void)
{
    while (__sync_lock_test_and_set(&s_mix_lock, 1) != 0)
        sceKernelDelayThread(100u);
}

static void unlock_mixer(void)
{
    __sync_lock_release(&s_mix_lock);
}

static uint16_t be16(const unsigned char* p)
{
    return (uint16_t) ((uint16_t) p[0] << 8u | p[1]);
}

static int16_t be_s16(const unsigned char* p)
{
    return (int16_t) be16(p);
}

static uint32_t be32(const unsigned char* p)
{
    return (uint32_t) p[0] << 24u | (uint32_t) p[1] << 16u |
           (uint32_t) p[2] << 8u | p[3];
}

static int16_t clamp_s16(int64_t value)
{
    return value > 32767 ? 32767 :
           value < -32768 ? -32768 : (int16_t) value;
}

static uint32_t dsp_nibbles_to_samples(uint32_t nibbles)
{
    const uint32_t remainder = nibbles & 15u;
    return (nibbles >> 4u) * 14u +
           (remainder >= 2u ? remainder - 2u : 0u);
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

static int begin_block(struct melee_vita_opening_audio* audio)
{
    if (audio->block_offset > audio->file_size - HPS_BLOCK_HEADER_SIZE)
        return 0;
    const unsigned char* header = audio->file + audio->block_offset;
    const uint32_t block_size = be32(header);
    const uint32_t block_nibbles = be32(header + 4u) + 1u;
    if (block_size == 0u || (block_size & 1u) != 0u ||
        block_size > audio->file_size - audio->block_offset -
                         HPS_BLOCK_HEADER_SIZE)
        return 0;
    const uint32_t channel_size = block_size / HPS_CHANNEL_COUNT;
    if (channel_size == 0u || block_nibbles > channel_size * 2u)
        return 0;

    audio->block_frames = dsp_nibbles_to_samples(block_nibbles);
    audio->block_frame = 0u;
    const unsigned char* payload = header + HPS_BLOCK_HEADER_SIZE;
    for (uint32_t channel = 0u; channel < HPS_CHANNEL_COUNT; ++channel) {
        struct dsp_channel* state = &audio->channels[channel];
        const unsigned char* loop = header + 0x0cu + channel * 8u;
        state->yn1 = be_s16(loop + 2u);
        state->yn2 = be_s16(loop + 4u);
        state->data = payload + channel * channel_size;
        state->data_size = channel_size;
        state->frame_offset = 0u;
        state->sample_in_frame = 0u;
        state->predictor = 0u;
        state->scale = 0u;
    }
    return audio->block_frames != 0u;
}

static int next_block(struct melee_vita_opening_audio* audio)
{
    const unsigned char* header = audio->file + audio->block_offset;
    const uint32_t next = be32(header + 8u);
    if (next == UINT32_MAX) return 0;
    if (next <= audio->block_offset || next >= audio->file_size) return -1;
    audio->block_offset = next;
    return begin_block(audio) ? 1 : -1;
}

static int decode_channel(struct dsp_channel* channel, int16_t* output)
{
    if (channel->sample_in_frame == 0u) {
        if (channel->frame_offset + 8u > channel->data_size) return 0;
        const uint8_t frame_header = channel->data[channel->frame_offset];
        channel->predictor = (frame_header >> 4u) & 7u;
        channel->scale = frame_header & 15u;
    }
    const uint32_t packed_offset = channel->frame_offset + 1u +
                                   channel->sample_in_frame / 2u;
    if (packed_offset >= channel->data_size) return 0;
    const uint8_t packed = channel->data[packed_offset];
    int32_t nibble = (channel->sample_in_frame & 1u) != 0u
        ? packed & 15u : packed >> 4u;
    if (nibble >= 8) nibble -= 16;
    const int64_t decoded = (int64_t) nibble *
                                (INT64_C(1) << channel->scale) * 2048 + 1024 +
                            (int64_t) channel->coefficients
                                [channel->predictor][0] * channel->yn1 +
                            (int64_t) channel->coefficients
                                [channel->predictor][1] * channel->yn2;
    *output = clamp_s16(decoded >> 11u);
    channel->yn2 = channel->yn1;
    channel->yn1 = *output;
    ++channel->sample_in_frame;
    if (channel->sample_in_frame == 14u) {
        channel->sample_in_frame = 0u;
        channel->frame_offset += 8u;
    }
    return 1;
}

static int decode_frame(struct melee_vita_opening_audio* audio,
                        int16_t* left, int16_t* right)
{
    if (audio->played_frames >= audio->total_frames) return 0;
    if (audio->block_frame >= audio->block_frames) {
        const int advanced = next_block(audio);
        if (advanced <= 0) return advanced;
    }
    if (!decode_channel(&audio->channels[0], left) ||
        !decode_channel(&audio->channels[1], right))
        return -1;
    ++audio->block_frame;
    return 1;
}

static int16_t mix_s16(int16_t current, int16_t added)
{
    const int32_t mixed = (int32_t) current + added;
    return mixed > 32767 ? 32767 : mixed < -32768 ? -32768
                                                     : (int16_t) mixed;
}

struct melee_vita_opening_audio* melee_vita_opening_audio_load_file(
    const char* filename)
{
    melee_vita_opening_audio_poll();
    if (s_handoff_audio != NULL) {
        melee_vita_opening_audio_stop(s_handoff_audio);
        melee_vita_opening_audio_free(s_handoff_audio);
        s_handoff_audio = NULL;
    }

    struct melee_vita_opening_audio* audio = calloc(1, sizeof(*audio));
    if (audio == NULL) return NULL;

    FILE* disc = fopen(MELEE_VITA_DISC_PATH, "rb");
    uint32_t disc_offset = 0u;
    if (disc == NULL || filename == NULL ||
        !find_disc_file(disc, filename, &disc_offset, &audio->file_size) ||
        audio->file_size < HPS_HEADER_SIZE + HPS_BLOCK_HEADER_SIZE ||
        audio->file_size > HPS_MAX_FILE_SIZE) {
        if (disc != NULL) fclose(disc);
        free(audio);
        return NULL;
    }
    audio->file = malloc(audio->file_size);
    if (audio->file == NULL ||
        !read_at(disc, disc_offset, audio->file, audio->file_size)) {
        fclose(disc);
        free(audio->file);
        free(audio);
        return NULL;
    }
    fclose(disc);

    if (memcmp(audio->file, " HALPST\0", 8u) != 0 ||
        be32(audio->file + 0x0cu) != HPS_CHANNEL_COUNT) {
        free(audio->file);
        free(audio);
        return NULL;
    }
    audio->sample_rate = be32(audio->file + 8u);
    const uint32_t total_nibbles = be32(audio->file + 0x18u);
    audio->total_frames = dsp_nibbles_to_samples(total_nibbles) + 1u;
    if (audio->sample_rate != 32000u || audio->total_frames == 0u) {
        free(audio->file);
        free(audio);
        return NULL;
    }
    for (uint32_t channel = 0u; channel < HPS_CHANNEL_COUNT; ++channel) {
        const unsigned char* channel_header =
            audio->file + 0x10u + channel * HPS_CHANNEL_HEADER_SIZE;
        for (uint32_t predictor = 0u; predictor < 8u; ++predictor) {
            audio->channels[channel].coefficients[predictor][0] =
                be_s16(channel_header + 0x10u + predictor * 4u);
            audio->channels[channel].coefficients[predictor][1] =
                be_s16(channel_header + 0x12u + predictor * 4u);
        }
    }
    audio->block_offset = HPS_HEADER_SIZE;
    if (!begin_block(audio)) {
        free(audio->file);
        free(audio);
        return NULL;
    }

    audio->ready = 1;
    melee_vita_log_info(
        "FRONTEND movie audio ready file=%s rate=%u channels=2 frames=%u "
        "bytes=%u",
        filename, audio->sample_rate, audio->total_frames, audio->file_size);
    return audio;
}

struct melee_vita_opening_audio* melee_vita_opening_audio_load(void)
{
    return melee_vita_opening_audio_load_file("opening.hps");
}

void melee_vita_opening_audio_start(struct melee_vita_opening_audio* audio)
{
    if (audio == NULL ||
        __atomic_load_n(&audio->ready, __ATOMIC_ACQUIRE) == 0)
        return;
    __atomic_store_n(&audio->start_requested, 1, __ATOMIC_RELEASE);
    lock_mixer();
    s_output_audio = audio;
    unlock_mixer();
}

void melee_vita_opening_audio_stop(struct melee_vita_opening_audio* audio)
{
    if (audio == NULL) return;
    __atomic_store_n(&audio->stop_requested, 1, __ATOMIC_RELEASE);
    lock_mixer();
    if (s_output_audio == audio) s_output_audio = NULL;
    __atomic_store_n(&audio->finished, 1, __ATOMIC_RELEASE);
    unlock_mixer();
}

void melee_vita_opening_audio_free(struct melee_vita_opening_audio* audio)
{
    if (audio == NULL) return;
    melee_vita_opening_audio_stop(audio);
    free(audio->file);
    free(audio);
}

void melee_vita_opening_audio_mix(int16_t* output, uint32_t frames)
{
    struct melee_vita_opening_audio* audio;
    uint32_t produced = 0u;
    int decode_error = 0;
    if (output == NULL || frames == 0u) return;

    lock_mixer();
    audio = s_output_audio;
    if (audio == NULL ||
        __atomic_load_n(&audio->start_requested, __ATOMIC_ACQUIRE) == 0)
        goto unlock;
    if (__atomic_load_n(&audio->stop_requested, __ATOMIC_ACQUIRE) != 0)
        goto unlock;

    while (produced < frames &&
           audio->played_frames + produced < audio->total_frames) {
        int16_t left;
        int16_t right;
        const int result = decode_frame(audio, &left, &right);
        if (result <= 0) {
            decode_error = result < 0;
            break;
        }
        output[produced * 2u] =
            mix_s16(output[produced * 2u], left);
        output[produced * 2u + 1u] =
            mix_s16(output[produced * 2u + 1u], right);
        ++produced;
    }
    __atomic_add_fetch(&audio->played_frames, produced, __ATOMIC_RELEASE);
    if (decode_error || audio->played_frames >= audio->total_frames) {
        if (s_output_audio == audio) s_output_audio = NULL;
        __atomic_store_n(&audio->finished, 1, __ATOMIC_RELEASE);
        melee_vita_log_info(
            "FRONTEND opening audio %s frame=%llu",
            decode_error ? "decode failed" : "finished",
            (unsigned long long) audio->played_frames);
    }

unlock:
    unlock_mixer();
}

void melee_vita_opening_audio_handoff(struct melee_vita_opening_audio* audio)
{
    if (audio == NULL) return;
    if (s_handoff_audio != NULL) {
        melee_vita_opening_audio_stop(s_handoff_audio);
        melee_vita_opening_audio_free(s_handoff_audio);
    }
    s_handoff_audio = audio;
}

void melee_vita_opening_audio_poll(void)
{
    if (s_handoff_audio == NULL ||
        !melee_vita_opening_audio_finished(s_handoff_audio))
        return;
    melee_vita_opening_audio_free(s_handoff_audio);
    s_handoff_audio = NULL;
}

void melee_vita_opening_audio_shutdown(void)
{
    if (s_handoff_audio == NULL) return;
    melee_vita_opening_audio_stop(s_handoff_audio);
    melee_vita_opening_audio_free(s_handoff_audio);
    s_handoff_audio = NULL;
}

void melee_vita_opening_audio_cancel_handoff(void)
{
    melee_vita_opening_audio_shutdown();
}

bool melee_vita_opening_audio_ready(
    const struct melee_vita_opening_audio* audio)
{
    return audio != NULL &&
           __atomic_load_n(&audio->ready, __ATOMIC_ACQUIRE) != 0;
}

bool melee_vita_opening_audio_finished(
    const struct melee_vita_opening_audio* audio)
{
    return audio == NULL ||
           __atomic_load_n(&audio->finished, __ATOMIC_ACQUIRE) != 0;
}

uint64_t melee_vita_opening_audio_played_frames(
    const struct melee_vita_opening_audio* audio)
{
    return audio != NULL
        ? __atomic_load_n(&audio->played_frames, __ATOMIC_ACQUIRE) : 0u;
}
