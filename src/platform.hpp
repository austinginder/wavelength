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

// Replace `path` with `data` so that a concurrent reader sees the old file or the new one, never a
// partial one: write a temporary file named for this process beside it, then rename it over.
bool writeFileAtomic(const std::filesystem::path &path, const std::string &data, std::string &err);

// An exclusive lock between processes on a lock file (created if missing), held until destroyed.
// Released by the system if the process dies; child processes do not inherit it.
class FileLock {
public:
    FileLock() = default;
    ~FileLock();
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;
    // Wait up to `timeoutSec` for the lock; false when it could not be taken (err says why).
    bool acquire(const std::filesystem::path &path, int timeoutSec, std::string &err);
    void release();
    bool held() const { return handle_ != invalid(); }
private:
    static std::intptr_t invalid() { return -1; }
    std::intptr_t handle_ = -1;
};

// A child process running this executable (or another). Its stdout goes to /dev/null or, with
// captureStdout, to a pipe read by readOutput(); stderr is inherited unless quiet.
struct Process {
    std::intptr_t handle = 0;   // pid on POSIX, HANDLE on Windows; 0 when it could not start
    std::intptr_t out = -1;     // read end of the stdout pipe
    std::intptr_t in = -1;      // write end of the stdin pipe (spawn with pipeStdin)
    std::string pending;        // stdout read past the last line readLine() returned
    int id = 0;
};
// pipeStdin: the child reads its stdin from a pipe written with writeInput() (else from /dev/null)
bool spawn(const std::vector<std::string> &args, Process &p, bool captureStdout, bool quietStderr, bool pipeStdin = false);
// Write to the child's stdin pipe; false when the child has gone.
bool writeInput(Process &p, const std::string &data);
// One line of the child's stdout (without the newline), waiting up to `timeoutMs`; false on a timeout or
// when the child closed its stdout.
bool readLine(Process &p, std::string &line, int timeoutMs);
// Whatever the child wrote to stdout so far (what readLine left over first), waiting up to `timeoutMs` for
// something; false once the child closed its stdout and nothing is left.
bool readSome(Process &p, std::string &out, int timeoutMs);
// Non-blocking: true once the process has ended. `crash` describes an abnormal end (a signal or
// an exception code), empty for a normal exit.
bool finished(Process &p, std::string &crash);
void kill(Process &p);   // kill and reap
// End the child from another thread without reaping it: whoever waits on it (finished(), readOutput())
// sees it end as usual.
void terminate(const Process &p);
// Read the child's stdout until it closes it or `timeoutSec` passes (then false).
bool readOutput(Process &p, std::string &out, int timeoutSec);

// A plugin library's exported symbol; the library stays loaded for the life of the process.
void *loadLibrarySymbol(const std::string &path, const char *symbol, std::string &err);

// A plain shared library (.dylib, .so, .dll: not a plugin bundle) by path or loader name, and one of
// its symbols. The first name that loads wins; the library stays loaded. nullptr when none loads.
void *openSharedLibrary(const std::vector<std::string> &names, std::string &loaded);
void *sharedSymbol(void *library, const char *symbol);
// A program on $PATH ("ffmpeg" finds ffmpeg.exe on Windows); "" when there is none.
std::string findProgram(const std::string &name);

// Why a library won't load, from the system loader ("" when it loads). The VST3 SDK's Linux
// loader only says "dlopen failed".
std::string libraryLoadError(const std::string &path);

// Move a file or folder to the user's trash (macOS ~/.Trash, the freedesktop trash on Linux, the Recycle
// Bin on Windows), renamed when the trash already holds that name. `where` gets its new path when known.
bool moveToTrash(const std::filesystem::path &path, std::string &where, std::string &err);
// Show a file or folder in the system file browser (Finder, Explorer, the Linux default).
bool reveal(const std::filesystem::path &path, std::string &err);

// Run the main thread's event loop for `ms` (plugins post work to it).
void pumpEvents(double ms);

// Plugins print to stdout: keep the real stdout for Wavelength's own output and point fd 1 at
// stderr. Returns the stream for Wavelength's output.
std::FILE *takeStdout();

// Leave without running static destructors (JUCE plugins crash in them at exit).
[[noreturn]] void quickExit(int code);

} // namespace wl::platform
