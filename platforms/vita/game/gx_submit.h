/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * GXM submit worker.
 *
 * The game thread translates GX state into GxrDraw records; turning those
 * into render-queue commands (shader lookup, texture resolution, uniform
 * packing, recording) runs on a worker thread on core 2.  Everything the
 * game thread pushes to the render queue while the worker is active goes
 * through the same ordered ring, so command order is unchanged.
 *
 * Sync points (gxs_drain): present, texture invalidation, EFB copies to
 * texture, anything that waits for the render thread, and draws whose
 * shaders have not been seen yet (they run synchronously so a failed shader
 * can still fall back to the CPU path).
 *
 * Define MELEE_VITA_NO_SUBMIT_THREAD to keep everything on the game thread.
 */
#ifndef MELEE_VITA_GX_SUBMIT_H
#define MELEE_VITA_GX_SUBMIT_H

#include <dolphin/types.h>
#include <stdbool.h>

typedef void (*GxsRun)(void* payload);

/* Starts the worker (after the render queue is up). */
void gxs_init(void);
/* True when records should be queued (worker running). */
bool gxs_active(void);
/* Reserves a record; it becomes visible to the worker at the next
 * gxs_alloc/gxs_publish/gxs_drain.  Returns NULL if the queue is off. */
void* gxs_alloc(GxsRun run, u32 payload_size);
void gxs_publish(void);
/* Waits until the worker has run every queued record.  No-op on the worker
 * itself or when the queue is off. */
void gxs_drain(void);

#endif
