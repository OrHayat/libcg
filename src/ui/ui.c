#include "ui/ui.h"
#include "render/framebuffer.h"
#include <stdio.h>

/* Metrics at scale 1; every use multiplies by ui->scale. */
#define PAD       4
#define GAP       3
#define TITLE_H  12
#define ROW_H    14
#define SWATCH_W 14
#define CLOSE_W  10

static const pcolor_t COL_PANEL  = { .rgba = RGB(0x2A, 0x2E, 0x38) };
static const pcolor_t COL_TITLE  = { .rgba = RGB(0x3A, 0x40, 0x4E) };
static const pcolor_t COL_EDGE   = { .rgba = RGB(0x15, 0x17, 0x1D) };
static const pcolor_t COL_BTN    = { .rgba = RGB(0x44, 0x4B, 0x5A) };
static const pcolor_t COL_HOT    = { .rgba = RGB(0x57, 0x60, 0x72) };
static const pcolor_t COL_DOWN   = { .rgba = RGB(0x24, 0x29, 0x33) };
static const pcolor_t COL_ON     = { .rgba = RGB(0x2E, 0x7D, 0xD1) };
static const pcolor_t COL_TEXT   = { .rgba = RGB(0xEC, 0xEF, 0xF4) };
static const pcolor_t COL_TRACK  = { .rgba = RGB(0x1E, 0x22, 0x2A) };

static bool in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void rect_outline(platform_framebuffer_t *fb, int x, int y, int w, int h, pcolor_t c) {
    framebuffer_fill_rect(fb, x,         y,         w, 1, c);
    framebuffer_fill_rect(fb, x,         y + h - 1, w, 1, c);
    framebuffer_fill_rect(fb, x,         y,         1, h, c);
    framebuffer_fill_rect(fb, x + w - 1, y,         1, h, c);
}

static void blend_rect(platform_framebuffer_t *fb, int x, int y, int w, int h, pcolor_t src) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb->width  ? fb->width  : x + w;
    int y1 = y + h > fb->height ? fb->height : y + h;
    pcolor_t *px = pcolor_pixels(fb->pixels);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            pcolor_t *dst = &px[yy * fb->width + xx];
            *dst = color_blend(*dst, src);
        }
}

/* Alpha backdrop, so a translucent swatch reads as translucent. */
static void checker_rect(platform_framebuffer_t *fb, int x, int y, int w, int h, int cell) {
    const pcolor_t a = { .rgba = RGB(0x90, 0x90, 0x90) };
    const pcolor_t b = { .rgba = RGB(0x60, 0x60, 0x60) };
    if (cell < 1) cell = 1;
    for (int yy = 0; yy < h; yy += cell)
        for (int xx = 0; xx < w; xx += cell) {
            int cw = xx + cell > w ? w - xx : cell;
            int chh = yy + cell > h ? h - yy : cell;
            framebuffer_fill_rect(fb, x + xx, y + yy, cw, chh,
                                  ((xx / cell + yy / cell) & 1) ? b : a);
        }
}

/* Panel geometry. The title bar sits above the content box, and the close
   box is inset in its right end. */
static int panel_h(const ui_panel_t *p) { return p->last_h; }

static void close_box(const ui_t *ui, const ui_panel_t *p, int *x, int *y, int *w, int *h) {
    int s = ui->scale;
    *w = CLOSE_W * s;
    *h = CLOSE_W * s;
    *x = p->x + p->w * s - *w - PAD * s / 2;
    *y = p->y + (TITLE_H * s - *h) / 2;
}

/* ---- input ---- */

