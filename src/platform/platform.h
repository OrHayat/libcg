#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t *pixels;
    int       width;
    int       height;
} platform_framebuffer_t;

typedef enum {
    PLATFORM_KEY_NONE = 0,
    PLATFORM_KEY_Q,
    PLATFORM_KEY_COUNT
} platform_key_t;

typedef struct {
    bool quit_requested;
    bool keys_pressed[PLATFORM_KEY_COUNT];
} platform_input_t;

/* Lifecycle */
bool platform_init(int width, int height, const char *title);
void platform_shutdown(void);

/* Per-frame */
void platform_poll_events(platform_input_t *input);
void platform_present(void);

/* Accessors */
platform_framebuffer_t *platform_get_framebuffer(void);

#endif /* PLATFORM_H */
