// editor_window.hpp where plugin windows aren't hosted yet (Linux, Windows): open() says so.
#include "editor_window.hpp"

#if !defined(__APPLE__)
namespace wl::editorwin {

Window *open(const std::string &, int, int, bool, std::string &err) {
    err = "plugin windows open on macOS only for now; turn the knobs on the page";
    return nullptr;
}
void *view(Window *) { return nullptr; }
void resize(Window *, int, int) {}
bool resized(Window *, int &, int &) { return false; }
bool closedByUser(Window *) { return false; }
void close(Window *) {}
void pump() {}

} // namespace wl::editorwin
#endif
