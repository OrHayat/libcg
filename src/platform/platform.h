#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t *pixels;
    int       width;
    int       height;
} platform_framebuffer_t;

/* Lifecycle */
bool platform_init(int width, int height, const char *title);
void platform_shutdown(void);

/* Per-frame */
void platform_poll_events(bool *quit_requested);
void platform_present(void);

/* Accessors */
platform_framebuffer_t *platform_get_framebuffer(void);

#endif /* PLATFORM_H */
