/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Small host-runtime hooks that do not belong to a Dolphin hardware module. */
#include <dolphin/os.h>

unsigned int aurora_draw_tag;

void Exception_ReportStackTrace(OSContext* context, int max_depth)
{
    (void) context;
    (void) max_depth;
    OSReport("Melee Vita: stack trace requested\n");
}

void Exception_ReportCodeline(u16 error, int dsisr, int dar,
                              OSContext* context)
{
    (void) context;
    OSReport("Melee Vita: exception %u, dsisr=%08X dar=%08X\n",
             (unsigned int) error, (unsigned int) dsisr, (unsigned int) dar);
}

void Exception_StoreDebugLevel(int level)
{
    (void) level;
}

void hsd_80397DA4(OSContext* context)
{
    (void) context;
}

void hsd_80397DFC(u32 size)
{
    (void) size;
}
