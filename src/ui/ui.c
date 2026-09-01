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

static const pcolor_t COL_PANEL  = PCOLOR_RGB_INIT(0x2A, 0x2E, 0x38);
static const pcolor_t COL_TITLE  = PCOLOR_RGB_INIT(0x3A, 0x40, 0x4E);
static const pcolor_t COL_EDGE   = PCOLOR_RGB_INIT(0x15, 0x17, 0x1D);
static const pcolor_t COL_BTN    = PCOLOR_RGB_INIT(0x44, 0x4B, 0x5A);
static const pcolor_t COL_HOT    = PCOLOR_RGB_INIT(0x57, 0x60, 0x72);
static const pcolor_t COL_DOWN   = PCOLOR_RGB_INIT(0x24, 0x29, 0x33);
static const pcolor_t COL_ON     = PCOLOR_RGB_INIT(0x2E, 0x7D, 0xD1);
static const pcolor_t COL_TEXT   = PCOLOR_RGB_INIT(0xEC, 0xEF, 0xF4);
static const pcolor_t COL_TRACK  = PCOLOR_RGB_INIT(0x1E, 0x22, 0x2A);

/* 1px border drawn just inside r. */
static void rect_outline(platform_framebuffer_t *fb, rect2d_t r, pcolor_t c) {
    framebuffer_fill_rect(fb, rect2d(r.x,             r.y,             r.w, 1  ), c);
    framebuffer_fill_rect(fb, rect2d(r.x,             r.y + r.h - 1,   r.w, 1  ), c);
    framebuffer_fill_rect(fb, rect2d(r.x,             r.y,             1,   r.h), c);
    framebuffer_fill_rect(fb, rect2d(r.x + r.w - 1,   r.y,             1,   r.h), c);
}

static void blend_rect(platform_framebuffer_t *fb, rect2d_t r, pcolor_t src) {
    r = rect2d_intersect(r, rect2d(0, 0, fb->width, fb->height));
    pcolor_t *px = pcolor_pixels(fb->pixels);
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            pcolor_t *dst = &px[y * fb->width + x];
            *dst = color_blend(*dst, src);
        }
}

/* Alpha backdrop, so a translucent swatch reads as translucent. */
static void checker_rect(platform_framebuffer_t *fb, rect2d_t r, int cell) {
    const pcolor_t a = PCOLOR_RGB_INIT(0x90, 0x90, 0x90);
    const pcolor_t b = PCOLOR_RGB_INIT(0x60, 0x60, 0x60);
    if (cell < 1) cell = 1;
    for (int y = 0; y < r.h; y += cell)
        for (int x = 0; x < r.w; x += cell) {
            /* The last cell in each direction is clipped to the rect
               rather than allowed to overhang it. */
            int cw = x + cell > r.w ? r.w - x : cell;
            int ch = y + cell > r.h ? r.h - y : cell;
            framebuffer_fill_rect(fb, rect2d(r.x + x, r.y + y, cw, ch),
                                  ((x / cell + y / cell) & 1) ? b : a);
        }
}

/* Panel geometry. The title bar sits above the content box, and the close
   box is inset in its right end. */
static rect2d_t panel_rect(const ui_t *ui, const ui_panel_t *p) {
    return rect2d(p->pos.x, p->pos.y, p->w * ui->scale, p->last_h);
}

static rect2d_t close_box(const ui_t *ui, const ui_panel_t *p) {
    int s = ui->scale;
    int side = CLOSE_W * s;
    return rect2d(p->pos.x + p->w * s - side - PAD * s / 2,
                  p->pos.y + (TITLE_H * s - side) / 2,
                  side, side);
}

/* ---- input ---- */

/* Mouse events arrive in LOGICAL POINTS; every rect the UI lays out and
   hit-tests is in FRAMEBUFFER PIXELS. They are the same numbers only at
   scale 1, which is why hit testing appeared to work until the backing
   scale started coming through correctly — then every widget sat twice as
   far out as the cursor reported. Convert once, here, so no hit test
   downstream has to know which space it is in.

   device_scale, NOT scale: the widget size is a look, the device scale is
   the coordinate spaces' actual ratio. */
static vec2_t to_framebuffer(const ui_t *ui, int x, int y) {
    int s = ui->device_scale ? ui->device_scale : 1;
    return vec2(x * s, y * s);
}

