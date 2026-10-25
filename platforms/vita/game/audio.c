/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita ARAM/AX backend. Melee drives GameCube AX voices backed by ARAM; this
 * file decodes and mixes those voices into a native SceAudioOut stream. */
#include "vita_platform.h"
#include "opening_audio.h"
#include "../vita_log.h"

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/axfx.h>

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdlib.h>
#include <string.h>

#define VITA_AX_VOICES 64
#define VITA_ARQ_CAPACITY 128
#define VITA_AX_RATE 32000
#define VITA_AX_FRAME 160
#define VITA_AUDIO_GRAIN 512
#define VITA_AX_FALLBACK_FRAMES 3

#define AX_FORMAT_ADPCM 0
#define AX_FORMAT_PCM16 10
#define AX_FORMAT_PCM8 25

/* What the AX setters changed since the last frame went to the mixer. */
enum {
    SYNC_ADDR = 1 << 0,  /* loop, end and current address, history cleared */
    SYNC_LOOP = 1 << 1,
    SYNC_END = 1 << 2,
    SYNC_CUR = 1 << 3,
    SYNC_ADPCM = 1 << 4, /* predictor and history */
    SYNC_SRC = 1 << 5,   /* resampler position */
    SYNC_VOL = 1 << 6,   /* current volume */
    SYNC_STATE = 1 << 7, /* state written: a finished voice may play again */
    SYNC_NEW = 0xFF,     /* AXAcquireVoice */
};

/* Game thread side of a voice. The decode fields hold what the setters
 * last asked for; the mixer keeps its own running copy (MixerVoice). */
typedef struct VitaVoice {
    AXVPB voice;
    GXBool used;
    u32 fraction;
    u16 predictor_scale;
    s32 yn1;
    s32 yn2;
    u32 loop_address;
    u32 end_address;
    u32 current_address;
    u16 sync;        /* SYNC_* not yet sent to the mixer */
    u32 addr_frame;  /* first mix frame with the latest address write */
    u32 vol_frame;   /* ... volume write */
    u32 state_frame; /* ... state write */
} VitaVoice;

typedef struct VitaArqJob {
    ARQRequest* request;
    u32 type;
    uintptr_t source;
    uintptr_t destination;
    u32 length;
    ARQCallback callback;
} VitaArqJob;

static u8* s_aram;
static u32 s_aram_stack = 0x4000;
static u32* s_aram_lengths;
static u32 s_aram_blocks;
static u32 s_aram_free_blocks;
static VitaArqJob s_arq[VITA_ARQ_CAPACITY];
static u32 s_arq_read;
static u32 s_arq_write;
static VitaVoice s_voices[VITA_AX_VOICES];
static void (*s_ax_callback)(void);
static void (*s_aux_a_callback)(void*, void*);
static void* s_aux_a_context;
static void (*s_aux_b_callback)(void*, void*);
static void* s_aux_b_context;
static void* (*s_fx_alloc)(size_t);
static void (*s_fx_free)(void*);
static int s_audio_port = -1;

/* Mixing runs on its own thread, off the game thread. The game thread still
 * owns every AX voice and runs the synth callback once per 5 ms AX frame,
 * then hands the mixer a copy of the voices (a MixFrame). The mixer decodes
 * and mixes it into the output ring and reports back where each voice got
 * to, so the synth sees positions and finished voices about a frame late,
 * as it did with the GameCube's DSP. */
typedef struct MixVoice {
    u8 index;
    u8 sync;
    u16 state;
    u16 format;
    u16 loop_flag;
    u16 coef[8][2];
    u16 loop_pred_scale;
    u16 loop_yn1;
    u16 loop_yn2;
    u32 ratio;
    s16 delta;
    u16 left;
    u16 right;
    u16 volume; /* with SYNC_VOL */
    u32 loop_address, end_address, current_address, fraction;
    u16 predictor_scale;
    s32 yn1, yn2;
} MixVoice;

typedef struct MixFrame {
    u32 index;
    u32 count;
    MixVoice voice[VITA_AX_VOICES];
} MixFrame;

/* the mixer's running decode state */
typedef struct MixerVoice {
    s16 previous_sample;
    s16 current_sample;
    u32 fraction;
    u16 predictor_scale;
    s32 yn1;
    s32 yn2;
    u32 loop_address;
    u32 end_address;
    u32 current_address;
    s32 volume;
    GXBool ended;
} MixerVoice;

/* mixer -> game, per voice. seq is odd while it's being written. */
typedef struct MixReport {
    volatile u32 seq;
    u32 frame;
    u32 address;
    u16 volume;
    u8 ended;
} MixReport;

#define VITA_MIX_FRAMES 32u
static MixFrame* s_mix_frames;
static volatile u32 s_mix_read;  /* advanced by the mixer */
static volatile u32 s_mix_write; /* advanced by the game thread */
static u32 s_mix_next = 1;       /* index of the next frame built (0 = none) */
static MixerVoice s_mixer[VITA_AX_VOICES];
static MixReport s_reports[VITA_AX_VOICES];
static SceUID s_mix_thread = -1;
static SceUID s_mix_sema = -1;
static volatile int s_mix_running;

