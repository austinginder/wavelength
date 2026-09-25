#pragma once
// Everything that differs between macOS, Linux and Windows: folders, child processes, loading
// plugin libraries, the main-thread event loop and leaving the process.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace wl::platform {

// Call first in main(): Windows gets UTF-8 arguments and no crash dialogs (a crashing plugin in
// a worker must end the worker, not wait for someone to click a dialog).
void init(int &argc, char **&argv);

std::filesystem::path homeDir();
// Caches (plugin scan, NKS index, auditions): ~/Library/Caches/wavelength on macOS,
// $XDG_CACHE_HOME/wavelength on Linux, %LOCALAPPDATA%\wavelength\cache on Windows.
std::filesystem::path cacheDir();
// User settings and extracted presets: ~/Library/Application Support/Wavelength on macOS,
// $XDG_CONFIG_HOME/wavelength on Linux, %APPDATA%\Wavelength on Windows.
std::filesystem::path dataDir();
// Separator for path lists in environment variables (':' or ';').
char pathListSeparator();
std::vector<std::string> envPathList(const char *var);

std::string selfExecutable();
int processId();
// True when process `pid` has a window on screen (macOS; false elsewhere). Headless plugins never
// should: a window in a worker is a licence or registration dialog waiting for a click.
bool hasOnscreenWindow(int pid);
// macOS: the CPU architectures a plugin bundle (or a plain library/executable) contains, e.g.
// {"x86_64"} for an Intel-only plugin; empty when it can't be read or elsewhere.
std::vector<std::string> binaryArchs(const std::string &path);
// The architecture this process runs as ("arm64", "x86_64").
std::string hostArch();
// The command prefix that runs this executable as `arch` (macOS Rosetta: {"/usr/bin/arch", "-x86_64"}),
// or an error when this executable has no slice for it (not a universal build).
bool archPrefix(const std::string &arch, std::vector<std::string> &prefix, std::string &err);
// One-minute load average (runnable processes), or -1 where the system doesn't report it.
double loadAverage();

// A child process running this executable (or another). Its stdout goes to /dev/null or, with
// captureStdout, to a pipe read by readOutput(); stderr is inherited unless quiet.
struct Process {
    std::intptr_t handle = 0;   // pid on POSIX, HANDLE on Windows; 0 when it could not start
    std::intptr_t out = -1;     // read end of the stdout pipe
    int id = 0;
};
bool spawn(const std::vector<std::string> &args, Process &p, bool captureStdout, bool quietStderr);
// Non-blocking: true once the process has ended. `crash` describes an abnormal end (a signal or
// an exception code), empty for a normal exit.
bool finished(Process &p, std::string &crash);
void kill(Process &p);   // kill and reap
// Read the child's stdout until it closes it or `timeoutSec` passes (then false).
bool readOutput(Process &p, std::string &out, int timeoutSec);

// A plugin library's exported symbol; the library stays loaded for the life of the process.
void *loadLibrarySymbol(const std::string &path, const char *symbol, std::string &err);

// Why a library won't load, from the system loader ("" when it loads). The VST3 SDK's Linux
// loader only says "dlopen failed".
std::string libraryLoadError(const std::string &path);

// Run the main thread's event loop for `ms` (plugins post work to it).
void pumpEvents(double ms);

// Plugins print to stdout: keep the real stdout for Wavelength's own output and point fd 1 at
// stderr. Returns the stream for Wavelength's output.
std::FILE *takeStdout();

// Leave without running static destructors (JUCE plugins crash in them at exit).
[[noreturn]] void quickExit(int code);

} // namespace wl::platform
