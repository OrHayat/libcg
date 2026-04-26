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
    PLATFORM_KEY_UNKNOWN = 0,

    PLATFORM_KEY_A, PLATFORM_KEY_B, PLATFORM_KEY_C, PLATFORM_KEY_D,
    PLATFORM_KEY_E, PLATFORM_KEY_F, PLATFORM_KEY_G, PLATFORM_KEY_H,
    PLATFORM_KEY_I, PLATFORM_KEY_J, PLATFORM_KEY_K, PLATFORM_KEY_L,
    PLATFORM_KEY_M, PLATFORM_KEY_N, PLATFORM_KEY_O, PLATFORM_KEY_P,
    PLATFORM_KEY_Q, PLATFORM_KEY_R, PLATFORM_KEY_S, PLATFORM_KEY_T,
    PLATFORM_KEY_U, PLATFORM_KEY_V, PLATFORM_KEY_W, PLATFORM_KEY_X,
    PLATFORM_KEY_Y, PLATFORM_KEY_Z,

    PLATFORM_KEY_0, PLATFORM_KEY_1, PLATFORM_KEY_2, PLATFORM_KEY_3,
    PLATFORM_KEY_4, PLATFORM_KEY_5, PLATFORM_KEY_6, PLATFORM_KEY_7,
    PLATFORM_KEY_8, PLATFORM_KEY_9,

    PLATFORM_KEY_APOSTROPHE, PLATFORM_KEY_BACKSLASH, PLATFORM_KEY_COMMA,
    PLATFORM_KEY_EQUAL, PLATFORM_KEY_GRAVE, PLATFORM_KEY_LEFT_BRACKET,
    PLATFORM_KEY_MINUS, PLATFORM_KEY_PERIOD, PLATFORM_KEY_RIGHT_BRACKET,
    PLATFORM_KEY_SEMICOLON, PLATFORM_KEY_SLASH,

    PLATFORM_KEY_BACKSPACE, PLATFORM_KEY_DELETE, PLATFORM_KEY_END,
    PLATFORM_KEY_ENTER, PLATFORM_KEY_ESCAPE, PLATFORM_KEY_HOME,
    PLATFORM_KEY_INSERT, PLATFORM_KEY_PAGE_DOWN, PLATFORM_KEY_PAGE_UP,
    PLATFORM_KEY_SPACE, PLATFORM_KEY_TAB,

    PLATFORM_KEY_LEFT, PLATFORM_KEY_RIGHT, PLATFORM_KEY_UP, PLATFORM_KEY_DOWN,

    PLATFORM_KEY_CAPS_LOCK,
    PLATFORM_KEY_LEFT_ALT,     PLATFORM_KEY_LEFT_CONTROL,
    PLATFORM_KEY_LEFT_SHIFT,   PLATFORM_KEY_LEFT_SUPER,
    PLATFORM_KEY_RIGHT_ALT,    PLATFORM_KEY_RIGHT_CONTROL,
    PLATFORM_KEY_RIGHT_SHIFT,  PLATFORM_KEY_RIGHT_SUPER,

    PLATFORM_KEY_F1,  PLATFORM_KEY_F2,  PLATFORM_KEY_F3,  PLATFORM_KEY_F4,
    PLATFORM_KEY_F5,  PLATFORM_KEY_F6,  PLATFORM_KEY_F7,  PLATFORM_KEY_F8,
    PLATFORM_KEY_F9,  PLATFORM_KEY_F10, PLATFORM_KEY_F11, PLATFORM_KEY_F12,

    PLATFORM_KEY_COUNT
} platform_key_t;

#define PLATFORM_TEXT_BUFFER 16

typedef struct {
    int  half_transition_count;
    bool ended_down;
} platform_button_t;

typedef enum {
    PLATFORM_MOUSE_LEFT   = 0,
    PLATFORM_MOUSE_RIGHT  = 1,
    PLATFORM_MOUSE_MIDDLE = 2,
    PLATFORM_MOUSE_COUNT
} platform_mouse_button_t;

typedef struct {
    platform_button_t buttons[PLATFORM_MOUSE_COUNT];
    int   x, y;
    float scroll_dx, scroll_dy;
} platform_mouse_t;