/* Audio output runs on its own thread so a slow game frame never starves
 * SceAudioOut.  The game thread runs the AX callback, the mixer thread
 * (below) mixes, and together they keep a ring buffer ~250 ms ahead. */
#define VITA_RING_FRAMES 16384u
#define VITA_RING_TARGET 8000u
#define VITA_RING_MAX_RENDER 256u
static s16 s_ring[VITA_RING_FRAMES * 2u];
static volatile u32 s_ring_read;
static volatile u32 s_ring_write;
static volatile int s_audio_thread_running;
static SceUID s_audio_thread = -1;
static volatile u32 s_audio_underruns;
static volatile int s_audio_flush_requested;

static u32 ring_fill(void)
{
    return __atomic_load_n(&s_ring_write, __ATOMIC_ACQUIRE) -
           __atomic_load_n(&s_ring_read, __ATOMIC_ACQUIRE);
}

static int audio_output_thread(SceSize args, void* argp)
{
    static s16 grain[VITA_AUDIO_GRAIN * 2] __attribute__((aligned(64)));
    (void) args;
    (void) argp;
    while (__atomic_load_n(&s_audio_thread_running, __ATOMIC_ACQUIRE)) {
        if (__atomic_exchange_n(&s_audio_flush_requested, 0,
                                __ATOMIC_ACQ_REL) != 0) {
            const u32 write =
                __atomic_load_n(&s_ring_write, __ATOMIC_ACQUIRE);
            __atomic_store_n(&s_ring_read, write, __ATOMIC_RELEASE);
        }
        u32 available = ring_fill();
        u32 take = available < VITA_AUDIO_GRAIN ? available : VITA_AUDIO_GRAIN;
        u32 read = __atomic_load_n(&s_ring_read, __ATOMIC_ACQUIRE);
        u32 i;
        for (i = 0; i < take; ++i) {
            const u32 slot = (read + i) % VITA_RING_FRAMES;
            grain[i * 2u] = s_ring[slot * 2u];
            grain[i * 2u + 1u] = s_ring[slot * 2u + 1u];
        }
        if (take < VITA_AUDIO_GRAIN) {
            memset(grain + take * 2u, 0, (VITA_AUDIO_GRAIN - take) * 2u * sizeof(grain[0]));
            if (take != 0u || s_audio_underruns == 0u) ++s_audio_underruns;
        }
        __atomic_add_fetch(&s_ring_read, take, __ATOMIC_RELEASE);
        melee_vita_opening_audio_mix(grain, VITA_AUDIO_GRAIN);
        sceAudioOutOutput(s_audio_port, grain);
    }
    return 0;
}

static u16 from_be16(u16 value) { return __builtin_bswap16(value); }
static u32 pair(u16 high, u16 low) { return (u32) high << 16 | low; }
static void set_pair(u16* high, u16* low, u32 value)
{ *high = (u16) (value >> 16); *low = (u16) value; }

static s16 clamp_s16(s64 value)
{
    return value > 32767 ? 32767 : value < -32768 ? -32768 : (s16) value;
}

static GXBool next_sample(MixerVoice* voice, const MixVoice* params, s16* output)
{
    const u16 format = params->format;
    if (s_aram == NULL) return GX_FALSE;
    switch (format) {
    case AX_FORMAT_ADPCM: {
        if ((voice->current_address & 15u) == 0) {
            if ((voice->current_address >> 1) + 2u > MELEE_VITA_ARAM_SIZE)
                return GX_FALSE;
            voice->predictor_scale = s_aram[voice->current_address >> 1];
            voice->current_address += 2u;
        }
        if ((voice->current_address >> 1) >= MELEE_VITA_ARAM_SIZE)
            return GX_FALSE;
        const u8 byte = s_aram[voice->current_address >> 1];
        s32 nibble = (voice->current_address & 1u) ? byte & 15u : byte >> 4;
        const u32 coefficient = (voice->predictor_scale >> 4) & 7u;
        const s32 scale = 1 << (voice->predictor_scale & 15u);
        if (nibble >= 8) nibble -= 16;
        const s64 decoded = (s64) nibble * scale * 2048 + 1024 +
            (s64) (s16) params->coef[coefficient][0] * voice->yn1 +
            (s64) (s16) params->coef[coefficient][1] * voice->yn2;
        const s16 sample = clamp_s16(decoded >> 11);
        voice->yn2 = voice->yn1;
        voice->yn1 = sample;
        *output = sample;
        break;
    }
    case AX_FORMAT_PCM16: {
        if (voice->current_address >= MELEE_VITA_ARAM_SIZE / 2u)
            return GX_FALSE;
        const u8* sample = s_aram + voice->current_address * 2u;
        *output = (s16) ((u16) sample[0] << 8 | sample[1]);
        break;
    }
    case AX_FORMAT_PCM8:
        if (voice->current_address >= MELEE_VITA_ARAM_SIZE) return GX_FALSE;
        *output = (s16) ((s8) s_aram[voice->current_address] * 256);
        break;
    default:
        return GX_FALSE;
    }
    if (voice->current_address == voice->end_address) {
        if (params->loop_flag != 0) {
            voice->current_address = voice->loop_address;
            if (format == AX_FORMAT_ADPCM) {
                voice->predictor_scale = params->loop_pred_scale;
                voice->yn1 = (s16) params->loop_yn1;
                voice->yn2 = (s16) params->loop_yn2;
            }
        } else {
            voice->ended = GX_TRUE;
        }
    } else {
        ++voice->current_address;
    }
    return GX_TRUE;
}

