#include "vita_skin.h"

#ifdef MELEE_VITA_SKIN_JOBS

#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "displayfunc.h"
#include "dobj.h"
#include "gobj.h"
#include "jobj.h"
#include "list.h"
#include "mtx.h"
#include "pobj.h"
#include "vita_prof.h"

#include <dolphin/mtx.h>

#define SKIN_PLINK_FIGHTER 8
#define SKIN_MAX_JOBS 1536
#define SKIN_MAX_MTX 10
#define SKIN_HASH_SIZE 4096 /* power of two, > 2 * SKIN_MAX_JOBS */
#define SKIN_SPIN_LIMIT_US 30

#define SKIN_ZONE_PREPASS 52u
#define SKIN_ZONE_HITS 53u
#define SKIN_ZONE_MAIN 54u
#define SKIN_ZONE_FALLBACK 55u
#define SKIN_ZONE_WAIT 56u

enum {
    SKIN_PENDING = 0,
    SKIN_WORKER = 1,
    SKIN_MAIN = 2,
    SKIN_DONE = 3,
};

typedef struct SkinJob {
    HSD_PObj* pobj;
    HSD_JObj* owner;
    int state;
    int count;
    int has_view;
    Mtx view;
    Mtx w[SKIN_MAX_MTX];
    Mtx pos[SKIN_MAX_MTX];
    Mtx nrm[SKIN_MAX_MTX];
} SkinJob;

/* The view matrix each (pobj, owner) was last drawn with.  The worker
 * predicts the next frame uses the same one (true for static cameras such
 * as the results screen and menus) and also produces the final position and
 * normal matrices; the draw then only compares the view matrix. */
#define SKIN_VIEW_SIZE 4096
typedef struct SkinView {
    HSD_PObj* pobj;
    HSD_JObj* owner;
    u32 epoch;
    Mtx view;
} SkinView;
static SkinView* s_views;

typedef struct SkinSlot {
    HSD_PObj* pobj;
    HSD_JObj* owner;
    u32 epoch;
    u32 job;
} SkinSlot;

/* Heap-allocated for the same reason as the GX submit ring. */
static SkinJob* s_jobs;
static u32 s_stat_view_hits;
static SkinSlot s_hash[SKIN_HASH_SIZE];
static int s_job_count;
static u32 s_epoch;
static int s_frame_valid;

static SceUID s_thread = -1;
static SceUID s_sema = -1;
static int s_thread_failed;
static volatile int s_busy;
static volatile int s_abort;

static u32 s_stat_hits, s_stat_main, s_stat_fallback;
static u64 s_stat_wait_us;

static inline u32 skin_hash(const void* a, const void* b)
{
    u32 h = (u32) (uintptr_t) a * 0x9E3779B1u ^ (u32) (uintptr_t) b * 0x85EBCA6Bu;
    return (h ^ (h >> 15)) & (SKIN_HASH_SIZE - 1);
}

/* Same math as SetupEnvelopeModelMtx up to (but excluding) the view matrix.
 * Joint matrices must already be set up; nothing here writes shared state. */
static int skin_compute(HSD_PObj* pobj, HSD_JObj* owner, Mtx* out)
{
    Mtx rmtx;
    MtxPtr right = _HSD_mkEnvelopeModelNodeMtx(owner, rmtx);
    HSD_SList* list = pobj->u.envelope_list;
    int idx;

    for (idx = 0; idx < SKIN_MAX_MTX && list; idx++, list = list->next) {
        HSD_Envelope* envelope = list->data;
        Mtx mtx, tmp;
        MtxPtr mtxp;

        if (envelope->weight >= (1.0f - FLT_EPSILON)) {
            if (right) {
                MTXConcat(envelope->jobj->mtx, envelope->jobj->envelopemtx,
                          mtx);
                mtxp = mtx;
            } else {
                mtxp = envelope->jobj->mtx;
            }
        } else {
            memset(mtx, 0, sizeof(Mtx));
            while (envelope) {
                HSD_JObj* jp = envelope->jobj;
                MTXConcat(jp->mtx, jp->envelopemtx, tmp);
                HSD_MtxScaledAdd(tmp, mtx, mtx, envelope->weight);
                envelope = envelope->next;
            }
            mtxp = mtx;
        }
        if (right) {
            MTXConcat(mtxp, right, out[idx]);
        } else {
            MTXCopy(mtxp, out[idx]);
        }
    }
    return idx;
}

