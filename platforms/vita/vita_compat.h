/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included into every shared game translation unit on Vita. */
#ifndef MELEE_VITA_COMPAT_H
#define MELEE_VITA_COMPAT_H

/* Newlib hides useful constants such as M_PI under feature-test macros. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <math.h>

/* Reuse the PC port's endian-aware representation of GameCube disc data.
 * TARGET_VITA selects its direct 32-bit pointer path. */
#include "pc/disc.h"

#endif
