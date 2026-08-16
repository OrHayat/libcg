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

typedef enum {
    PLATFORM_MOUSE_LEFT   = 0,
    PLATFORM_MOUSE_RIGHT  = 1,
    PLATFORM_MOUSE_MIDDLE = 2,
    PLATFORM_MOUSE_COUNT
} platform_mouse_button_t;

/* ============================================================
   Events. The platform delivers each input event through event_cb
   exactly once, in arrival order, before the next frame_cb fires.
   Callers must only read the union member matching event.kind.
   For PLATFORM_EV_FOCUS / UNFOCUS / QUIT_REQUESTED / NONE the
   union contents are unspecified.
   ============================================================ */

typedef enum {
    PLATFORM_EV_NONE = 0,
    PLATFORM_EV_KEY_DOWN,
    PLATFORM_EV_KEY_UP,
    PLATFORM_EV_TEXT_INPUT,        /* one printable codepoint per event */
    PLATFORM_EV_MOUSE_DOWN,
    PLATFORM_EV_MOUSE_UP,
    PLATFORM_EV_MOUSE_MOVE,
    PLATFORM_EV_SCROLL,
    PLATFORM_EV_RESIZE,            /* framebuffer dimensions changed */
    PLATFORM_EV_FOCUS,             /* no payload */
    PLATFORM_EV_UNFOCUS,           /* no payload */
    PLATFORM_EV_QUIT_REQUESTED,    /* no payload */
    PLATFORM_EV_COUNT
} platform_event_kind_t;

typedef struct {
    platform_key_t key;
    bool           repeat;
} platform_key_event_t;

typedef struct {
    char ch[8];                      /* one UTF-8 codepoint (1-4 bytes) + null terminator */
} platform_text_event_t;

typedef struct {
    platform_mouse_button_t btn;
    int                     x, y;
} platform_mouse_event_t;

typedef struct {
    int x, y;                        /* current cursor position */
    int dx, dy;                      /* delta since last move */
} platform_mouse_move_event_t;

typedef struct {
    float dx, dy;
} platform_scroll_event_t;

typedef struct {
    int w, h;                        /* logical points */
    int fb_w, fb_h;                  /* physical pixels (= w/h × backing scale) */
} platform_resize_event_t;

typedef struct {
    platform_event_kind_t kind;
    uint64_t              frame_index; /* matches platform_frame_count() at delivery */

    union {                            /* anonymous — access as e->key.key */
        platform_key_event_t        key;
        platform_text_event_t       text;
        platform_mouse_event_t      mouse;
        platform_mouse_move_event_t move;
        platform_scroll_event_t     scroll;
        platform_resize_event_t     resize;
    };
} platform_event_t;

/* ============================================================
   App descriptor + entry point. Sokol/SDL3-style: caller fills in
   a desc struct and platform_run owns the event loop. frame_cb is
   the only required callback; init_cb / event_cb / cleanup_cb are
   optional.
   ============================================================ */

typedef struct {
    /* Lifecycle */
    void (*init_cb)   (void *user_data);
    void (*frame_cb)  (void *user_data);                                  /* required */
    void (*event_cb)  (const platform_event_t *e, void *user_data);
    void (*cleanup_cb)(void *user_data);

    /* Window config */
    int         width;
    int         height;
    const char *title;
    bool        transparent;       /* setOpaque:NO + clearColor */
    bool        resizable;
    bool        high_dpi;          /* allocate fb at backing pixel size, not logical */

    /* Opaque pointer passed back to all callbacks */
    void       *user_data;
} platform_app_desc_t;

/* The application provides main() and calls platform_run(desc) with its
   callbacks and window configuration. platform_run owns the event loop,
   drives the desc callbacks, and returns when the window closes or
   platform_request_quit() flips the running flag. main() should return
   platform_run's result. */
int platform_run(const platform_app_desc_t *desc);

/* Request the run loop to exit. The next iteration runs cleanup_cb and
   returns from platform_run, even if the OS window is still open.
   No-op outside platform_run. */
void platform_request_quit(void);

/* ============================================================
   Accessors — call from inside any of the desc callbacks.
   ============================================================ */

/* Writable framebuffer for the current frame. fb->pixels is valid for
   the duration of one frame_cb call only — do not cache the pointer
   past the call. fb->width/height reflect the current backing size and
   may differ from the previous frame after a resize. */
platform_framebuffer_t *platform_get_framebuffer(void);

/* Monotonic seconds since the start of platform_run. */
double platform_now(void);

/* Seconds elapsed since the previous frame_cb returned. 0 on the first
   frame. */
double platform_dt(void);

/* Number of completed frames (i.e. the index of the current frame_cb
   call, starting at 0). */
uint64_t platform_frame_count(void);

/* Multiplier from logical-point mouse-event coords to framebuffer-pixel
   coords. 1.0 when desc->high_dpi is false (fb is at logical points
   already). On retina with desc->high_dpi true, returns the window's
   backingScaleFactor (typically 2.0). */
double platform_get_dpi_scale(void);

/* Window controls */
void platform_toggle_fullscreen(void);
bool platform_is_fullscreen(void);

/* ============================================================
   Display info (multi-monitor enumeration / video modes).
   ============================================================ */

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
