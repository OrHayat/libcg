#include "platform.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
   Event queue. NSResponder methods on LibcgView and NSWindowDelegate
   methods on AppDelegate enqueue platform_event_t. drain_event_queue
   drains the queue and dispatches each via desc->event_cb. Drain runs
   at the top of present_frame, so ordering is: pump_events fills the
   queue → present_frame drains it → frame_cb runs → commit. During
   tracking-mode resize the delegate calls present_frame directly,
   which still drains its own queued resize events before frame_cb.
   ============================================================ */

#define EVENT_QUEUE_CAPACITY 256

/* ============================================================
   Buffer ownership model:
   - state.fb.pixels is the active rendering target. frame_cb writes into it.
   - On commit_to_layer the buffer is handed (no copy) to a CGDataProvider
     whose release callback frees it. The CGImage built from that provider
     becomes layer.contents; the buffer lives until the layer drops the
     image (typically when the next commit replaces it).
   - commit_to_layer then allocates a fresh state.fb.pixels at state.next_w
     × state.next_h for the following frame.
   - Resize delegates only update state.next_w/h. The size flips on the
     next sync_fb_size — never mid-frame, never inside the pump.
   ============================================================ */
typedef struct {
    NSApplication *ns_app;
    NSWindow *ns_window;
    NSView *ns_view;
    platform_framebuffer_t fb;     /* pixels owned for one frame at a time */
    int next_w, next_h;            /* pending backing size */
    int last_mouse_x, last_mouse_y;/* for synthesizing dx/dy on MOUSE_MOVE */
    bool running;

    /* Event ring buffer */
    platform_event_t events[EVENT_QUEUE_CAPACITY];
    int events_head;
    int events_tail;

    /* Active platform_run desc + timing */
    const platform_app_desc_t *active_desc;
    CFAbsoluteTime t0;
    CFAbsoluteTime t_prev;
    CFAbsoluteTime t_now;          /* start-of-current-frame_cb timestamp */
    uint64_t frame_count;
} platform_state_t;

static platform_state_t state;

// macOS virtual-keycode → platform_key_t lookup table.
// Values are ADB hardware scan codes from Apple's HIToolbox/Events.h (kVK_*).
// Stable since 1986; unrelated to ASCII, based on physical keyboard position.
// Copied from GLFW's cocoa_init.m createKeyTables() — same encoding, different
// destination enum. Unlisted slots default to PLATFORM_KEY_UNKNOWN (0).
// clang-format off
static const platform_key_t kc_to_key[256] = {
    [0x00] = PLATFORM_KEY_A,          [0x0B] = PLATFORM_KEY_B,
    [0x08] = PLATFORM_KEY_C,          [0x02] = PLATFORM_KEY_D,
    [0x0E] = PLATFORM_KEY_E,          [0x03] = PLATFORM_KEY_F,
    [0x05] = PLATFORM_KEY_G,          [0x04] = PLATFORM_KEY_H,
    [0x22] = PLATFORM_KEY_I,          [0x26] = PLATFORM_KEY_J,
    [0x28] = PLATFORM_KEY_K,          [0x25] = PLATFORM_KEY_L,
    [0x2E] = PLATFORM_KEY_M,          [0x2D] = PLATFORM_KEY_N,
    [0x1F] = PLATFORM_KEY_O,          [0x23] = PLATFORM_KEY_P,
    [0x0C] = PLATFORM_KEY_Q,          [0x0F] = PLATFORM_KEY_R,
    [0x01] = PLATFORM_KEY_S,          [0x11] = PLATFORM_KEY_T,
    [0x20] = PLATFORM_KEY_U,          [0x09] = PLATFORM_KEY_V,
    [0x0D] = PLATFORM_KEY_W,          [0x07] = PLATFORM_KEY_X,
    [0x10] = PLATFORM_KEY_Y,          [0x06] = PLATFORM_KEY_Z,

    [0x1D] = PLATFORM_KEY_0,          [0x12] = PLATFORM_KEY_1,
    [0x13] = PLATFORM_KEY_2,          [0x14] = PLATFORM_KEY_3,
    [0x15] = PLATFORM_KEY_4,          [0x17] = PLATFORM_KEY_5,
    [0x16] = PLATFORM_KEY_6,          [0x1A] = PLATFORM_KEY_7,
    [0x1C] = PLATFORM_KEY_8,          [0x19] = PLATFORM_KEY_9,

    [0x27] = PLATFORM_KEY_APOSTROPHE, [0x2A] = PLATFORM_KEY_BACKSLASH,
    [0x2B] = PLATFORM_KEY_COMMA,      [0x18] = PLATFORM_KEY_EQUAL,
    [0x32] = PLATFORM_KEY_GRAVE,      [0x21] = PLATFORM_KEY_LEFT_BRACKET,
    [0x1B] = PLATFORM_KEY_MINUS,      [0x2F] = PLATFORM_KEY_PERIOD,
    [0x1E] = PLATFORM_KEY_RIGHT_BRACKET,
    [0x29] = PLATFORM_KEY_SEMICOLON,  [0x2C] = PLATFORM_KEY_SLASH,

    [0x33] = PLATFORM_KEY_BACKSPACE,  [0x75] = PLATFORM_KEY_DELETE,
    [0x77] = PLATFORM_KEY_END,        [0x24] = PLATFORM_KEY_ENTER,
    [0x35] = PLATFORM_KEY_ESCAPE,     [0x73] = PLATFORM_KEY_HOME,
    [0x72] = PLATFORM_KEY_INSERT,     [0x79] = PLATFORM_KEY_PAGE_DOWN,
    [0x74] = PLATFORM_KEY_PAGE_UP,    [0x31] = PLATFORM_KEY_SPACE,
    [0x30] = PLATFORM_KEY_TAB,

    [0x7B] = PLATFORM_KEY_LEFT,       [0x7C] = PLATFORM_KEY_RIGHT,
    [0x7E] = PLATFORM_KEY_UP,         [0x7D] = PLATFORM_KEY_DOWN,

    [0x39] = PLATFORM_KEY_CAPS_LOCK,
    [0x3A] = PLATFORM_KEY_LEFT_ALT,    [0x3B] = PLATFORM_KEY_LEFT_CONTROL,
    [0x38] = PLATFORM_KEY_LEFT_SHIFT,  [0x37] = PLATFORM_KEY_LEFT_SUPER,
    [0x3D] = PLATFORM_KEY_RIGHT_ALT,   [0x3E] = PLATFORM_KEY_RIGHT_CONTROL,
    [0x3C] = PLATFORM_KEY_RIGHT_SHIFT, [0x36] = PLATFORM_KEY_RIGHT_SUPER,

    [0x7A] = PLATFORM_KEY_F1,         [0x78] = PLATFORM_KEY_F2,
    [0x63] = PLATFORM_KEY_F3,         [0x76] = PLATFORM_KEY_F4,
    [0x60] = PLATFORM_KEY_F5,         [0x61] = PLATFORM_KEY_F6,
    [0x62] = PLATFORM_KEY_F7,         [0x64] = PLATFORM_KEY_F8,
    [0x65] = PLATFORM_KEY_F9,         [0x6D] = PLATFORM_KEY_F10,
    [0x67] = PLATFORM_KEY_F11,        [0x6F] = PLATFORM_KEY_F12,
};
// clang-format on

