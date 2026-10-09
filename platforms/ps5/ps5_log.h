/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_PS5_LOG_H
#define MELEE_PS5_LOG_H

/* Writes to /data/melee/melee-ps5.log (and stdout, which klog shows). */
void melee_ps5_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void melee_ps5_log_flush(void);

#endif
