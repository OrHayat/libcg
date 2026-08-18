#include "modes/pattern.h"
#include "render/color.h"
#include "render/draw2d.h"
#include "render/framebuffer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    PATTERN_SOLID = 0,
    PATTERN_GRADIENT,
    PATTERN_CYCLE,
    PATTERN_NOISE,
    PATTERN_PRIMITIVES,
    PATTERN_CUSTOM_COLOR,
} pattern_t;

typedef struct {
    pattern_t pattern;
    color_t   custom_color;          /* straight; premultiplied at draw time */
    bool      bg_checkerboard;
} pattern_state_t;

/* ---- render helpers: fb + args only, no state ---- */

static void render_custom_color(platform_framebuffer_t *fb, color_t c) {
    pcolor_t  src = color_premultiply(c);
    pcolor_t *px  = pcolor_pixels(fb->pixels);
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        px[i] = color_blend(px[i], src);
    }
}

static void render_solid(platform_framebuffer_t *fb) {
    framebuffer_clear(fb, PCOLOR_RGB(0xFF, 0x88, 0x00));
}

static void render_gradient(platform_framebuffer_t *fb) {
    pcolor_t *px = pcolor_pixels(fb->pixels);
    int w = fb->width > 1 ? fb->width - 1 : 1;
    for (int y = 0; y < fb->height; y++) {
        for (int x = 0; x < fb->width; x++) {
            u8 r = (u8)((x * 255) / w);
            px[y * fb->width + x] = PCOLOR_RGB(r, 0, 255 - r);
        }
    }
}

static void render_cycle(platform_framebuffer_t *fb, double t) {
    u8 r = (u8)(128.0 + 127.0 * sin(t));
    u8 g = (u8)(128.0 + 127.0 * sin(t + 2.094));
    u8 b = (u8)(128.0 + 127.0 * sin(t + 4.188));
    framebuffer_clear(fb, PCOLOR_RGB(r, g, b));
}

static void render_noise(platform_framebuffer_t *fb) {
    pcolor_t *px = pcolor_pixels(fb->pixels);
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        u8 v = (u8)(rand() & 0xFF);
        px[i] = PCOLOR_RGB(v, v, v);
    }
}

/* draw2d showcase: one primitive per quadrant, sized off the framebuffer
   so it holds up at any window size and on retina.
     TL  starburst of lines (every octant of the Bresenham walk)
     TR  wireframe triangle
     BL  two overlapping filled triangles — edge coverage under overwrite
     BR  two filled triangles sharing a diagonal — the top-left fill rule
         means the seam shows neither a background gap nor a double-drawn
         line; any dark pixels along it would be a rasterizer bug. */
static void render_primitives(platform_framebuffer_t *fb) {
    framebuffer_clear(fb, PCOLOR_RGB(24, 24, 32));

    int qw = fb->width / 2, qh = fb->height / 2;
    if (qw < 8 || qh < 8) return;

    int m = (qw < qh ? qw : qh) / 8;          /* margin */

    /* --- TL: starburst --- */
    {
        int cx = qw / 2, cy = qh / 2;
        int r  = (qw < qh ? qw : qh) / 2 - m;
        for (int i = 0; i < 24; i++) {
            double a = (double)i * 6.28318530718 / 24.0;
            int ex = cx + (int)(cos(a) * r);
            int ey = cy + (int)(sin(a) * r);
            u8 v = (u8)(80 + (i * 175) / 24);
            draw2d_line(fb, cx, cy, ex, ey, PCOLOR_RGB(v, v, 255 - v));
        }
    }

    /* --- TR: wireframe triangle --- */
    {
        int ox = qw;
        draw2d_triangle_wire(fb,
                             ox + qw / 2, m,
                             ox + qw - m, qh - m,
                             ox + m,      qh - m,
                             PCOLOR_RGB(255, 255, 255));
    }

    /* --- BL: overlapping fills --- */
    {
        int oy = qh;
        draw2d_triangle_fill(fb,
                             m,          oy + m,
                             qw - m,     oy + qh / 2,
                             qw / 2,     oy + qh - m,
                             PCOLOR_RGB(220, 60, 60));
        draw2d_triangle_fill(fb,
                             qw - m,     oy + m,
                             qw / 2 + m, oy + qh - m,
                             m,          oy + qh / 2,
                             PCOLOR_RGB(60, 200, 90));
    }

    /* --- BR: shared-edge quad (seam check) --- */
    {
        int ox = qw, oy = qh;
        int x0 = ox + m,      y0 = oy + m;
        int x1 = ox + qw - m, y1 = oy + qh - m;
        draw2d_triangle_fill(fb, x0, y0, x1, y0, x0, y1, PCOLOR_RGB(70, 110, 230));
        draw2d_triangle_fill(fb, x1, y0, x1, y1, x0, y1, PCOLOR_RGB(240, 160, 40));
    }
}

static void render_checkerboard(platform_framebuffer_t *fb) {
    pcolor_t *px = pcolor_pixels(fb->pixels);
    int cell = 32;
    for (int y = 0; y < fb->height; y++) {
        for (int x = 0; x < fb->width; x++) {
            bool light = (((x / cell) + (y / cell)) & 1) == 0;
            px[y * fb->width + x] = light ? PCOLOR_RGB(220, 220, 220) : PCOLOR_RGB(160, 160, 160);
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
    st->custom_color    = COLOR_RGB(0xFF, 0x88, 0x00);   /* default orange */
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
        if (e->key.repeat || !platform_key_is_plain(e)) break;
        switch (e->key.key) {
        case PLATFORM_KEY_1: st->pattern = PATTERN_SOLID;    break;
        case PLATFORM_KEY_2: st->pattern = PATTERN_GRADIENT; break;
        case PLATFORM_KEY_3: st->pattern = PATTERN_CYCLE;    break;
        case PLATFORM_KEY_4: st->pattern = PATTERN_NOISE;      break;
        case PLATFORM_KEY_5: st->pattern = PATTERN_PRIMITIVES; break;
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
    else                     framebuffer_clear(fb, PCOLOR_CLEAR);

    switch (st->pattern) {
    case PATTERN_SOLID:        render_solid(fb);                          break;
    case PATTERN_GRADIENT:     render_gradient(fb);                       break;
    case PATTERN_CYCLE:        render_cycle(fb, platform_now() * 3.0);    break;
    case PATTERN_NOISE:        render_noise(fb);                          break;
    case PATTERN_PRIMITIVES:   render_primitives(fb);                     break;
    case PATTERN_CUSTOM_COLOR: render_custom_color(fb, st->custom_color); break;
    }
}

static void set_color(app_mode_t *m, color_t c) {
    pattern_state_t *st = m->state;
    st->custom_color = c;
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
