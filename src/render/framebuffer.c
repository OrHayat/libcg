#include "framebuffer.h"

void framebuffer_clear(platform_framebuffer_t *fb, pcolor_t color) {
    pcolor_t *px = pcolor_pixels(fb->pixels);
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        px[i] = color;
    }
}

void framebuffer_set_pixel(platform_framebuffer_t *fb, int x, int y, pcolor_t color) {
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    pcolor_pixels(fb->pixels)[y * fb->width + x] = color;
}

void framebuffer_fill_rect(platform_framebuffer_t *fb, rect2d_t r, pcolor_t color) {
    r = rect2d_intersect(r, rect2d(0, 0, fb->width, fb->height));
    if (rect2d_is_empty(r)) return;

    pcolor_t *pixels = pcolor_pixels(fb->pixels);
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            pixels[y * fb->width + x] = color;
        }
    }
}
