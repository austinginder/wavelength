#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace wl::platform {

namespace {
std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v ? v : "";
}

#ifdef _WIN32
std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string lastError() {
    wchar_t *msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   GetLastError(), 0, (LPWSTR)&msg, 0, nullptr);
    std::string s = msg ? narrow(msg) : "error " + std::to_string(GetLastError());
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// one argument quoted for CreateProcess (the CommandLineToArgvW rules)
std::wstring quoteArg(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
#endif
} // namespace

void init(int &argc, char **&argv) {
#ifdef _WIN32
    // no Windows Error Reporting dialog when a plugin crashes a worker
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetConsoleOutputCP(CP_UTF8);
    // UTF-8 arguments (the manifest makes the process code page UTF-8 for file names)
    int n = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wargv) {
        static std::vector<std::string> store;
        static std::vector<char *> ptrs;
        for (int i = 0; i < n; ++i) store.push_back(narrow(wargv[i]));
        for (auto &s : store) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
        argc = n;
        argv = ptrs.data();
        LocalFree(wargv);
    }
#else
    (void)argc;
    (void)argv;
#endif
}

fs::path homeDir() {
#ifdef _WIN32
    return env("USERPROFILE");
#else
    return env("HOME");
#endif
}

fs::path cacheDir() {
#if defined(__APPLE__)
    return homeDir() / "Library/Caches/wavelength";
#elif defined(_WIN32)
    return fs::path(env("LOCALAPPDATA")) / "wavelength" / "cache";
#else
    const std::string x = env("XDG_CACHE_HOME");
    return (x.empty() ? homeDir() / ".cache" : fs::path(x)) / "wavelength";
#endif
}

fs::path dataDir() {
#if defined(__APPLE__)
    return homeDir() / "Library/Application Support/Wavelength";
#elif defined(_WIN32)
    return fs::path(env("APPDATA")) / "Wavelength";
#else
    const std::string x = env("XDG_CONFIG_HOME");
    return (x.empty() ? homeDir() / ".config" : fs::path(x)) / "wavelength";
#endif
}

char pathListSeparator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

std::vector<std::string> envPathList(const char *var) {
    std::vector<std::string> paths;
    std::stringstream ss(env(var));
    std::string item;
    while (std::getline(ss, item, pathListSeparator())) if (!item.empty()) paths.push_back(item);
    return paths;
}

std::string selfExecutable() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::canonical(buf).string();
    return "wavelength";
#elif defined(_WIN32)
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return n ? narrow(buf) : "wavelength.exe";
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? "/proc/self/exe" : p.string();
#endif
}

int processId() {
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

bool writeFileAtomic(const std::filesystem::path &path, const std::string &data, std::string &err) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += "." + std::to_string(processId()) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), (std::streamsize)data.size());
        out.flush();
        if (!out) { err = "cannot write " + tmp.string(); out.close(); std::filesystem::remove(tmp, ec); return false; }
    }
    std::filesystem::rename(tmp, path, ec);   // atomic on POSIX; MoveFileEx(REPLACE_EXISTING) on Windows
    if (ec) { err = "cannot replace " + path.string() + ": " + ec.message(); std::filesystem::remove(tmp, ec); return false; }
    return true;
}

FileLock::~FileLock() { release(); }

bool FileLock::acquire(const std::filesystem::path &path, int timeoutSec, std::string &err) {
    release();
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
#ifdef _WIN32
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);   // not inheritable by default
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open lock file " + path.string(); return false; }
    for (;;) {
        OVERLAPPED ov{};
        if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) break;
        if (std::chrono::steady_clock::now() >= deadline) { CloseHandle(h); err = "timed out waiting for " + path.string(); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    handle_ = (std::intptr_t)h;
#else
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { err = "cannot open lock file " + path.string() + ": " + std::strerror(errno); return false; }
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) break;
        if (errno != EWOULDBLOCK && errno != EINTR) { err = "cannot lock " + path.string() + ": " + std::strerror(errno); close(fd); return false; }
        if (std::chrono::steady_clock::now() >= deadline) { close(fd); err = "timed out waiting for " + path.string(); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    handle_ = fd;
#endif
    return true;
}

void FileLock::release() {
    if (handle_ == invalid()) return;
#ifdef _WIN32
    OVERLAPPED ov{};
    UnlockFileEx((HANDLE)handle_, 0, 1, 0, &ov);
    CloseHandle((HANDLE)handle_);
#else
    flock((int)handle_, LOCK_UN);
    close((int)handle_);
#endif
    handle_ = invalid();
}