/* --- Forward decls --- */
static void enqueue_event(const platform_event_t *e);
static void sync_fb_size(void);
static void present_frame(void);
static void commit_to_layer(void);
static NSSize get_backing_size(NSWindow *window);

/* Push an event into the ring buffer. Drops on overflow (logs to stderr).
   Single-threaded — only called from AppKit handlers and delegate
   methods on the main thread. */
static void enqueue_event(const platform_event_t *e) {
    int next = (state.events_tail + 1) % EVENT_QUEUE_CAPACITY;
    if (next == state.events_head) {
        fprintf(stderr, "platform: event queue full, dropping event kind=%d\n", e->kind);
        return;
    }
    state.events[state.events_tail] = *e;
    state.events[state.events_tail].frame_index = state.frame_count;
    state.events_tail = next;
}

/* Drain queued events into desc->event_cb. Called from present_frame
   before frame_cb. Drops all events if event_cb is NULL. */
static void drain_event_queue(void) {
    if (!state.active_desc) {
        state.events_head = state.events_tail;
        return;
    }
    if (!state.active_desc->event_cb) {
        state.events_head = state.events_tail;
        return;
    }
    while (state.events_head != state.events_tail) {
        platform_event_t e = state.events[state.events_head];
        state.events_head = (state.events_head + 1) % EVENT_QUEUE_CAPACITY;
        state.active_desc->event_cb(&e, state.active_desc->user_data);
    }
}

/* --- LibcgView (custom NSView; NSResponder for input events) --- */

@interface LibcgView : NSView
@end

@implementation LibcgView
/* No-op. Layer-backed view with explicit layer.contents — frames flow into
   the layer via commit_to_layer. AppKit composites the existing contents
   on damage / expose / occlusion without our involvement. */
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
}

- (BOOL)isFlipped {
    return YES; /* top-left origin to match framebuffer */
}