static void mix_voice(MixerVoice* voice, const MixVoice* params, float* mix)
{
    const float left = params->left / 32767.0f;
    const float right = params->right / 32767.0f;
    s32 volume = voice->volume;
    u32 sample;
    for (sample = 0; sample < VITA_AX_FRAME; ++sample) {
        voice->fraction += params->ratio;
        while (voice->fraction >= 0x10000u) {
            s16 decoded;
            // a voice that ended plays out the rest of this frame as silence
            if (voice->ended || !next_sample(voice, params, &decoded)) {
                voice->ended = GX_TRUE;
                voice->volume = volume;
                return;
            }
            voice->previous_sample = voice->current_sample;
            voice->current_sample = decoded;
            voice->fraction -= 0x10000u;
        }
        const float phase = voice->fraction * (1.0f / 65536.0f);
        const float value = ((float) voice->previous_sample + phase *
            (float) (voice->current_sample - voice->previous_sample)) *
            (1.0f / 32768.0f) * ((float) volume / 32767.0f);
        mix[sample * 2u] += value * left;
        mix[sample * 2u + 1u] += value * right;
        volume += params->delta;
        if (volume < 0) volume = 0;
        else if (volume > 32767) volume = 32767;
    }
    voice->volume = volume;
}

/* Mixer side: apply what the game changed, mix, report back. */
static void mix_frame(const MixFrame* frame, s16* output)
{
    float mix[VITA_AX_FRAME * 2] = { 0 };
    u32 i;
    for (i = 0; i < frame->count; ++i) {
        const MixVoice* params = &frame->voice[i];
        MixerVoice* voice = &s_mixer[params->index];
        MixReport* report = &s_reports[params->index];
        if (params->sync & SYNC_ADDR) {
            voice->previous_sample = voice->current_sample = 0;
            voice->current_address = params->current_address;
            voice->loop_address = params->loop_address;
            voice->end_address = params->end_address;
            voice->ended = GX_FALSE;
        }
        if (params->sync & SYNC_LOOP) voice->loop_address = params->loop_address;
        if (params->sync & SYNC_END) voice->end_address = params->end_address;
        if (params->sync & SYNC_CUR) {
            voice->current_address = params->current_address;
            voice->ended = GX_FALSE;
        }
        if (params->sync & (SYNC_ADDR | SYNC_SRC)) voice->fraction = params->fraction;
        if (params->sync & SYNC_ADPCM) {
            voice->predictor_scale = params->predictor_scale;
            voice->yn1 = params->yn1;
            voice->yn2 = params->yn2;
        }
        if (params->sync & SYNC_VOL) voice->volume = params->volume;
        if (params->sync & SYNC_STATE) voice->ended = GX_FALSE;
        if (params->state != 0 && !voice->ended) mix_voice(voice, params, mix);

        __atomic_add_fetch(&report->seq, 1u, __ATOMIC_ACQ_REL);
        report->frame = frame->index;
        report->address = voice->current_address;
        report->volume = (u16) voice->volume;
        report->ended = voice->ended;
        __atomic_add_fetch(&report->seq, 1u, __ATOMIC_ACQ_REL);
    }
    for (i = 0; i < VITA_AX_FRAME * 2u; ++i) {
        float value = mix[i] * 32767.0f;
        output[i] = value > 32767.0f ? 32767 :
                    value < -32768.0f ? -32768 : (s16) value;
    }
}

/* Game thread: take what the mixer reported, unless the game wrote that
 * field again after the frame the report is from. */
