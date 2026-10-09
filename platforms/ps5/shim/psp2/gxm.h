/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5: the GXM types that cross the renderer interface (gx_render.h). */
#ifndef MELEE_PS5_GXM_H
#define MELEE_PS5_GXM_H

#include "../psp2_shim.h"

/* A texture as the OpenGL backend sees it. */
typedef struct SceGxmTexture {
    unsigned int gl_name;
    unsigned int width;
    unsigned int height;
    unsigned int flags;
} SceGxmTexture;

#endif