// Required for the view to receive keyboard events. Without this, AppKit
// sends key events to the window's field editor instead.
- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)nsevent {
    platform_key_t key = kc_to_key[[nsevent keyCode] & 0xFF];
    if (key == PLATFORM_KEY_UNKNOWN) return;

    platform_event_t e = {
        .kind = PLATFORM_EV_KEY_DOWN,
        .key  = { .key = key, .repeat = (bool)[nsevent isARepeat] },
    };
    enqueue_event(&e);

    /* Capture printable ASCII characters as PLATFORM_EV_TEXT_INPUT events,
       one per codepoint. Multi-byte UTF-8 input would land here as multiple
       single-byte events — caller would see fragmented codepoints. The
       platform_text_event_t.ch array can hold a full 1-4 byte codepoint;
       extending to proper UTF-8 codepoint slicing is a follow-up. */
    NSString *chars = [nsevent characters];
    if (chars && [chars length] > 0) {
        const char *cstr = [chars UTF8String];
        for (const char *p = cstr; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c >= 0x20 && c < 0x7F) {
                platform_event_t te = {
                    .kind = PLATFORM_EV_TEXT_INPUT,
                };
                te.text.ch[0] = (char)c;
                te.text.ch[1] = '\0';
                enqueue_event(&te);
            }
        }
    }
}

- (void)keyUp:(NSEvent *)nsevent {
    platform_key_t key = kc_to_key[[nsevent keyCode] & 0xFF];
    if (key == PLATFORM_KEY_UNKNOWN) return;

    platform_event_t e = {
        .kind = PLATFORM_EV_KEY_UP,
        .key  = { .key = key, .repeat = false },
    };
    enqueue_event(&e);
}

// Modifier keys (shift, ctrl, alt, cmd, caps lock) don't fire keyDown:/keyUp:.
// AppKit sends flagsChanged: instead. Use device-specific masks (NX_DEVICE*)
// from IOKit to distinguish left from right modifiers — the high-level
// NSEventModifierFlag* constants merge both sides into one bit.
// clang-format off
- (void)flagsChanged:(NSEvent *)nsevent {
    unsigned short keyCode = [nsevent keyCode] & 0xFF;
    platform_key_t key = kc_to_key[keyCode];
    if (key == PLATFORM_KEY_UNKNOWN) return;

    NSUInteger flags = [nsevent modifierFlags];
    bool is_down;
    switch (keyCode) {
        case 0x38: is_down = (flags & NX_DEVICELSHIFTKEYMASK) != 0; break;
        case 0x3C: is_down = (flags & NX_DEVICERSHIFTKEYMASK) != 0; break;
        case 0x3B: is_down = (flags & NX_DEVICELCTLKEYMASK)   != 0; break;
        case 0x3E: is_down = (flags & NX_DEVICERCTLKEYMASK)   != 0; break;
        case 0x3A: is_down = (flags & NX_DEVICELALTKEYMASK)   != 0; break;
        case 0x3D: is_down = (flags & NX_DEVICERALTKEYMASK)   != 0; break;
        case 0x37: is_down = (flags & NX_DEVICELCMDKEYMASK)   != 0; break;
        case 0x36: is_down = (flags & NX_DEVICERCMDKEYMASK)   != 0; break;
        case 0x39: is_down = (flags & NSEventModifierFlagCapsLock) != 0; break;
        default: return;
    }
    // clang-format on

    platform_event_t e = {
        .kind = is_down ? PLATFORM_EV_KEY_DOWN : PLATFORM_EV_KEY_UP,
        .key  = { .key = key, .repeat = false },
    };
    enqueue_event(&e);
}

- (NSPoint)mousePosition:(NSEvent *)nsevent {
    return [self convertPoint:[nsevent locationInWindow] fromView:nil];
}

- (void)dispatchMouseMove:(NSEvent *)nsevent {
    NSPoint p = [self mousePosition:nsevent];
    int x = (int)p.x, y = (int)p.y;
    platform_event_t e = {
        .kind = PLATFORM_EV_MOUSE_MOVE,
        .move = {
            .x = x, .y = y,
            .dx = x - state.last_mouse_x,
            .dy = y - state.last_mouse_y,
        },
    };
    state.last_mouse_x = x;
    state.last_mouse_y = y;
    enqueue_event(&e);
}

- (void)mouseMoved:(NSEvent *)nsevent        { [self dispatchMouseMove:nsevent]; }
- (void)mouseDragged:(NSEvent *)nsevent      { [self dispatchMouseMove:nsevent]; }
- (void)rightMouseDragged:(NSEvent *)nsevent { [self dispatchMouseMove:nsevent]; }
- (void)otherMouseDragged:(NSEvent *)nsevent { [self dispatchMouseMove:nsevent]; }

- (void)dispatchMouseButton:(NSEvent *)nsevent kind:(platform_event_kind_t)kind btn:(platform_mouse_button_t)btn {
    NSPoint p = [self mousePosition:nsevent];
    int x = (int)p.x, y = (int)p.y;
    platform_event_t e = {
        .kind = kind,
        .mouse = { .btn = btn, .x = x, .y = y },
    };
    enqueue_event(&e);
    state.last_mouse_x = x;
    state.last_mouse_y = y;
}

