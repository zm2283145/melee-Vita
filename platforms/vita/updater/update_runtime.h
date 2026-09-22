/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_VITA_UPDATE_RUNTIME_H
#define MELEE_VITA_UPDATE_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

int melee_vita_updater_init(void);
void melee_vita_updater_poll(void);
void melee_vita_updater_render_dialog(void);
void melee_vita_updater_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
