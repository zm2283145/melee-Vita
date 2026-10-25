/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita controls presented through the GameCube PAD API. */
#include <dolphin/pad.h>

#include "pad_vita.h"
#include "../vita_log.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/touch.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define VITA_PAD_CONFIG_PATH "ux0:data/melee/control-settings.bin"
#define VITA_PAD_CONFIG_TEMP_PATH "ux0:data/melee/control-settings.tmp"
#define VITA_PAD_CONFIG_MAGIC 0x31435056u
#define VITA_PAD_CONFIG_VERSION 2u
#define VITA_PAD_CONFIG_VERSION_LEGACY 1u
#define VITA_STICK_CAL_PATH "ux0:data/melee/sticks.txt"
#define VITA_STICK_CAL_TEMP_PATH "ux0:data/melee/sticks.tmp"

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
static MeleeVitaStickCal s_stick_cal[MELEE_VITA_STICK_COUNT];
static bool s_stick_cal_on; // false: never calibrated, sticks go through raw as before
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

/* Stick calibration. A stick that rests off centre or doesn't reach the
 * edge gets moved back to centre and stretched to full range, then a
 * radial deadzone zeroes out small drift. Melee itself clamps to a radius
 * of 80, so anything past that is full tilt. */
void melee_vita_pad_stick_cal_defaults(MeleeVitaStickCal* cal)
{
    cal->center_x = cal->center_y = 128;
    cal->min_x = cal->min_y = 0;
    cal->max_x = cal->max_y = 255;
    cal->dead = 0;
}

static void stick_cal_update_on(void)
{
    MeleeVitaStickCal def;
    int i;
    melee_vita_pad_stick_cal_defaults(&def);
    s_stick_cal_on = false;
    for (i = 0; i < MELEE_VITA_STICK_COUNT; ++i)
        if (memcmp(&s_stick_cal[i], &def, sizeof(def)) != 0)
            s_stick_cal_on = true;
}

static bool stick_cal_valid(const MeleeVitaStickCal* cal)
{
    return cal->min_x >= 0 && cal->min_x < cal->center_x && cal->center_x < cal->max_x &&
           cal->max_x <= 255 && cal->min_y >= 0 && cal->min_y < cal->center_y &&
           cal->center_y < cal->max_y && cal->max_y <= 255 && cal->dead >= 0 && cal->dead <= 50;
}

#ifndef TARGET_PS5
static void stick_cal_load(void)
{
    static const char* const names[MELEE_VITA_STICK_COUNT] = { "left", "right" };
    char line[128];
    FILE* file;
    int i;

    for (i = 0; i < MELEE_VITA_STICK_COUNT; ++i)
        melee_vita_pad_stick_cal_defaults(&s_stick_cal[i]);
    file = fopen(VITA_STICK_CAL_PATH, "r");
    if (file == NULL)
        return;
    while (fgets(line, sizeof(line), file)) {
        char name[16];
        MeleeVitaStickCal cal;
        if (sscanf(line, "%15[a-z]=%d %d %d %d %d %d %d", name, &cal.center_x, &cal.center_y,
                   &cal.min_x, &cal.max_x, &cal.min_y, &cal.max_y, &cal.dead) != 8)
            continue;
        for (i = 0; i < MELEE_VITA_STICK_COUNT; ++i)
            if (!strcmp(name, names[i]) && stick_cal_valid(&cal))
                s_stick_cal[i] = cal;
    }
    fclose(file);
    stick_cal_update_on();
}
#endif

static bool stick_cal_save(void)
{
    static const char* const names[MELEE_VITA_STICK_COUNT] = { "left", "right" };
    FILE* file = fopen(VITA_STICK_CAL_TEMP_PATH, "w");
    bool valid = file != NULL;
    int i;
    if (valid) {
        fprintf(file, "# stick calibration, raw 0-255: center_x center_y min_x max_x "
                      "min_y max_y deadzone_percent\n");
        for (i = 0; i < MELEE_VITA_STICK_COUNT; ++i) {
            const MeleeVitaStickCal* c = &s_stick_cal[i];
            fprintf(file, "%s=%d %d %d %d %d %d %d\n", names[i], c->center_x, c->center_y,
                    c->min_x, c->max_x, c->min_y, c->max_y, c->dead);
        }
        valid = fclose(file) == 0;
    }
    if (valid) {
        remove(VITA_STICK_CAL_PATH);
        valid = rename(VITA_STICK_CAL_TEMP_PATH, VITA_STICK_CAL_PATH) == 0;
    }
    if (!valid) {
        remove(VITA_STICK_CAL_TEMP_PATH);
        melee_vita_log_info("[PAD] failed to persist stick calibration");
    }
    return valid;
}