bool ui_event(ui_t *ui, const platform_event_t *e) {
    int s = ui->scale ? ui->scale : 1;

    switch (e->kind) {
    case PLATFORM_EV_MOUSE_MOVE:
        ui->mouse = to_framebuffer(ui, e->move.x, e->move.y);
        if (ui->drag) {
            ui->drag->pos = vec2_sub(ui->mouse, ui->drag_grab);
            return true;
        }
        /* A widget holding capture (a slider) still owns the cursor even
           once it has left the panel. */
        return ui->active != 0;

    case PLATFORM_EV_MOUSE_DOWN: {
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) return false;
        ui->mouse = to_framebuffer(ui, e->mouse.x, e->mouse.y);

        /* Hit-tested against last frame's rects: the panel is drawn during
           frame(), which has not run yet for this event. A panel moved on
           the previous frame is therefore one frame stale — invisible in
           practice, and the same trade every immediate-mode UI makes. */

        /* An open dropdown is above everything, so it is tested first. */
        if (ui->menu_open && rect2d_contains(ui->menu, ui->mouse)) {
            ui->mouse_down    = true;
            ui->mouse_pressed = true;
            return true;
        }
        if (ui->menubar_h && ui->mouse.y < ui->menubar_h) {
            ui->mouse_down    = true;
            ui->mouse_pressed = true;
            return true;
        }
        /* Anywhere else dismisses an open menu, and eats the click that
           did so — otherwise closing a menu would also paint a dot. */
        if (ui->menu_open) {
            ui->menu_open = 0;
            return true;
        }

        for (int i = ui->seen_count - 1; i >= 0; i--) {
            ui_panel_t *p = ui->seen[i];
            if (!p->open) continue;
            if (!rect2d_contains(panel_rect(ui, p), ui->mouse)) continue;

            bool on_close = rect2d_contains(close_box(ui, p), ui->mouse);
            if (!on_close && ui->mouse.y < p->pos.y + TITLE_H * s) {
                ui->drag      = p;
                ui->drag_grab = vec2_sub(ui->mouse, p->pos);
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
        ui->mouse = to_framebuffer(ui, e->mouse.x, e->mouse.y);
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
    for (int i = 0; i < ui->seen_count; i++) {
        const ui_panel_t *p = ui->seen[i];
        if (p->open && rect2d_contains(panel_rect(ui, p), ui->mouse)) return true;
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
    ui->menubar_h   = 0;
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
static bool widget_input(ui_t *ui, ui_id id, rect2d_t r) {
    bool over = rect2d_contains(r, ui->mouse);
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
    rect2d_t body = panel_rect(ui, p);
    if (!rect2d_is_empty(body)) {
        framebuffer_fill_rect(ui->fb, body, COL_PANEL);
        rect_outline(ui->fb, body, COL_EDGE);
    }
    framebuffer_fill_rect(ui->fb, rect2d(p->pos.x, p->pos.y, w, TITLE_H * s), COL_TITLE);
    ui_font_draw(ui->fb, p->pos.x + PAD * s,
                 p->pos.y + (TITLE_H * s - UI_FONT_CELL_H * s) / 2,
                 p->title, s, COL_TEXT);

    /* Close box — the only widget in the title bar, so it takes the
       panel's first id. */
    ui->panel       = p;
    ui->seq         = 0;
    ui->panel_index = ui->seen_count - 1;

    rect2d_t cb  = close_box(ui, p);
    ui_id    cid = next_id(ui);
    bool clicked = widget_input(ui, cid, cb);
    framebuffer_fill_rect(ui->fb, cb,
                          ui->active == cid ? COL_DOWN : ui->hot == cid ? COL_HOT : COL_TITLE);
    ui_font_draw(ui->fb, cb.x + (cb.w - UI_FONT_ADVANCE * s) / 2,
                 cb.y + (cb.h - UI_FONT_CELL_H * s) / 2, "x", s, COL_TEXT);
    if (clicked) p->open = false;

    ui->content_x = p->pos.x + PAD * s;
    ui->content_w = w - 2 * PAD * s;
    ui->cursor_y  = p->pos.y + TITLE_H * s + PAD * s;
    return true;
}

void ui_panel_end(ui_t *ui) {
    ui_panel_t *p = ui->panel;
    p->last_h = ui->cursor_y + PAD * ui->scale - p->pos.y;
    ui->panel = NULL;
}

/* ---- widgets ---- */

/* The next full-width row in the current panel, `h` tall. Advancing the
   layout cursor is left to the caller: a widget that draws several rows
   (swatches) advances once for all of them. */
static rect2d_t next_row(const ui_t *ui, int h) {
    return rect2d(ui->content_x, ui->cursor_y, ui->content_w, h);
}

void ui_label(ui_t *ui, const char *text) {
    int s = ui->scale;
    ui_font_draw(ui->fb, ui->content_x, ui->cursor_y, text, s, COL_TEXT);
    ui->cursor_y += UI_FONT_CELL_H * s + GAP * s;
}

bool ui_button(ui_t *ui, const char *label, bool selected) {
    int s = ui->scale;
    rect2d_t r = next_row(ui, ROW_H * s);
    ui_id id = next_id(ui);
    bool clicked = widget_input(ui, id, r);

    pcolor_t bg = selected ? COL_ON
                : ui->active == id ? COL_DOWN
                : ui->hot    == id ? COL_HOT
                : COL_BTN;
    framebuffer_fill_rect(ui->fb, r, bg);
    rect_outline(ui->fb, r, COL_EDGE);
    ui_font_draw(ui->fb, r.x + PAD * s, r.y + (r.h - UI_FONT_CELL_H * s) / 2,
                 label, s, COL_TEXT);

    ui->cursor_y += r.h + GAP * s;
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
        rect2d_t r = rect2d(ui->content_x + col * (sw + GAP * s),
                            ui->cursor_y  + row * (sw + GAP * s),
                            sw, sw);

        ui_id id = next_id(ui);
        if (widget_input(ui, id, r)) {
            *picked = i;
            hit = true;
        }
        /* Checker first, colour blended over it: swatches carry real alpha,
           and the checker is what shows a translucent colour as translucent
           instead of quietly rendering it opaque. */
        checker_rect(ui->fb, r, 4 * s);
        blend_rect(ui->fb, r, color_premultiply(colors[i]));
        rect_outline(ui->fb, r,
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

    rect2d_t r = next_row(ui, ROW_H * s);
    ui_id id = next_id(ui);
    widget_input(ui, id, r);

    /* Dragging keeps working past the track's ends: once this slider owns
       capture the cursor's x is clamped into range, so a fast drag doesn't
       drop the grab the moment it overshoots. */
    bool changed = false;
    if (ui->active == id) {
        int span = max - min;
        int rel  = ui->mouse.x - r.x;
        if (rel < 0)   rel = 0;
        if (rel > r.w) rel = r.w;
        int v = min + (r.w ? (rel * span + r.w / 2) / r.w : 0);
        if (v != *value) { *value = v; changed = true; }
    }

    framebuffer_fill_rect(ui->fb, r, COL_TRACK);
    int span = max - min;
    int fill = span ? ((*value - min) * r.w) / span : 0;
    framebuffer_fill_rect(ui->fb, rect2d(r.x, r.y, fill, r.h),
                          (ui->active == id || ui->hot == id) ? COL_ON : COL_BTN);
    rect_outline(ui->fb, r, COL_EDGE);

    ui->cursor_y += r.h + GAP * s;
    return changed;
}

/* ---- menu bar ---- */

/* Ids for the strip must not collide with any panel's, so it takes the
   slot one past the panel array. */
void ui_menubar_begin(ui_t *ui) {
    int s = ui->scale;
    ui->menubar_h   = TITLE_H * s;
    ui->menubar_x   = PAD * s;
    ui->panel_index = UI_MAX_PANELS;
    ui->seq         = 0;

    framebuffer_fill_rect(ui->fb, rect2d(0, 0, ui->fb->width, ui->menubar_h), COL_TITLE);
    framebuffer_fill_rect(ui->fb, rect2d(0, ui->menubar_h - 1, ui->fb->width, 1), COL_EDGE);
}

void ui_menubar_end(ui_t *ui) { (void)ui; }

bool ui_menu_begin(ui_t *ui, const char *label, int items) {
    int s = ui->scale;
    rect2d_t tab = rect2d(ui->menubar_x, 0,
                          ui_font_width(label, s) + 2 * PAD * s, ui->menubar_h);

    ui_id id = next_id(ui);
    if (widget_input(ui, id, tab)) ui->menu_open = (ui->menu_open == id) ? 0 : id;

    bool open = ui->menu_open == id;
    framebuffer_fill_rect(ui->fb, tab,
                          open ? COL_ON : ui->hot == id ? COL_HOT : COL_TITLE);
    ui_font_draw(ui->fb, tab.x + PAD * s, tab.y + (tab.h - UI_FONT_CELL_H * s) / 2,
                 label, s, COL_TEXT);
    ui->menubar_x += tab.w;

    if (!open) return false;

    /* Width is generous rather than measured: the rows have not been
       issued yet, so there is nothing to measure them from. */
    ui->menu      = rect2d(tab.x, tab.h, 96 * s, items * ROW_H * s + 2 * s);
    ui->menu_item = 0;
    framebuffer_fill_rect(ui->fb, ui->menu, COL_PANEL);
    rect_outline(ui->fb, ui->menu, COL_EDGE);
    return true;
}

void ui_menu_end(ui_t *ui) { (void)ui; }

bool ui_menu_item(ui_t *ui, const char *label, bool checked) {
    int s = ui->scale;
    /* Inset by the 1px (scaled) border so a row never paints over it. */
    rect2d_t r = rect2d(ui->menu.x + s,
                        ui->menu.y + s + ui->menu_item * ROW_H * s,
                        ui->menu.w - 2 * s, ROW_H * s);
    ui->menu_item++;

    ui_id id = next_id(ui);
    bool clicked = widget_input(ui, id, r);

    framebuffer_fill_rect(ui->fb, r, ui->hot == id ? COL_HOT : COL_PANEL);
    int text_y = r.y + (r.h - UI_FONT_CELL_H * s) / 2;
    ui_font_draw(ui->fb, r.x + PAD * s, text_y, checked ? "*" : " ", s, COL_ON);
    ui_font_draw(ui->fb, r.x + PAD * s + UI_FONT_ADVANCE * 2 * s, text_y,
                 label, s, COL_TEXT);

    if (clicked) ui->menu_open = 0;      /* picking an item closes the menu */
    return clicked;
}