- (void)mouseDown:(NSEvent *)e        { [self dispatchMouseButton:e kind:PLATFORM_EV_MOUSE_DOWN btn:PLATFORM_MOUSE_LEFT]; }
- (void)mouseUp:(NSEvent *)e          { [self dispatchMouseButton:e kind:PLATFORM_EV_MOUSE_UP   btn:PLATFORM_MOUSE_LEFT]; }
- (void)rightMouseDown:(NSEvent *)e   { [self dispatchMouseButton:e kind:PLATFORM_EV_MOUSE_DOWN btn:PLATFORM_MOUSE_RIGHT]; }
- (void)rightMouseUp:(NSEvent *)e     { [self dispatchMouseButton:e kind:PLATFORM_EV_MOUSE_UP   btn:PLATFORM_MOUSE_RIGHT]; }
- (void)otherMouseDown:(NSEvent *)e   { [self dispatchMouseButton:e kind:PLATFORM_EV_MOUSE_DOWN btn:PLATFORM_MOUSE_MIDDLE]; }
- (void)otherMouseUp:(NSEvent *)e     { [self dispatchMouseButton:e kind:PLATFORM_EV_MOUSE_UP   btn:PLATFORM_MOUSE_MIDDLE]; }

- (void)scrollWheel:(NSEvent *)nsevent {
    platform_event_t e = {
        .kind = PLATFORM_EV_SCROLL,
        .scroll = {
            .dx = (float)[nsevent scrollingDeltaX],
            .dy = (float)[nsevent scrollingDeltaY],
        },
    };
    enqueue_event(&e);
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

/* --- Forward declarations of private helpers --- */

static NSApplication *create_application(void);
static NSMenu *create_menu(const char *title);
static NSWindow *create_window(const platform_app_desc_t *desc, id delegate);
static void activate_app(NSApplication *app);
static void pump_events(NSApplication *app);
static bool platform_init(const platform_app_desc_t *desc);
static void platform_shutdown(void);

/* --- AppDelegate (NSWindowDelegate) --- */

@implementation AppDelegate

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    state.running = false;
    platform_event_t e = { .kind = PLATFORM_EV_QUIT_REQUESTED };
    enqueue_event(&e);
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    platform_event_t e = { .kind = PLATFORM_EV_FOCUS };
    enqueue_event(&e);
}

- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    platform_event_t e = { .kind = PLATFORM_EV_UNFOCUS };
    enqueue_event(&e);
}

/* All resize delegates: stash the new backing size in state.next_w/h,
   queue a PLATFORM_EV_RESIZE, then trigger a present so frames keep
   flowing during AppKit's tracking-mode runloop. */
static void update_pending_size_and_queue_event(void) {
    NSSize backing = get_backing_size(state.ns_window);
    state.next_w = (int)backing.width;
    state.next_h = (int)backing.height;

    NSRect content = [state.ns_window contentRectForFrameRect:[state.ns_window frame]];
    platform_event_t e = {
        .kind = PLATFORM_EV_RESIZE,
        .resize = {
            .w    = (int)content.size.width,
            .h    = (int)content.size.height,
            .fb_w = state.next_w,
            .fb_h = state.next_h,
        },
    };
    enqueue_event(&e);
}

- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    update_pending_size_and_queue_event();
    present_frame();
}

// Fires when the window moves between displays with different scale factors
// (e.g. retina internal ↔ 1× external) or the user changes display scaling.
// Logical point size doesn't change, so windowDidResize: doesn't fire — but
// the backing pixel size does, and our framebuffer needs to follow.
- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    (void)notification;
    update_pending_size_and_queue_event();
    present_frame();
}

// Fires when the window crosses to a different display. Same-scale moves
// don't fire backingProperties; same-scale moves no-op naturally because
// next_w/next_h won't change.
- (void)windowDidChangeScreen:(NSNotification *)notification {
    (void)notification;
    update_pending_size_and_queue_event();
    present_frame();
}

@end

/* --- Private helpers --- */

// Return the content area size in backing pixels (physical device pixels).
// On retina displays this is 2× the logical size.
static NSSize get_backing_size(NSWindow *window) {
    NSView *view = [window contentView];
    NSSize logical = [view bounds].size;
    return [view convertSizeToBacking:logical];
}

/* Bring fb.pixels in line with the pending size. Same-size = no-op.
   Old buffer (if any) is freed; new one is calloc'd fresh. */
static void sync_fb_size(void) {
    if (state.fb.pixels && state.fb.width == state.next_w && state.fb.height == state.next_h) {
        return;
    }
    free(state.fb.pixels);
    state.fb.width  = state.next_w;
    state.fb.height = state.next_h;
    state.fb.pixels = calloc((size_t)state.next_w * (size_t)state.next_h, sizeof(uint32_t));
}

static NSApplication *create_application(void) {
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    return app;
}

