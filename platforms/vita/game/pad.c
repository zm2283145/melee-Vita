/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita controls presented through the GameCube PAD API. */
#include <dolphin/pad.h>

#include <psp2/ctrl.h>

#include <string.h>

static BOOL s_initialized;

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

BOOL PADInit(void)
{
    if (!s_initialized) {
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
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

    memset(&vita, 0, sizeof(vita));
    read = sceCtrlPeekBufferPositive(0, &vita, 1);
    if (read < 1) {
        status[0].err = PAD_ERR_NOT_READY;
        return 0;
    }

    status[0].err = PAD_ERR_NONE;
    status[0].stickX = stick_axis(vita.lx);
    status[0].stickY = stick_axis_inverted(vita.ly);
    status[0].substickX = stick_axis(vita.rx);
    status[0].substickY = stick_axis_inverted(vita.ry);

    if (vita.buttons & SCE_CTRL_LEFT) status[0].button |= PAD_BUTTON_LEFT;
    if (vita.buttons & SCE_CTRL_RIGHT) status[0].button |= PAD_BUTTON_RIGHT;
    if (vita.buttons & SCE_CTRL_DOWN) status[0].button |= PAD_BUTTON_DOWN;
    if (vita.buttons & SCE_CTRL_UP) status[0].button |= PAD_BUTTON_UP;
    if (vita.buttons & SCE_CTRL_CROSS) {
        status[0].button |= PAD_BUTTON_A;
        status[0].analogA = 255;
    }
    if (vita.buttons & SCE_CTRL_CIRCLE) {
        status[0].button |= PAD_BUTTON_B;
        status[0].analogB = 255;
    }
    if (vita.buttons & SCE_CTRL_SQUARE) status[0].button |= PAD_BUTTON_X;
    if (vita.buttons & SCE_CTRL_TRIANGLE) status[0].button |= PAD_BUTTON_Y;
    if (vita.buttons & SCE_CTRL_SELECT) status[0].button |= PAD_TRIGGER_Z;
    if (vita.buttons & SCE_CTRL_START) status[0].button |= PAD_BUTTON_START;
    if (vita.buttons & SCE_CTRL_LTRIGGER) {
        status[0].button |= PAD_TRIGGER_L;
        status[0].triggerLeft = 255;
    }
    if (vita.buttons & SCE_CTRL_RTRIGGER) {
        /* R is the GameCube Z button (grab, and the menu shortcuts that use it);
         * shielding stays on L, which reports a full trigger press. */
        status[0].button |= PAD_TRIGGER_Z;
    }
    return 0;
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