std::vector<std::string> binaryArchs(const std::string &path) {
    std::vector<std::string> out;
#if defined(__APPLE__)
    fs::path file = path;
    std::error_code ec;
    if (fs::is_directory(file, ec)) {   // a bundle: its executable in Contents/MacOS
        const fs::path dir = file / "Contents" / "MacOS";
        file.clear();
        for (auto &e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec)) { file = e.path(); break; }
        if (file.empty()) return out;
    }
    std::ifstream in(file, std::ios::binary);
    unsigned char h[8] = {};
    if (!in.read(reinterpret_cast<char *>(h), 8)) return out;
    auto be = [](const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; };
    auto le = [](const unsigned char *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; };
    auto name = [](uint32_t cpu) { return cpu == 0x01000007 ? std::string("x86_64") : cpu == 0x0100000C ? std::string("arm64") : std::string(); };
    const uint32_t magic = be(h);
    if (magic == 0xCAFEBABE || magic == 0xCAFEBABF) {   // universal: a list of architectures
        const uint32_t n = be(h + 4), entry = magic == 0xCAFEBABE ? 20 : 32;
        for (uint32_t i = 0; i < n && i < 16; ++i) {
            unsigned char a[32] = {};
            if (!in.read(reinterpret_cast<char *>(a), entry)) break;
            if (auto s = name(be(a)); !s.empty()) out.push_back(s);
        }
    } else if (le(h) == 0xFEEDFACF) {   // one 64-bit architecture
        if (auto s = name(le(h + 4)); !s.empty()) out.push_back(s);
    }
#else
    (void)path;
#endif
    return out;
}

std::string hostArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "x86_64";
#endif
}

bool archPrefix(const std::string &arch, std::vector<std::string> &prefix, std::string &err) {
    prefix.clear();
    if (arch.empty() || arch == hostArch()) return true;
#if defined(__APPLE__)
    const auto self = binaryArchs(selfExecutable());
    if (std::find(self.begin(), self.end(), arch) == self.end()) {
        err = "this plugin is " + arch + "-only and this Wavelength build is " + hostArch() +
              "-only: build it universal (cmake -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\") to run it under Rosetta";
        return false;
    }
    prefix = {"/usr/bin/arch", "-" + arch};
    return true;
#else
    err = "this plugin is built for " + arch + ", not " + hostArch();
    return false;
#endif
}

bool hasOnscreenWindow(int pid) {
#if defined(__APPLE__)
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return false;
    bool found = false;
    for (CFIndex i = 0, n = CFArrayGetCount(list); i < n && !found; ++i) {
        auto info = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        auto owner = (CFNumberRef)CFDictionaryGetValue(info, kCGWindowOwnerPID);
        int p = 0;
        if (owner && CFNumberGetValue(owner, kCFNumberIntType, &p) && p == pid) found = true;
    }
    CFRelease(list);
    return found;
#else
    (void)pid;
    return false;
#endif
}

double loadAverage() {
#ifdef _WIN32
    return -1;
#else
    double l[1];
    return getloadavg(l, 1) == 1 ? l[0] : -1;
#endif
}

