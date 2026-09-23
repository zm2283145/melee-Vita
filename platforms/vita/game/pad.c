/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita controls presented through the GameCube PAD API. */
#include <dolphin/pad.h>

#include "pad_vita.h"
#include "../vita_log.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/touch.h>

#include <stdio.h>
#include <string.h>

#define VITA_PAD_CONFIG_PATH "ux0:data/melee/control-settings.bin"
#define VITA_PAD_CONFIG_TEMP_PATH "ux0:data/melee/control-settings.tmp"
#define VITA_PAD_CONFIG_MAGIC 0x31435056u
#define VITA_PAD_CONFIG_VERSION 2u
#define VITA_PAD_CONFIG_VERSION_LEGACY 1u

typedef struct MeleeVitaPadConfig {
    u32 magic;
    u32 version;
    u8 mapping[MELEE_VITA_BUTTON_COUNT];
} MeleeVitaPadConfig;

typedef struct MeleeVitaPadConfigV1 {
    u32 magic;
    u32 version;
    u8 mapping[MELEE_VITA_LEGACY_BUTTON_COUNT];
} MeleeVitaPadConfigV1;

static BOOL s_initialized;
static bool s_is_pstv;
static uint8_t s_mapping[MELEE_VITA_BUTTON_COUNT];
static uint32_t s_raw_buttons;
static uint32_t s_raw_buttons_triggered;
static SceTouchPanelInfo s_touch_panel;
static int s_touch_ready;
static int s_touch_down;
static int s_touch_triggered;
static int s_touch_x;
static int s_touch_y;

static const uint32_t s_physical_buttons[MELEE_VITA_BUTTON_COUNT] = {
    SCE_CTRL_CROSS,
    SCE_CTRL_CIRCLE,
    SCE_CTRL_SQUARE,
    SCE_CTRL_TRIANGLE,
    SCE_CTRL_L1,
    SCE_CTRL_R1,
    SCE_CTRL_SELECT,
    SCE_CTRL_START,
    SCE_CTRL_L2,
    SCE_CTRL_R2,
    SCE_CTRL_L3,
    SCE_CTRL_R3,
};

static const u16 s_action_buttons[MELEE_VITA_ACTION_COUNT] = {
    PAD_BUTTON_A,
    PAD_BUTTON_B,
    PAD_BUTTON_X,
    PAD_BUTTON_Y,
    PAD_TRIGGER_L,
    PAD_TRIGGER_R,
    PAD_TRIGGER_Z,
    PAD_BUTTON_START,
};

static void pad_config_save(void);

static void pad_config_load(void)
{
    MeleeVitaPadConfig config;
    MeleeVitaPadConfigV1 legacy;
    FILE* file;
    bool valid;

    melee_vita_pad_mapping_defaults(s_mapping);
    file = fopen(VITA_PAD_CONFIG_PATH, "rb");
    if (file == NULL)
        return;
    memset(&config, 0, sizeof(config));
    valid = fread(&config.magic, sizeof(config.magic), 1u, file) == 1u &&
            fread(&config.version, sizeof(config.version), 1u, file) == 1u &&
            config.magic == VITA_PAD_CONFIG_MAGIC;
    if (valid && config.version == VITA_PAD_CONFIG_VERSION) {
        valid = fread(
                    config.mapping, sizeof(config.mapping), 1u, file) == 1u &&
                fgetc(file) == EOF &&
                melee_vita_pad_mapping_valid(config.mapping);
    } else if (valid &&
               config.version == VITA_PAD_CONFIG_VERSION_LEGACY)
    {
        legacy.magic = config.magic;
        legacy.version = config.version;
        valid = fread(
                    legacy.mapping, sizeof(legacy.mapping), 1u, file) == 1u &&
                fgetc(file) == EOF &&
                melee_vita_pad_mapping_migrate_v1(
                    config.mapping, legacy.mapping);
    } else {
        valid = false;
    }
    fclose(file);
    if (!valid) {
        melee_vita_log_info(
            "[PAD] ignored malformed control settings; using defaults");
        return;
    }
    memcpy(s_mapping, config.mapping, sizeof(s_mapping));
    if (config.version == VITA_PAD_CONFIG_VERSION_LEGACY)
        pad_config_save();
}

