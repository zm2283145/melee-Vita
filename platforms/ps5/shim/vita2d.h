/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5: vita2d is not used.  The shared Vita layer only passes texture
 * handles around; the PS5 OpenGL backend (gl_game.c) defines the type. */
#ifndef MELEE_PS5_VITA2D_H
#define MELEE_PS5_VITA2D_H

#include "psp2_shim.h"

typedef struct vita2d_texture vita2d_texture;

#endif