static void apply_reports(void)
{
    u32 i;
    for (i = 0; i < VITA_AX_VOICES; ++i) {
        VitaVoice* voice = &s_voices[i];
        const MixReport* report = &s_reports[i];
        u32 seq, frame, address;
        u16 volume;
        u8 ended;
        if (!voice->used) continue;
        seq = __atomic_load_n(&report->seq, __ATOMIC_ACQUIRE);
        if (seq & 1u) continue; // being written, next time
        frame = report->frame;
        address = report->address;
        volume = report->volume;
        ended = report->ended;
        __atomic_thread_fence(__ATOMIC_ACQUIRE); // the reads above stay above
        if (__atomic_load_n(&report->seq, __ATOMIC_ACQUIRE) != seq || frame == 0)
            continue;
        if (frame >= voice->addr_frame)
            set_pair(&voice->voice.pb.addr.currentAddressHi,
                     &voice->voice.pb.addr.currentAddressLo, address);
        if (frame >= voice->vol_frame) voice->voice.pb.ve.currentVolume = volume;
        if (ended && frame >= voice->state_frame) voice->voice.pb.state = 0;
    }
}

/* Game thread: run one AX frame of the synth and copy the voices out. */
static void build_frame(MixFrame* frame)
{
    static GXBool logged_running_voice;
    u32 i;
    apply_reports();
    if (s_ax_callback != NULL) s_ax_callback();
    frame->index = s_mix_next++;
    frame->count = 0;
    for (i = 0; i < VITA_AX_VOICES; ++i) {
        VitaVoice* voice = &s_voices[i];
        const AXPB* pb = &voice->voice.pb;
        MixVoice* out;
        u32 k;
        if (!voice->used && voice->sync == 0) continue;
        if (voice->used && pb->state != 0 && !logged_running_voice) {
            melee_vita_log_info(
                "[AUDIO] first AX voice: format=%u ratio=%u volume=%u",
                pb->addr.format, pair(pb->src.ratioHi, pb->src.ratioLo),
                pb->ve.currentVolume);
            logged_running_voice = GX_TRUE;
        }
        out = &frame->voice[frame->count++];
        out->index = (u8) i;
        out->sync = (u8) voice->sync;
        out->state = voice->used ? pb->state : 0;
        out->format = pb->addr.format;
        out->loop_flag = pb->addr.loopFlag;
        for (k = 0; k < 8; ++k) {
            out->coef[k][0] = pb->adpcm.a[k][0];
            out->coef[k][1] = pb->adpcm.a[k][1];
        }
        out->loop_pred_scale = pb->adpcmLoop.loop_pred_scale;
        out->loop_yn1 = pb->adpcmLoop.loop_yn1;
        out->loop_yn2 = pb->adpcmLoop.loop_yn2;
        out->ratio = pair(pb->src.ratioHi, pb->src.ratioLo);
        out->delta = pb->ve.currentDelta;
        out->left = pb->mix.vL;
        out->right = pb->mix.vR;
        out->volume = pb->ve.currentVolume;
        out->loop_address = voice->loop_address;
        out->end_address = voice->end_address;
        out->current_address = voice->current_address;
        out->fraction = voice->fraction;
        out->predictor_scale = voice->predictor_scale;
        out->yn1 = voice->yn1;
        out->yn2 = voice->yn2;
        voice->sync = 0;
    }
}

#ifndef TARGET_PS5
static int audio_mix_thread(SceSize args, void* argp)
{
    static s16 ax_frame[VITA_AX_FRAME * 2] __attribute__((aligned(64)));
    (void) args;
    (void) argp;
    while (__atomic_load_n(&s_mix_running, __ATOMIC_ACQUIRE)) {
        const u32 read = s_mix_read;
        u32 write, i;
        if (read == __atomic_load_n(&s_mix_write, __ATOMIC_ACQUIRE)) {
            sceKernelWaitSema(s_mix_sema, 1, NULL);
            continue;
        }
        mix_frame(&s_mix_frames[read % VITA_MIX_FRAMES], ax_frame);
        write = __atomic_load_n(&s_ring_write, __ATOMIC_ACQUIRE);
        for (i = 0; i < VITA_AX_FRAME; ++i) {
            const u32 slot = (write + i) % VITA_RING_FRAMES;
            s_ring[slot * 2u] = ax_frame[i * 2u];
            s_ring[slot * 2u + 1u] = ax_frame[i * 2u + 1u];
        }
        __atomic_add_fetch(&s_ring_write, VITA_AX_FRAME, __ATOMIC_RELEASE);
        __atomic_store_n(&s_mix_read, read + 1u, __ATOMIC_RELEASE);
    }
    return 0;
}
#endif

/* Without the mixer thread: synth and mix in one go, as before. */
static void render_audio_frame(s16* output)
{
    static MixFrame frame;
    build_frame(&frame);
    mix_frame(&frame, output);
}

u8* aurora_aram_base(void) { return s_aram; }
void* ARGetStorageAddress(void) { return s_aram; }
u32 ARGetSize(void) { return MELEE_VITA_ARAM_SIZE; }
BOOL ARCheckInit(void) { return s_aram != NULL; }

u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    if (s_aram == NULL) s_aram = calloc(1, MELEE_VITA_ARAM_SIZE);
    s_aram_stack = 0x4000;
    s_aram_lengths = stack_index_addr;
    s_aram_blocks = num_entries;
    s_aram_free_blocks = num_entries;
    return s_aram != NULL ? s_aram_stack : 0;
}

