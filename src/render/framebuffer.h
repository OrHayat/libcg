#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "platform/platform.h"
#include "render/color.h"
#include "render/geom.h"

/* Everything written to the framebuffer is premultiplied — that is what the
   platform layer hands to the compositor. */
void framebuffer_clear(platform_framebuffer_t *fb, pcolor_t color);
void framebuffer_set_pixel(platform_framebuffer_t *fb, int x, int y, pcolor_t color);

/* Solid rectangle, clipped to fb. An empty rect (w or h <= 0) draws
   nothing rather than wrapping, which is what lets callers pass a
   computed rect without pre-checking it. */
void framebuffer_fill_rect(platform_framebuffer_t *fb, rect2d_t r, pcolor_t color);

#endif /* FRAMEBUFFER_H */
