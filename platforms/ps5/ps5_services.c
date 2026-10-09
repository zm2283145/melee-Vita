/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5 user, pad, audio and notification services.  The pad record and the
 * audio-out entry points follow the public ps5-payload-dev SDL2 port. */
#include "ps5_services.h"
#include "ps5_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct Ps5PadTouch {
    uint16_t x, y;
    uint8_t finger;
    uint8_t pad[3];
} Ps5PadTouch;

typedef struct Ps5PadData {
    uint32_t buttons;
    struct { uint8_t x, y; } left_stick, right_stick;
    struct { uint8_t l2, r2; } analog_buttons;
    uint16_t padding;
    float quat[4];
    float vel[3];
    float accel[3];
    struct {
        uint8_t fingers;
        uint8_t pad1[3];
        uint32_t pad2;
        Ps5PadTouch touch[2];
    } touch;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t count;
    uint8_t unknown[15];
} Ps5PadData;

int sceUserServiceInitialize(void*);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int scePadInit(void);
int scePadOpen(int user_id, int type, int index, void* param);
int scePadReadState(int handle, Ps5PadData* data);
int scePadClose(int handle);
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq,
                    uint32_t param);
int sceAudioOutOutput(int handle, const void* p);
int sceAudioOutClose(int handle);
#ifdef MELEE_PS5_NO_NOTIFY
/* Importing libSceNotification makes a sandboxed title crash at load. */
static int sceNotificationSend(int u, bool l, const char* p) { (void) u; (void) l; (void) p; return 0; }
#else
int sceNotificationSend(int user_id, bool is_logged, const char* payload);
#endif

#define PS5_USER_ID_INVALID (-1)
#define PS5_AUDIO_USER_SYSTEM 0xFF
#define PS5_AUDIO_PORT_MAIN 0
#define PS5_AUDIO_S16_STEREO 1
#define PS5_PAD_TOUCH_PAD 0x100000u
#define PS5_PAD_INTERCEPTED 0x80000000u

typedef struct PadSlot {
    int user_id;
    int handle;
} PadSlot;

static PadSlot s_pads[MELEE_PS5_MAX_PADS];
static bool s_pad_ready;
static unsigned s_detect_tick;

static void detect_pads(void)
{
    int users[4];
    for (int i = 0; i < 4; ++i) users[i] = PS5_USER_ID_INVALID;
    if (sceUserServiceGetLoginUserIdList(users) != 0) return;
    /* Close pads whose user logged out. */
    for (int slot = 0; slot < MELEE_PS5_MAX_PADS; ++slot) {
        bool present = false;
        if (s_pads[slot].user_id == PS5_USER_ID_INVALID) continue;
        for (int u = 0; u < 4; ++u) present |= users[u] == s_pads[slot].user_id;
        if (!present) {
            if (s_pads[slot].handle >= 0) scePadClose(s_pads[slot].handle);
            s_pads[slot].user_id = PS5_USER_ID_INVALID;
            s_pads[slot].handle = -1;
        }
    }
    /* Give new users the first free slot, in login order. */
    for (int u = 0; u < 4; ++u) {
        bool known = false;
        if (users[u] == PS5_USER_ID_INVALID) continue;
        for (int slot = 0; slot < MELEE_PS5_MAX_PADS; ++slot)
            known |= s_pads[slot].user_id == users[u];
        if (known) continue;
        for (int slot = 0; slot < MELEE_PS5_MAX_PADS; ++slot) {
            if (s_pads[slot].user_id != PS5_USER_ID_INVALID) continue;
            s_pads[slot].handle = scePadOpen(users[u], 0, 0, NULL);
            if (s_pads[slot].handle >= 0) {
                s_pads[slot].user_id = users[u];
                melee_ps5_log("pad %d: user 0x%08x handle %d", slot, users[u],
                              s_pads[slot].handle);
            } else {
                melee_ps5_log("scePadOpen(0x%08x) failed: 0x%08x", users[u],
                              (unsigned) s_pads[slot].handle);
                s_pads[slot].handle = -1;
            }
            break;
        }
    }
}