u32 ARAlloc(u32 length)
{
    u32 result;
    length = (length + 31u) & ~31u;
    if (s_aram == NULL || s_aram_free_blocks == 0 ||
        length > MELEE_VITA_ARAM_SIZE - s_aram_stack) return 0;
    result = s_aram_stack;
    s_aram_stack += length;
    if (s_aram_lengths != NULL)
        s_aram_lengths[s_aram_blocks - s_aram_free_blocks] = length;
    --s_aram_free_blocks;
    return result;
}

u32 ARFree(u32* length)
{
    u32 released = 0;
    if (s_aram_free_blocks < s_aram_blocks && s_aram_lengths != NULL) {
        released = s_aram_lengths[s_aram_blocks - s_aram_free_blocks - 1u];
        if (released <= s_aram_stack - 0x4000u) s_aram_stack -= released;
        ++s_aram_free_blocks;
    }
    if (length != NULL) *length = released;
    return s_aram_stack;
}

void ARQInit(void) { s_arq_read = s_arq_write = 0; }

void ARQPostRequest(ARQRequest* request, uintptr_t owner, u32 type,
                    u32 priority, uintptr_t source, uintptr_t destination,
                    u32 length, ARQCallback callback)
{
    u32 next = (s_arq_write + 1u) % VITA_ARQ_CAPACITY;
    if (request == NULL) return;
    request->next = NULL;
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = destination;
    request->length = length;
    request->callback = callback;
    if (next == s_arq_read) return;
    s_arq[s_arq_write] = (VitaArqJob) {
        request, type, source, destination, length, callback
    };
    s_arq_write = next;
}

static void poll_arq(void)
{
    while (s_arq_read != s_arq_write) {
        VitaArqJob job = s_arq[s_arq_read];
        s_arq_read = (s_arq_read + 1u) % VITA_ARQ_CAPACITY;
        if (job.type == ARAM_DIR_MRAM_TO_ARAM) {
            if (s_aram != NULL && job.destination < MELEE_VITA_ARAM_SIZE &&
                job.length <= MELEE_VITA_ARAM_SIZE - job.destination)
                memcpy(s_aram + job.destination, (const void*) job.source,
                       job.length);
        } else if (s_aram != NULL && job.source < MELEE_VITA_ARAM_SIZE &&
                   job.length <= MELEE_VITA_ARAM_SIZE - job.source) {
            memcpy((void*) job.destination, s_aram + job.source, job.length);
        }
        if (job.callback != NULL) job.callback(job.request);
    }
}

