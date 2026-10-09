/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interfaces between the PS5 OpenGL frame sink and the GX renderer. */
#ifndef MELEE_PS5_GL_GAME_H
#define MELEE_PS5_GL_GAME_H

#include <dolphin/types.h>
#include <stdbool.h>
#include <vita2d.h>

unsigned melee_ps5_gl_program(const char* vertex_source, const char* fragment_source);
unsigned melee_ps5_texture_name(const vita2d_texture* texture);
unsigned melee_ps5_texture_depth_name(const vita2d_texture* texture);
unsigned melee_ps5_texture_fbo(const vita2d_texture* texture);
unsigned melee_ps5_texture_width(const vita2d_texture* texture);
unsigned melee_ps5_texture_height(const vita2d_texture* texture);
vita2d_texture* melee_ps5_texture_create_white(void);
u32 melee_ps5_frame_counter(void);
/* Pixels of the bound render target per Vita-space unit. */
f32 melee_ps5_target_pixel_scale(void);
void melee_ps5_gl_default_depth(void);
/* Draws the depth texture's region (pixels, top row first) into target as
 * GX Z24X8 copy data. */
bool melee_ps5_gl_copy_depth(unsigned depth_texture, int x0, int y0, int x1, int y1,
                             unsigned source_width, unsigned source_height,
                             vita2d_texture* target);

#endif