static int skin_thread(SceSize args, void* argp)
{
    (void) args;
    (void) argp;
    for (;;) {
        int i, n;
        if (sceKernelWaitSema(s_sema, 1, NULL) < 0) {
            break;
        }
        n = s_job_count;
        for (i = 0; i < n; i++) {
            SkinJob* job = &s_jobs[i];
            int expected = SKIN_PENDING;
            if (__atomic_load_n(&s_abort, __ATOMIC_ACQUIRE)) {
                break;
            }
            if (!__atomic_compare_exchange_n(&job->state, &expected,
                                             SKIN_WORKER, false,
                                             __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE))
            {
                continue;
            }
            job->count = skin_compute(job->pobj, job->owner, job->w);
            if (job->has_view) {
                int m;
                for (m = 0; m < job->count; m++) {
                    MTXConcat(job->view, job->w[m], job->pos[m]);
                    HSD_MtxInverseTranspose(job->pos[m], job->nrm[m]);
                }
            }
            __atomic_store_n(&job->state, SKIN_DONE, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);
    }
    return sceKernelExitDeleteThread(0);
}

static int skin_ensure_thread(void)
{
    if (s_thread >= 0) {
        return 1;
    }
    if (s_thread_failed) {
        return 0;
    }
    s_jobs = malloc(sizeof(SkinJob) * SKIN_MAX_JOBS);
    s_views = calloc(SKIN_VIEW_SIZE, sizeof(SkinView));
    if (s_jobs == NULL || s_views == NULL) {
        free(s_jobs);
        free(s_views);
        s_jobs = NULL;
        s_views = NULL;
        s_thread_failed = 1;
        return 0;
    }
    s_sema = sceKernelCreateSema("melee skin", 0, 0, 1, NULL);
    if (s_sema >= 0) {
        /* Below the audio output thread (0x10000100 - 20), core 2 only. */
        s_thread = sceKernelCreateThread("melee skin", skin_thread,
                                         0x10000100 - 10, 0x4000, 0,
                                         SCE_KERNEL_CPU_MASK_USER_2, NULL);
        if (s_thread >= 0 && sceKernelStartThread(s_thread, 0, NULL) < 0) {
            sceKernelDeleteThread(s_thread);
            s_thread = -1;
        }
    }
    if (s_thread < 0) {
        s_thread_failed = 1;
        return 0;
    }
    return 1;
}

static void skin_wait_idle(void)
{
    __atomic_store_n(&s_abort, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) {
        /* The worker checks s_abort between jobs; each job is a few us. */
    }
}

static SkinView* skin_view_find(HSD_PObj* pobj, HSD_JObj* owner, int insert)
{
    u32 h = skin_hash(pobj, owner) & (SKIN_VIEW_SIZE - 1);
    SkinView* victim = NULL;
    int probe;
    for (probe = 0; probe < 8; probe++) {
        SkinView* v = &s_views[(h + probe) & (SKIN_VIEW_SIZE - 1)];
        if (v->pobj == pobj && v->owner == owner) {
            return v;
        }
        if (victim == NULL || v->epoch < victim->epoch) {
            victim = v;
        }
    }
    if (!insert) {
        return NULL;
    }
    victim->pobj = pobj;
    victim->owner = owner;
    return victim;
}

static void skin_add_jobj(HSD_JObj* jobj)
{
    HSD_DObj* dobj;
    HSD_PObj* pobj;

    if (!union_type_dobj(jobj) || (jobj->flags & JOBJ_HIDDEN)) {
        return;
    }
    for (dobj = jobj->u.dobj; dobj; dobj = dobj->next) {
        if (dobj->flags & DOBJ_HIDDEN) {
            continue;
        }
        for (pobj = dobj->pobj; pobj; pobj = pobj->next) {
            SkinJob* job;
            u32 h;
            if (pobj_type(pobj) != POBJ_ENVELOPE ||
                pobj->u.envelope_list == NULL ||
                s_job_count >= SKIN_MAX_JOBS)
            {
                continue;
            }
            h = skin_hash(pobj, jobj);
            while (s_hash[h].epoch == s_epoch) {
                if (s_hash[h].pobj == pobj && s_hash[h].owner == jobj) {
                    break;
                }
                h = (h + 1) & (SKIN_HASH_SIZE - 1);
            }
            if (s_hash[h].epoch == s_epoch) {
                continue; /* already queued */
            }
            job = &s_jobs[s_job_count];
            job->pobj = pobj;
            job->owner = jobj;
            job->count = 0;
            job->state = SKIN_PENDING;
            {
                const SkinView* v = skin_view_find(pobj, jobj, 0);
                job->has_view = v != NULL && s_epoch - v->epoch <= 2u;
                if (job->has_view) {
                    MTXCopy((MtxPtr) v->view, job->view);
                }
            }
            s_hash[h].pobj = pobj;
            s_hash[h].owner = jobj;
            s_hash[h].job = (u32) s_job_count;
            s_hash[h].epoch = s_epoch;
            s_job_count++;
        }
    }
}

static void skin_walk(HSD_JObj* jobj)
{
    for (; jobj != NULL; jobj = jobj->next) {
        HSD_JObjSetupMatrix(jobj);
        skin_add_jobj(jobj);
        if (jobj->child != NULL) {
            skin_walk(jobj->child);
        }
    }
}

void melee_vita_skin_begin_frame(void)
{
    const u64 t0 = HSD_VPROF_BEGIN();
    HSD_GObj* gobj;

    skin_wait_idle();
    s_frame_valid = 0;
    s_job_count = 0;
    if (++s_epoch == 0) {
        memset(s_hash, 0, sizeof(s_hash));
        s_epoch = 1;
    }
    if (!skin_ensure_thread()) {
        return;
    }
    for (gobj = HSD_GObjPLinkHead[SKIN_PLINK_FIGHTER]; gobj != NULL;
         gobj = gobj->next)
    {
        if (gobj->hsd_obj != NULL) {
            skin_walk(gobj->hsd_obj);
        }
    }
    if (s_job_count > 0) {
        s_frame_valid = 1;
        __atomic_store_n(&s_abort, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_busy, 1, __ATOMIC_RELEASE);
        sceKernelSignalSema(s_sema, 1);
    }
    HSD_VPROF_END(SKIN_ZONE_PREPASS, t0);
}

void melee_vita_skin_end_frame(void)
{
    skin_wait_idle();
    s_frame_valid = 0;
    if (melee_vita_profiler_record_duration != NULL) {
        melee_vita_profiler_record_duration(SKIN_ZONE_HITS, s_stat_hits);
        melee_vita_profiler_record_duration(SKIN_ZONE_MAIN, s_stat_main);
        melee_vita_profiler_record_duration(SKIN_ZONE_FALLBACK,
                                            s_stat_fallback);
        melee_vita_profiler_record_duration(SKIN_ZONE_WAIT, s_stat_wait_us);
        melee_vita_profiler_record_duration(57u, s_stat_view_hits);
    }
    s_stat_view_hits = 0;
    s_stat_hits = s_stat_main = s_stat_fallback = 0;
    s_stat_wait_us = 0;
}

static void skin_invalidate_frame(void)
{
    /* A joint changed after the prepass: stop the worker before anything
     * rewrites joint matrices it may be reading, and use the original path
     * for the rest of the frame. */
    skin_wait_idle();
    s_frame_valid = 0;
}

static void skin_remember_view(HSD_PObj* pobj, HSD_JObj* owner, MtxPtr vmtx)
{
    SkinView* v = skin_view_find(pobj, owner, 1);
    v->epoch = s_epoch;
    MTXCopy(vmtx, v->view);
}

int melee_vita_skin_lookup(HSD_PObj* pobj, HSD_JObj* owner, MtxPtr vmtx,
                           MeleeVitaSkinResult* out, Mtx* scratch)
{
    SkinJob* job;
    HSD_SList* list;
    u32 h;
    int state, idx;

    out->pos = NULL;
    out->nrm = NULL;
    if (!s_frame_valid) {
        return 0;
    }
    h = skin_hash(pobj, owner);
    for (;;) {
        if (s_hash[h].epoch != s_epoch) {
            s_stat_fallback++;
            return 0;
        }
        if (s_hash[h].pobj == pobj && s_hash[h].owner == owner) {
            break;
        }
        h = (h + 1) & (SKIN_HASH_SIZE - 1);
    }
    job = &s_jobs[s_hash[h].job];

    /* The original path would (re)build dirty joint matrices here. */
    if (HSD_JObjMtxIsDirty(owner)) {
        goto invalid;
    }
    for (idx = 0, list = pobj->u.envelope_list; idx < SKIN_MAX_MTX && list;
         idx++, list = list->next)
    {
        HSD_Envelope* envelope;
        for (envelope = list->data; envelope; envelope = envelope->next) {
            if (envelope->jobj == NULL ||
                HSD_JObjMtxIsDirty(envelope->jobj))
            {
                goto invalid;
            }
        }
    }
    skin_remember_view(pobj, owner, vmtx);

    state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (state == SKIN_PENDING) {
        int expected = SKIN_PENDING;
        if (__atomic_compare_exchange_n(&job->state, &expected, SKIN_MAIN,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
        {
            /* World matrices only; the caller applies the view. */
            job->has_view = 0;
            job->count = skin_compute(pobj, owner, job->w);
            __atomic_store_n(&job->state, SKIN_DONE, __ATOMIC_RELEASE);
            s_stat_main++;
            out->count = job->count;
            out->world = (const Mtx*) job->w;
            return 1;
        }
        state = expected;
    }
    if (state == SKIN_WORKER) {
        const u64 t0 = sceKernelGetProcessTimeWide();
        u64 now = t0;
        while (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) != SKIN_DONE) {
            now = sceKernelGetProcessTimeWide();
            if (now - t0 > SKIN_SPIN_LIMIT_US) {
                /* Worker was preempted mid-job; compute a private copy. */
                s_stat_wait_us += now - t0;
                s_stat_main++;
                out->count = skin_compute(pobj, owner, scratch);
                out->world = (const Mtx*) scratch;
                return 1;
            }
        }
        s_stat_wait_us += now - t0;
    }
    s_stat_hits++;
    out->count = job->count;
    out->world = (const Mtx*) job->w;
    if (job->has_view && memcmp(job->view, vmtx, sizeof(Mtx)) == 0) {
        out->pos = (const Mtx*) job->pos;
        out->nrm = (const Mtx*) job->nrm;
        s_stat_view_hits++;
    }
    return 1;

invalid:
    skin_invalidate_frame();
    s_stat_fallback++;
    return 0;
}

#endif