void melee_vita_pad_get_stick_cal(int stick, MeleeVitaStickCal* cal)
{
    if (stick < 0 || stick >= MELEE_VITA_STICK_COUNT) {
        melee_vita_pad_stick_cal_defaults(cal);
        return;
    }
    *cal = s_stick_cal[stick];
}

bool melee_vita_pad_replace_stick_cal(const MeleeVitaStickCal cal[MELEE_VITA_STICK_COUNT])
{
    int i;
    for (i = 0; i < MELEE_VITA_STICK_COUNT; ++i)
        if (!stick_cal_valid(&cal[i]))
            return false;
    memcpy(s_stick_cal, cal, sizeof(s_stick_cal));
    stick_cal_update_on();
    return stick_cal_save();
}

// one axis: offset from centre, stretched so the measured edge is 127
static float stick_cal_axis(int value, int center, int lo, int hi)
{
    const int offset = value - center;
    const int span = offset >= 0 ? hi - center : center - lo;
    float out = (float) offset * 127.0f / (float) (span > 0 ? span : 127);
    return out < -127.0f ? -127.0f : out > 127.0f ? 127.0f : out;
}

void melee_vita_pad_stick_apply(const MeleeVitaStickCal* cal, int raw_x, int raw_y,
                                int8_t* x, int8_t* y)
{
    float fx = stick_cal_axis(raw_x, cal->center_x, cal->min_x, cal->max_x);
    // Vita y grows downwards, GameCube y grows upwards
    float fy = -stick_cal_axis(raw_y, cal->center_y, cal->min_y, cal->max_y);
    const float r = sqrtf(fx * fx + fy * fy);
    const float dead = (float) cal->dead * 127.0f / 100.0f;
    if (r <= dead) {
        *x = *y = 0;
        return;
    }
    if (dead > 0.0f) {
        // rescale so the edge of the deadzone starts at 0, not with a jump
        const float k = (r - dead) / r * 127.0f / (127.0f - dead);
        fx *= k;
        fy *= k;
    }
    fx = fx < -127.0f ? -127.0f : fx > 127.0f ? 127.0f : fx;
    fy = fy < -127.0f ? -127.0f : fy > 127.0f ? 127.0f : fy;
    *x = (int8_t) lroundf(fx);
    *y = (int8_t) lroundf(fy);
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
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
        if (s_is_pstv)
            sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
        s_touch_ready =
            sceTouchSetSamplingState(
                SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START) >= 0 &&
            sceTouchGetPanelInfo(
                SCE_TOUCH_PORT_FRONT, &s_touch_panel) >= 0;
        pad_config_load();
#ifndef TARGET_PS5
        stick_cal_load(); // PS5 has no Settings screen, its sticks stay raw
#endif
        s_initialized = TRUE;
    }
    return TRUE;
}

/* GC status for one controller from a Vita/PS TV controller sample.
 * cal: stick calibration for this controller, or NULL for raw sticks. */