static void pad_config_save(void)
{
    MeleeVitaPadConfig config;
    FILE* file = fopen(VITA_PAD_CONFIG_TEMP_PATH, "wb");
    bool valid = file != NULL;
    config.magic = VITA_PAD_CONFIG_MAGIC;
    config.version = VITA_PAD_CONFIG_VERSION;
    memcpy(config.mapping, s_mapping, sizeof(config.mapping));
    if (valid)
        valid = fwrite(&config, sizeof(config), 1u, file) == 1u;
    if (file != NULL && fclose(file) != 0)
        valid = false;
    if (valid) {
        remove(VITA_PAD_CONFIG_PATH);
        valid = rename(VITA_PAD_CONFIG_TEMP_PATH, VITA_PAD_CONFIG_PATH) == 0;
    }
    if (!valid) {
        remove(VITA_PAD_CONFIG_TEMP_PATH);
        melee_vita_log_info("[PAD] failed to persist control settings");
    }
}

static s8 stick_axis(u8 value)
{
    int converted = (int) value - 128;
    return (s8) (converted < -127 ? -127 : converted);
}

static s8 stick_axis_inverted(u8 value)
{
    int converted = 128 - (int) value;
    return (s8) (converted > 127 ? 127 : converted);
}

static void update_touch(void)
{
    SceTouchData touch;
    int down;

    if (!s_touch_ready)
        return;
    memset(&touch, 0, sizeof(touch));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 1) {
        s_touch_down = 0;
        return;
    }
    down = touch.reportNum > 0;
    if (down && !s_touch_down) {
        int const width =
            s_touch_panel.maxDispX - s_touch_panel.minDispX;
        int const height =
            s_touch_panel.maxDispY - s_touch_panel.minDispY;
        if (width > 0 && height > 0) {
            s_touch_x =
                ((int) touch.report[0].x - s_touch_panel.minDispX) * 960 /
                width;
            s_touch_y =
                ((int) touch.report[0].y - s_touch_panel.minDispY) * 544 /
                height;
            if (s_touch_x < 0)
                s_touch_x = 0;
            if (s_touch_x > 959)
                s_touch_x = 959;
            if (s_touch_y < 0)
                s_touch_y = 0;
            if (s_touch_y > 543)
                s_touch_y = 543;
            s_touch_triggered = 1;
        }
    }
    s_touch_down = down;
}

BOOL PADInit(void)
{
    if (!s_initialized) {
        s_is_pstv = sceKernelIsPSVitaTV() > 0;
        if (s_is_pstv) {
            sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
        } else {
            sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
        }
        s_touch_ready =
            sceTouchSetSamplingState(
                SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START) >= 0 &&
            sceTouchGetPanelInfo(
                SCE_TOUCH_PORT_FRONT, &s_touch_panel) >= 0;
        pad_config_load();
        s_initialized = TRUE;
    }
    return TRUE;
}

u32 PADRead(PADStatus* status)
{
    SceCtrlData vita;
    int read;
    int i;
    if (!s_initialized) PADInit();
    memset(status, 0, sizeof(*status) * PAD_MAX_CONTROLLERS);
    for (i = 1; i < PAD_MAX_CONTROLLERS; ++i)
        status[i].err = PAD_ERR_NO_CONTROLLER;

    update_touch();
    memset(&vita, 0, sizeof(vita));
    read = s_is_pstv
               ? sceCtrlPeekBufferPositiveExt2(0, &vita, 1)
               : sceCtrlPeekBufferPositive(0, &vita, 1);
    if (read < 1) {
        s_raw_buttons = 0;
        status[0].err = PAD_ERR_NOT_READY;
        return 0;
    }

    s_raw_buttons_triggered |= vita.buttons & ~s_raw_buttons;
    s_raw_buttons = vita.buttons;
    status[0].err = PAD_ERR_NONE;
    status[0].stickX = stick_axis(vita.lx);
    status[0].stickY = stick_axis_inverted(vita.ly);
    status[0].substickX = stick_axis(vita.rx);
    status[0].substickY = stick_axis_inverted(vita.ry);

    if (vita.buttons & SCE_CTRL_LEFT) status[0].button |= PAD_BUTTON_LEFT;
    if (vita.buttons & SCE_CTRL_RIGHT) status[0].button |= PAD_BUTTON_RIGHT;
    if (vita.buttons & SCE_CTRL_DOWN) status[0].button |= PAD_BUTTON_DOWN;
    if (vita.buttons & SCE_CTRL_UP) status[0].button |= PAD_BUTTON_UP;
    for (i = 0; i < MELEE_VITA_BUTTON_COUNT; ++i) {
        uint32_t physical_button = s_physical_buttons[i];
        u16 action_button;
        if (!s_is_pstv) {
            if (i == MELEE_VITA_BUTTON_L)
                physical_button = SCE_CTRL_LTRIGGER;
            if (i == MELEE_VITA_BUTTON_R)
                physical_button = SCE_CTRL_RTRIGGER;
            if (i >= MELEE_VITA_BUTTON_L2)
                physical_button = 0;
        }
        if (physical_button == 0 ||
            (vita.buttons & physical_button) == 0)
            continue;
        if (s_mapping[i] >= MELEE_VITA_ACTION_COUNT)
            continue;
        action_button = s_action_buttons[s_mapping[i]];
        status[0].button |= action_button;
        if (action_button == PAD_BUTTON_A)
            status[0].analogA = 255;
        if (action_button == PAD_BUTTON_B)
            status[0].analogB = 255;
        if (action_button == PAD_TRIGGER_L)
            status[0].triggerLeft = 255;
        if (action_button == PAD_TRIGGER_R)
            status[0].triggerRight = 255;
    }
    return 0;
}

