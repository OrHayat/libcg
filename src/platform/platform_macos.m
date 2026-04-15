#include "platform.h"
#import <Cocoa/Cocoa.h>

/* --- Private types (needed by the public API signatures) --- */

typedef struct {
    CGContextRef  context;
    uint32_t     *pixels;
    int           width;
    int           height;
} framebuffer_t;

typedef struct {
    NSApplication *ns_app;
    NSWindow      *ns_window;
    NSView        *ns_view;
    framebuffer_t  fb;
    bool           running;
} platform_state_t;

static platform_state_t        state;
static platform_framebuffer_t  s_public_fb;
static platform_input_t       *s_current_input;

/* persistent mouse position, carried across polls */
static struct {
    int mouse_x, mouse_y;
} s_persistent;

/* macOS virtual key codes (US layout) */
enum {
    KC_Q      = 12,
    KC_M      = 46,
    KC_F      = 3,
    KC_ESCAPE = 53,
    KC_1      = 18,
    KC_2      = 19,
    KC_3      = 20,
    KC_4      = 21,
    KC_B      = 11,
};

/* --- LibcgView (custom NSView that blits the framebuffer) --- */

@interface LibcgView : NSView
@end

@implementation LibcgView
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!state.fb.context) return;

    CGContextRef cg = [[NSGraphicsContext currentContext] CGContext];
    CGImageRef image = CGBitmapContextCreateImage(state.fb.context);
    CGContextDrawImage(cg, [self bounds], image);
    CGImageRelease(image);
}

- (BOOL)isFlipped {
    return YES;  /* top-left origin to match framebuffer */
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    if (!s_current_input) return;
    unsigned short keyCode = [event keyCode];
    switch (keyCode) {
        case KC_Q:      s_current_input->keys_pressed[PLATFORM_KEY_Q]      = true; break;
        case KC_M:      s_current_input->keys_pressed[PLATFORM_KEY_M]      = true; break;
        case KC_F:      s_current_input->keys_pressed[PLATFORM_KEY_F]      = true; break;
        case KC_ESCAPE: s_current_input->keys_pressed[PLATFORM_KEY_ESCAPE] = true; break;
        case KC_1:      s_current_input->keys_pressed[PLATFORM_KEY_1]      = true; break;
        case KC_2:      s_current_input->keys_pressed[PLATFORM_KEY_2]      = true; break;
        case KC_3:      s_current_input->keys_pressed[PLATFORM_KEY_3]      = true; break;
        case KC_4:      s_current_input->keys_pressed[PLATFORM_KEY_4]      = true; break;
        case KC_B:      s_current_input->keys_pressed[PLATFORM_KEY_B]      = true; break;
        default: break;
    }
}

- (void)updateMousePosition:(NSEvent *)event {
    if (!s_current_input) return;
    NSPoint local = [self convertPoint:[event locationInWindow] fromView:nil];
    s_current_input->mouse_x = (int)local.x;
    s_current_input->mouse_y = (int)local.y;
}

- (void)mouseMoved:(NSEvent *)event       { [self updateMousePosition:event]; }
- (void)mouseDragged:(NSEvent *)event     { [self updateMousePosition:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self updateMousePosition:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self updateMousePosition:event]; }

- (void)mouseDown:(NSEvent *)event {
    [self updateMousePosition:event];
    if (s_current_input) s_current_input->mouse_left_pressed = true;
}
- (void)rightMouseDown:(NSEvent *)event {
    [self updateMousePosition:event];
    if (s_current_input) s_current_input->mouse_right_pressed = true;
}
- (void)otherMouseDown:(NSEvent *)event {
    [self updateMousePosition:event];
    if (s_current_input) s_current_input->mouse_middle_pressed = true;
}

- (void)scrollWheel:(NSEvent *)event {
    if (s_current_input) s_current_input->scroll_dy += (float)[event scrollingDeltaY];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

/* --- Forward declarations of private helpers --- */

static NSApplication *create_application(void);
static NSMenu        *create_menu(const char *title);
static NSWindow      *create_window(int w, int h, const char *title, id delegate);
static framebuffer_t  create_framebuffer(int w, int h);
static void           destroy_framebuffer(framebuffer_t *fb);
static void           activate_app(NSApplication *app);
static void           pump_events(NSApplication *app);

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

    NSSize backing = [state.ns_view convertSizeToBacking:NSMakeSize(width, height)];
    state.fb = create_framebuffer((int)backing.width, (int)backing.height);
    s_public_fb.pixels = state.fb.pixels;
    s_public_fb.width  = state.fb.width;
    s_public_fb.height = state.fb.height;

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
    *input = (platform_input_t){0};
    /* carry persistent state forward */
    input->mouse_x = s_persistent.mouse_x;
    input->mouse_y = s_persistent.mouse_y;

    s_current_input = input;
    pump_events(state.ns_app);
    s_current_input = NULL;

    /* update persistent state */
    s_persistent.mouse_x = input->mouse_x;
    s_persistent.mouse_y = input->mouse_y;

    input->quit_requested = !state.running;
}

void platform_present(void) {
    // [view display] paints right now. The alternative, [view setNeedsDisplay:YES],
    // only asks AppKit to paint "later" — and "later" is triggered by runloop code
    // our manual event pump doesn't run, so it would never actually happen.
    [state.ns_view display];
}

platform_framebuffer_t *platform_get_framebuffer(void) {
    return &s_public_fb;
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
    NSWindow *window = [notification object];
    NSSize logical = [[window contentView] bounds].size;
    NSSize backing = [[window contentView] convertSizeToBacking:logical];

    destroy_framebuffer(&state.fb);
    state.fb = create_framebuffer((int)backing.width, (int)backing.height);
    s_public_fb.pixels = state.fb.pixels;
    s_public_fb.width  = state.fb.width;
    s_public_fb.height = state.fb.height;

    [state.ns_view setNeedsDisplay:YES];
}

@end

/* --- Private helpers --- */

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
        .width  = w,
        .height = h,
        .pixels = calloc((size_t)(w * h), sizeof(uint32_t)),
    };

    // CG types are manually refcounted (ARC doesn't cover them). CGBitmapContextCreate
    // retains cs internally, so we release our own reference here to balance the Create.
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();

    // Wrap the raw pixel array in a Core Graphics context WITHOUT copying — both
    // `fb.pixels` and `fb.context` point at the same memory. Writes via either path
    // are immediately visible to the other; the view's drawRect: blits fb.context.
    //
    // Format flag decoded: AlphaPremultipliedFirst + ByteOrder32Little means when
    // you write a uint32 like 0xAARRGGBB, it's interpreted with A in the high byte,
    // then R, G, B. This is the layout that makes `pixels[i] = 0xFFFF8800` produce
    // opaque orange the way a human reads the hex.
    fb.context = CGBitmapContextCreate(fb.pixels,
                                       (size_t)w, (size_t)h,
                                       8,              // bits per component (R/G/B/A)
                                       (size_t)(w * 4), // bytes per row (stride)
                                       cs,
                                       kCGImageAlphaPremultipliedFirst
                                           | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);

    return fb;
}

static void destroy_framebuffer(framebuffer_t *fb) {
    if (fb->context) {
        CGContextRelease(fb->context);
    }
    free(fb->pixels);
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
