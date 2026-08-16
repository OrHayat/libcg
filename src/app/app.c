#include "app/app.h"
#include "app/color_input.h"
#include "debug/display_info.h"
#include <stdio.h>

typedef struct {
    app_mode_t       *modes;
    int           count;
    int           current;
    color_input_t color_input;
    bool          print_mouse_coords;
} app_t;

static app_mode_t *active(app_t *a) {
    return &a->modes[a->current];
}

static void switch_mode(app_t *a, int idx) {
    if (idx < 0 || idx >= a->count || idx == a->current) return;
    app_mode_t *old = active(a);
    if (old->leave) old->leave(old);
    a->current = idx;
    app_mode_t *m = active(a);
    if (m->enter) m->enter(m);
    printf("mode: %s\n", m->name);
}

/* Returns true if the event was consumed by the shell. */
static bool handle_global(app_t *a, const platform_event_t *e) {
    if (e->kind == PLATFORM_EV_MOUSE_MOVE) {
        if (a->print_mouse_coords && !a->color_input.active)
            printf("mouse: (%d, %d)\n", e->move.x, e->move.y);
        return false;                       /* modes still see MOVE */
    }
    if (e->kind != PLATFORM_EV_KEY_DOWN || e->key.repeat) return false;

    platform_key_t k = e->key.key;
    if (k >= PLATFORM_KEY_F1 && k <= PLATFORM_KEY_F12) {
        switch_mode(a, k - PLATFORM_KEY_F1);
        return true;
    }
    switch (k) {
    case PLATFORM_KEY_Q:   platform_request_quit();                       return true;
    case PLATFORM_KEY_F:   platform_toggle_fullscreen();                  return true;
    case PLATFORM_KEY_TAB: switch_mode(a, (a->current + 1) % a->count);   return true;
    case PLATFORM_KEY_T:   display_info_print_all();                      return true;
    case PLATFORM_KEY_L:   display_info_print_window_modes();             return true;
    case PLATFORM_KEY_M:
        a->print_mouse_coords = !a->print_mouse_coords;
        printf("mouse coord printing: %s\n", a->print_mouse_coords ? "ON" : "OFF");
        return true;
    case PLATFORM_KEY_ESCAPE:
        if (platform_is_fullscreen()) { platform_toggle_fullscreen(); return true; }
        return false;
    default:
        return false;
    }
}

static void on_init(void *ud) {
    app_t *a = ud;
    for (int i = 0; i < a->count; i++) {
        app_mode_t *m = &a->modes[i];
        if (m->init) m->init(m);
    }
    app_mode_t *m = active(a);
    if (m->enter) m->enter(m);
}

static void on_cleanup(void *ud) {
    app_t *a = ud;
    app_mode_t *m = active(a);
    if (m->leave) m->leave(m);
    for (int i = a->count - 1; i >= 0; i--) {
        m = &a->modes[i];
        if (m->cleanup) m->cleanup(m);
    }
}

static void on_event(const platform_event_t *e, void *ud) {
    app_t *a = ud;

    /* Overlay first: while active it eats every event, including the
       global hotkeys — 'f' is a hex digit, so letting F-fullscreen run
       here would toggle the window while the user types #ff0000. Esc
       cancels the entry (never exits fullscreen while typing). On a
       completed entry the color goes to the active mode. */
    if (a->color_input.active) {
        u32 c;
        if (color_input_event(&a->color_input, e, &c)) {
            app_mode_t *m = active(a);
            if (m->set_color) m->set_color(m, c);
            else printf("mode %s does not take a color\n", m->name);
        }
        return;
    }

    if (handle_global(a, e)) return;

    if (e->kind == PLATFORM_EV_TEXT_INPUT && e->text.ch[0] == '#') {
        color_input_begin(&a->color_input);
        return;
    }

    app_mode_t *m = active(a);
    if (m->event) m->event(m, e);
}

static void on_frame(void *ud) {
    app_t *a = ud;
    app_mode_t *m = active(a);
    m->frame(m, platform_get_framebuffer());
}

int app_run(app_mode_t *modes, int mode_count, const app_config_t *cfg) {
    app_t a = {
        .modes   = modes,
        .count   = mode_count,
        .current = 0,
    };
    return platform_run(&(platform_app_desc_t){
        .width       = cfg->width,
        .height      = cfg->height,
        .title       = cfg->title,
        .transparent = cfg->transparent,
        .resizable   = cfg->resizable,
        .high_dpi    = cfg->high_dpi,
        .user_data   = &a,
        .init_cb     = on_init,
        .frame_cb    = on_frame,
        .event_cb    = on_event,
        .cleanup_cb  = on_cleanup,
    });
}