void AXInit(void)
{
    u32 i;
    memset(s_voices, 0, sizeof(s_voices));
    for (i = 0; i < VITA_AX_VOICES; ++i) s_voices[i].voice.index = i;
    if (s_audio_port < 0) {
        s_audio_port = sceAudioOutOpenPort(
            SCE_AUDIO_OUT_PORT_TYPE_BGM,
            VITA_AUDIO_GRAIN,
            VITA_AX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
        melee_vita_log_info("[AUDIO] SceAudioOut port result: %d",
                            s_audio_port);
        if (s_audio_port >= 0 && s_audio_thread < 0) {
            s_audio_thread_running = 1;
            s_audio_thread = sceKernelCreateThread(
                "melee audio out", audio_output_thread, 0x10000100 - 20,
                0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
            if (s_audio_thread >= 0)
                sceKernelStartThread(s_audio_thread, 0, NULL);
            melee_vita_log_info("[AUDIO] output thread=%d", s_audio_thread);
        }
#ifndef TARGET_PS5
        /* Mixer: cores 1-2, below the render and GX submit threads, so it
         * only takes time they don't use. The ring has ~250 ms of slack.
         * PS5 keeps mixing on the game thread. */
        if (s_audio_thread >= 0 && s_mix_thread < 0) {
            s_mix_frames = malloc(VITA_MIX_FRAMES * sizeof(MixFrame));
            s_mix_sema = sceKernelCreateSema("melee audio mix", 0, 0, 0x7fffffff, NULL);
            if (s_mix_frames != NULL && s_mix_sema >= 0) {
                s_mix_running = 1;
                s_mix_thread = sceKernelCreateThread(
                    "melee audio mix", audio_mix_thread, 0x10000100 + 4, 0x10000, 0,
                    SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL);
                if (s_mix_thread >= 0 && sceKernelStartThread(s_mix_thread, 0, NULL) < 0) {
                    sceKernelDeleteThread(s_mix_thread);
                    s_mix_thread = -1;
                }
            }
            melee_vita_log_info("[AUDIO] mixer thread=%d", s_mix_thread);
        }
#endif
    }
}

void AXQuit(void)
{
    if (s_mix_thread >= 0) {
        __atomic_store_n(&s_mix_running, 0, __ATOMIC_RELEASE);
        sceKernelSignalSema(s_mix_sema, 1);
        sceKernelWaitThreadEnd(s_mix_thread, NULL, NULL);
        sceKernelDeleteThread(s_mix_thread);
        s_mix_thread = -1;
    }
    if (s_audio_thread >= 0) {
        __atomic_store_n(&s_audio_thread_running, 0, __ATOMIC_RELEASE);
        sceKernelWaitThreadEnd(s_audio_thread, NULL, NULL);
        sceKernelDeleteThread(s_audio_thread);
        s_audio_thread = -1;
    }
    if (s_audio_port >= 0) {
        sceAudioOutReleasePort(s_audio_port);
        s_audio_port = -1;
    }
}

/* A setter changed something the mixer needs; the address, volume and
 * state writes also make older mixer reports for them stale. */
static void touch(AXVPB* p, u16 sync)
{
    VitaVoice* voice = (VitaVoice*) p;
    voice->sync |= sync;
    if (sync & (SYNC_ADDR | SYNC_CUR)) voice->addr_frame = s_mix_next;
    if (sync & SYNC_VOL) voice->vol_frame = s_mix_next;
    if (sync & (SYNC_ADDR | SYNC_CUR | SYNC_STATE)) voice->state_frame = s_mix_next;
}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 user_context)
{
    VitaVoice* selected = NULL;
    u32 i;
    for (i = 0; i < VITA_AX_VOICES; ++i) {
        if (!s_voices[i].used) { selected = &s_voices[i]; break; }
    }
    if (selected == NULL) {
        for (i = 0; i < VITA_AX_VOICES; ++i) {
            if ((u32) s_voices[i].voice.priority < priority &&
                (selected == NULL || s_voices[i].voice.priority < selected->voice.priority))
                selected = &s_voices[i];
        }
    }
    if (selected == NULL) return NULL;
    if (selected->used && selected->voice.callback != NULL)
        selected->voice.callback(&selected->voice);
    i = selected->voice.index;
    memset(selected, 0, sizeof(*selected));
    selected->used = GX_TRUE;
    selected->voice.index = i;
    selected->voice.priority = (int) priority;
    selected->voice.callback = callback;
    selected->voice.userContext = user_context;
    touch(&selected->voice, SYNC_NEW);
    return &selected->voice;
}

void AXFreeVoice(AXVPB* voice)
{
    if (voice == NULL) return;
    ((VitaVoice*) voice)->used = GX_FALSE;
    voice->pb.state = 0;
    touch(voice, SYNC_STATE);
}

void AXRegisterCallback(void (*callback)(void)) { s_ax_callback = callback; }
void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{ s_aux_a_callback = callback; s_aux_a_context = context; }
void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{ s_aux_b_callback = callback; s_aux_b_context = context; }
void AXSetVoicePriority(AXVPB* p, u32 v) { if (p) p->priority = (int) v; }
void AXSetVoiceState(AXVPB* p, u16 v) { if (p) { p->pb.state = v; touch(p, SYNC_STATE); } }
void AXSetVoiceMix(AXVPB* p, AXPBMIX* v) { if (p && v) p->pb.mix = *v; }
void AXSetVoiceItdOn(AXVPB* p) { if (p) p->pb.itd.flag = 1; }
void AXSetVoiceItdTarget(AXVPB* p, u16 l, u16 r)
{ if (p) { p->pb.itd.targetShiftL = l; p->pb.itd.targetShiftR = r; } }
void AXSetVoiceVe(AXVPB* p, AXPBVE* v) { if (p && v) { p->pb.ve = *v; touch(p, SYNC_VOL); } }
void AXSetVoiceVeDelta(AXVPB* p, s16 v) { if (p) p->pb.ve.currentDelta = v; }
void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* value)
{
    VitaVoice* voice = (VitaVoice*) p;
    u32 i;
    if (p == NULL || value == NULL) return;
    for (i = 0; i < 8; ++i) {
        p->pb.adpcm.a[i][0] = from_be16(value->a[i][0]);
        p->pb.adpcm.a[i][1] = from_be16(value->a[i][1]);
    }
    p->pb.adpcm.gain = from_be16(value->gain);
    p->pb.adpcm.pred_scale = from_be16(value->pred_scale);
    p->pb.adpcm.yn1 = from_be16(value->yn1);
    p->pb.adpcm.yn2 = from_be16(value->yn2);
    voice->predictor_scale = p->pb.adpcm.pred_scale;
    voice->yn1 = (s16) p->pb.adpcm.yn1;
    voice->yn2 = (s16) p->pb.adpcm.yn2;
    touch(p, SYNC_ADPCM);
}
void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* value)
{
    if (p == NULL || value == NULL) return;
    p->pb.adpcmLoop.loop_pred_scale = from_be16(value->loop_pred_scale);
    p->pb.adpcmLoop.loop_yn1 = from_be16(value->loop_yn1);
    p->pb.adpcmLoop.loop_yn2 = from_be16(value->loop_yn2);
}
void AXSetVoiceSrc(AXVPB* p, AXPBSRC* value)
{
    VitaVoice* voice = (VitaVoice*) p;
    if (p == NULL || value == NULL) return;
    p->pb.src = *value;
    voice->fraction = value->currentAddressFrac;
    touch(p, SYNC_SRC);
}
void AXSetVoiceSrcRatio(AXVPB* p, float ratio)
{ if (p) { u32 fixed = (u32) (ratio * 65536.0f); p->pb.src.ratioHi = fixed >> 16; p->pb.src.ratioLo = fixed; } }

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* value)
{
    VitaVoice* voice = (VitaVoice*) p;
    if (p == NULL || value == NULL) return;
    p->pb.addr.loopFlag = from_be16(value->loopFlag);
    p->pb.addr.format = from_be16(value->format);
    p->pb.addr.loopAddressHi = from_be16(value->loopAddressHi);
    p->pb.addr.loopAddressLo = from_be16(value->loopAddressLo);
    p->pb.addr.endAddressHi = from_be16(value->endAddressHi);
    p->pb.addr.endAddressLo = from_be16(value->endAddressLo);
    p->pb.addr.currentAddressHi = from_be16(value->currentAddressHi);
    p->pb.addr.currentAddressLo = from_be16(value->currentAddressLo);
    voice->loop_address = pair(p->pb.addr.loopAddressHi,
                               p->pb.addr.loopAddressLo);
    voice->end_address = pair(p->pb.addr.endAddressHi,
                              p->pb.addr.endAddressLo);
    voice->current_address = pair(p->pb.addr.currentAddressHi,
                                  p->pb.addr.currentAddressLo);
    voice->fraction = 0;
    touch(p, SYNC_ADDR);
}

