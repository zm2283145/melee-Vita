/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Initial Vita ARAM/AX backend. It advances the game's audio engine silently
 * until the SceAudioOut mixer is connected. */
#include "vita_platform.h"

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/axfx.h>

#include <stdlib.h>
#include <string.h>

#define VITA_AX_VOICES 64
#define VITA_ARQ_CAPACITY 128

typedef struct VitaVoice {
    AXVPB voice;
    GXBool used;
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

static u16 from_be16(u16 value) { return __builtin_bswap16(value); }
static u32 pair(u16 high, u16 low) { return (u32) high << 16 | low; }
static void set_pair(u16* high, u16* low, u32 value)
{ *high = (u16) (value >> 16); *low = (u16) value; }

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
    return &selected->voice;
}

void AXFreeVoice(AXVPB* voice)
{
    if (voice == NULL) return;
    ((VitaVoice*) voice)->used = GX_FALSE;
    voice->pb.state = 0;
}

void AXRegisterCallback(void (*callback)(void)) { s_ax_callback = callback; }
void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{ s_aux_a_callback = callback; s_aux_a_context = context; }
void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{ s_aux_b_callback = callback; s_aux_b_context = context; }
void AXSetVoicePriority(AXVPB* p, u32 v) { if (p) p->priority = (int) v; }
void AXSetVoiceState(AXVPB* p, u16 v) { if (p) p->pb.state = v; }
void AXSetVoiceMix(AXVPB* p, AXPBMIX* v) { if (p && v) p->pb.mix = *v; }
void AXSetVoiceItdOn(AXVPB* p) { if (p) p->pb.itd.flag = 1; }
void AXSetVoiceItdTarget(AXVPB* p, u16 l, u16 r)
{ if (p) { p->pb.itd.targetShiftL = l; p->pb.itd.targetShiftR = r; } }
void AXSetVoiceVe(AXVPB* p, AXPBVE* v) { if (p && v) p->pb.ve = *v; }
void AXSetVoiceVeDelta(AXVPB* p, s16 v) { if (p) p->pb.ve.currentDelta = v; }
void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* v) { if (p && v) p->pb.adpcm = *v; }
void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* v) { if (p && v) p->pb.adpcmLoop = *v; }
void AXSetVoiceSrc(AXVPB* p, AXPBSRC* v) { if (p && v) p->pb.src = *v; }
void AXSetVoiceSrcRatio(AXVPB* p, float ratio)
{ if (p) { u32 fixed = (u32) (ratio * 65536.0f); p->pb.src.ratioHi = fixed >> 16; p->pb.src.ratioLo = fixed; } }

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* value)
{
    if (p == NULL || value == NULL) return;
    p->pb.addr.loopFlag = from_be16(value->loopFlag);
    p->pb.addr.format = from_be16(value->format);
    p->pb.addr.loopAddressHi = from_be16(value->loopAddressHi);
    p->pb.addr.loopAddressLo = from_be16(value->loopAddressLo);
    p->pb.addr.endAddressHi = from_be16(value->endAddressHi);
    p->pb.addr.endAddressLo = from_be16(value->endAddressLo);
    p->pb.addr.currentAddressHi = from_be16(value->currentAddressHi);
    p->pb.addr.currentAddressLo = from_be16(value->currentAddressLo);
}

void AXSetVoiceLoop(AXVPB* p, u16 value) { if (p) p->pb.addr.loopFlag = value; }
void AXSetVoiceLoopAddr(AXVPB* p, u32 value) { if (p) set_pair(&p->pb.addr.loopAddressHi, &p->pb.addr.loopAddressLo, value); }
void AXSetVoiceEndAddr(AXVPB* p, u32 value) { if (p) set_pair(&p->pb.addr.endAddressHi, &p->pb.addr.endAddressLo, value); }
void AXSetVoiceCurrentAddr(AXVPB* p, u32 value) { if (p) set_pair(&p->pb.addr.currentAddressHi, &p->pb.addr.currentAddressLo, value); }

static void advance_voices(void)
{
    u32 i;
    for (i = 0; i < VITA_AX_VOICES; ++i) {
        AXVPB* voice = &s_voices[i].voice;
        u32 current, end, loop, ratio, advance;
        if (!s_voices[i].used || voice->pb.state == 0) continue;
        current = pair(voice->pb.addr.currentAddressHi, voice->pb.addr.currentAddressLo);
        end = pair(voice->pb.addr.endAddressHi, voice->pb.addr.endAddressLo);
        loop = pair(voice->pb.addr.loopAddressHi, voice->pb.addr.loopAddressLo);
        ratio = pair(voice->pb.src.ratioHi, voice->pb.src.ratioLo);
        advance = (160u * (ratio != 0 ? ratio : 0x10000u)) >> 16;
        current += advance != 0 ? advance : 1;
        if (current >= end) {
            if (voice->pb.addr.loopFlag) current = loop;
            else voice->pb.state = 0;
        }
        set_pair(&voice->pb.addr.currentAddressHi,
                 &voice->pb.addr.currentAddressLo, current);
    }
}

void melee_vita_audio_poll(void)
{
    u32 i;
    poll_arq();
    for (i = 0; i < 3; ++i) {
        advance_voices();
        if (s_ax_callback != NULL) s_ax_callback();
    }
}

void melee_vita_audio_shutdown(void)
{
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