bool ui_event(ui_t *ui, const platform_event_t *e) {
    int s = ui->scale ? ui->scale : 1;

    switch (e->kind) {
    case PLATFORM_EV_MOUSE_MOVE:
        ui->mouse_x = e->move.x;
        ui->mouse_y = e->move.y;
        if (ui->drag) {
            ui->drag->x = e->move.x - ui->drag_dx;
            ui->drag->y = e->move.y - ui->drag_dy;
            return true;
        }
        /* A widget holding capture (a slider) still owns the cursor even
           once it has left the panel. */
        return ui->active != 0;

    case PLATFORM_EV_MOUSE_DOWN: {
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) return false;
        ui->mouse_x = e->mouse.x;
        ui->mouse_y = e->mouse.y;

        /* Hit-tested against last frame's rects: the panel is drawn during
           frame(), which has not run yet for this event. A panel moved on
           the previous frame is therefore one frame stale — invisible in
           practice, and the same trade every immediate-mode UI makes. */
        for (int i = ui->seen_count - 1; i >= 0; i--) {
            ui_panel_t *p = ui->seen[i];
            if (!p->open) continue;
            int w = p->w * s, h = panel_h(p);
            if (!in_rect(ui->mouse_x, ui->mouse_y, p->x, p->y, w, h)) continue;

            int cx, cy, cw, ch;
            close_box(ui, p, &cx, &cy, &cw, &ch);
            bool on_close = in_rect(ui->mouse_x, ui->mouse_y, cx, cy, cw, ch);
            if (!on_close && ui->mouse_y < p->y + TITLE_H * s) {
                ui->drag    = p;
                ui->drag_dx = ui->mouse_x - p->x;
                ui->drag_dy = ui->mouse_y - p->y;
            }
            ui->mouse_down    = true;
            ui->mouse_pressed = true;
            return true;                     /* the canvas must not see this */
        }
        ui->mouse_down    = true;
        ui->mouse_pressed = true;
        return false;
    }

    case PLATFORM_EV_MOUSE_UP: {
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) return false;
        ui->mouse_x = e->mouse.x;
        ui->mouse_y = e->mouse.y;
        bool owned = ui->drag != NULL || ui->active != 0;
        ui->drag           = NULL;
        ui->mouse_down     = false;
        ui->mouse_released = true;
        return owned;
    }

    default:
        return false;
    }
}

bool ui_wants_mouse(const ui_t *ui) {
    if (ui->drag || ui->active) return true;
    int s = ui->scale ? ui->scale : 1;
    for (int i = 0; i < ui->seen_count; i++) {
        const ui_panel_t *p = ui->seen[i];
        if (p->open && in_rect(ui->mouse_x, ui->mouse_y, p->x, p->y, p->w * s, panel_h(p)))
            return true;
    }
    return false;
}

/* ---- frame ---- */

void ui_begin_frame(ui_t *ui, platform_framebuffer_t *fb, int scale) {
    ui->fb          = fb;
    ui->scale       = scale < 1 ? 1 : scale;
    ui->hot         = 0;
    ui->seen_count  = 0;
    ui->panel_index = 0;
}

void ui_end_frame(ui_t *ui) {
    /* Capture is released on the frame that sees the release, not in
       ui_event — the widget has to observe mouse_released to report its
       click before active is cleared. */
    if (ui->mouse_released) ui->active = 0;
    ui->mouse_pressed  = false;
    ui->mouse_released = false;
}

static ui_id next_id(ui_t *ui) {
    return (ui_id)((ui->panel_index + 1) * 1000u + ui->seq++);
}

/* Shared hot/active bookkeeping. Returns true on a completed click. */
static bool widget_input(ui_t *ui, ui_id id, int x, int y, int w, int h) {
    bool over = in_rect(ui->mouse_x, ui->mouse_y, x, y, w, h);
    if (over) ui->hot = id;
    if (over && ui->mouse_pressed) ui->active = id;
    return ui->mouse_released && ui->active == id && over;
}

/* ---- panels ---- */

bool ui_panel_begin(ui_t *ui, ui_panel_t *p) {
    if (ui->seen_count < UI_MAX_PANELS) ui->seen[ui->seen_count++] = p;
    if (!p->open) return false;

    int s = ui->scale;
    int w = p->w * s;

    /* Chrome is drawn from last frame's height, because the content that
       determines this frame's height has not been issued yet. The lag
       shows only on the first frame a panel is opened, and only as a
       missing background. */
    int h = panel_h(p);
    if (h > 0) {
        framebuffer_fill_rect(ui->fb, p->x, p->y, w, h, COL_PANEL);
        rect_outline(ui->fb, p->x, p->y, w, h, COL_EDGE);
    }
    framebuffer_fill_rect(ui->fb, p->x, p->y, w, TITLE_H * s, COL_TITLE);
    ui_font_draw(ui->fb, p->x + PAD * s, p->y + (TITLE_H * s - UI_FONT_CELL_H * s) / 2,
                 p->title, s, COL_TEXT);

    /* Close box — the only widget in the title bar, so it takes the
       panel's first id. */
    ui->panel       = p;
    ui->seq         = 0;
    ui->panel_index = ui->seen_count - 1;

    int cx, cy, cw, ch;
    close_box(ui, p, &cx, &cy, &cw, &ch);
    ui_id cid = next_id(ui);
    bool clicked = widget_input(ui, cid, cx, cy, cw, ch);
    framebuffer_fill_rect(ui->fb, cx, cy, cw, ch,
                          ui->active == cid ? COL_DOWN : ui->hot == cid ? COL_HOT : COL_TITLE);
    ui_font_draw(ui->fb, cx + (cw - UI_FONT_ADVANCE * s) / 2,
                 cy + (ch - UI_FONT_CELL_H * s) / 2, "x", s, COL_TEXT);
    if (clicked) p->open = false;

    ui->content_x = p->x + PAD * s;
    ui->content_w = w - 2 * PAD * s;
    ui->cursor_y  = p->y + TITLE_H * s + PAD * s;
    return true;
}

