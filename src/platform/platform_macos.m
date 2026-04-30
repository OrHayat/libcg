#include "platform.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer ownership model:
   - state.fb.pixels is the active rendering target. frame_cb writes into it.
   - On commit_to_layer the buffer is handed (no copy) to a CGDataProvider
     whose release callback frees it. The CGImage built from that provider
     becomes layer.contents; the buffer lives until the layer drops the
     image (typically when the next commit replaces it).
   - commit_to_layer then allocates a fresh state.fb.pixels at state.next_w
     × state.next_h for the following frame.
   - Resize delegates only update state.next_w/h. The size flips on the
     next sync_fb_size — never mid-frame, never inside the pump. This kills
     the realloc-vs-present race at the architectural level. */
typedef struct {
    NSApplication *ns_app;
    NSWindow *ns_window;
    NSView *ns_view;
    platform_framebuffer_t fb;     /* pixels owned for one frame at a time */
    int next_w, next_h;            /* pending backing size */
    platform_input_t pending;
    bool running;
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

/* --- LibcgView (custom NSView; presentation is via layer.contents) --- */

/* Forward decls for rendering helpers; full bodies live further down. */
static void sync_fb_size(void);      /* ensure fb.pixels matches next_w/next_h */
static void present_frame(void);     /* run frame_cb, then hand fb to the layer */

@interface LibcgView : NSView
@end

@implementation LibcgView
/* No-op. This is a layer-backed view and frames flow into layer.contents
   via present_frame() (callback API) / platform_present() (legacy polled
   API). AppKit composites the current layer.contents on its own for
   damage / expose / occlusion — drawRect: doesn't need to repaint anything.
   Calling commit_to_layer here was the source of the startup flash: AppKit
   could fire drawRect: during view setup before any frame had been
   rendered, committing an all-zero (transparent) buffer to layer.contents. */
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

// Record key-down transition. Repeats are filtered — we track physical
// press/release, not the OS auto-repeat stream.
- (void)keyDown:(NSEvent *)event {
    platform_key_t key = kc_to_key[[event keyCode] & 0xFF];
    if (key == PLATFORM_KEY_UNKNOWN)
        return;
    state.pending.keys[key].half_transition_count++;
    if (![event isARepeat]) {
        state.pending.keys[key].ended_down = true;
    }

    /* Capture printable characters into the text input buffer */
    NSString *chars = [event characters];
    if (chars && [chars length] > 0) {
        const char *cstr = [chars UTF8String];
        for (const char *p = cstr; *p && state.pending.text_len < PLATFORM_TEXT_BUFFER - 1; p++) {
            unsigned char c = (unsigned char)*p;
            if (c >= 0x20 && c < 0x7F) {
                state.pending.text[state.pending.text_len++] = (char)c;
            }
        }
        state.pending.text[state.pending.text_len] = '\0';
    }
}

// Record key-up transition.
- (void)keyUp:(NSEvent *)event {
    platform_key_t key = kc_to_key[[event keyCode] & 0xFF];
    if (key == PLATFORM_KEY_UNKNOWN)
        return;
    state.pending.keys[key].half_transition_count++;
    state.pending.keys[key].ended_down = false;
}

// Modifier keys (shift, ctrl, alt, cmd, caps lock) don't fire keyDown:/keyUp:.
// AppKit sends flagsChanged: instead. We use device-specific masks (NX_DEVICE*)
// from IOKit to distinguish left from right modifiers — the high-level
// NSEventModifierFlag* constants merge both sides into one bit.
// clang-format off
- (void)flagsChanged:(NSEvent *)event {
    unsigned short keyCode = [event keyCode] & 0xFF;
    platform_key_t key = kc_to_key[keyCode];
    if (key == PLATFORM_KEY_UNKNOWN)
        return;

    NSUInteger flags = [event modifierFlags];
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

    if (is_down != state.pending.keys[key].ended_down) {
        state.pending.keys[key].half_transition_count++;
        state.pending.keys[key].ended_down = is_down;
    }
}

- (void)updateMousePosition:(NSEvent *)event {
    NSPoint local = [self convertPoint:[event locationInWindow] fromView:nil];
    state.pending.mouse.x = (int)local.x;
    state.pending.mouse.y = (int)local.y;
}

- (void)mouseMoved:(NSEvent *)event {
    [self updateMousePosition:event];
}
- (void)mouseDragged:(NSEvent *)event {
    [self updateMousePosition:event];
}
- (void)rightMouseDragged:(NSEvent *)event {
    [self updateMousePosition:event];
}
- (void)otherMouseDragged:(NSEvent *)event {
    [self updateMousePosition:event];
}

- (void)mouseDown:(NSEvent *)event {
    [self updateMousePosition:event];
    state.pending.mouse.buttons[PLATFORM_MOUSE_LEFT].half_transition_count++;
    state.pending.mouse.buttons[PLATFORM_MOUSE_LEFT].ended_down = true;
}
- (void)mouseUp:(NSEvent *)event {
    [self updateMousePosition:event];
    state.pending.mouse.buttons[PLATFORM_MOUSE_LEFT].half_transition_count++;
    state.pending.mouse.buttons[PLATFORM_MOUSE_LEFT].ended_down = false;
}
- (void)rightMouseDown:(NSEvent *)event {
    [self updateMousePosition:event];
    state.pending.mouse.buttons[PLATFORM_MOUSE_RIGHT].half_transition_count++;
    state.pending.mouse.buttons[PLATFORM_MOUSE_RIGHT].ended_down = true;
}
- (void)rightMouseUp:(NSEvent *)event {
    [self updateMousePosition:event];
    state.pending.mouse.buttons[PLATFORM_MOUSE_RIGHT].half_transition_count++;
    state.pending.mouse.buttons[PLATFORM_MOUSE_RIGHT].ended_down = false;
}
- (void)otherMouseDown:(NSEvent *)event {
    [self updateMousePosition:event];
    state.pending.mouse.buttons[PLATFORM_MOUSE_MIDDLE].half_transition_count++;
    state.pending.mouse.buttons[PLATFORM_MOUSE_MIDDLE].ended_down = true;
}
- (void)otherMouseUp:(NSEvent *)event {
    [self updateMousePosition:event];
    state.pending.mouse.buttons[PLATFORM_MOUSE_MIDDLE].half_transition_count++;
    state.pending.mouse.buttons[PLATFORM_MOUSE_MIDDLE].ended_down = false;
}

- (void)scrollWheel:(NSEvent *)event {
    state.pending.mouse.scroll_dx += (float)[event scrollingDeltaX];
    state.pending.mouse.scroll_dy += (float)[event scrollingDeltaY];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

/* --- Forward declarations of private helpers --- */

static NSApplication *create_application(void);
static NSMenu *create_menu(const char *title);
static NSWindow *create_window(int w, int h, const char *title, id delegate);
static NSSize get_backing_size(NSWindow *window);
static void activate_app(NSApplication *app);
static void pump_events(NSApplication *app);
static void commit_to_layer(void);

/* --- Public API --- */

bool platform_init(int width, int height, const char *title) {
    state.ns_app = create_application();

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [state.ns_app setDelegate:delegate];

    [state.ns_app setMainMenu:create_menu(title)];

    state.ns_window = create_window(width, height, title, delegate);

    state.ns_view = [[LibcgView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    [state.ns_window setContentView:state.ns_view];
    [state.ns_window makeFirstResponder:state.ns_view];
    [state.ns_window setAcceptsMouseMovedEvents:YES];

    /* Layer-backed view with explicit content-placement. Presentation works
       by assigning CGImages to view.layer.contents — the only path that
       updates pixels mid-drag during a live resize. The placement controls
       how the CALayer scales contents when the view bounds change. */
    [state.ns_view setWantsLayer:YES];
    state.ns_view.layerContentsPlacement = NSViewLayerContentsPlacementScaleProportionallyToFit;

    // On retina/HiDPI displays the backing store is larger than the logical
    // window size (e.g. 1280×720 logical → 2560×1440 backing at 2× scale).
    // Allocate the framebuffer at backing resolution so we render at native
    // pixel density.
    NSSize backing = get_backing_size(state.ns_window);
    state.next_w  = (int)backing.width;
    state.next_h  = (int)backing.height;
    state.fb.width  = state.next_w;
    state.fb.height = state.next_h;
    state.fb.pixels = calloc((size_t)state.next_w * (size_t)state.next_h, sizeof(uint32_t));

    activate_app(state.ns_app);
    pump_events(state.ns_app);

    state.running = true;
    return true;
}

void platform_shutdown(void) {
    free(state.fb.pixels);
    state.fb.pixels = NULL;
    [state.ns_window close];
    state.running = false;
}

void platform_poll_events(platform_input_t *input) {
    // Reset per-frame transition counts, preserve held state.
    for (int i = 0; i < PLATFORM_KEY_COUNT; i++)
        state.pending.keys[i].half_transition_count = 0;
    for (int i = 0; i < PLATFORM_MOUSE_COUNT; i++)
        state.pending.mouse.buttons[i].half_transition_count = 0;
    state.pending.mouse.scroll_dx = 0.0f;
    state.pending.mouse.scroll_dy = 0.0f;
    state.pending.text_len = 0;
    state.pending.text[0] = '\0';
    // mouse_x, mouse_y persist in state.pending across frames.

    pump_events(state.ns_app);

    /* Apply any resize that fired from a delegate during pump_events
       BEFORE the caller observes fb. Otherwise the legacy polled API
       would render one frame at the previous size after every resize
       (caller reads stale fb dims, renders, presents at stale size). */
    sync_fb_size();

    *input = state.pending;
    input->quit_requested = !state.running;
}

void platform_present(void) {
    /* Hands fb.pixels to the layer (ownership transfer; the layer owns it
       until the next commit replaces it) and allocates a fresh fb.pixels
       for the next frame at the pending size. Polled-API callers must
       re-fetch via platform_get_framebuffer() each iteration — the pointer
       is per-frame, not stable across present calls. */
    commit_to_layer();
}

platform_framebuffer_t *platform_get_framebuffer(void) {
    return &state.fb;
}

/* --- AppDelegate implementation --- */

@implementation AppDelegate

// Manual event pump doesn't run the runloop deep enough to trigger AppKit's
// auto-terminate flow, so hook the window-close notification directly and
// drop `running` to exit the C main loop.
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    state.running = false;
}

/* All resize delegates do the same thing: stash the new backing size in
   state.next_w/h and trigger a present. The buffer doesn't get touched
   here — sync_fb_size at the top of present_frame (or commit_to_layer's
   tail allocation) flips to the new size between frames, never mid-frame.
   This is why we don't need re-entry-safe reallocate_framebuffer anymore:
   the dangerous "free buffer being written to" sequence can't form. */
static void update_pending_size(void) {
    NSSize backing = get_backing_size(state.ns_window);
    state.next_w = (int)backing.width;
    state.next_h = (int)backing.height;
}

- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    update_pending_size();
    present_frame();
}

// Fires when the window moves between displays with different scale factors
// (e.g. retina internal ↔ 1× external) or the user changes display scaling.
// Logical point size doesn't change, so windowDidResize: doesn't fire — but
// the backing pixel size does, and our framebuffer needs to follow.
- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    (void)notification;
    update_pending_size();
    present_frame();
}

// Fires when the window crosses to a different display. Same-scale moves
// don't fire backingProperties; same-scale moves no-op naturally because
// next_w/next_h won't change.
- (void)windowDidChangeScreen:(NSNotification *)notification {
    (void)notification;
    update_pending_size();
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

/* Bring fb.pixels in line with the pending size. Called at the top of
   present_frame so the size only flips between frames, never mid-frame.
   Same-size is a no-op. The old buffer (if any) is freed; the new one
   is calloc'd fresh — frame_cb is expected to fully overwrite it, but
   zero-init is cheap insurance against a frame_cb that reads before it
   writes. */
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
// Caller is responsible for installing it via setMainMenu:.
static NSMenu *create_menu(const char *title) {
    // top bar + first slot (AppKit auto-labels this with the app name)
    NSMenu *menubar = [[NSMenu alloc] init];
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];

    // dropdown that hangs off the app slot, with just "Quit <title>" in it
    NSMenu *appMenu = [[NSMenu alloc] init];
    NSString *quitTitle = [NSString stringWithFormat:@"Quit %s", title];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                      action:@selector(terminate:)
                                               keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];

    return menubar;
}

