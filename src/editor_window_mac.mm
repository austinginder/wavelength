// editor_window.hpp on macOS: an NSWindow per plugin editor. Compiled with ARC.
#import <Cocoa/Cocoa.h>

#include "editor_window.hpp"

@interface WLEditorDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL closed;
@property(nonatomic) BOOL sizeChanged;
@property(nonatomic) BOOL ourResize;
@end

@implementation WLEditorDelegate
// the close button hides the window; its owner detaches the plugin's view, then closes it
- (BOOL)windowShouldClose:(NSWindow *)sender {
    self.closed = YES;
    [sender orderOut:nil];
    return NO;
}
- (void)windowDidResize:(NSNotification *)note {
    (void)note;
    if (!self.ourResize) self.sizeChanged = YES;
}
@end

namespace wl::editorwin {

struct Window {
    NSWindow *win = nil;
    WLEditorDelegate *delegate = nil;
};

namespace {
int openCount = 0;

// a command-line process becomes a regular app (a Dock icon, windows in front) while a window is open
void becomeApp() {
    static bool launched = false;
    [NSApplication sharedApplication];
    if (!launched) {
        [NSApp finishLaunching];
        launched = true;
    }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}
} // namespace

Window *open(const std::string &title, int w, int h, bool resizable, std::string &err) {
    @autoreleasepool {
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
            err = "the plugin asked for a window of " + std::to_string(w) + " x " + std::to_string(h);
            return nullptr;
        }
        becomeApp();
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (resizable) style |= NSWindowStyleMaskResizable;
        NSWindow *nw = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h) styleMask:style backing:NSBackingStoreBuffered defer:NO];
        if (!nw) { err = "could not make a window"; return nullptr; }
        nw.releasedWhenClosed = NO;
        nw.title = [NSString stringWithUTF8String:title.c_str()] ?: @"Plugin";
        auto *win = new Window;
        win->win = nw;
        win->delegate = [WLEditorDelegate new];
        nw.delegate = win->delegate;
        [nw center];
        [nw makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        ++openCount;
        return win;
    }
}

void *view(Window *win) { return win ? (__bridge void *)win->win.contentView : nullptr; }

void resize(Window *win, int w, int h) {
    if (!win || w <= 0 || h <= 0) return;
    win->delegate.ourResize = YES;
    [win->win setContentSize:NSMakeSize(w, h)];
    win->delegate.ourResize = NO;
}

bool resized(Window *win, int &w, int &h) {
    if (!win || !win->delegate.sizeChanged) return false;
    win->delegate.sizeChanged = NO;
    const NSSize s = win->win.contentView.frame.size;
    w = (int)s.width;
    h = (int)s.height;
    return true;
}

bool closedByUser(Window *win) { return win && win->delegate.closed; }

void close(Window *win) {
    if (!win) return;
    @autoreleasepool {
        win->win.delegate = nil;
        [win->win orderOut:nil];
        [win->win close];
        win->win = nil;
        win->delegate = nil;
    }
    delete win;
    if (--openCount <= 0) {
        openCount = 0;
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   // no Dock icon once the last one closes
    }
}

void pump() {
    if (!openCount) return;
    @autoreleasepool {
        for (;;) {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            [NSApp sendEvent:e];
        }
        [NSApp updateWindows];
    }
}

} // namespace wl::editorwin