void AXSetVoiceLoop(AXVPB* p, u16 value) { if (p) p->pb.addr.loopFlag = value; }
void AXSetVoiceLoopAddr(AXVPB* p, u32 value)
{ if (p) { ((VitaVoice*) p)->loop_address = value; set_pair(&p->pb.addr.loopAddressHi, &p->pb.addr.loopAddressLo, value); touch(p, SYNC_LOOP); } }
void AXSetVoiceEndAddr(AXVPB* p, u32 value)
{ if (p) { ((VitaVoice*) p)->end_address = value; set_pair(&p->pb.addr.endAddressHi, &p->pb.addr.endAddressLo, value); touch(p, SYNC_END); } }
void AXSetVoiceCurrentAddr(AXVPB* p, u32 value)
{ if (p) { ((VitaVoice*) p)->current_address = value; set_pair(&p->pb.addr.currentAddressHi, &p->pb.addr.currentAddressLo, value); touch(p, SYNC_CUR); } }

// time spent on audio on the game thread (the synth, plus mixing when
// there's no mixer thread), for the benchmark
u64 g_melee_vita_audio_mix_total_us;

void melee_vita_audio_poll(void)
{
    static s16 output[VITA_AUDIO_GRAIN * 2] __attribute__((aligned(64)));
    static s16 ax_frame[VITA_AX_FRAME * 2] __attribute__((aligned(64)));
    static u32 ax_frame_offset = VITA_AX_FRAME;
    u32 sample;
    poll_arq();
    (void) output;
    (void) ax_frame_offset;
    (void) sample;
    if (s_audio_port >= 0 && s_audio_thread >= 0 && s_mix_thread >= 0) {
        /* Hand the mixer whole AX frames until ~250 ms is queued between
         * frames waiting and the output ring. AX's clock advances exactly
         * once per frame, so music tempo doesn't depend on the frame rate. */
        u32 rendered = 0;
        const u64 mix_start = sceKernelGetProcessTimeWide();
        static u32 last_underruns;
        for (;;) {
            const u32 queued = s_mix_write - __atomic_load_n(&s_mix_read, __ATOMIC_ACQUIRE);
            if (queued >= VITA_MIX_FRAMES || rendered >= VITA_RING_MAX_RENDER ||
                ring_fill() + (queued + 1u) * VITA_AX_FRAME > VITA_RING_TARGET)
                break;
            build_frame(&s_mix_frames[s_mix_write % VITA_MIX_FRAMES]);
            __atomic_store_n(&s_mix_write, s_mix_write + 1u, __ATOMIC_RELEASE);
            sceKernelSignalSema(s_mix_sema, 1);
            ++rendered;
        }
        apply_reports(); // finished voices show up for the game's own logic too
        {
            extern void melee_vita_prof_add(int zone, u64 us);
            const u64 mix_us = sceKernelGetProcessTimeWide() - mix_start;
            melee_vita_prof_add(10 /* audio_mix */, mix_us);
            g_melee_vita_audio_mix_total_us += mix_us;
        }
        if (s_audio_underruns != last_underruns && (s_audio_underruns % 50u) == 1u) {
            melee_vita_log_info("[AUDIO] underruns=%u", s_audio_underruns);
        }
        last_underruns = s_audio_underruns;
    } else if (s_audio_port >= 0 && s_audio_thread >= 0) {
        /* No mixer thread: mix here, as before. */
        u32 rendered = 0;
        const u64 mix_start = sceKernelGetProcessTimeWide();
        static u32 last_underruns;
        while (ring_fill() + VITA_AX_FRAME <= VITA_RING_TARGET &&
               rendered < VITA_RING_MAX_RENDER) {
            u32 write = __atomic_load_n(&s_ring_write, __ATOMIC_ACQUIRE);
            u32 i;
            render_audio_frame(ax_frame);
            for (i = 0; i < VITA_AX_FRAME; ++i) {
                const u32 slot = (write + i) % VITA_RING_FRAMES;
                s_ring[slot * 2u] = ax_frame[i * 2u];
                s_ring[slot * 2u + 1u] = ax_frame[i * 2u + 1u];
            }
            __atomic_add_fetch(&s_ring_write, VITA_AX_FRAME, __ATOMIC_RELEASE);
            ++rendered;
        }
        {
            extern void melee_vita_prof_add(int zone, u64 us);
            const u64 mix_us = sceKernelGetProcessTimeWide() - mix_start;
            extern u64 g_melee_vita_audio_mix_total_us;
            melee_vita_prof_add(10 /* audio_mix */, mix_us);
            g_melee_vita_audio_mix_total_us += mix_us;
        }
        if (s_audio_underruns != last_underruns && (s_audio_underruns % 50u) == 1u) {
            melee_vita_log_info("[AUDIO] underruns=%u", s_audio_underruns);
        }
        last_underruns = s_audio_underruns;
    } else if (s_audio_port >= 0) {
        for (sample = 0; sample < VITA_AUDIO_GRAIN; ++sample) {
            if (ax_frame_offset == VITA_AX_FRAME) {
                render_audio_frame(ax_frame);
                ax_frame_offset = 0;
            }
            output[sample * 2u] = ax_frame[ax_frame_offset * 2u];
            output[sample * 2u + 1u] = ax_frame[ax_frame_offset * 2u + 1u];
            ++ax_frame_offset;
        }
        sceAudioOutOutput(s_audio_port, output);
    } else {
        for (sample = 0; sample < VITA_AX_FALLBACK_FRAMES; ++sample)
            if (s_ax_callback != NULL) s_ax_callback();
    }
}

