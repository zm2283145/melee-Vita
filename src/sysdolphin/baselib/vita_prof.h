#ifndef SYSDOLPHIN_BASELIB_VITA_PROF_H
#define SYSDOLPHIN_BASELIB_VITA_PROF_H

/* Live-profiler zones for HSD display work on Vita.  The recorder only
 * exists in live-profiler builds; elsewhere the weak symbol is NULL and the
 * macros cost a single branch. */
#ifdef TARGET_VITA
#include <dolphin/types.h>
extern void melee_vita_profiler_record_duration(unsigned int zone,
                                                u64 elapsed_us)
    __attribute__((weak));
extern u64 sceKernelGetProcessTimeWide(void);
#define HSD_VPROF_ZONE_MOBJ 34u
#define HSD_VPROF_ZONE_POBJ_MTX 35u
#define HSD_VPROF_ZONE_POBJ_DRAW 36u
#define HSD_VPROF_ZONE_VTXDESC 37u
#define HSD_VPROF_BEGIN() \
    (melee_vita_profiler_record_duration != NULL \
         ? sceKernelGetProcessTimeWide() : 0u)
#define HSD_VPROF_END(zone, start) \
    do { \
        if (melee_vita_profiler_record_duration != NULL) \
            melee_vita_profiler_record_duration( \
                (zone), sceKernelGetProcessTimeWide() - (start)); \
    } while (0)
#else
#define HSD_VPROF_BEGIN() 0u
#define HSD_VPROF_END(zone, start) ((void) (start))
#endif

#endif
