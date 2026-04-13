#include "framebuffer.h"

void framebuffer_clear(platform_framebuffer_t *fb, uint32_t color) {
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        fb->pixels[i] = color;
    }
}

void framebuffer_set_pixel(platform_framebuffer_t *fb, int x, int y, uint32_t color) {
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    fb->pixels[y * fb->width + x] = color;
}

void framebuffer_fill_rect(platform_framebuffer_t *fb, int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb->width  ? fb->width  : x + w;
    int y1 = y + h > fb->height ? fb->height : y + h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            fb->pixels[yy * fb->width + xx] = color;
        }
    }
}