static NSWindow *create_window(int width, int height, const char *title, id delegate) {
    // clang-format off
    NSUInteger style = NSWindowStyleMaskTitled
                     | NSWindowStyleMaskClosable
                     | NSWindowStyleMaskMiniaturizable
                     | NSWindowStyleMaskResizable;
    // clang-format on

    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [window setTitle:[NSString stringWithUTF8String:title]];
    [window center];
    [window setDelegate:delegate];
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
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

// Drain any pending events. distantPast = non-blocking poll; never enters the
// runloop, so runloop sources/timers/observers won't fire here.
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

/* --- New callback API --- */

/* Static state for the active platform_run invocation.
   CFAbsoluteTime (plain double) instead of NSDate avoids autorelease
   accumulation in the hot present_frame loop. */
static const platform_app_desc_t *_libcg_active_desc     = NULL;
static CFAbsoluteTime             _libcg_run_t0          = 0.0;
static CFAbsoluteTime             _libcg_run_t_prev      = 0.0;
static uint64_t                   _libcg_run_frame_index = 0;

void platform_request_quit(void) {
    state.running = false;
}

/* CGDataProvider release callback — frees the pixel buffer that was
   handed to the layer when the CGImage built around it finally drops
   (typically when the next commit replaces layer.contents). */
static void release_fb_buffer(void *info, const void *data, size_t size) {
    (void)info;
    (void)size;
    free((void *)data);
}

/* Hand the active fb buffer to the layer (ownership transfer — no copy)
   and immediately allocate a fresh fb buffer at the pending size for the
   next frame. Bypasses AppKit's drawRect coalescing — Core Animation's
   main-thread transactions commit during live-resize tracking-mode
   runloops, so pixels reach screen mid-drag.

   Why ownership transfer (not copy-on-commit):
   CGBitmapContextCreateImage's documented copy-on-write only triggers on
   CGContext-API drawing. We mutate pixels via raw pointer writes which CG
   can't observe, so any CGImage made from a shared bitmap context is
   effectively aliasing our buffer. The earlier snapshot+provider workaround
   memcpy'd ~8 MB/frame at retina-720p × 60 Hz. Handing the buffer itself
   to the data provider is faster and structurally race-free: the buffer
   lives until the layer drops the image, which only happens when the next
   commit hands a new buffer. State.fb.pixels is then a fresh allocation
   that no one else has a pointer to. */
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

    /* Wrap the contents assignment in an explicit CATransaction with
       implicit-actions disabled. Without this the assignment lands in the
       implicit per-runloop transaction, which only commits when the
       runloop iterates — but platform_run's tight while loop never enters
       the runloop. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    state.ns_view.layer.contents = (__bridge id)img;
    [CATransaction commit];

    CGImageRelease(img);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);

    /* Buffer ownership has transferred to the layer's data provider.
       Allocate a fresh one for the next frame at the pending size. */
    state.fb.width  = state.next_w;
    state.fb.height = state.next_h;
    state.fb.pixels = calloc((size_t)state.next_w * (size_t)state.next_h, sizeof(uint32_t));
}

