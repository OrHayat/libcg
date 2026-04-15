#include "platform.h"
#import <Cocoa/Cocoa.h>

/* --- Private types (needed by the public API signatures) --- */

typedef struct {
    NSApplication *ns_app;
    NSWindow *ns_window;
    bool running;
} platform_state_t;

static platform_state_t state;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

/* --- Forward declarations of private helpers --- */

static NSApplication *create_application(void);
static NSWindow      *create_window(int w, int h, const char *title, id delegate);
static void           activate_app(NSApplication *app);
static void           pump_events(NSApplication *app);

/* --- Public API --- */

bool platform_init(int width, int height, const char *title) {
    state.ns_app = create_application();

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [state.ns_app setDelegate:delegate];

    state.ns_window = create_window(width, height, title, delegate);

    activate_app(state.ns_app);
    pump_events(state.ns_app);

    state.running = true;
    return true;
}

void platform_shutdown(void) {
    [state.ns_window close];
    state.running = false;
}

void platform_poll_events(bool *quit_requested) {
    pump_events(state.ns_app);
    *quit_requested = !state.running;
}

void platform_present(void) {
    /* nothing to present yet — no pixel buffer */
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

@end

/* --- Private helpers --- */

static NSApplication *create_application(void) {
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    return app;
}

static NSWindow *create_window(int width, int height, const char *title, id delegate) {
    // clang-format off
    NSUInteger style = NSWindowStyleMaskTitled
                     | NSWindowStyleMaskClosable
                     | NSWindowStyleMaskMiniaturizable;
    // clang-format on

    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [window setTitle:[NSString stringWithUTF8String:title]];
    [window center];
    [window setDelegate:delegate];
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