void ui_panel_end(ui_t *ui) {
    ui_panel_t *p = ui->panel;
    p->last_h = ui->cursor_y + PAD * ui->scale - p->y;
    ui->panel = NULL;
}

/* ---- widgets ---- */

void ui_label(ui_t *ui, const char *text) {
    int s = ui->scale;
    ui_font_draw(ui->fb, ui->content_x, ui->cursor_y, text, s, COL_TEXT);
    ui->cursor_y += UI_FONT_CELL_H * s + GAP * s;
}

bool ui_button(ui_t *ui, const char *label, bool selected) {
    int s = ui->scale;
    int x = ui->content_x, y = ui->cursor_y, w = ui->content_w, h = ROW_H * s;
    ui_id id = next_id(ui);
    bool clicked = widget_input(ui, id, x, y, w, h);

    pcolor_t bg = selected ? COL_ON
                : ui->active == id ? COL_DOWN
                : ui->hot    == id ? COL_HOT
                : COL_BTN;
    framebuffer_fill_rect(ui->fb, x, y, w, h, bg);
    rect_outline(ui->fb, x, y, w, h, COL_EDGE);
    ui_font_draw(ui->fb, x + PAD * s, y + (h - UI_FONT_CELL_H * s) / 2, label, s, COL_TEXT);

    ui->cursor_y += h + GAP * s;
    return clicked;
}

bool ui_swatches(ui_t *ui, const color_t *colors, int count, int selected, int *picked) {
    int s   = ui->scale;
    int sw  = SWATCH_W * s;
    int per = ui->content_w / (sw + GAP * s);
    if (per < 1) per = 1;

    bool hit = false;
    for (int i = 0; i < count; i++) {
        int col = i % per, row = i / per;
        int x = ui->content_x + col * (sw + GAP * s);
        int y = ui->cursor_y  + row * (sw + GAP * s);

        ui_id id = next_id(ui);
        if (widget_input(ui, id, x, y, sw, sw)) {
            *picked = i;
            hit = true;
        }
        /* Checker first, colour blended over it: swatches carry real alpha,
           and the checker is what shows a translucent colour as translucent
           instead of quietly rendering it opaque. */
        checker_rect(ui->fb, x, y, sw, sw, 4 * s);
        blend_rect(ui->fb, x, y, sw, sw, color_premultiply(colors[i]));
        rect_outline(ui->fb, x, y, sw, sw,
                     i == selected ? COL_ON : ui->hot == id ? COL_TEXT : COL_EDGE);
    }

    int rows = (count + per - 1) / per;
    ui->cursor_y += rows * (sw + GAP * s);
    return hit;
}

bool ui_slider(ui_t *ui, const char *label, int *value, int min, int max) {
    int s = ui->scale;
    char text[64];
    snprintf(text, sizeof text, "%s %d", label, *value);
    ui_label(ui, text);

    int x = ui->content_x, y = ui->cursor_y, w = ui->content_w, h = ROW_H * s;
    ui_id id = next_id(ui);
    widget_input(ui, id, x, y, w, h);

    /* Dragging keeps working past the track's ends: once this slider owns
       capture the cursor's x is clamped into range, so a fast drag doesn't
       drop the grab the moment it overshoots. */
    bool changed = false;
    if (ui->active == id) {
        int span = max - min;
        int rel  = ui->mouse_x - x;
        if (rel < 0) rel = 0;
        if (rel > w) rel = w;
        int v = min + (w ? (rel * span + w / 2) / w : 0);
        if (v != *value) { *value = v; changed = true; }
    }

    framebuffer_fill_rect(ui->fb, x, y, w, h, COL_TRACK);
    int span = max - min;
    int fill = span ? ((*value - min) * w) / span : 0;
    framebuffer_fill_rect(ui->fb, x, y, fill, h,
                          (ui->active == id || ui->hot == id) ? COL_ON : COL_BTN);
    rect_outline(ui->fb, x, y, w, h, COL_EDGE);

    ui->cursor_y += h + GAP * s;
    return changed;
}
