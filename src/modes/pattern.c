#include "modes/pattern.h"
#include "render/color.h"
#include "render/framebuffer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    PATTERN_SOLID = 0,
    PATTERN_GRADIENT,
    PATTERN_CYCLE,
    PATTERN_NOISE,
    PATTERN_CUSTOM_COLOR,
} pattern_t;

typedef struct {
    pattern_t pattern;
    u32       custom_color;          /* straight ARGB, premultiplied at draw time */
    bool      bg_checkerboard;
} pattern_state_t;

/* ---- render helpers: fb + args only, no state ---- */

static void render_custom_color(platform_framebuffer_t *fb, u32 argb) {
    u32 src = color_premultiply(argb);
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        fb->pixels[i] = color_blend(fb->pixels[i], src);
    }
}

static void render_solid(platform_framebuffer_t *fb) {
    framebuffer_clear(fb, RGB(0xFF, 0x88, 0x00));
}

static void render_gradient(platform_framebuffer_t *fb) {
    int w = fb->width > 1 ? fb->width - 1 : 1;
    for (int y = 0; y < fb->height; y++) {
        for (int x = 0; x < fb->width; x++) {
            u8 r = (u8)((x * 255) / w);
            fb->pixels[y * fb->width + x] = RGB(r, 0, 255 - r);
        }
    }
}

static void render_cycle(platform_framebuffer_t *fb, double t) {
    u8 r = (u8)(128.0 + 127.0 * sin(t));
    u8 g = (u8)(128.0 + 127.0 * sin(t + 2.094));
    u8 b = (u8)(128.0 + 127.0 * sin(t + 4.188));
    framebuffer_clear(fb, RGB(r, g, b));
}

static void render_noise(platform_framebuffer_t *fb) {
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        u8 v = (u8)(rand() & 0xFF);
        fb->pixels[i] = RGB(v, v, v);
    }
}

static void render_checkerboard(platform_framebuffer_t *fb) {
    int cell = 32;
    for (int y = 0; y < fb->height; y++) {
        for (int x = 0; x < fb->width; x++) {
            bool light = (((x / cell) + (y / cell)) & 1) == 0;
            fb->pixels[y * fb->width + x] = light ? RGB(220, 220, 220) : RGB(160, 160, 160);
        }
    }
}

static const char *mouse_button_name(platform_mouse_button_t b) {
    switch (b) {
    case PLATFORM_MOUSE_LEFT:   return "left";
    case PLATFORM_MOUSE_RIGHT:  return "right";
    case PLATFORM_MOUSE_MIDDLE: return "middle";
    default:                    return "?";
    }
}

/* ---- mode callbacks ---- */

static void init(app_mode_t *m) {
    pattern_state_t *st = calloc(1, sizeof *st);
    st->pattern         = PATTERN_SOLID;
    st->custom_color    = 0xFFFF8800;    /* default orange */
    st->bg_checkerboard = false;
    m->state = st;
}

static void cleanup(app_mode_t *m) {
    free(m->state);
    m->state = NULL;
}

static void event(app_mode_t *m, const platform_event_t *e) {
    pattern_state_t *st = m->state;
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.repeat) break;
        switch (e->key.key) {
        case PLATFORM_KEY_1: st->pattern = PATTERN_SOLID;    break;
        case PLATFORM_KEY_2: st->pattern = PATTERN_GRADIENT; break;
        case PLATFORM_KEY_3: st->pattern = PATTERN_CYCLE;    break;
        case PLATFORM_KEY_4: st->pattern = PATTERN_NOISE;    break;
        case PLATFORM_KEY_B:
            st->bg_checkerboard = !st->bg_checkerboard;
            printf("background: %s\n", st->bg_checkerboard ? "checkerboard" : "transparent");
            break;
        default: break;
        }
        break;
    case PLATFORM_EV_MOUSE_DOWN:
        printf("mouse %s pressed at (%d, %d)\n",
               mouse_button_name(e->mouse.btn), e->mouse.x, e->mouse.y);
        break;
    case PLATFORM_EV_MOUSE_UP:
        printf("mouse %s released at (%d, %d)\n",
               mouse_button_name(e->mouse.btn), e->mouse.x, e->mouse.y);
        break;
    case PLATFORM_EV_SCROLL:
        if (e->scroll.dx != 0.0f || e->scroll.dy != 0.0f)
            printf("scroll: dx=%.1f dy=%.1f\n",
                   (double)e->scroll.dx, (double)e->scroll.dy);
        break;
    default:
        break;
    }
}

static void frame(app_mode_t *m, platform_framebuffer_t *fb) {
    pattern_state_t *st = m->state;
    /* Background first — either checker or fully transparent.
       Required so alpha-blended patterns (CUSTOM_COLOR) have a correct
       base and don't pick up last frame's pixels. */
    if (st->bg_checkerboard) render_checkerboard(fb);
    else                     framebuffer_clear(fb, 0x00000000);

    switch (st->pattern) {
    case PATTERN_SOLID:        render_solid(fb);                          break;
    case PATTERN_GRADIENT:     render_gradient(fb);                       break;
    case PATTERN_CYCLE:        render_cycle(fb, platform_now() * 3.0);    break;
    case PATTERN_NOISE:        render_noise(fb);                          break;
    case PATTERN_CUSTOM_COLOR: render_custom_color(fb, st->custom_color); break;
    }
}

static void set_color(app_mode_t *m, u32 argb) {
    pattern_state_t *st = m->state;
    st->custom_color = argb;
    st->pattern      = PATTERN_CUSTOM_COLOR;
}

app_mode_t pattern_mode(void) {
    return (app_mode_t){
        .name      = "pattern",
        .init      = init,
        .cleanup   = cleanup,
        .event     = event,
        .frame     = frame,
        .set_color = set_color,
    };
}
