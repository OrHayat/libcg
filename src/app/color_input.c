#include "app/color_input.h"
#include "render/color.h"
#include <stdio.h>

static void reset(color_input_t *ci) {
    ci->active = false;
    ci->len    = 0;
    ci->buf[0] = '\0';
}

static void echo(const color_input_t *ci) {
    printf("\r\x1b[Kcolor input: #%s", ci->buf);
    fflush(stdout);
}

void color_input_begin(color_input_t *ci) {
    reset(ci);
    ci->active = true;
    printf("color input mode (type hex digits, Enter to apply, Esc to cancel)\n");
    printf("color input: #");
    fflush(stdout);
}

bool color_input_event(color_input_t *ci, const platform_event_t *e, u32 *out) {
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.key == PLATFORM_KEY_BACKSPACE && ci->len > 0) {
            ci->buf[--ci->len] = '\0';
            echo(ci);
        } else if (e->key.key == PLATFORM_KEY_ENTER && !e->key.repeat) {
            bool ok = color_parse_hex(ci->buf, ci->len, out);
            if (ok) printf("\ncolor applied: #%s -> 0x%08X\n", ci->buf, *out);
            else    printf("\r\x1b[K#%s is not valid format\n", ci->buf);
            reset(ci);
            return ok;
        } else if (e->key.key == PLATFORM_KEY_ESCAPE && !e->key.repeat) {
            printf("\ncolor input cancelled\n");
            reset(ci);
        }
        break;
    case PLATFORM_EV_TEXT_INPUT: {
        char c = e->text.ch[0];
        if (c != '#' && color_hex_digit(c) >= 0 && ci->len < 8) {
            ci->buf[ci->len++] = c;
            ci->buf[ci->len]   = '\0';
            echo(ci);
        }
    } break;
    default:
        break;
    }
    return false;
}