/* Sync size, run frame_cb (which fills fb), then commit. Bypasses
   AppKit's drawRect coalescing — Core Animation's main-thread transactions
   commit during live-resize tracking-mode runloops, which is exactly when
   we recurse into ourselves: AppKit calls our windowDidResize: delegate
   from inside frame_cb's platform_poll_events (pump_events → sendEvent →
   tracking-mode runloop → delegate → present_frame). The recursion is
   intentional — without it nothing reaches the layer mid-drag and the
   window stays empty for the duration of the resize. Each recursive
   call is a real frame at a different size; frame_index/dt advance
   accordingly, which is the correct accounting. The buffer-ownership
   model means recursion can no longer free a buffer being written to. */
static void present_frame(void) {
    /* No active platform_run — either the caller is on the legacy polled
       API path (no frame_cb to invoke) or platform_run already cleared
       _libcg_active_desc in its cleanup tail and AppKit fired a late
       delegate notification during [window close]. Nothing to drive, bail. */
    if (!_libcg_active_desc) return;

    /* Apply any pending resize before frame_cb sees fb. */
    sync_fb_size();

    if (_libcg_active_desc->frame_cb) {
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        platform_frame_t frame = {
            .fb          = &state.fb,
            .dt          = now - _libcg_run_t_prev,
            .time        = now - _libcg_run_t0,
            .frame_index = _libcg_run_frame_index++,
        };
        _libcg_active_desc->frame_cb(&frame, _libcg_active_desc->user_data);
        _libcg_run_t_prev = now;
    }

    /* frame_cb may flip state.running (platform_request_quit, or
       windowWillClose during platform_poll_events). Skip the commit so we
       don't push pixels to a window that's on its way out. The run-loop's
       while-condition will exit on the next iteration. */
    if (state.running) commit_to_layer();
}

