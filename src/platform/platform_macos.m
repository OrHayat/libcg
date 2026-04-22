#include "platform.h"
#import <Cocoa/Cocoa.h>

/* --- Private types (needed by the public API signatures) --- */
typedef struct {
    CGContextRef context;
    platform_framebuffer_t pub; // pixels, width, height — exposed via platform_get_framebuffer
} framebuffer_t;

typedef struct {
    NSApplication *ns_app;
    NSWindow *ns_window;
    NSView *ns_view;
    framebuffer_t fb;
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

/* --- LibcgView (custom NSView that blits the framebuffer) --- */

@interface LibcgView : NSView
@end

@implementation LibcgView
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!state.fb.context)
        return;

    CGContextRef cg = [[NSGraphicsContext currentContext] CGContext];
    CGImageRef image = CGBitmapContextCreateImage(state.fb.context);
    CGContextDrawImage(cg, [self bounds], image);
    CGImageRelease(image);
}

- (BOOL)isFlipped {
    return YES;  /* top-left origin to match framebuffer */
}

// Required for the view to receive keyboard events. Without this, AppKit
// sends key events to the window's field editor instead.
- (BOOL)acceptsFirstResponder {
    return YES;
}

// Record key-down transition. Repeats are filtered — we track physical
// press/release, not the OS auto-repeat stream.
- (void)keyDown:(NSEvent *)event {
    if ([event isARepeat])
        return;
    platform_key_t key = kc_to_key[[event keyCode] & 0xFF];
    if (key == PLATFORM_KEY_UNKNOWN)
        return;
    state.pending.keys[key].half_transition_count++;
    state.pending.keys[key].ended_down = true;

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

- (void)mouseMoved:(NSEvent *)event       { [self updateMousePosition:event]; }
- (void)mouseDragged:(NSEvent *)event     { [self updateMousePosition:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self updateMousePosition:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self updateMousePosition:event]; }

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
static framebuffer_t create_framebuffer(int w, int h);
static void destroy_framebuffer(framebuffer_t *fb);
static NSSize get_backing_size(NSWindow *window);
static void reallocate_framebuffer(void);
static void activate_app(NSApplication *app);
static void pump_events(NSApplication *app);

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

    // On retina/HiDPI displays the backing store is larger than the logical
    // window size (e.g. 1280×720 logical → 2560×1440 backing at 2× scale).
    // Allocate the framebuffer at backing resolution so we render at native
    // pixel density.
    NSSize backing = get_backing_size(state.ns_window);
    state.fb = create_framebuffer((int)backing.width, (int)backing.height);

    activate_app(state.ns_app);
    pump_events(state.ns_app);

    state.running = true;
    return true;
}

void platform_shutdown(void) {
    destroy_framebuffer(&state.fb);
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

    *input = state.pending;
    input->quit_requested = !state.running;
}

void platform_present(void) {
    // [view display] paints right now. The alternative, [view setNeedsDisplay:YES],
    // only asks AppKit to paint "later" — and "later" is triggered by runloop code
    // our manual event pump doesn't run, so it would never actually happen.
    [state.ns_view display];
}

platform_framebuffer_t *platform_get_framebuffer(void) {
    return &state.fb.pub;
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

- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    reallocate_framebuffer();
}

// Fires when the window moves between displays with different scale factors
// (e.g. retina internal ↔ 1× external) or the user changes display scaling.
// Logical point size doesn't change, so windowDidResize: doesn't fire — but
// the backing pixel size does, and our framebuffer needs to follow.
- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    (void)notification;
    reallocate_framebuffer();
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

// Destroy + recreate rather than resize in place because the CGBitmapContext
// is bound to the pixel buffer at creation time — there's no way to re-point
// it at a differently-sized allocation.
static void reallocate_framebuffer(void) {
    NSSize backing = get_backing_size(state.ns_window);
    destroy_framebuffer(&state.fb);
    state.fb = create_framebuffer((int)backing.width, (int)backing.height);
    [state.ns_view setNeedsDisplay:YES];
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

// Allocate a new framebuffer at the given size. Caller owns the returned value
// and must free it with destroy_framebuffer when done.
static framebuffer_t create_framebuffer(int w, int h) {
    framebuffer_t fb = {
        .pub =
            {
                .width = w,
                .height = h,
                .pixels = calloc((size_t)(w * h), sizeof(uint32_t)),
            },
    };

    // CG types are manually refcounted (ARC doesn't cover them). CGBitmapContextCreate
    // retains cs internally, so we release our own reference here to balance the Create.
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();

    // Wrap the raw pixel array in a Core Graphics context WITHOUT copying — both
    // `fb.pub.pixels` and `fb.context` point at the same memory. Writes via either
    // path are immediately visible to the other; the view's drawRect: blits fb.context.
    //
    // Format flag decoded: AlphaPremultipliedFirst + ByteOrder32Little means when
    // you write a uint32 like 0xAARRGGBB, it's interpreted with A in the high byte,
    // then R, G, B. This is the layout that makes `pixels[i] = 0xFFFF8800` produce
    // opaque orange the way a human reads the hex.
    fb.context =
        CGBitmapContextCreate(fb.pub.pixels, (size_t)w, (size_t)h,
                              8,               // bits per component (R/G/B/A)
                              (size_t)(w * 4), // bytes per row (stride)
                              cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);

    return fb;
}

static void destroy_framebuffer(framebuffer_t *fb) {
    if (fb->context) {
        CGContextRelease(fb->context);
    }
    free(fb->pub.pixels);
    *fb = (framebuffer_t){0};
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
