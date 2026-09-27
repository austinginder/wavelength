#include "term.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace wl::term {

namespace {

Style gOut, gErr;
std::FILE *gOutFile = stdout;

bool terminal(std::FILE *f) {
#ifdef _WIN32
    if (!_isatty(_fileno(f))) return false;
    // Windows 10+ consoles understand escape codes once asked; older ones get plain text
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
    if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) && !SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return false;
    SetConsoleOutputCP(CP_UTF8);
    return true;
#else
    return isatty(fileno(f)) != 0;
#endif
}

bool wanted(std::FILE *f, bool json) {
    const char *force = std::getenv("FORCE_COLOR"), *no = std::getenv("NO_COLOR"), *termName = std::getenv("TERM");
    if (json) return false;
    if (no && *no) return false;
    if (force && *force && std::strcmp(force, "0") != 0) { terminal(f); return true; }
    if (termName && std::strcmp(termName, "dumb") == 0) return false;
    return terminal(f);
}

} // namespace

void init(std::FILE *out, bool json) {
    gOutFile = out;
    gOut.on = wanted(out, json);
    gErr.on = wanted(stderr, json);
}

const Style &out() { return gOut; }
const Style &err() { return gErr; }

int width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo((HANDLE)_get_osfhandle(_fileno(gOutFile)), &info) || GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
#else
    winsize w{};
    for (int fd : {fileno(gOutFile), 2})
        if (ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
#endif
    if (const char *c = std::getenv("COLUMNS")) { const int n = std::atoi(c); if (n > 20) return n; }
    return 100;
}

size_t visibleWidth(const std::string &s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {   // skip an escape sequence
            while (i < s.size() && !(s[i] >= 'A' && s[i] <= 'Z') && !(s[i] >= 'a' && s[i] <= 'z')) ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) ++n;   // count characters, not continuation bytes
    }
    return n;
}

std::string pad(const std::string &s, size_t w) {
    const size_t v = visibleWidth(s);
    return v >= w ? s : s + std::string(w - v, ' ');
}

// ---- progress ---------------------------------------------------------------------------------
struct Progress::State {
    std::string title;
    std::mutex m;
    size_t done = 0, total = 0;
    std::string now;
    std::atomic<bool> running{true};
    std::thread thread;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool drawn = false;
};

Progress::Progress(std::string title) : s_(new State) {
    s_->title = std::move(title);
    if (!gErr.on) { s_->running = false; return; }
    s_->thread = std::thread([s = s_] {
        static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        int frame = 0;
        while (s->running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->start).count();
            if (!s->running || secs < 0.4) continue;   // quick commands never flash a line
            std::string line, now;
            {
                std::lock_guard<std::mutex> lock(s->m);
                line = gErr.cyan(frames[frame++ % 10]) + " " + gErr.bold(s->title);
                if (s->total) {
                    const int cells = 16, full = (int)(cells * s->done / s->total);
                    std::string bar;
                    for (int c = 0; c < cells; ++c) bar += c < full ? "\u2588" : "\u2591";
                    line += "  " + gErr.blue(bar) + " " + std::to_string(s->done) + "/" + std::to_string(s->total);
                }
                now = s->now;
            }
            char t[16];
            std::snprintf(t, sizeof t, "%d:%02d", (int)secs / 60, (int)secs % 60);
            line += "  " + gErr.dim(t);
            // what's running now, cut to what's left of the terminal's width
            const size_t room = (size_t)std::max(20, width() - 1), used = visibleWidth(line) + 2;
            if (!now.empty() && used + 4 < room) {
                std::string cut;
                size_t chars = 0;
                for (size_t i = 0; i < now.size(); ++i) {
                    if (((unsigned char)now[i] & 0xC0) != 0x80 && ++chars > room - used) break;
                    cut += now[i];
                }
                line += "  " + gErr.dim(cut);
            }
            std::fprintf(stderr, "\r\x1b[2K%s", line.c_str());
            std::fflush(stderr);
            s->drawn = true;
        }
    });
}

void Progress::update(size_t done, size_t total, const std::string &now) {
    std::lock_guard<std::mutex> lock(s_->m);
    s_->done = done;
    s_->total = total;
    s_->now = now;
}

void Progress::stop() {
    if (!s_ || !s_->thread.joinable()) return;
    s_->running = false;
    s_->thread.join();
    if (s_->drawn) { std::fprintf(stderr, "\r\x1b[2K"); std::fflush(stderr); }
}

Progress::~Progress() {
    stop();
    delete s_;
}

} // namespace wl::term