bool spawn(const std::vector<std::string> &args, Process &p, bool captureStdout, bool quietStderr, bool pipeStdin) {
    p = Process{};
    // one spawn at a time: a pipe's write end exists only inside this lock, so no other child
    // can inherit it and keep the pipe open after its own child exits
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE rd = nullptr, wr = nullptr, inRd = nullptr, inWr = nullptr;
    if (captureStdout) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) { CloseHandle(nul); return false; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }
    if (pipeStdin) {
        if (!CreatePipe(&inRd, &inWr, &sa, 0)) { CloseHandle(nul); if (rd) { CloseHandle(rd); CloseHandle(wr); } return false; }
        SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipeStdin ? inRd : nul;
    si.hStdOutput = captureStdout ? wr : nul;
    si.hStdError = quietStderr ? nul : GetStdHandle(STD_ERROR_HANDLE);
    std::wstring cmd;
    for (auto &a : args) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(widen(a));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(widen(args[0]).c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(nul);
    if (wr) CloseHandle(wr);
    if (inRd) CloseHandle(inRd);
    if (!ok) { if (rd) CloseHandle(rd); if (inWr) CloseHandle(inWr); return false; }
    CloseHandle(pi.hThread);
    p.handle = (std::intptr_t)pi.hProcess;
    p.id = (int)pi.dwProcessId;
    if (captureStdout) p.out = (std::intptr_t)rd;
    if (pipeStdin) p.in = (std::intptr_t)inWr;
    return true;
#else
    int pipefd[2] = {-1, -1}, infd[2] = {-1, -1};
    if (captureStdout && pipe(pipefd) != 0) return false;
    if (pipeStdin && pipe(infd) != 0) { if (captureStdout) { close(pipefd[0]); close(pipefd[1]); } return false; }
    if (pipeStdin) fcntl(infd[1], F_SETFD, FD_CLOEXEC);   // later children must not hold this child's stdin open
    if (captureStdout) fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (pipeStdin) {
        posix_spawn_file_actions_adddup2(&fa, infd[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&fa, infd[1]);
    }
    if (captureStdout) {
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (quietStderr) posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::vector<std::string> copy = args;
    std::vector<char *> argv;
    for (auto &s : copy) argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, args[0].c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (captureStdout) close(pipefd[1]);
    if (pipeStdin) close(infd[0]);
    if (rc != 0) { if (captureStdout) close(pipefd[0]); if (pipeStdin) close(infd[1]); return false; }
    p.handle = pid;
    p.id = pid;
    if (captureStdout) p.out = pipefd[0];
    if (pipeStdin) p.in = infd[1];
    return true;
#endif
}

bool finished(Process &p, std::string &crash) {
    crash.clear();
    if (!p.handle) return true;
#ifdef _WIN32
    HANDLE h = (HANDLE)p.handle;
    if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    if (code >= 0xC0000000u) {   // an unhandled exception (access violation, stack overflow, ...)
        char buf[32];
        std::snprintf(buf, sizeof buf, "exception 0x%08lX", (unsigned long)code);
        crash = buf;
    }
    CloseHandle(h);
#else
    int status = 0;
    if (waitpid((pid_t)p.handle, &status, WNOHANG) != (pid_t)p.handle) return false;
    if (WIFSIGNALED(status)) crash = "signal " + std::to_string(WTERMSIG(status));
#endif
    p.handle = 0;
    return true;
}

namespace {
void closePipes(Process &p) {
#ifdef _WIN32
    if (p.in != -1) CloseHandle((HANDLE)p.in);
    if (p.out != -1) CloseHandle((HANDLE)p.out);
#else
    if (p.in != -1) close((int)p.in);
    if (p.out != -1) close((int)p.out);
#endif
    p.in = p.out = -1;
}
} // namespace

void kill(Process &p) {
    closePipes(p);
    if (!p.handle) return;
#ifdef _WIN32
    TerminateProcess((HANDLE)p.handle, 1);
    WaitForSingleObject((HANDLE)p.handle, INFINITE);
    CloseHandle((HANDLE)p.handle);
#else
    ::kill((pid_t)p.handle, SIGKILL);
    waitpid((pid_t)p.handle, nullptr, 0);
#endif
    p.handle = 0;
}

bool writeInput(Process &p, const std::string &data) {
    if (p.in == -1) return false;
    size_t done = 0;
    while (done < data.size()) {
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile((HANDLE)p.in, data.data() + done, (DWORD)(data.size() - done), &n, nullptr) || !n) return false;
#else
        const ssize_t n = write((int)p.in, data.data() + done, data.size() - done);   // SIGPIPE: callers ignore it
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
#endif
        done += (size_t)n;
    }
    return true;
}

bool readLine(Process &p, std::string &line, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[65536];
    for (;;) {
        const size_t nl = p.pending.find('\n');
        if (nl != std::string::npos) { line = p.pending.substr(0, nl); p.pending.erase(0, nl + 1); return true; }
        if (p.out == -1) return false;
        const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe((HANDLE)p.out, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (!avail) { Sleep(5); continue; }
        DWORD n = 0;
        if (!ReadFile((HANDLE)p.out, buf, (DWORD)std::min<size_t>(avail, sizeof buf), &n, nullptr) || !n) return false;
        p.pending.append(buf, n);
#else
        pollfd pfd{(int)p.out, POLLIN, 0};
        if (poll(&pfd, 1, std::min(left, 200)) > 0) {
            const ssize_t n = read((int)p.out, buf, sizeof buf);
            if (n <= 0) return false;
            p.pending.append(buf, (size_t)n);
        }
#endif
    }
}

bool readSome(Process &p, std::string &out, int timeoutMs) {
    out.clear();
    if (!p.pending.empty()) { out.swap(p.pending); return true; }
    if (p.out == -1) return false;
    char buf[65536];
#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe((HANDLE)p.out, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail) {
            DWORD n = 0;
            if (!ReadFile((HANDLE)p.out, buf, (DWORD)std::min<size_t>(avail, sizeof buf), &n, nullptr) || !n) return false;
            out.assign(buf, n);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return true;
        Sleep(2);
    }
#else
    pollfd pfd{(int)p.out, POLLIN, 0};
    if (poll(&pfd, 1, timeoutMs) <= 0) return true;   // nothing yet
    const ssize_t n = read((int)p.out, buf, sizeof buf);
    if (n <= 0) return false;
    out.assign(buf, (size_t)n);
    return true;
#endif
}

void terminate(const Process &p) {
    if (!p.handle) return;
#ifdef _WIN32
    TerminateProcess((HANDLE)p.handle, 1);
#else
    ::kill((pid_t)p.handle, SIGKILL);
#endif
}

namespace {
// `dir`/`name`, or "name 2", "name 3", ... when that is taken
std::filesystem::path freeName(const std::filesystem::path &dir, const std::string &name) {
    std::error_code ec;
    std::filesystem::path to = dir / name;
    for (int i = 2; std::filesystem::exists(to, ec) && i < 1000; ++i) to = dir / (name + " " + std::to_string(i));
    return to;
}
} // namespace

bool moveToTrash(const std::filesystem::path &path, std::string &where, std::string &err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) { err = path.string() + " does not exist"; return false; }
    const fs::path abs = fs::absolute(path, ec);
#ifdef _WIN32
    std::wstring from = abs.wstring();
    from.push_back(L'\0');   // SHFileOperation takes a double-NUL-terminated list
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) { err = "the Recycle Bin refused " + abs.string(); return false; }
    where = "Recycle Bin";
    return true;
#elif defined(__APPLE__)
    const fs::path trash = homeDir() / ".Trash";
    fs::create_directories(trash, ec);
    const fs::path to = freeName(trash, abs.filename().string());
    fs::rename(abs, to, ec);
    if (ec) { err = "could not move " + abs.string() + " to the Trash: " + ec.message(); return false; }
    where = to.string();
    return true;
#else
    // freedesktop.org trash spec: files/<name> plus info/<name>.trashinfo (where it came from, when)
    const char *xdg = std::getenv("XDG_DATA_HOME");
    const fs::path trash = (xdg && *xdg ? fs::path(xdg) : homeDir() / ".local" / "share") / "Trash";
    fs::create_directories(trash / "files", ec);
    fs::create_directories(trash / "info", ec);
    const fs::path to = freeName(trash / "files", abs.filename().string());
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    std::string encoded;   // the original path, percent-encoded
    for (unsigned char c : abs.string()) {
        if (std::isalnum(c) || std::strchr("/-_.~", c)) encoded += (char)c;
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); encoded += b; }
    }
    std::ofstream(trash / "info" / (to.filename().string() + ".trashinfo")) << "[Trash Info]\nPath=" << encoded << "\nDeletionDate=" << when << "\n";
    fs::rename(abs, to, ec);
    if (ec) {
        fs::remove(trash / "info" / (to.filename().string() + ".trashinfo"), ec);
        err = "could not move " + abs.string() + " to the trash: " + ec.message();
        return false;
    }
    where = to.string();
    return true;