// Build a minimal menu bar: one app menu with a Quit item bound to Cmd-Q.
static NSMenu *create_menu(const char *title) {
    NSMenu *menubar = [[NSMenu alloc] init];
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];

    NSMenu *appMenu = [[NSMenu alloc] init];
    NSString *quitTitle = [NSString stringWithFormat:@"Quit %s", title];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                      action:@selector(terminate:)
                                               keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];

    return menubar;
}

static NSWindow *create_window(const platform_app_desc_t *desc, id delegate) {
    NSUInteger style = NSWindowStyleMaskTitled
                     | NSWindowStyleMaskClosable
                     | NSWindowStyleMaskMiniaturizable;
    if (desc->resizable) style |= NSWindowStyleMaskResizable;

    NSRect frame = NSMakeRect(0, 0, desc->width, desc->height);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [window setTitle:[NSString stringWithUTF8String:desc->title ? desc->title : "libcg"]];
    [window center];
    [window setDelegate:delegate];

    if (desc->transparent) {
        [window setOpaque:NO];
        [window setBackgroundColor:[NSColor clearColor]];
    }

    [window makeKeyAndOrderFront:nil];
    return window;
}

// activate was added in macOS 14 and activateIgnoringOtherApps: was deprecated
// in the same release. Branch at runtime to use the right API on each version.
static void activate_app(NSApplication *app) {
    if (@available(macOS 14.0, *)) {
        [app activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [app activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}

// Drain any pending AppKit events. distantPast = non-blocking poll. The
// NSResponder methods on LibcgView and NSWindowDelegate methods on
// AppDelegate (called via [NSApp sendEvent:] / notifications) push
// platform_event_t entries into our event queue.
static void pump_events(NSApplication *app) {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [app nextEventMatchingMask:NSEventMaskAny
                                         untilDate:[NSDate distantPast]
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES])) {
            [app sendEvent:event];
        }
    }
}

static bool platform_init(const platform_app_desc_t *desc) {
    state.ns_app = create_application();

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [state.ns_app setDelegate:delegate];

    const char *title = desc->title ? desc->title : "libcg";
    [state.ns_app setMainMenu:create_menu(title)];

    state.ns_window = create_window(desc, delegate);

    state.ns_view = [[LibcgView alloc] initWithFrame:NSMakeRect(0, 0, desc->width, desc->height)];
    [state.ns_window setContentView:state.ns_view];
    [state.ns_window makeFirstResponder:state.ns_view];
    [state.ns_window setAcceptsMouseMovedEvents:YES];

    [state.ns_view setWantsLayer:YES];
    state.ns_view.layerContentsPlacement = NSViewLayerContentsPlacementScaleProportionallyToFit;

    /* Allocate framebuffer at backing pixels (HiDPI-aware) or logical points
       (1× rendering, CALayer scales). */
    NSSize backing = desc->high_dpi
        ? get_backing_size(state.ns_window)
        : NSMakeSize(desc->width, desc->height);
    state.next_w = (int)backing.width;
    state.next_h = (int)backing.height;
    state.fb.width  = state.next_w;
    state.fb.height = state.next_h;
    state.fb.pixels = calloc((size_t)state.next_w * (size_t)state.next_h, sizeof(uint32_t));

    activate_app(state.ns_app);
    pump_events(state.ns_app);

    state.running = true;
    return true;
}

static void platform_shutdown(void) {
    free(state.fb.pixels);
    state.fb.pixels = NULL;
    [state.ns_window close];
    state.running = false;
}

/* --- Public API --- */

void platform_request_quit(void) {
    state.running = false;
}

platform_framebuffer_t *platform_get_framebuffer(void) {
    return &state.fb;
}

double platform_now(void) {
    if (state.t0 == 0.0) return 0.0;
    return state.t_now - state.t0;
}

double platform_dt(void) {
    if (state.t_prev == 0.0) return 0.0;
    return state.t_now - state.t_prev;
}

uint64_t platform_frame_count(void) {
    return state.frame_count;
}

void platform_toggle_fullscreen(void) {
    [state.ns_window toggleFullScreen:nil];
}