typedef struct {
    platform_button_t keys[PLATFORM_KEY_COUNT];
    platform_mouse_t  mouse;
    bool quit_requested;

    /* text input typed this frame (printable characters only) */
    char  text[PLATFORM_TEXT_BUFFER];
    int   text_len;
} platform_input_t;

static inline bool platform_is_key_down(const platform_input_t *input, platform_key_t k) {
    return input->keys[k].ended_down;
}
static inline bool platform_is_key_pressed(const platform_input_t *input, platform_key_t k) {
    return input->keys[k].half_transition_count >= 1 && input->keys[k].ended_down;
}
static inline bool platform_is_key_released(const platform_input_t *input, platform_key_t k) {
    return input->keys[k].half_transition_count >= 1 && !input->keys[k].ended_down;
}

static inline bool platform_is_mouse_down(const platform_input_t *input, platform_mouse_button_t b) {
    return input->mouse.buttons[b].ended_down;
}
static inline bool platform_is_mouse_pressed(const platform_input_t *input, platform_mouse_button_t b) {
    return input->mouse.buttons[b].half_transition_count >= 1 && input->mouse.buttons[b].ended_down;
}
static inline bool platform_is_mouse_released(const platform_input_t *input, platform_mouse_button_t b) {
    return input->mouse.buttons[b].half_transition_count >= 1 && !input->mouse.buttons[b].ended_down;
}

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

/* Display info */

typedef enum {
    PLATFORM_CONNECTION_UNKNOWN = 0,
    PLATFORM_CONNECTION_INTERNAL,
    PLATFORM_CONNECTION_HDMI,
    PLATFORM_CONNECTION_DISPLAYPORT,
    PLATFORM_CONNECTION_THUNDERBOLT,
    PLATFORM_CONNECTION_VGA,
    PLATFORM_CONNECTION_DVI,
    PLATFORM_CONNECTION_AIRPLAY,
} platform_connection_type_t;

typedef struct {
    char     name[64];           /* localizedName — user-rename if set, else EDID, else "Built-in Retina Display" */
    char     name_original[64];  /* raw EDID name; empty for built-in or unavailable */

    uint32_t id;                 /* CGDirectDisplayID */
    float    scale;              /* backingScaleFactor (1.0, 2.0) */

    int      bounds_x;           /* global display origin in logical points */
    int      bounds_y;
    int      bounds_w;           /* logical points */
    int      bounds_h;

    int      work_x;             /* visibleFrame — bounds minus menu bar / dock */
    int      work_y;
    int      work_w;
    int      work_h;

    int      pixels_w;           /* physical pixels of current display mode */
    int      pixels_h;

    int      size_mm_w;          /* panel physical size in mm; 0 if unknown */
    int      size_mm_h;

    float    refresh_hz;         /* 0.0 if variable / unavailable (ProMotion) */

    bool     builtin;            /* CGDisplayIsBuiltin */
    bool     is_main;            /* CGDisplayIsMain — system primary */
    bool     is_online;          /* CGDisplayIsOnline */
    bool     rotation_supported; /* IOFramebuffer kIOFBRotateMask has bits beyond 0° */

    uint32_t mirrors_id;         /* CGDisplayMirrorsDisplay — id mirrored, 0 if not mirroring */
    int      rotation;           /* 0/90/180/270 (CGDisplayRotation) */

    platform_connection_type_t connection_type;
} platform_display_info_t;

/* Returns number of displays; fills up to `max` entries in out[] */
int platform_get_displays(platform_display_info_t *out, int max);

typedef struct {
    int   width, height;         /* logical points */
    int   pixels_w, pixels_h;    /* physical pixels */
    float refresh_hz;
    bool  is_current;            /* mode currently active for this display */
} platform_video_mode_t;

/* Returns number of modes for the given display; fills up to `max` entries */
int platform_get_display_modes(uint32_t display_id, platform_video_mode_t *out, int max);

/* CGDirectDisplayID of the display the window is currently on; match against
   platform_display_info_t.id from platform_get_displays. */
uint32_t platform_get_window_display_id(void);

#endif /* PLATFORM_H */