uint32_t melee_vita_pad_raw_buttons_triggered(void)
{
    uint32_t const buttons = s_raw_buttons_triggered;
    s_raw_buttons_triggered = 0;
    return buttons;
}

uint32_t melee_vita_pad_raw_buttons_held(void)
{
    return s_raw_buttons;
}

uint32_t melee_vita_pad_primary_shoulder_mask(void)
{
    return s_is_pstv
               ? SCE_CTRL_L1 | SCE_CTRL_R1
               : SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
}

bool melee_vita_pad_touch_triggered(int* x, int* y)
{
    if (!s_touch_triggered || x == NULL || y == NULL)
        return false;
    *x = s_touch_x;
    *y = s_touch_y;
    s_touch_triggered = 0;
    return true;
}

int melee_vita_pad_get_mapping(int physical_button)
{
    if (physical_button < 0 || physical_button >= MELEE_VITA_BUTTON_COUNT)
        return -1;
    return s_mapping[physical_button];
}

int melee_vita_pad_get_physical_button_for_action(int action)
{
    int physical_button;
    if (action < 0 || action >= MELEE_VITA_ACTION_COUNT)
        return -1;
    for (physical_button = 0;
         physical_button < MELEE_VITA_BUTTON_COUNT; ++physical_button)
    {
        if (s_mapping[physical_button] == action)
            return physical_button;
    }
    return -1;
}

int melee_vita_pad_physical_button_from_raw(uint32_t raw_buttons)
{
    int physical_button;
    for (physical_button = 0;
         physical_button < MELEE_VITA_BUTTON_COUNT; ++physical_button)
    {
        uint32_t mask = s_physical_buttons[physical_button];
        if (!s_is_pstv) {
            if (physical_button == MELEE_VITA_BUTTON_L)
                mask = SCE_CTRL_LTRIGGER;
            if (physical_button == MELEE_VITA_BUTTON_R)
                mask = SCE_CTRL_RTRIGGER;
            if (physical_button >= MELEE_VITA_BUTTON_L2)
                mask = 0;
        }
        if (mask != 0u && (raw_buttons & mask) != 0u)
            return physical_button;
    }
    return -1;
}

bool melee_vita_pad_set_mapping(int physical_button, int action)
{
    if (!melee_vita_pad_mapping_assign(s_mapping, physical_button, action))
        return false;
    pad_config_save();
    return true;
}

void melee_vita_pad_reset_mapping(void)
{
    melee_vita_pad_mapping_defaults(s_mapping);
    pad_config_save();
}

BOOL PADReset(u32 mask) { (void) mask; return TRUE; }
BOOL PADRecalibrate(u32 mask) { (void) mask; return TRUE; }
void PADClamp(PADStatus* status) { (void) status; }
void PADClampCircle(PADStatus* status) { (void) status; }
void PADControlMotor(u32 channel, u32 command) { (void) channel; (void) command; }
void PADSetSpec(u32 spec) { (void) spec; }
void PADSetAnalogMode(u32 mode) { (void) mode; }
void PADSetSamplingRate(u32 milliseconds) { (void) milliseconds; }

void PADControlAllMotors(const u32* commands)
{
    (void) commands;
}
