#ifndef SYSDOLPHIN_BASELIB_VITA_SKIN_H
#define SYSDOLPHIN_BASELIB_VITA_SKIN_H

/* Vita: envelope (skinned) model matrices for fighters are blended ahead of
 * drawing by a worker thread on core 2.  The render path consumes the
 * precomputed world matrices and only applies the view matrix.  Anything not
 * precomputed, or whose joints changed after the prepass, takes the original
 * path.  Define MELEE_VITA_NO_SKIN_JOBS to compile it out. */
#if defined(TARGET_VITA) && !defined(MELEE_VITA_NO_SKIN_JOBS)
#define MELEE_VITA_SKIN_JOBS 1

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/forward.h>

/* Main thread, before the scene is drawn. */
void melee_vita_skin_begin_frame(void);
/* Main thread, after the scene is drawn (before game logic runs again). */
void melee_vita_skin_end_frame(void);
typedef struct MeleeVitaSkinResult {
    int count;
    const Mtx* world; /* blended envelope matrices (model space) */
    const Mtx* pos;   /* view * world, or NULL if the view was not predicted */
    const Mtx* nrm;   /* inverse transpose of pos (valid with pos) */
} MeleeVitaSkinResult;

/* Returns 1 with the pobj's envelope matrices in *out, or 0 if the caller
 * must use the original path.  scratch holds SKIN_MAX_MTX (10) matrices. */
int melee_vita_skin_lookup(HSD_PObj* pobj, HSD_JObj* owner, MtxPtr vmtx,
                           MeleeVitaSkinResult* out, Mtx* scratch);
#endif

#endif