#endif
}

bool reveal(const std::filesystem::path &path, std::string &err) {
    Process p;
#ifdef _WIN32
    const std::vector<std::string> args = {"explorer.exe", "/select," + path.string()};
#elif defined(__APPLE__)
    const std::vector<std::string> args = {"/usr/bin/open", "-R", path.string()};
#else
    const std::string opener = findProgram("xdg-open");
    if (opener.empty()) { err = "xdg-open is not installed"; return false; }
    const std::vector<std::string> args = {opener, path.parent_path().string()};
#endif
    if (!spawn(args, p, false, true)) { err = "could not start " + args[0]; return false; }
    std::thread([p]() mutable { std::string crash; while (!finished(p, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }).detach();
    return true;
}

bool readOutput(Process &p, std::string &out, int timeoutSec) {
    if (p.out == -1) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    bool ok = true;
    char buf[65536];
    for (;;) {
        const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { ok = false; break; }
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe((HANDLE)p.out, nullptr, 0, nullptr, &avail, nullptr)) break;   // closed
        if (!avail) { Sleep(20); continue; }
        DWORD n = 0;
        if (!ReadFile((HANDLE)p.out, buf, (DWORD)std::min<size_t>(avail, sizeof buf), &n, nullptr) || !n) break;
        out.append(buf, n);
#else
        pollfd pfd{(int)p.out, POLLIN, 0};
        if (poll(&pfd, 1, std::min(left, 500)) > 0) {
            const ssize_t n = read((int)p.out, buf, sizeof buf);
            if (n <= 0) break;
            out.append(buf, (size_t)n);
        }
#endif
    }
#ifdef _WIN32
    CloseHandle((HANDLE)p.out);
#else
    close((int)p.out);
#endif
    p.out = -1;
    return ok;
}

void *loadLibrarySymbol(const std::string &path, const char *symbol, std::string &err) {
#if defined(__APPLE__)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.c_str()),
                                                           (CFIndex)path.size(), true);
    if (!url) { err = "invalid bundle path"; return nullptr; }
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
    CFRelease(url);
    if (!bundle) { err = "not a bundle: " + path; return nullptr; }
    CFErrorRef cfErr = nullptr;
    if (!CFBundleLoadExecutableAndReturnError(bundle, &cfErr)) {
        err = "could not load executable in " + path;
        if (cfErr) {
            CFStringRef desc = CFErrorCopyDescription(cfErr);
            char buf[512];
            if (desc && CFStringGetCString(desc, buf, sizeof buf, kCFStringEncodingUTF8)) err += ": " + std::string(buf);
            if (desc) CFRelease(desc);
            CFRelease(cfErr);
        }
        return nullptr;
    }
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, symbol, kCFStringEncodingUTF8);
    void *sym = CFBundleGetDataPointerForName(bundle, name);   // bundle intentionally never released
    CFRelease(name);
