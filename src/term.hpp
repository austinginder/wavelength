#pragma once
// Terminal styling for people, invisible to agents: colours, symbols and a progress line appear only
// when the stream is a terminal, NO_COLOR is unset, TERM isn't "dumb" and the command isn't writing JSON.
// Piped output (how agents read Wavelength) is exactly the plain text it always was. FORCE_COLOR=1
// styles a pipe anyway.
#include <cstdio>
#include <string>

namespace wl::term {

// once, after main takes its stdout: `out` is Wavelength's own output stream, `json` a --json command
void init(std::FILE *out, bool json);

// a style for one stream: every call returns the text unchanged when that stream isn't styled
struct Style {
    bool on = false;
    std::string bold(const std::string &s) const { return wrap("1", s); }
    std::string dim(const std::string &s) const { return wrap("2", s); }
    std::string red(const std::string &s) const { return wrap("31", s); }
    std::string green(const std::string &s) const { return wrap("32", s); }
    std::string yellow(const std::string &s) const { return wrap("33", s); }
    std::string blue(const std::string &s) const { return wrap("34", s); }
    std::string cyan(const std::string &s) const { return wrap("36", s); }
    std::string accent(const std::string &s) const { return wrap("1;34", s); }   // command names, titles
    // symbols, with an ASCII word when plain
    std::string ok() const { return on ? green("✓") : "ok"; }
    std::string warn() const { return on ? yellow("⚠") : "!"; }
    std::string fail() const { return on ? red("✗") : "x"; }
    std::string arrow() const { return on ? "→" : "->"; }
    std::string dot() const { return on ? "·" : "-"; }
    std::string wrap(const char *code, const std::string &s) const { return on ? "\x1b[" + std::string(code) + "m" + s + "\x1b[0m" : s; }
};

const Style &out();   // Wavelength's output (the stream `init` was given)
const Style &err();   // stderr: errors, warnings, progress

// the terminal's width in columns (100 when it can't be read)
int width();
// characters a string takes on screen: escape codes don't count, UTF-8 counts once per character
size_t visibleWidth(const std::string &s);
// pad on the right to `width` visible characters
std::string pad(const std::string &s, size_t width);

// a progress line on stderr while something runs, redrawn from its own thread (a render that's busy
// inside one track still ticks); nothing at all when stderr isn't styled
class Progress {
public:
    explicit Progress(std::string title);
    ~Progress();
    void update(size_t done, size_t total, const std::string &now);
    void stop();   // clears the line
private:
    struct State;
    State *s_;
};

} // namespace wl::term
