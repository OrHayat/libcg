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
    PLATFORM_KEY_M,
    PLATFORM_KEY_F,
    PLATFORM_KEY_ESCAPE,
    PLATFORM_KEY_1,
    PLATFORM_KEY_2,
    PLATFORM_KEY_3,
    PLATFORM_KEY_4,
    PLATFORM_KEY_COUNT
} platform_key_t;

typedef struct {
    bool  quit_requested;
    bool  keys_pressed[PLATFORM_KEY_COUNT];

    /* mouse — position is persistent, button presses + scroll are edge-triggered */
    int   mouse_x, mouse_y;
    bool  mouse_left_pressed;
    bool  mouse_right_pressed;
    bool  mouse_middle_pressed;
    float scroll_dy;
} platform_input_t;

/* Lifecycle */
bool platform_init(int width, int height, const char *title);
void platform_shutdown(void);

/* Per-frame */
void platform_poll_events(platform_input_t *input);
void platform_present(void);

/* Accessors */
platform_framebuffer_t *platform_get_framebuffer(void);

/* Window controls */
void platform_toggle_fullscreen(void);
bool platform_is_fullscreen(void);

#endif /* PLATFORM_H */
