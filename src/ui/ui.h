#ifndef UI_H
#define UI_H

#include "platform/platform.h"
#include "render/color.h"
#include "ui/font.h"

/* ============================================================
   Immediate-mode widgets, retained panels.

   A widget is a function call, not an object: it draws itself and returns
   what happened, so there is nothing to create, register or free, and no
   callback to keep in sync with the state it edits. Call it every frame
   from the mode's frame() and read the return value inline:

       if (ui_button(&ui, "Pencil", st->tool == TOOL_PENCIL))
           paint_exec(st, CMD_TOOL_PENCIL);

   Panels are the exception and *are* retained — a closed panel has to
   reopen where the user left it, so its position and open flag outlive
   the frame. The caller owns that struct; there is no registry.

   Widget identity is (panel slot, call order within the panel), so a
   panel must issue the same widget sequence every frame. A widget that
   needs to disappear should be drawn disabled instead, or the ids after
   it shift and a drag in progress would jump to another widget.
   ============================================================ */

#define UI_MAX_PANELS 8

typedef u32 ui_id;                   /* 0 = nothing */

typedef struct {
    const char *title;
    int         x, y;                /* top-left; the layer moves these on drag */
    int         w;                   /* fixed width; height follows the content */
    bool        open;

    int         last_h;              /* previous frame's height — see ui.c */
} ui_panel_t;

typedef struct {
    platform_framebuffer_t *fb;
    int   scale;                     /* integer pixel multiplier (2 = retina) */

    /* Input, fed by ui_event() and consumed by the widget calls. */
    int   mouse_x, mouse_y;
    bool  mouse_down;                /* left button held */
    bool  mouse_pressed;             /* went down since the last frame */
    bool  mouse_released;            /* went up since the last frame */

    ui_id hot;                       /* under the cursor */
    ui_id active;                    /* pressed on — holds capture until release */

    ui_panel_t *drag;                /* panel being dragged by its title bar */
    int   drag_dx, drag_dy;

    /* Current panel's layout cursor. */
    ui_panel_t *panel;
    int   cursor_y, content_x, content_w;
    u32   seq;
    int   panel_index;

    /* Panels drawn last frame, for event-time hit testing. */
    ui_panel_t *seen[UI_MAX_PANELS];
    int   seen_count;
} ui_t;

/* Feed every event here before the mode sees it. Returns true when the UI
   consumed it — the caller must then not act on it, or a click that hits a
   button would also land on the canvas underneath. */
bool ui_event(ui_t *ui, const platform_event_t *e);

/* True while the cursor is over a panel or a drag is in progress. Lets a
   mode suppress hover-follow behaviour that isn't event-driven. */
bool ui_wants_mouse(const ui_t *ui);

void ui_begin_frame(ui_t *ui, platform_framebuffer_t *fb, int scale);
void ui_end_frame(ui_t *ui);

/* False when the panel is closed — skip its body. Panels nest never. */
bool ui_panel_begin(ui_t *ui, ui_panel_t *p);
void ui_panel_end(ui_t *ui);

void ui_label (ui_t *ui, const char *text);
bool ui_button(ui_t *ui, const char *label, bool selected);

/* Row of colour squares; sets *picked to the index clicked. */
bool ui_swatches(ui_t *ui, const color_t *colors, int count, int selected, int *picked);

/* Drag or click anywhere in the track. Writes through on change. */
bool ui_slider(ui_t *ui, const char *label, int *value, int min, int max);

#endif /* UI_H */