bool platform_is_fullscreen(void) {
    return ([state.ns_window styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

/* --- Display info --- */

// Find the IODisplayConnect IOService matching a CGDirectDisplayID by
// comparing vendor/product/serial. Returns 0 if no match. Caller must
// IOObjectRelease() the result if non-zero.
static io_service_t io_service_for_display(CGDirectDisplayID display) {
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IODisplayConnect"),
                                     &iter) != kIOReturnSuccess) {
        return 0;
    }

    uint32_t want_v = CGDisplayVendorNumber(display);
    uint32_t want_p = CGDisplayModelNumber(display);
    uint32_t want_s = CGDisplaySerialNumber(display);

    io_service_t result = 0;
    io_service_t serv;
    while ((serv = IOIteratorNext(iter)) != 0) {
        CFDictionaryRef info = IODisplayCreateInfoDictionary(serv, 0);
        if (info) {
            uint32_t v = 0, p = 0, s = 0;
            CFNumberRef vn = CFDictionaryGetValue(info, CFSTR(kDisplayVendorID));
            CFNumberRef pn = CFDictionaryGetValue(info, CFSTR(kDisplayProductID));
            CFNumberRef sn = CFDictionaryGetValue(info, CFSTR(kDisplaySerialNumber));
            if (vn) CFNumberGetValue(vn, kCFNumberSInt32Type, &v);
            if (pn) CFNumberGetValue(pn, kCFNumberSInt32Type, &p);
            if (sn) CFNumberGetValue(sn, kCFNumberSInt32Type, &s);
            CFRelease(info);

            if (v == want_v && p == want_p && s == want_s) {
                result = serv;
                break;
            }
        }
        IOObjectRelease(serv);
    }
    IOObjectRelease(iter);
    return result;
}

// Read raw EDID name from IODisplayConnect. The DisplayProductName key holds
// a per-language NSDictionary; we pick user's preferred language, then "en",
// then any. Empty out[] if no name is available (typical for built-in panels).
static void fill_edid_name(io_service_t serv, char *out, size_t cap) {
    out[0] = '\0';
    if (!serv) return;

    CFDictionaryRef info = IODisplayCreateInfoDictionary(serv, 0);
    if (!info) return;

    CFDictionaryRef names = CFDictionaryGetValue(info, CFSTR(kDisplayProductName));
    if (!names || CFGetTypeID(names) != CFDictionaryGetTypeID() || CFDictionaryGetCount(names) == 0) {
        CFRelease(info);
        return;
    }

    CFStringRef name_str = NULL;
    NSArray *langs = [NSLocale preferredLanguages];
    if ([langs count] > 0) {
        name_str = CFDictionaryGetValue(names, (__bridge CFStringRef)langs[0]);
    }
    if (!name_str) name_str = CFDictionaryGetValue(names, CFSTR("en"));
    if (!name_str) {
        CFIndex count = CFDictionaryGetCount(names);
        if (count > 0) {
            const void **values = (const void **)malloc(sizeof(void *) * (size_t)count);
            CFDictionaryGetKeysAndValues(names, NULL, values);
            name_str = (CFStringRef)values[0];
            free(values);
        }
    }

    if (name_str && CFGetTypeID(name_str) == CFStringGetTypeID()) {
        CFStringGetCString(name_str, out, (CFIndex)cap, kCFStringEncodingUTF8);
    }
    CFRelease(info);
}

// IOFramebuffer (parent of IODisplayConnect) has the rotate-mask property.
// Bit 0 is always 0° (identity); >1 means at least one other rotation works.
static bool query_rotation_supported(io_service_t serv) {
    if (!serv) return false;

    io_service_t parent = 0;
    if (IORegistryEntryGetParentEntry(serv, kIOServicePlane, &parent) != kIOReturnSuccess) {
        return false;
    }

    bool supported = false;
    CFNumberRef rot = (CFNumberRef)IORegistryEntryCreateCFProperty(
        parent, CFSTR("rotate-mask"), kCFAllocatorDefault, 0);
    if (rot && CFGetTypeID(rot) == CFNumberGetTypeID()) {
        uint32_t mask = 0;
        CFNumberGetValue(rot, kCFNumberSInt32Type, &mask);
        supported = mask > 1;
    }
    if (rot) CFRelease(rot);
    IOObjectRelease(parent);
    return supported;
}

// Walk parent chain looking for a class name that identifies the connection.
// Built-in is detected upfront via CGDisplayIsBuiltin and short-circuits here.
static platform_connection_type_t detect_connection_type(io_service_t serv, bool builtin) {
    if (builtin) return PLATFORM_CONNECTION_INTERNAL;
    if (!serv)   return PLATFORM_CONNECTION_UNKNOWN;

    io_service_t s = serv;
    IOObjectRetain(s);
    platform_connection_type_t result = PLATFORM_CONNECTION_UNKNOWN;
    for (int i = 0; i < 8; i++) {
        io_name_t class_name;
        if (IOObjectGetClass(s, class_name) == kIOReturnSuccess) {
            if      (strstr(class_name, "HDMI"))        { result = PLATFORM_CONNECTION_HDMI;        break; }
            else if (strstr(class_name, "DisplayPort")) { result = PLATFORM_CONNECTION_DISPLAYPORT; break; }
            else if (strstr(class_name, "Thunderbolt")) { result = PLATFORM_CONNECTION_THUNDERBOLT; break; }
            else if (strstr(class_name, "AirPlay"))     { result = PLATFORM_CONNECTION_AIRPLAY;     break; }
            else if (strstr(class_name, "VGA"))         { result = PLATFORM_CONNECTION_VGA;         break; }
            else if (strstr(class_name, "DVI"))         { result = PLATFORM_CONNECTION_DVI;         break; }
        }
        io_service_t parent = 0;
        if (IORegistryEntryGetParentEntry(s, kIOServicePlane, &parent) != kIOReturnSuccess) {
            break;
        }
        IOObjectRelease(s);
        s = parent;
    }
    IOObjectRelease(s);
    return result;
}

static NSScreen *screen_for_display(CGDirectDisplayID display) {
    for (NSScreen *s in [NSScreen screens]) {
        NSNumber *num = [s deviceDescription][@"NSScreenNumber"];
        if (num && (CGDirectDisplayID)[num unsignedIntValue] == display) {
            return s;
        }
    }
    return nil;
}

static void fill_display_info(CGDirectDisplayID id, platform_display_info_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = id;

    NSScreen *screen = screen_for_display(id);
    if (screen) {
        const char *cstr = [[screen localizedName] UTF8String];
        if (cstr) strncpy(out->name, cstr, sizeof(out->name) - 1);

        out->scale = (float)[screen backingScaleFactor];

        NSRect frame = [screen frame];
        out->bounds_x = (int)frame.origin.x;
        out->bounds_y = (int)frame.origin.y;
        out->bounds_w = (int)frame.size.width;
        out->bounds_h = (int)frame.size.height;

        NSRect work = [screen visibleFrame];
        out->work_x = (int)work.origin.x;
        out->work_y = (int)work.origin.y;
        out->work_w = (int)work.size.width;
        out->work_h = (int)work.size.height;

        out->refresh_hz = (float)[screen maximumFramesPerSecond];
    } else {
        CGRect b = CGDisplayBounds(id);
        out->bounds_x = (int)b.origin.x;
        out->bounds_y = (int)b.origin.y;
        out->bounds_w = (int)b.size.width;
        out->bounds_h = (int)b.size.height;
        out->scale    = 1.0f;
    }

    out->pixels_w   = (int)CGDisplayPixelsWide(id);
    out->pixels_h   = (int)CGDisplayPixelsHigh(id);

    CGSize mm = CGDisplayScreenSize(id);
    out->size_mm_w  = (int)mm.width;
    out->size_mm_h  = (int)mm.height;

    out->builtin    = CGDisplayIsBuiltin(id) ? true : false;
    out->is_main    = CGDisplayIsMain(id)    ? true : false;
    out->is_online  = CGDisplayIsOnline(id)  ? true : false;
    out->mirrors_id = (uint32_t)CGDisplayMirrorsDisplay(id);
    out->rotation   = (int)CGDisplayRotation(id);

    // Prefer mode's reported refresh if it's nonzero (more precise than
    // NSScreen's int Hz). 0 from the mode means built-in / variable.
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
    if (mode) {
        double hz = CGDisplayModeGetRefreshRate(mode);
        if (hz > 0.0) out->refresh_hz = (float)hz;
        CGDisplayModeRelease(mode);
    }

    io_service_t serv = io_service_for_display(id);
    fill_edid_name(serv, out->name_original, sizeof(out->name_original));
    out->rotation_supported = query_rotation_supported(serv);
    out->connection_type    = detect_connection_type(serv, out->builtin);
    if (serv) IOObjectRelease(serv);
}

int platform_get_displays(platform_display_info_t *out, int max) {
    if (max <= 0) return 0;
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) return 0;
    int n = (int)count < max ? (int)count : max;
    for (int i = 0; i < n; i++) {
        fill_display_info(ids[i], &out[i]);
    }
    return n;
}