#elif defined(_WIN32)
    HMODULE lib = LoadLibraryW(widen(path).c_str());
    if (!lib) { err = "could not load " + path + ": " + lastError(); return nullptr; }
    void *sym = reinterpret_cast<void *>(GetProcAddress(lib, symbol));
#else
    void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { err = "could not load " + path + ": " + dlerror(); return nullptr; }
    void *sym = dlsym(lib, symbol);
#endif
    if (!sym) err = std::string("no ") + symbol + " symbol in " + path;
    return sym;
}

void *openSharedLibrary(const std::vector<std::string> &names, std::string &loaded) {
    for (const auto &n : names) {
#ifdef _WIN32
        HMODULE lib = LoadLibraryW(widen(n).c_str());
#else
        void *lib = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (lib) { loaded = n; return reinterpret_cast<void *>(lib); }
    }
    return nullptr;
}

void *sharedSymbol(void *library, const char *symbol) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), symbol));
#else
    return dlsym(library, symbol);
#endif
}

std::string findProgram(const std::string &name) {
    const char *path = std::getenv("PATH");
    if (!path) return "";
#ifdef _WIN32
    const std::string file = name + ".exe";
#else
    const std::string &file = name;
#endif
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, pathListSeparator())) {
        if (dir.empty()) continue;
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::path(dir) / file;
        if (std::filesystem::is_regular_file(p, ec)) return p.string();
    }
    return "";
}

std::string libraryLoadError(const std::string &path) {
#if defined(__APPLE__) || defined(_WIN32)
    (void)path;
    return "";
#else
    void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib) { dlclose(lib); return ""; }
    return dlerror();
#endif
}

void pumpEvents(double ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((int64_t)(ms * 1000));
    do {
#if defined(__APPLE__)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
#elif defined(_WIN32)
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(2);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
    } while (std::chrono::steady_clock::now() < until);
}

std::FILE *takeStdout() {
    std::fflush(stdout);
#ifdef _WIN32
    const int real = _dup(1);
    std::FILE *f = real >= 0 ? _fdopen(real, "w") : nullptr;
    if (!f) return stdout;
    _setmode(real, _O_BINARY);   // "\n", not "\r\n", in JSON output
    _dup2(2, 1);
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    return f;
#else
    const int real = dup(STDOUT_FILENO);
    std::FILE *f = real >= 0 ? fdopen(real, "w") : nullptr;
    if (!f) return stdout;
    dup2(STDERR_FILENO, STDOUT_FILENO);
    return f;
#endif
}

void quickExit(int code) { std::_Exit(code); }

} // namespace wl::platform
