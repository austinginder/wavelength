#pragma once
// A desktop window that holds a plugin's own editor (serve's "Plugin window"): the plugin attaches its view to
// the window's content view. macOS only for now (an NSWindow; the process becomes a regular app while one is
// open); elsewhere open() fails saying so. Main thread only.
#include <string>

namespace wl::editorwin {

struct Window;

// A titled window `w` x `h` points in size, shown in front. Returns null and says why when it can't.
Window *open(const std::string &title, int w, int h, bool resizable, std::string &err);
// The view the plugin attaches to (NSView *).
void *view(Window *win);
// Resize the window's content to `w` x `h` points (the plugin asked).
void resize(Window *win, int w, int h);
// The content size the person dragged the window to, when it changed since the last call.
bool resized(Window *win, int &w, int &h);
// The person closed the window (its close button or Cmd-W): detach the plugin's view, then close() it.
bool closedByUser(Window *win);
void close(Window *win);
// Dispatch pending window events (mouse, keys, redraws). Call every few milliseconds while a window is open.
void pump();

} // namespace wl::editorwin