void melee_vita_audio_flush(void)
{
    if (s_audio_thread < 0) return;
    __atomic_store_n(&s_audio_flush_requested, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s_audio_flush_requested, __ATOMIC_ACQUIRE) != 0)
        sceKernelDelayThread(100u);
}

void melee_vita_audio_shutdown(void)
{
    AXQuit();
    free(s_aram);
    s_aram = NULL;
}

void AIInit(u8* stack) { (void) stack; }
void AISetDSPSampleRate(u32 rate) { (void) rate; }
void AISetStreamVolLeft(u8 volume) { (void) volume; }
void AISetStreamVolRight(u8 volume) { (void) volume; }

void AXFXSetHooks(void* (*alloc_hook)(size_t), void (*free_hook)(void*))
{ s_fx_alloc = alloc_hook; s_fx_free = free_hook; (void) s_fx_alloc; (void) s_fx_free; }
#define VITA_FX_INIT(name, type) int name(struct type* effect) { (void) effect; return 1; }
#define VITA_FX_STOP(name, type) int name(struct type* effect) { (void) effect; return 1; }
#define VITA_FX_CALLBACK(name, type) void name(struct AXFX_BUFFERUPDATE* update, struct type* effect) { (void) update; (void) effect; }
VITA_FX_INIT(AXFXChorusInit, AXFX_CHORUS)
VITA_FX_STOP(AXFXChorusShutdown, AXFX_CHORUS)
VITA_FX_CALLBACK(AXFXChorusCallback, AXFX_CHORUS)
VITA_FX_INIT(AXFXDelayInit, AXFX_DELAY)
VITA_FX_STOP(AXFXDelayShutdown, AXFX_DELAY)
VITA_FX_CALLBACK(AXFXDelayCallback, AXFX_DELAY)
VITA_FX_INIT(AXFXReverbHiInit, AXFX_REVERBHI)
VITA_FX_STOP(AXFXReverbHiShutdown, AXFX_REVERBHI)
VITA_FX_CALLBACK(AXFXReverbHiCallback, AXFX_REVERBHI)
VITA_FX_INIT(AXFXReverbStdInit, AXFX_REVERBSTD)
VITA_FX_STOP(AXFXReverbStdShutdown, AXFX_REVERBSTD)
VITA_FX_CALLBACK(AXFXReverbStdCallback, AXFX_REVERBSTD)