int platform_run(const platform_app_desc_t *desc) {
    if (!desc) return -1;
    if (desc->width <= 0 || desc->height <= 0) {
        fprintf(stderr, "platform_run: invalid window size %dx%d\n", desc->width, desc->height);
        return -1;
    }
    /* frame_cb is required: the run loop doesn't pump events itself in this
       transitional design — frame_cb's call to platform_poll_events drives
       AppKit. A null frame_cb would hang the app on the first iteration. */
    if (!desc->frame_cb) {
        fprintf(stderr, "platform_run: desc->frame_cb is required\n");
        return -1;
    }

    const char *title = desc->title ? desc->title : "libcg";

    /* TODO: desc->transparent, desc->resizable, desc->high_dpi are not yet
       wired through; platform_init currently hardcodes transparent + resizable
       + hi-dpi backing. Will plumb when callers actually need to opt out. */
    if (!platform_init(desc->width, desc->height, title)) {
        fprintf(stderr, "platform_init failed\n");
        return -1;
    }

    if (desc->init_cb) desc->init_cb(desc->user_data);

    _libcg_active_desc     = desc;
    _libcg_run_t0          = CFAbsoluteTimeGetCurrent();
    _libcg_run_t_prev      = _libcg_run_t0;
    _libcg_run_frame_index = 0;

    /* TRANSITIONAL: this loop doesn't pump OS events itself — the game's
       frame_cb is expected to call platform_poll_events() (legacy polled
       API, internally pumps NSApp). Adding pump_events() here would
       double-pump because platform_poll_events resets per-frame transition
       counts at its start, dropping any events the duplicate pump just
       delivered. Real fix lands in PR 2.4 (event_cb wiring) which
       restructures the pump path.

       The @autoreleasepool guards against autoreleased Obj-C objects
       frame_cb / present_frame might create (e.g. NSDate instances inside
       AppKit during display) accumulating across frames. */
    while (state.running) {
        @autoreleasepool {
            present_frame();
        }
    }

    if (desc->cleanup_cb) desc->cleanup_cb(desc->user_data);

    _libcg_active_desc     = NULL;
    _libcg_run_t0          = 0.0;
    _libcg_run_t_prev      = 0.0;
    _libcg_run_frame_index = 0;

    platform_shutdown();
    return 0;
}
