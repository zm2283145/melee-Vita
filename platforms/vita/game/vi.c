/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vita vertical-interrupt and frame-boundary implementation. */
#include "vita_platform.h"

#include <dolphin/gx.h>
#include <dolphin/vi.h>

#include <psp2/display.h>

static u32 s_retrace_count;
static VIRetraceCallback s_pre_callback;
static VIRetraceCallback s_post_callback;
static void* s_next_framebuffer;
static void* s_current_framebuffer;
static BOOL s_black = TRUE;

GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT, 640, 480, 480, 40, 0, 640, 480,
    VI_XFBMODE_DF, 0, 0,
    { { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
      { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 } },
    { 0, 0, 21, 22, 21, 0, 0 },
};

GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT, 640, 480, 480, 40, 0, 640, 480,
    VI_XFBMODE_DF, 0, 0,
    { { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
      { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 } },
    { 8, 8, 10, 12, 10, 8, 8 },
};

void VIInit(void) {}
void VIConfigure(const GXRenderModeObj* mode) { (void) mode; }
void VIConfigurePan(u16 x, u16 y, u16 width, u16 height)
{
    (void) x; (void) y; (void) width; (void) height;
}
void VIFlush(void) {}

void VIWaitForRetrace(void)
{
    sceDisplayWaitVblankStart();
    ++s_retrace_count;
    melee_vita_os_run_alarms();
    melee_vita_card_poll();
    if (s_pre_callback != NULL) s_pre_callback(s_retrace_count);
    s_current_framebuffer = s_next_framebuffer;
    if (s_post_callback != NULL) s_post_callback(s_retrace_count);
}

u32 VIGetTvFormat(void) { return VI_NTSC; }
u32 VIGetRetraceCount(void) { return s_retrace_count; }
u32 VIGetNextField(void) { return s_retrace_count & 1u; }
u32 VIGetDTVStatus(void) { return 0; }
void* VIGetCurrentFrameBuffer(void) { return s_current_framebuffer; }
void* VIGetNextFrameBuffer(void) { return s_next_framebuffer; }
void VISetNextFrameBuffer(void* framebuffer) { s_next_framebuffer = framebuffer; }
void VISetBlack(BOOL black) { s_black = black; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback previous = s_pre_callback;
    s_pre_callback = callback;
    return previous;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback previous = s_post_callback;
    s_post_callback = callback;
    return previous;
}

u16 VIPadFrameBufferWidth(u16 width)
{
    return (u16) ((width + 15u) & ~15u);
}
