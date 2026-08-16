#include "debug/display_info.h"
#include "platform/platform.h"
#include <stdio.h>

static const char *connection_type_str(platform_connection_type_t t) {
    switch (t) {
        case PLATFORM_CONNECTION_INTERNAL:    return "Internal";
        case PLATFORM_CONNECTION_HDMI:        return "HDMI";
        case PLATFORM_CONNECTION_DISPLAYPORT: return "DisplayPort";
        case PLATFORM_CONNECTION_THUNDERBOLT: return "Thunderbolt";
        case PLATFORM_CONNECTION_AIRPLAY:     return "AirPlay";
        case PLATFORM_CONNECTION_VGA:         return "VGA";
        case PLATFORM_CONNECTION_DVI:         return "DVI";
        default:                              return "Unknown";
    }
}

static void print_display(int idx, const platform_display_info_t *d, bool is_window) {
    printf("  [%d]%s %-32s  id=%u scale=%.1f main=%d builtin=%d online=%d\n",
           idx, is_window ? " *" : "  ", d->name,
           d->id, (double)d->scale, d->is_main, d->builtin, d->is_online);
    if (d->name_original[0])
        printf("       original=\"%s\"\n", d->name_original);
    printf("       bounds=(%d,%d)+(%dx%dpt)  work=(%d,%d)+(%dx%dpt)  pixels=%dx%d  size=%dx%dmm\n",
           d->bounds_x, d->bounds_y, d->bounds_w, d->bounds_h,
           d->work_x, d->work_y, d->work_w, d->work_h,
           d->pixels_w, d->pixels_h, d->size_mm_w, d->size_mm_h);
    printf("       refresh=%.2fHz  rotation=%d°%s  conn=%s%s\n",
           (double)d->refresh_hz, d->rotation,
           d->rotation_supported ? " (rotatable)" : "",
           connection_type_str(d->connection_type),
           d->mirrors_id ? "  (mirrors another)" : "");
}

/* One row per distinct pixel resolution (collapses HiDPI variants),
   integer refresh rates ≥ 60Hz only (drops video rates like 47.95/48/50/59.94),
   sorted by area ascending. Shaped like a game's resolution dropdown. */
void display_info_print_window_modes(void) {
    typedef struct {
        int  pw, ph;
        int  rates[8];     /* unique integer rates, sorted desc */
        int  rate_count;
        bool has_current;
        int  current_rate;
    } group_t;

    uint32_t my = platform_get_window_display_id();
    platform_video_mode_t modes[128];
    int n = platform_get_display_modes(my, modes, 128);

    group_t groups[16];
    int gc = 0;
    for (int i = 0; i < n; i++) {
        int rate = (int)(modes[i].refresh_hz + 0.5f);
        if (rate < 60) continue;  /* drop 24/30/48/50/etc — video cadences */

        int gi = -1;
        for (int j = 0; j < gc; j++) {
            if (groups[j].pw == modes[i].pixels_w && groups[j].ph == modes[i].pixels_h) {
                gi = j; break;
            }
        }
        if (gi < 0 && gc < 16) {
            gi = gc++;
            groups[gi].pw = modes[i].pixels_w;
            groups[gi].ph = modes[i].pixels_h;
            groups[gi].rate_count = 0;
            groups[gi].has_current = false;
        }
        if (gi < 0) continue;

        group_t *g = &groups[gi];
        bool seen = false;
        for (int r = 0; r < g->rate_count; r++) {
            if (g->rates[r] == rate) { seen = true; break; }
        }
        if (!seen && g->rate_count < 8) {
            int pos = g->rate_count;
            while (pos > 0 && g->rates[pos-1] < rate) { g->rates[pos] = g->rates[pos-1]; pos--; }
            g->rates[pos] = rate;
            g->rate_count++;
        }
        if (modes[i].is_current) {
            g->has_current = true;
            g->current_rate = rate;
        }
    }

    for (int i = 1; i < gc; i++) {
        group_t tmp = groups[i];
        int j = i;
        while (j > 0 && (long long)groups[j-1].pw * groups[j-1].ph > (long long)tmp.pw * tmp.ph) {
            groups[j] = groups[j-1]; j--;
        }
        groups[j] = tmp;
    }

    printf("modes for display id=%u: %d total, %d resolutions\n", my, n, gc);
    for (int i = 0; i < gc; i++) {
        group_t *g = &groups[i];
        char rates[64] = {0};
        size_t off = 0;
        for (int r = 0; r < g->rate_count; r++) {
            int wrote = snprintf(rates + off, sizeof(rates) - off,
                                 "%s%d", r == 0 ? "" : "/", g->rates[r]);
            if (wrote > 0 && (size_t)wrote < sizeof(rates) - off) off += (size_t)wrote;
        }
        if (g->has_current)
            printf("  %4dx%-4d  %sHz  ← current @ %dHz\n", g->pw, g->ph, rates, g->current_rate);
        else
            printf("  %4dx%-4d  %sHz\n", g->pw, g->ph, rates);
    }
}

void display_info_print_all(void) {
    platform_framebuffer_t *fb = platform_get_framebuffer();
    printf("framebuffer: %dx%d\n", fb->width, fb->height);

    platform_display_info_t displays[8];
    int n = platform_get_displays(displays, 8);
    uint32_t my = platform_get_window_display_id();
    printf("displays: %d\n", n);
    for (int i = 0; i < n; i++) {
        print_display(i, &displays[i], displays[i].id == my);
    }
}

