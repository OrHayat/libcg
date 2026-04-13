#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "platform/platform.h"

void framebuffer_clear(platform_framebuffer_t *fb, uint32_t color);
void framebuffer_set_pixel(platform_framebuffer_t *fb, int x, int y, uint32_t color);
void framebuffer_fill_rect(platform_framebuffer_t *fb, int x, int y, int w, int h, uint32_t color);

#endif /* FRAMEBUFFER_H */