int platform_get_display_modes(uint32_t display_id, platform_video_mode_t *out, int max) {
    if (max <= 0) return 0;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes((CGDirectDisplayID)display_id, NULL);
    if (!modes) return 0;

    CGDisplayModeRef cur = CGDisplayCopyDisplayMode((CGDirectDisplayID)display_id);
    int32_t cur_id = cur ? (int32_t)CGDisplayModeGetIODisplayModeID(cur) : -1;

    CFIndex count = CFArrayGetCount(modes);
    int n = (int)count < max ? (int)count : max;
    bool found_current = false;
    for (int i = 0; i < n; i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        out[i].width      = (int)CGDisplayModeGetWidth(m);
        out[i].height     = (int)CGDisplayModeGetHeight(m);
        out[i].pixels_w   = (int)CGDisplayModeGetPixelWidth(m);
        out[i].pixels_h   = (int)CGDisplayModeGetPixelHeight(m);
        out[i].refresh_hz = (float)CGDisplayModeGetRefreshRate(m);
        out[i].is_current = ((int32_t)CGDisplayModeGetIODisplayModeID(m) == cur_id);
        if (out[i].is_current) found_current = true;
    }

    /* CGDisplayCopyAllDisplayModes(NULL) hides scaled HiDPI modes. If the
       user is on one of those, the current mode is missing from the array —
       append it so callers always see a row marked is_current. */
    if (cur && !found_current && n < max) {
        out[n].width      = (int)CGDisplayModeGetWidth(cur);
        out[n].height     = (int)CGDisplayModeGetHeight(cur);
        out[n].pixels_w   = (int)CGDisplayModeGetPixelWidth(cur);
        out[n].pixels_h   = (int)CGDisplayModeGetPixelHeight(cur);
        out[n].refresh_hz = (float)CGDisplayModeGetRefreshRate(cur);
        out[n].is_current = true;
        n++;
    }

    if (cur) CGDisplayModeRelease(cur);
    CFRelease(modes);
    return n;
}