int melee_ps5_services_init(void)
{
    int result = sceUserServiceInitialize(NULL);
    if (result != 0 && result != (int) 0x80960003) {
        melee_ps5_log("sceUserServiceInitialize failed: 0x%08x", (unsigned) result);
        return result;
    }
    result = scePadInit();
    if (result != 0) {
        melee_ps5_log("scePadInit failed: 0x%08x", (unsigned) result);
        return result;
    }
    result = sceAudioOutInit();
    if (result != 0 && result != (int) 0x8026000e /* already initialised */)
        melee_ps5_log("sceAudioOutInit: 0x%08x", (unsigned) result);
    for (int i = 0; i < MELEE_PS5_MAX_PADS; ++i) {
        s_pads[i].user_id = PS5_USER_ID_INVALID;
        s_pads[i].handle = -1;
    }
    s_pad_ready = true;
    detect_pads();
    return 0;
}

bool melee_ps5_pad_read(int index, MeleePs5PadState* state)
{
    Ps5PadData data;
    if (!s_pad_ready || index < 0 || index >= MELEE_PS5_MAX_PADS) return false;
    /* Users log in and out at any time; look for them twice a second. */
    if (index == 0 && (++s_detect_tick % 30u) == 0u) detect_pads();
    if (s_pads[index].handle < 0) return false;
    memset(&data, 0, sizeof(data));
    if (scePadReadState(s_pads[index].handle, &data) != 0 || !data.connected) return false;
    /* The system menu owns the pad while it is open. */
    if (data.buttons & PS5_PAD_INTERCEPTED) data.buttons = 0;
    state->buttons = data.buttons & 0xffffu & ~0x1u;
    /* Touch pad click: right half is START, left half SELECT (the PS TV's
     * DualShock 4 layout the Vita build was tuned with). */
    if (data.buttons & PS5_PAD_TOUCH_PAD) {
        const bool right = data.touch.fingers > 0 ? data.touch.touch[0].x >= 960u : true;
        state->buttons |= right ? 0x8u /* START */ : 0x1u /* SELECT */;
    }
    {
        static uint32_t last_raw;
        static unsigned logged;
        if (data.buttons != last_raw && logged < 200u) {
            ++logged;
            melee_ps5_log("[PAD] raw buttons 0x%08x touch=%u x=%u", (unsigned) data.buttons,
                          (unsigned) data.touch.fingers, (unsigned) data.touch.touch[0].x);
        }
        last_raw = data.buttons;
    }
    {
        /* L3 + R3 together: save a screenshot (bring-up aid). */
        static bool held;
        const bool both = (data.buttons & 0x6u) == 0x6u;
        if (both && !held) {
            extern void melee_ps5_request_screenshot(void);
            melee_ps5_request_screenshot();
        }
        held = both;
    }
    state->lx = data.left_stick.x;
    state->ly = data.left_stick.y;
    state->rx = data.right_stick.x;
    state->ry = data.right_stick.y;
    state->l2 = data.analog_buttons.l2;
    state->r2 = data.analog_buttons.r2;
    return true;
}

int melee_ps5_audio_open(uint32_t frames_per_grain)
{
    const int handle = sceAudioOutOpen(PS5_AUDIO_USER_SYSTEM, PS5_AUDIO_PORT_MAIN, 0,
                                       frames_per_grain, 48000, PS5_AUDIO_S16_STEREO);
    melee_ps5_log("sceAudioOutOpen(%u frames @ 48 kHz) = 0x%08x", frames_per_grain,
                  (unsigned) handle);
    return handle > 0 ? handle : -1;
}

int melee_ps5_audio_output(int handle, const int16_t* stereo)
{
    return sceAudioOutOutput(handle, stereo);
}

void melee_ps5_audio_close(int handle)
{
    if (handle > 0) sceAudioOutClose(handle);
}

void melee_ps5_notify(const char* message)
{
    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
             "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
             "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
             "\"viewData\":{\"message\":{\"body\":\"%s\"}},"
             "\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"message\":"
             "{\"body\":\"%s\"}}}}},\"createdDateTime\":\"2026-01-01T00:00:00.000Z\","
             "\"localNotificationId\":\"588194\"}",
             message, message);
    sceNotificationSend(0xFE, true, payload);
}