static void fill_status(PADStatus* status, const SceCtrlData* vita, const SceCtrlData* analog,
                        const MeleeVitaStickCal* cal)
{
    int i;
    status->err = PAD_ERR_NONE;
    if (cal != NULL) {
        const SceCtrlData* src = analog != NULL ? analog : vita;
        melee_vita_pad_stick_apply(&cal[MELEE_VITA_STICK_LEFT], src->lx, src->ly,
                                   &status->stickX, &status->stickY);
        melee_vita_pad_stick_apply(&cal[MELEE_VITA_STICK_RIGHT], src->rx, src->ry,
                                   &status->substickX, &status->substickY);
    } else if (analog != NULL) {
        status->stickX = stick_axis(analog->lx);
        status->stickY = stick_axis_inverted(analog->ly);
        status->substickX = stick_axis(analog->rx);
        status->substickY = stick_axis_inverted(analog->ry);
    } else {
        status->stickX = stick_axis(vita->lx);
        status->stickY = stick_axis_inverted(vita->ly);
        status->substickX = stick_axis(vita->rx);
        status->substickY = stick_axis_inverted(vita->ry);
    }

    if (vita->buttons & SCE_CTRL_LEFT) status->button |= PAD_BUTTON_LEFT;
    if (vita->buttons & SCE_CTRL_RIGHT) status->button |= PAD_BUTTON_RIGHT;
    if (vita->buttons & SCE_CTRL_DOWN) status->button |= PAD_BUTTON_DOWN;
    if (vita->buttons & SCE_CTRL_UP) status->button |= PAD_BUTTON_UP;
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
            (vita->buttons & physical_button) == 0)
            continue;
        if (s_mapping[i] >= MELEE_VITA_ACTION_COUNT)
            continue;
        action_button = s_action_buttons[s_mapping[i]];
        status->button |= action_button;
        if (action_button == PAD_BUTTON_A)
            status->analogA = 255;
        if (action_button == PAD_BUTTON_B)
            status->analogB = 255;
        if (action_button == PAD_TRIGGER_L)
            status->triggerLeft = 255;
        if (action_button == PAD_TRIGGER_R)
            status->triggerRight = 255;
    }
}

u32 PADRead(PADStatus* status)
{
    SceCtrlData vita;
    SceCtrlData analog;
    int read;
    int analog_read = 0;
    int i;
    if (!s_initialized) PADInit();
    memset(status, 0, sizeof(*status) * PAD_MAX_CONTROLLERS);
    for (i = 1; i < PAD_MAX_CONTROLLERS; ++i)
        status[i].err = PAD_ERR_NO_CONTROLLER;

    update_touch();
    memset(&vita, 0, sizeof(vita));
    if (s_is_pstv) {
        /* Read the wireless axes through the standard controller API.
         * Ext2 is still needed for L2/R2/L3/R3 button mapping. */
        memset(&analog, 0, sizeof(analog));
        analog_read = sceCtrlPeekBufferPositive2(1, &analog, 1);
        read = sceCtrlPeekBufferPositiveExt2(1, &vita, 1);
        if (read < 1) {
            read = sceCtrlPeekBufferPositiveExt2(0, &vita, 1);
            if (analog_read < 1)
                analog_read = sceCtrlPeekBufferPositive2(0, &analog, 1);
        }
    } else {
        read = sceCtrlPeekBufferPositive(0, &vita, 1);
    }
    if (read < 1) {
        s_raw_buttons = 0;
        status[0].err = PAD_ERR_NOT_READY;
        return 0;
    }

    s_raw_buttons_triggered |= vita.buttons & ~s_raw_buttons;
    s_raw_buttons = vita.buttons;
#ifdef TARGET_PS5
    fill_status(&status[0], &vita, s_is_pstv && analog_read > 0 ? &analog : NULL, NULL);
#else
    fill_status(&status[0], &vita, s_is_pstv && analog_read > 0 ? &analog : NULL,
                s_stick_cal_on ? s_stick_cal : NULL);
#endif
#ifdef TARGET_PS5
    /* Couch play: PS5 pads 2-4 (in login order) drive GC ports 2-4.  They
     * share the remapping; menus listen to the first pad's raw buttons. */
    for (i = 1; i < PAD_MAX_CONTROLLERS; ++i) {
        SceCtrlData extra;
        memset(&extra, 0, sizeof(extra));
        if (sceCtrlPeekBufferPositiveExt2(i + 1, &extra, 1) < 1) continue;
        fill_status(&status[i], &extra, &extra, NULL);
    }
#endif
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

// LiveArea settings screen: save a whole mapping at once
bool melee_vita_pad_replace_mapping(const uint8_t mapping[MELEE_VITA_BUTTON_COUNT])
{
    if (!melee_vita_pad_mapping_valid(mapping))
        return false;
    memcpy(s_mapping, mapping, sizeof(s_mapping));
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