uint32_t platform_get_window_display_id(void) {
    NSScreen *screen = [state.ns_window screen];
    if (!screen) screen = [NSScreen mainScreen];
    NSNumber *num = [screen deviceDescription][@"NSScreenNumber"];
    return num ? (uint32_t)[num unsignedIntValue] : 0;
}

/* --- Run loop / present --- */

/* CGDataProvider release callback — frees the pixel buffer that was
   handed to the layer when the CGImage built around it finally drops. */
static void release_fb_buffer(void *info, const void *data, size_t size) {
    (void)info;
    (void)size;
    free((void *)data);
}

/* Hand the active fb buffer to the layer (ownership transfer — no copy)
   and immediately allocate a fresh fb buffer at the pending size for the
   next frame. */
static void commit_to_layer(void) {
    if (!state.fb.pixels || !state.ns_view) return;

    int    w         = state.fb.width;
    int    h         = state.fb.height;
    size_t row_bytes = (size_t)w * sizeof(uint32_t);
    size_t total     = row_bytes * (size_t)h;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, state.fb.pixels, total, release_fb_buffer);
    CGImageRef img = CGImageCreate(
        (size_t)w, (size_t)h,
        8,                                 /* bits per component */
        32,                                /* bits per pixel */
        row_bytes,
        cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
        provider,
        NULL,                              /* no decode array */
        false,                             /* no interpolation */
        kCGRenderingIntentDefault);

    /* Explicit CATransaction with implicit-actions disabled — assignment
       commits to the compositor synchronously. The platform_run loop never
       enters the runloop, so the implicit per-runloop transaction would
       never fire on its own. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    state.ns_view.layer.contents = (__bridge id)img;
    [CATransaction commit];

    CGImageRelease(img);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);

    /* Buffer ownership transferred to the layer's data provider.
       Allocate a fresh one for the next frame at the pending size. */
    state.fb.width  = state.next_w;
    state.fb.height = state.next_h;
    state.fb.pixels = calloc((size_t)state.next_w * (size_t)state.next_h, sizeof(uint32_t));
}

/* sync size, drain queued events into event_cb, run frame_cb, commit.
   Recurses cleanly via AppKit's tracking-mode runloop calling our
   resize delegates → present_frame, which still drains queued events
   (the resize was just queued) and presents at the new size. */
static void present_frame(void) {
    if (!state.active_desc) return;

    sync_fb_size();
    drain_event_queue();

    state.t_now = CFAbsoluteTimeGetCurrent();
    state.active_desc->frame_cb(state.active_desc->user_data);
    state.t_prev = state.t_now;
    state.frame_count++;

    if (state.running) commit_to_layer();
}

int platform_run(const platform_app_desc_t *desc) {
    if (!desc) return -1;
    if (desc->width <= 0 || desc->height <= 0) {
        fprintf(stderr, "platform_run: invalid window size %dx%d\n", desc->width, desc->height);
        return -1;
    }
    if (!desc->frame_cb) {
        fprintf(stderr, "platform_run: desc->frame_cb is required\n");
        return -1;
    }

    if (!platform_init(desc)) {
        fprintf(stderr, "platform_init failed\n");
        return -1;
    }

    if (desc->init_cb) desc->init_cb(desc->user_data);

    state.active_desc = desc;
    state.t0          = CFAbsoluteTimeGetCurrent();
    state.t_prev      = state.t0;
    state.t_now       = state.t0;
    state.frame_count = 0;

    /* Per-iteration: pump AppKit events (handlers enqueue platform_event_t),
       then present_frame drains the queue, calls frame_cb, commits. */
    while (state.running) {
        @autoreleasepool {
            pump_events(state.ns_app);
            if (!state.running) break;
            present_frame();
        }
    }

    if (desc->cleanup_cb) desc->cleanup_cb(desc->user_data);

    state.active_desc = NULL;
    state.t0          = 0.0;
    state.t_prev      = 0.0;
    state.t_now       = 0.0;
    state.frame_count = 0;

    platform_shutdown();
    return 0;
}
