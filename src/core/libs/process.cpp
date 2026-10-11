// @sonata/process: the running program, the child processes it starts, and signals.
// Facts about the machine itself (OS, CPU, memory, ...) are in @sonata/sys.
//
//     local process = require("@sonata/process")
//
// The running program
//     process.pid           --> 4242
//     process.ppid          --> 4200                            (nil if it cannot be determined)
//     process.execPath      --> "/usr/local/bin/sn"             (nil if it cannot be determined)
//     process.args          --> { "build", "--release" }        the program's arguments
//     process.env.HOME      --> "/home/me"                      (nil if unset)
//     process.env.FOO = "bar"                                   assigning nil removes the variable
//     for name, value in process.env do ... end
//     process.cwd()         --> "/home/me/project"
//     process.chdir("/tmp")
//     process.uptime()      --> seconds since the program started
//     process.memoryUsage() --> { rss: number?, luauHeap: number }   bytes in RAM / used by this VM's heap
//     process.exit(code?)   never returns; code defaults to 0
//
// Running a program and collecting its output
//     local result = process.exec("git", { "status", "--short" }, {
//         cwd = "repo",                -- working directory of the child
//         env = { GIT_PAGER = "cat" }, -- added to / overriding the inherited environment
//         stdin = "text",              -- data fed to the child's stdin
//         shell = false,               -- true: run "program args..." through /bin/sh (cmd.exe on Windows)
//         stdio = "pipe",              -- "pipe" captures stdout/stderr, "inherit" shares ours
//     })
//     result.ok, result.code, result.stdout, result.stderr
//
// Starting a program and talking to it while it runs
//     local child = process.spawn("python3", { "-i" }, {
//         cwd, env, shell,             -- as for exec
//         stdin = "pipe",              -- each of stdin/stdout/stderr: "pipe" (default) | "inherit" | "null"
//         stdout = "pipe",
//         stderr = "inherit",
//         stdio = "pipe",              -- sets all three at once (the individual options win)
//         detached = false,            -- true: own session / process group, survives our Ctrl+C
//     })
//     child.pid
//     child.stdin:write(data)      --> true | nil, err      child.stdin:close()   (sends EOF)
//     child.stdout:read(max?)      --> string?              whatever is available (up to max, default 64 KiB);
//                                                           nil at EOF
//     child.stdout:readLine(keepEnding?) --> string?        nil at EOF
//     child.stdout:readAll()       --> string               everything until EOF
//     child.stderr: the same
//     child:wait(timeout?)         --> { ok, code, signal? } | nil, "timed out"
//     child:status()               --> nil while it runs, the same table as wait() once it has exited
//     child:kill(signal?)          --> boolean             signal defaults to "terminate"
//
//   Pipes exist only for streams set to "pipe". A pipe read waits for data without
//   blocking other tasks; reading from two tasks at once is safe (they take turns).
//   A child that is never waited for is reaped in the background.
//
// Signals
//     process.kill(pid, signal?)   --> true | nil, err      signal defaults to "terminate"
//     process.onSignal(signal, handler?)                    handler(signalName) runs as a new task each
//                                                           time the signal arrives; nil restores the
//                                                           default behaviour (usually: exit)
//
//   Signal names: "interrupt" (Ctrl+C), "terminate", "hangup", "quit", "user1", "user2",
//   "resize" (terminal resized), "alarm", "continue", plus "kill" and "stop" for sending.
//   "SIGINT", "INT" and plain numbers work too. On Windows only "interrupt", "break"
//   (Ctrl+Break) and "hangup" (console closed) can be handled, and sending any signal
//   terminates the process.
//   A handler does not keep the program alive by itself.
//
// Notes:
//   * exec, wait, and pipe reads and writes let other tasks run while they wait
//     (see @sonata/task). A program that cannot be started raises an error; one
//     that exits non-zero does not (ok = false).
//   * A child killed by a signal reports code 128 + signal, like a shell would,
//     and wait() also names the signal.
//   * With stdio = "inherit" the child shares the terminal and result.stdout / result.stderr are "".
//   * process.exit uses std::exit: stdio is flushed and atexit handlers run, but stack objects of
//     the host are not unwound. Register cleanup with std::atexit if it must always happen.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // GetProcessMemoryInfo from kernel32, no psapi.lib needed
#endif
#include <psapi.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif
#endif

#include <sonata/core/library.hpp>
#include <sonata/core/scheduler.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

#if !defined(_WIN32) && !defined(__APPLE__)
extern char** environ;
#endif

namespace sonata::lib::libs {

namespace {

// The runtime stores the program's arguments here (see sonata/core/runtime.cpp).
constexpr const char* kArgsRegistryKey = "sonata.process.args";

// platform layer /////////////
//
// Plain C++: nothing in here touches Lua, so nothing in here can raise a Lua
// error. The bindings below call it, then turn failures into Lua errors only
// after every C++ object is out of scope (see the rule at the top of example.cpp).

namespace sys {

using EnvList = std::vector<std::pair<std::string, std::string>>;

std::string errorText(int code) {
    return std::system_category().message(code);
}

#if defined(_WIN32)

// Strings cross the Lua boundary as UTF-8; Windows is spoken to in UTF-16.
std::wstring widen(const char* text, std::size_t length) {
    if (length == 0) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), wide.data(), count);
    }
    return wide;
}

std::wstring widen(const std::string& text) {
    return widen(text.data(), text.size());
}

std::string narrow(const wchar_t* text, std::size_t length) {
    if (length == 0) {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(count), '\0');
    if (count > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), utf8.data(), count, nullptr, nullptr);
    }
    return utf8;
}

using WideEnvList = std::vector<std::pair<std::wstring, std::wstring>>;

WideEnvList currentEnvironmentWide() {
    WideEnvList vars;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) {
        return vars;
    }

    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        const wchar_t* equals = std::wcschr(entry + 1, L'='); // +1: "=C:=C:\" style entries start with '='
        if (!equals || entry[0] == L'=') {
            continue;
        }
        vars.emplace_back(std::wstring(entry, equals), std::wstring(equals + 1));
    }

    FreeEnvironmentStringsW(block);
    return vars;
}

bool sameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool getEnv(const char* name, std::string& value) {
    const std::wstring wideName = widen(name, std::strlen(name));
    std::wstring buffer(64, L'\0');

    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableW(wideName.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));

        if (n == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return false;
            }
            value.clear(); // exists, but empty
            return true;
        }
        if (n < buffer.size()) {
            value = narrow(buffer.data(), n);
            return true;
        }
        buffer.resize(n); // too small: n is the required size including the terminator
    }
}

// value == nullptr removes the variable.
bool setEnv(const char* name, const char* value, std::string& error) {
    const std::wstring wideName = widen(name, std::strlen(name));
    const std::wstring wideValue = value ? widen(value, std::strlen(value)) : std::wstring();

    if (SetEnvironmentVariableW(wideName.c_str(), value ? wideValue.c_str() : nullptr)) {
        return true;
    }
    if (!value && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return true; // removing something that is not there is fine
    }
    error = errorText(static_cast<int>(GetLastError()));
    return false;
}

void listEnv(EnvList& out) {
    for (const auto& [name, value] : currentEnvironmentWide()) {
        out.emplace_back(narrow(name.c_str(), name.size()), narrow(value.c_str(), value.size()));
    }
}

bool currentDir(std::string& path, std::string& error) {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (n == 0) {
            error = errorText(static_cast<int>(GetLastError()));
            return false;
        }
        if (n < buffer.size()) {
            path = narrow(buffer.data(), n);
            return true;
        }
        buffer.resize(n);
    }
}

bool changeDir(const char* path, std::string& error) {
    const std::wstring widePath = widen(path, std::strlen(path));
    if (SetCurrentDirectoryW(widePath.c_str())) {
        return true;
    }
    error = errorText(static_cast<int>(GetLastError()));
    return false;
}

bool executablePath(std::string& path) {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return false;
        }
        if (n < buffer.size()) {
            path = narrow(buffer.data(), n);
            return true;
        }
        buffer.resize(buffer.size() * 2); // truncated
    }
}

double processId() {
    return static_cast<double>(GetCurrentProcessId());
}

#else // POSIX

char** environBlock() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

void setEnvironBlock(char** block) {
#if defined(__APPLE__)
    *_NSGetEnviron() = block;
#else
    environ = block;
#endif
}

bool getEnv(const char* name, std::string& value) {
    const char* found = std::getenv(name);
    if (!found) {
        return false;
    }
    value = found;
    return true;
}

// value == nullptr removes the variable.
bool setEnv(const char* name, const char* value, std::string& error) {
    const int result = value ? ::setenv(name, value, 1) : ::unsetenv(name);
    if (result == 0) {
        return true;
    }
    error = errorText(errno);
    return false;
}

void listEnv(EnvList& out) {
    char** block = environBlock();
    for (char** entry = block; entry && *entry; ++entry) {
        const char* equals = std::strchr(*entry, '=');
        if (!equals || equals == *entry) {
            continue;
        }
        out.emplace_back(std::string(*entry, static_cast<std::size_t>(equals - *entry)), std::string(equals + 1));
    }
}

bool currentDir(std::string& path, std::string& error) {
    std::vector<char> buffer(256);

    for (;;) {
        if (::getcwd(buffer.data(), buffer.size())) {
            path = buffer.data();
            return true;
        }
        if (errno != ERANGE) {
            error = errorText(errno);
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool changeDir(const char* path, std::string& error) {
    if (::chdir(path) == 0) {
        return true;
    }
    error = errorText(errno);
    return false;
}

#if defined(__linux__)

bool executablePath(std::string& path) {
    std::vector<char> buffer(256);

    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (n < 0) {
            return false;
        }
        if (static_cast<std::size_t>(n) < buffer.size()) {
            path.assign(buffer.data(), static_cast<std::size_t>(n));
            return true;
        }
        buffer.resize(buffer.size() * 2); // possibly truncated
    }
}

#elif defined(__APPLE__)

bool executablePath(std::string& path) {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // fails, but reports the size needed

    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return false;
    }
    path = buffer.data();
    return true;
}

#else

bool executablePath(std::string&) {
    return false;
}

#endif

double processId() {
    return static_cast<double>(::getpid());
}

#endif // _WIN32

// running a child process ////////

struct ExecSpec {
    std::string program;
    std::vector<std::string> args;
    std::optional<std::string> cwd;
    EnvList env;                 // added to / overriding the inherited environment
    EnvList baseEnv;             // POSIX: the environment "env" applies to, read on the VM's thread
    std::string stdinData;
    bool hasStdin = false;
    bool shell = false;
    bool inheritStdio = false;
};

struct ExecOutcome {
    bool launched = false;
    std::string error;           // why it could not be launched
    int code = 0;
    std::string out;
    std::string err;
};

#if defined(_WIN32)

class Handle {
public:
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }

    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = nullptr) {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }
    HANDLE release() {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// Creates a pipe whose child end is inheritable and whose parent end is not.
bool makePipe(Handle& parentEnd, Handle& childEnd, bool childReads) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) {
        return false;
    }

    parentEnd.reset(childReads ? writeEnd : readEnd);
    childEnd.reset(childReads ? readEnd : writeEnd);
    return SetHandleInformation(parentEnd.get(), HANDLE_FLAG_INHERIT, 0) != 0;
}

std::string readAll(HANDLE handle) {
    std::string data;
    char buffer[16384];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        data.append(buffer, got);
    }
    return data;
}

void writeAll(HANDLE handle, const std::string& data) {
    std::size_t written = 0;
    while (written < data.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - written, 1u << 20));
        if (!WriteFile(handle, data.data() + written, want, &chunk, nullptr) || chunk == 0) {
            return; // the child closed its stdin
        }
        written += chunk;
    }
}

// Quotes one argument the way CommandLineToArgvW / the MSVC runtime will read it back.
std::wstring quoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(c);
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\'); // they precede the closing quote
    quoted.push_back(L'"');
    return quoted;
}

ExecOutcome runExec(const ExecSpec& spec) {
    ExecOutcome outcome;
    std::fflush(nullptr); // so our buffered output cannot appear after the child's

    std::wstring commandLine;
    if (spec.shell) {
        std::string command = spec.program;
        for (const std::string& arg : spec.args) {
            command += ' ';
            command += arg;
        }

        std::string comspec = "cmd.exe";
        std::string fromEnv;
        if (getEnv("ComSpec", fromEnv) && !fromEnv.empty()) {
            comspec = fromEnv;
        }
        commandLine = quoteArgument(widen(comspec)) + L" /d /s /c \"" + widen(command) + L"\"";
    } else {
        commandLine = quoteArgument(widen(spec.program));
        for (const std::string& arg : spec.args) {
            commandLine += L' ';
            commandLine += quoteArgument(widen(arg));
        }
    }

    // Environment block: current variables with the overrides applied, sorted as Windows wants.
    std::wstring environmentBlock;
    if (!spec.env.empty()) {
        WideEnvList merged = currentEnvironmentWide();
        for (const auto& [name, value] : spec.env) {
            const std::wstring wideName = widen(name);
            bool replaced = false;
            for (auto& existing : merged) {
                if (sameName(existing.first, wideName)) {
                    existing.second = widen(value);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged.emplace_back(wideName, widen(value));
            }
        }

        std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
            return CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });

        for (const auto& [name, value] : merged) {
            environmentBlock += name;
            environmentBlock += L'=';
            environmentBlock += value;
            environmentBlock += L'\0';
        }
        environmentBlock += L'\0';
    }

    const std::wstring workingDir = spec.cwd ? widen(*spec.cwd) : std::wstring();

    Handle inParent, inChild, outParent, outChild, errParent, errChild;
    if (!spec.inheritStdio) {
        if (!makePipe(inParent, inChild, true) || !makePipe(outParent, outChild, false) ||
            !makePipe(errParent, errChild, false)) {
            outcome.error = "failed to create pipes: " + errorText(static_cast<int>(GetLastError()));
            return outcome;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = spec.inheritStdio ? GetStdHandle(STD_INPUT_HANDLE) : inChild.get();
    startup.hStdOutput = spec.inheritStdio ? GetStdHandle(STD_OUTPUT_HANDLE) : outChild.get();
    startup.hStdError = spec.inheritStdio ? GetStdHandle(STD_ERROR_HANDLE) : errChild.get();

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE, // inherit the (inheritable) handles prepared above
        CREATE_UNICODE_ENVIRONMENT,
        spec.env.empty() ? nullptr : environmentBlock.data(),
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &startup,
        &info);

    if (!created) {
        outcome.error = "failed to run '" + spec.program + "': " + errorText(static_cast<int>(GetLastError()));
        return outcome;
    }

    CloseHandle(info.hThread);
    Handle process;
    process.reset(info.hProcess);

    // The child owns its ends now; keeping ours open would stop EOF from ever arriving.
    inChild.reset();
    outChild.reset();
    errChild.reset();

    if (!spec.inheritStdio) {
        std::thread errReader([&] { outcome.err = readAll(errParent.get()); });
        std::thread inWriter;

        if (spec.hasStdin && !spec.stdinData.empty()) {
            inWriter = std::thread([&] {
                writeAll(inParent.get(), spec.stdinData);
                inParent.reset();
            });
        } else {
            inParent.reset();
        }

        outcome.out = readAll(outParent.get());

        errReader.join();
        if (inWriter.joinable()) {
            inWriter.join();
        }
    }

    WaitForSingleObject(process.get(), INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.get(), &exitCode);

    outcome.launched = true;
    outcome.code = static_cast<int>(exitCode);
    return outcome;
}

#else // POSIX

class Fd {
public:
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;
};

// Takes a descriptor out of the 0-2 range (so a later dup2 onto 0-2 cannot clobber
// it) and marks it close-on-exec.
bool secureFd(Fd& fd) {
    if (!fd.valid()) {
        return false;
    }
    const int moved = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 3);
    if (moved < 0) {
        return false;
    }
    fd.reset(moved);
    return true;
}

bool makePipe(Fd& readEnd, Fd& writeEnd) {
    int fds[2];
    if (::pipe(fds) != 0) {
        return false;
    }
    readEnd.reset(fds[0]);
    writeEnd.reset(fds[1]);
    return secureFd(readEnd) && secureFd(writeEnd);
}

struct ChildFailure {
    int stage; // 0: stdio setup, 1: chdir, 2: exec
    int error;
};

[[noreturn]] void childFail(int reportFd, int stage, int error) {
    const ChildFailure failure{stage, error};
    const ssize_t ignored = ::write(reportFd, &failure, sizeof(failure));
    (void)ignored;
    ::_exit(127);
}

// Keeps SIGPIPE from killing us when the child closes its stdin before we are done writing.
class SigpipeBlocker {
public:
    SigpipeBlocker() {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        active_ = pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0 && !sigismember(&previous_, SIGPIPE);
    }

    ~SigpipeBlocker() {
        if (active_) {
            sigset_t pending;
            sigemptyset(&pending);
            if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
                int ignored = 0;
                sigwait(&set_, &ignored); // consume it so it is not delivered once unblocked
            }
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
        }
    }

    SigpipeBlocker(const SigpipeBlocker&) = delete;
    SigpipeBlocker& operator=(const SigpipeBlocker&) = delete;

private:
    sigset_t set_;
    sigset_t previous_;
    bool active_ = false;
};

// Feeds "input" to the child while draining its output, so a full pipe in either
// direction can never deadlock us. Closes all three descriptors when done.
void pump(const std::string& input, Fd& inWrite, Fd& outRead, Fd& errRead, std::string& out, std::string& err) {
    std::size_t written = 0;

    if (inWrite.valid()) {
        if (input.empty()) {
            inWrite.reset(); // the child sees EOF straight away
        } else {
            const int flags = ::fcntl(inWrite.get(), F_GETFL);
            ::fcntl(inWrite.get(), F_SETFL, flags | O_NONBLOCK);
        }
    }

    char buffer[16384];

    while (inWrite.valid() || outRead.valid() || errRead.valid()) {
        pollfd fds[3];
        int kinds[3];
        nfds_t count = 0;

        if (inWrite.valid()) {
            fds[count] = {inWrite.get(), POLLOUT, 0};
            kinds[count++] = 0;
        }
        if (outRead.valid()) {
            fds[count] = {outRead.get(), POLLIN, 0};
            kinds[count++] = 1;
        }
        if (errRead.valid()) {
            fds[count] = {errRead.get(), POLLIN, 0};
            kinds[count++] = 2;
        }

        if (::poll(fds, count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].revents == 0) {
                continue;
            }

            if (kinds[i] == 0) {
                const ssize_t n = ::write(inWrite.get(), input.data() + written, input.size() - written);
                if (n > 0) {
                    written += static_cast<std::size_t>(n);
                }
                const bool failed = n < 0 && errno != EAGAIN && errno != EINTR;
                if (failed || written == input.size()) {
                    inWrite.reset();
                }
            } else {
                Fd& source = kinds[i] == 1 ? outRead : errRead;
                std::string& sink = kinds[i] == 1 ? out : err;

                const ssize_t n = ::read(source.get(), buffer, sizeof(buffer));
                if (n > 0) {
                    sink.append(buffer, static_cast<std::size_t>(n));
                } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                    source.reset();
                }
            }
        }
    }

    inWrite.reset();
    outRead.reset();
    errRead.reset();
}

int reap(pid_t pid) {
    int status = 0;
    pid_t result;
    do {
        result = ::waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    return status;
}

ExecOutcome runExec(const ExecSpec& spec) {
    ExecOutcome outcome;
    std::fflush(nullptr); // so our buffered output cannot appear after the child's

    // Everything the child needs is prepared here: after fork() it may not allocate.
    std::vector<std::string> words;
    if (spec.shell) {
        std::string command = spec.program;
        for (const std::string& arg : spec.args) {
            command += ' ';
            command += arg;
        }
        words.push_back("/bin/sh");
        words.push_back("-c");
        words.push_back(std::move(command));
    } else {
        words.push_back(spec.program);
        words.insert(words.end(), spec.args.begin(), spec.args.end());
    }

    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> environment;
    std::vector<char*> envp;
    const bool customEnv = !spec.env.empty();
    if (customEnv) {
        EnvList merged = spec.baseEnv;

        for (const auto& [name, value] : spec.env) {
            bool replaced = false;
            for (auto& existing : merged) {
                if (existing.first == name) {
                    existing.second = value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged.emplace_back(name, value);
            }
        }

        for (const auto& [name, value] : merged) {
            environment.push_back(name + "=" + value);
        }
        for (std::string& entry : environment) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
    }

    const char* cwd = spec.cwd ? spec.cwd->c_str() : nullptr;

    Fd inRead, inWrite, outRead, outWrite, errRead, errWrite, failRead, failWrite;
    if (!spec.inheritStdio) {
        if (!makePipe(inRead, inWrite) || !makePipe(outRead, outWrite) || !makePipe(errRead, errWrite)) {
            outcome.error = "failed to create pipes: " + errorText(errno);
            return outcome;
        }
    }
    if (!makePipe(failRead, failWrite)) {
        outcome.error = "failed to create pipes: " + errorText(errno);
        return outcome;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        outcome.error = "failed to fork: " + errorText(errno);
        return outcome;
    }

    if (pid == 0) {
        // Child. Only async-signal-safe calls from here until exec; it never returns.
        if (!spec.inheritStdio) {
            if (::dup2(inRead.get(), 0) < 0 || ::dup2(outWrite.get(), 1) < 0 || ::dup2(errWrite.get(), 2) < 0) {
                childFail(failWrite.get(), 0, errno);
            }
        }
        if (cwd && ::chdir(cwd) != 0) {
            childFail(failWrite.get(), 1, errno);
        }
        if (customEnv) {
            setEnvironBlock(envp.data());
        }
        ::execvp(argv[0], argv.data());
        childFail(failWrite.get(), 2, errno);
    }

    // Parent: drop the child's ends so EOF can be seen, then learn whether exec worked
    // (the report pipe closes on a successful exec, thanks to close-on-exec).
    inRead.reset();
    outWrite.reset();
    errWrite.reset();
    failWrite.reset();

    ChildFailure failure{0, 0};
    ssize_t got;
    do {
        got = ::read(failRead.get(), &failure, sizeof(failure));
    } while (got < 0 && errno == EINTR);
    failRead.reset();

    if (got == static_cast<ssize_t>(sizeof(failure))) {
        reap(pid);
        if (failure.stage == 1) {
            outcome.error = "failed to change directory to '" + *spec.cwd + "': " + errorText(failure.error);
        } else if (failure.stage == 2) {
            outcome.error = "failed to run '" + spec.program + "': " + errorText(failure.error);
        } else {
            outcome.error = "failed to set up the child's stdio: " + errorText(failure.error);
        }
        return outcome;
    }

    if (spec.inheritStdio) {
        // nothing to pump
    } else if (spec.hasStdin && !spec.stdinData.empty()) {
        SigpipeBlocker blocker;
        pump(spec.stdinData, inWrite, outRead, errRead, outcome.out, outcome.err);
    } else {
        pump(std::string(), inWrite, outRead, errRead, outcome.out, outcome.err);
    }

    const int status = reap(pid);
    outcome.launched = true;
    if (WIFEXITED(status)) {
        outcome.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        outcome.code = 128 + WTERMSIG(status);
    } else {
        outcome.code = -1;
    }
    return outcome;
}

#endif // _WIN32


// child processes that keep running ////////
//
// Everything below runs either on the VM's thread (spawning, killing, polling)
// or on scheduler worker threads (reading, writing, waiting); never in Lua.

enum class StdioMode { Pipe, Inherit, Null };

struct SpawnSpec {
    ExecSpec exec; // program, args, cwd, env, baseEnv, shell
    StdioMode modes[3] = {StdioMode::Pipe, StdioMode::Pipe, StdioMode::Pipe};
    bool detached = false;
};

// Our end of a pipe to a child. Shared with the worker jobs that use it, so it
// stays valid while one of them is blocked in a read or write.
struct PipeEnd {
#if defined(_WIN32)
    HANDLE handle = nullptr;
#else
    int fd = -1;
#endif
    std::mutex ioMutex;  // one read or write job at a time (held for the whole job)
    std::mutex mutex;    // guards everything below
    std::string buffer;  // read side: received but not consumed yet
    bool eof = false;
    std::string error;   // read side: why reading stopped early
    bool closed = false; // close() was called
    int busy = 0;        // jobs using the descriptor right now

    PipeEnd() = default;
    PipeEnd(const PipeEnd&) = delete;
    PipeEnd& operator=(const PipeEnd&) = delete;
    ~PipeEnd() { release(); }

    // Closes the descriptor. Only when nobody uses it (busy == 0).
    void release() {
#if defined(_WIN32)
        if (handle != nullptr) {
            CloseHandle(handle);
            handle = nullptr;
        }
#else
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
    }

    // close() from the VM's thread: now if possible, otherwise as soon as the job using it is done.
    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        if (busy == 0) {
            release();
        }
    }
};

using PipePtr = std::shared_ptr<PipeEnd>;

// One read straight from the OS. Bytes read, 0 at EOF, -1 on failure (error set).
long long osRead(PipeEnd& p, char* destination, std::size_t size, std::string& error) {
#if defined(_WIN32)
    DWORD got = 0;
    if (ReadFile(p.handle, destination, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30)), &got, nullptr)) {
        return got;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
        return 0;
    }
    error = errorText(static_cast<int>(code));
    return -1;
#else
    for (;;) {
        const ssize_t got = ::read(p.fd, destination, size);
        if (got >= 0) {
            return got;
        }
        if (errno != EINTR) {
            error = errorText(errno);
            return -1;
        }
    }
#endif
}

// Worker side of a read: reads until "enough" is true of the buffer, or EOF.
void fillUntil(PipeEnd& p, const std::function<bool(const std::string&)>& enough) {
    std::lock_guard<std::mutex> io(p.ioMutex);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.closed || p.eof || enough(p.buffer)) {
            return;
        }
        ++p.busy;
    }

    std::vector<char> chunk(64 * 1024);
    for (;;) {
        std::string error;
        const long long got = osRead(p, chunk.data(), chunk.size(), error);

        std::lock_guard<std::mutex> lock(p.mutex);
        if (got > 0) {
            p.buffer.append(chunk.data(), static_cast<std::size_t>(got));
        } else {
            p.eof = true;
            p.error = error;
        }
        if (p.eof || p.closed || enough(p.buffer)) {
            if (--p.busy == 0 && p.closed) {
                p.release();
            }
            return;
        }
    }
}

// Worker side of a write. Returns an error message, or "" once everything is written.
std::string writeTo(PipeEnd& p, const std::string& data) {
    std::lock_guard<std::mutex> io(p.ioMutex);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.closed) {
            return "the pipe is closed";
        }
        ++p.busy;
    }

    std::string error;
    std::size_t written = 0;
#if defined(_WIN32)
    while (written < data.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - written, 1u << 20));
        if (!WriteFile(p.handle, data.data() + written, want, &chunk, nullptr)) {
            error = errorText(static_cast<int>(GetLastError()));
            break;
        }
        written += chunk;
    }
#else
    {
        SigpipeBlocker blocker; // a child that stopped reading must not kill us
        while (written < data.size()) {
            const ssize_t n = ::write(p.fd, data.data() + written, data.size() - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = errorText(errno);
                break;
            }
            written += static_cast<std::size_t>(n);
        }
    }
#endif

    std::lock_guard<std::mutex> lock(p.mutex);
    if (--p.busy == 0 && p.closed) {
        p.release();
    }
    return error;
}

struct ChildState {
    std::mutex mutex;
#if defined(_WIN32)
    HANDLE process = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
    bool exited = false;
    int code = 0;
    int signal = 0; // POSIX: the signal that killed it, 0 if none

    ChildState() = default;
    ChildState(const ChildState&) = delete;
    ChildState& operator=(const ChildState&) = delete;

    ~ChildState() {
#if defined(_WIN32)
        if (process != nullptr) {
            CloseHandle(process);
        }
#else
        if (!exited && pid > 0) {
            // Nobody waited for it: reap it in the background so it does not stay a zombie.
            const pid_t child = pid;
            std::thread([child] {
                int status = 0;
                while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
            }).detach();
        }
#endif
    }
};

using ChildPtr = std::shared_ptr<ChildState>;

// Has it exited? Checks without waiting. Caller holds child.mutex.
bool pollExit(ChildState& child) {
    if (child.exited) {
        return true;
    }
#if defined(_WIN32)
    if (WaitForSingleObject(child.process, 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(child.process, &code);
    child.code = static_cast<int>(code);
    child.exited = true;
#else
    int status = 0;
    const pid_t result = ::waitpid(child.pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) {
        return false;
    }
    child.exited = true;
    if (result < 0) {
        child.code = -1; // someone else reaped it
    } else if (WIFEXITED(status)) {
        child.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        child.signal = WTERMSIG(status);
        child.code = 128 + child.signal;
    } else {
        child.code = -1;
    }
#endif
    return true;
}

// Worker side of wait(): true once it has exited, false if "timeout" seconds passed first.
bool waitExit(ChildState& child, double timeout) {
#if defined(_WIN32)
    const DWORD ms = timeout >= static_cast<double>(INFINITE) / 1000.0 ? INFINITE : static_cast<DWORD>(std::ceil(timeout * 1000.0));
    WaitForSingleObject(child.process, ms);
    std::lock_guard<std::mutex> lock(child.mutex);
    return pollExit(child);
#else
    // waitpid() has no timeout, and a blocking one would hold the lock: poll with a backoff.
    const auto start = std::chrono::steady_clock::now();
    auto pause = std::chrono::milliseconds(1);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(child.mutex);
            if (pollExit(child)) {
                return true;
            }
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout) {
            return false;
        }
        const auto left = std::chrono::duration<double>(timeout - elapsed);
        std::this_thread::sleep_for(left < pause ? std::chrono::duration_cast<std::chrono::milliseconds>(left) : pause);
        pause = std::min(pause * 2, std::chrono::milliseconds(50));
    }
#endif
}

bool killChild(ChildState& child, int signal, std::string& error) {
    std::lock_guard<std::mutex> lock(child.mutex);
    if (pollExit(child)) {
        error = "the process has already exited";
        return false;
    }
#if defined(_WIN32)
    (void)signal;
    if (!TerminateProcess(child.process, 1)) {
        error = errorText(static_cast<int>(GetLastError()));
        return false;
    }
#else
    if (::kill(child.pid, signal) != 0) {
        error = errorText(errno);
        return false;
    }
#endif
    return true;
}

struct Spawned {
    ChildPtr child;
    PipePtr pipes[3];
    std::string error; // why it could not be started
};

#if defined(_WIN32)

Spawned spawnChild(const SpawnSpec& spec) {
    Spawned out;
    const ExecSpec& exec = spec.exec;
    std::fflush(nullptr);

    std::wstring commandLine;
    if (exec.shell) {
        std::string command = exec.program;
        for (const std::string& arg : exec.args) {
            command += ' ';
            command += arg;
        }
        std::string comspec = "cmd.exe";
        std::string fromEnv;
        if (getEnv("ComSpec", fromEnv) && !fromEnv.empty()) {
            comspec = fromEnv;
        }
        commandLine = quoteArgument(widen(comspec)) + L" /d /s /c \"" + widen(command) + L"\"";
    } else {
        commandLine = quoteArgument(widen(exec.program));
        for (const std::string& arg : exec.args) {
            commandLine += L' ';
            commandLine += quoteArgument(widen(arg));
        }
    }

    std::wstring environmentBlock;
    if (!exec.env.empty()) {
        WideEnvList merged = currentEnvironmentWide();
        for (const auto& [name, value] : exec.env) {
            const std::wstring wideName = widen(name);
            bool replaced = false;
            for (auto& existing : merged) {
                if (sameName(existing.first, wideName)) {
                    existing.second = widen(value);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged.emplace_back(wideName, widen(value));
            }
        }
        std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
            return CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        for (const auto& [name, value] : merged) {
            environmentBlock += name;
            environmentBlock += L'=';
            environmentBlock += value;
            environmentBlock += L'\0';
        }
        environmentBlock += L'\0';
    }

    const std::wstring workingDir = exec.cwd ? widen(*exec.cwd) : std::wstring();

    Handle parentEnds[3], childEnds[3];
    const DWORD stdIds[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE given[3] = {};
    for (int i = 0; i < 3; ++i) {
        switch (spec.modes[i]) {
        case StdioMode::Pipe:
            if (!makePipe(parentEnds[i], childEnds[i], i == 0)) {
                out.error = "failed to create pipes: " + errorText(static_cast<int>(GetLastError()));
                return out;
            }
            given[i] = childEnds[i].get();
            break;
        case StdioMode::Null: {
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;
            childEnds[i].reset(CreateFileW(L"NUL", i == 0 ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           &attributes, OPEN_EXISTING, 0, nullptr));
            if (!childEnds[i].valid()) {
                out.error = "failed to open the null device: " + errorText(static_cast<int>(GetLastError()));
                return out;
            }
            given[i] = childEnds[i].get();
            break;
        }
        case StdioMode::Inherit:
            given[i] = GetStdHandle(stdIds[i]);
            break;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = given[0];
    startup.hStdOutput = given[1];
    startup.hStdError = given[2];

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (spec.detached) {
        flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    }

    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, flags,
                        exec.env.empty() ? nullptr : environmentBlock.data(),
                        workingDir.empty() ? nullptr : workingDir.c_str(), &startup, &info)) {
        out.error = "failed to run '" + exec.program + "': " + errorText(static_cast<int>(GetLastError()));
        return out;
    }
    CloseHandle(info.hThread);

    out.child = std::make_shared<ChildState>();
    out.child->process = info.hProcess;
    out.child->pid = info.dwProcessId;
    for (int i = 0; i < 3; ++i) {
        if (parentEnds[i].valid()) {
            out.pipes[i] = std::make_shared<PipeEnd>();
            out.pipes[i]->handle = parentEnds[i].release();
        }
    }
    return out;
}

#else // POSIX

Spawned spawnChild(const SpawnSpec& spec) {
    Spawned out;
    const ExecSpec& exec = spec.exec;
    std::fflush(nullptr);

    // Everything the child needs is prepared here: after fork() it may not allocate.
    std::vector<std::string> words;
    if (exec.shell) {
        std::string command = exec.program;
        for (const std::string& arg : exec.args) {
            command += ' ';
            command += arg;
        }
        words = {"/bin/sh", "-c", std::move(command)};
    } else {
        words.push_back(exec.program);
        words.insert(words.end(), exec.args.begin(), exec.args.end());
    }
    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> environment;
    std::vector<char*> envp;
    const bool customEnv = !exec.env.empty();
    if (customEnv) {
        EnvList merged = exec.baseEnv;
        for (const auto& [name, value] : exec.env) {
            const auto it = std::find_if(merged.begin(), merged.end(), [&](const auto& e) { return e.first == name; });
            if (it != merged.end()) {
                it->second = value;
            } else {
                merged.emplace_back(name, value);
            }
        }
        for (const auto& [name, value] : merged) {
            environment.push_back(name + "=" + value);
        }
        for (std::string& entry : environment) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
    }
    const char* cwd = exec.cwd ? exec.cwd->c_str() : nullptr;

    Fd parentEnds[3], childEnds[3], failRead, failWrite;
    for (int i = 0; i < 3; ++i) {
        if (spec.modes[i] == StdioMode::Pipe) {
            Fd readEnd, writeEnd;
            if (!makePipe(readEnd, writeEnd)) {
                out.error = "failed to create pipes: " + errorText(errno);
                return out;
            }
            childEnds[i].reset(i == 0 ? readEnd.release() : writeEnd.release());
            parentEnds[i].reset(i == 0 ? writeEnd.release() : readEnd.release());
        } else if (spec.modes[i] == StdioMode::Null) {
            childEnds[i].reset(::open("/dev/null", i == 0 ? O_RDONLY : O_WRONLY));
            if (!secureFd(childEnds[i])) {
                out.error = "failed to open /dev/null: " + errorText(errno);
                return out;
            }
        }
    }
    if (!makePipe(failRead, failWrite)) {
        out.error = "failed to create pipes: " + errorText(errno);
        return out;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        out.error = "failed to fork: " + errorText(errno);
        return out;
    }

    if (pid == 0) {
        // Child. Only async-signal-safe calls from here until exec; it never returns.
        if (spec.detached) {
            ::setsid();
        }
        for (int i = 0; i < 3; ++i) {
            if (childEnds[i].valid() && ::dup2(childEnds[i].get(), i) < 0) {
                childFail(failWrite.get(), 0, errno);
            }
        }
        if (cwd && ::chdir(cwd) != 0) {
            childFail(failWrite.get(), 1, errno);
        }
        if (customEnv) {
            setEnvironBlock(envp.data());
        }
        ::execvp(argv[0], argv.data());
        childFail(failWrite.get(), 2, errno);
    }

    for (Fd& end : childEnds) {
        end.reset();
    }
    failWrite.reset();

    ChildFailure failure{0, 0};
    ssize_t got;
    do {
        got = ::read(failRead.get(), &failure, sizeof(failure));
    } while (got < 0 && errno == EINTR);
    failRead.reset();

    if (got == static_cast<ssize_t>(sizeof(failure))) {
        reap(pid);
        if (failure.stage == 1) {
            out.error = "failed to change directory to '" + *exec.cwd + "': " + errorText(failure.error);
        } else if (failure.stage == 2) {
            out.error = "failed to run '" + exec.program + "': " + errorText(failure.error);
        } else {
            out.error = "failed to set up the child's stdio: " + errorText(failure.error);
        }
        return out;
    }

    out.child = std::make_shared<ChildState>();
    out.child->pid = pid;
    for (int i = 0; i < 3; ++i) {
        if (parentEnds[i].valid()) {
            out.pipes[i] = std::make_shared<PipeEnd>();
            out.pipes[i]->fd = parentEnds[i].release();
        }
    }
    return out;
}

#endif // _WIN32

// the running process ////////

std::optional<double> parentProcessId() {
#if defined(_WIN32)
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::optional<double> parent;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self) {
            parent = static_cast<double>(entry.th32ParentProcessID);
            break;
        }
    }
    CloseHandle(snapshot);
    return parent;
#else
    return static_cast<double>(::getppid());
#endif
}

// Resident set size in bytes.
std::optional<double> residentMemory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return std::nullopt;
    }
    return static_cast<double>(counters.WorkingSetSize);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return std::nullopt;
    }
    return static_cast<double>(info.resident_size);
#elif defined(__linux__)
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (!file) {
        return std::nullopt;
    }
    unsigned long long size = 0;
    unsigned long long resident = 0;
    const int read = std::fscanf(file, "%llu %llu", &size, &resident);
    std::fclose(file);
    if (read != 2) {
        return std::nullopt;
    }
    return static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE));
#else
    return std::nullopt;
#endif
}

bool killProcess(double pid, int signal, std::string& error) {
#if defined(_WIN32)
    (void)signal;
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        error = errorText(static_cast<int>(GetLastError()));
        return false;
    }
    const BOOL done = TerminateProcess(process, 1);
    if (!done) {
        error = errorText(static_cast<int>(GetLastError()));
    }
    CloseHandle(process);
    return done != 0;
#else
    if (::kill(static_cast<pid_t>(pid), signal) != 0) {
        error = errorText(errno);
        return false;
    }
    return true;
#endif
}

} // namespace sys

// bindings ////////////
//
// Same discipline as example.cpp: validate every argument first (these checks hold
// no C++ objects), then do the real work, and raise failures from the platform layer
// only once the objects that carried them are gone.

bool hasNoNul(const char* text, std::size_t length) {
    return std::strlen(text) == length;
}

// process.cwd(): string
int processCwd(lua_State* L) {
    {
        std::string path;
        std::string error;
        if (sys::currentDir(path, error)) {
            lua_pushlstring(L, path.data(), path.size());
            return 1;
        }
        lua_pushlstring(L, error.data(), error.size());
    }
    luaL_error(L, "cannot determine the working directory: %s", lua_tostring(L, -1));
}

// process.chdir(path: string)
int processChdir(lua_State* L) {
    std::size_t length = 0;
    const char* path = luaL_checklstring(L, 1, &length);
    luaL_argcheck(L, length != 0 && hasNoNul(path, length), 1, "path must be a non-empty string without null bytes");

    {
        std::string error;
        if (sys::changeDir(path, error)) {
            return 0;
        }
        lua_pushlstring(L, error.data(), error.size());
    }
    luaL_error(L, "cannot change directory to '%s': %s", path, lua_tostring(L, -1));
}

// process.exit(code: number?)
int processExit(lua_State* L) {
    const int code = luaL_optinteger(L, 1, 0);
    std::exit(code);
}

// ---- process.env ----

// process.env.NAME: string?
int envIndex(lua_State* L) {
    std::size_t length = 0;
    const char* name = nullptr;

    if (lua_type(L, 2) == LUA_TSTRING) {
        name = lua_tolstring(L, 2, &length);
    }

    if (name && length != 0 && hasNoNul(name, length)) {
        std::string value;
        if (sys::getEnv(name, value)) {
            lua_pushlstring(L, value.data(), value.size());
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

// process.env.NAME = value: string?
int envNewIndex(lua_State* L) {
    luaL_argcheck(L, lua_type(L, 2) == LUA_TSTRING, 2, "environment variable names must be strings");

    std::size_t nameLength = 0;
    const char* name = lua_tolstring(L, 2, &nameLength);
    luaL_argcheck(L, nameLength != 0 && hasNoNul(name, nameLength) && !std::strchr(name, '='), 2,
        "environment variable names must be non-empty and contain no '=' or null bytes");

    const char* value = nullptr;
    if (!lua_isnil(L, 3)) {
        luaL_argcheck(L, lua_type(L, 3) == LUA_TSTRING, 3, "environment variable values must be strings or nil");

        std::size_t valueLength = 0;
        value = lua_tolstring(L, 3, &valueLength);
        luaL_argcheck(L, hasNoNul(value, valueLength), 3, "environment variable values must not contain null bytes");
    }

    {
        std::string error;
        if (sys::setEnv(name, value, error)) {
            return 0;
        }
        lua_pushlstring(L, error.data(), error.size());
    }
    luaL_error(L, "cannot set environment variable '%s': %s", name, lua_tostring(L, -1));
}

// The iterator behind "for name, value in process.env": walks a snapshot taken
// when the loop started, kept in upvalue 1.
int envIterNext(lua_State* L) {
    lua_settop(L, 2);                          // (state, control); the control is the previous name
    lua_pushvalue(L, lua_upvalueindex(1));     // snapshot
    lua_pushvalue(L, 2);
    if (lua_next(L, -2) != 0) {
        return 2;                              // name, value
    }
    return 0;                                  // finished
}

// __iter: returns the iterator function (state and control start as nil).
int envIter(lua_State* L) {
    lua_createtable(L, 0, 32);
    {
        sys::EnvList vars;
        sys::listEnv(vars);

        for (const auto& [name, value] : vars) {
            lua_pushlstring(L, value.data(), value.size());
            lua_setfield(L, -2, name.c_str());
        }
    }

    lua_pushcclosure(L, envIterNext, "next", 1); // takes the snapshot as its upvalue
    return 1;
}

// A table with no entries of its own whose accesses all go to the real environment.
void pushEnvironmentProxy(lua_State* L) {
    lua_createtable(L, 0, 0);

    lua_createtable(L, 0, 4);
    lua_pushcfunction(L, envIndex, "env.__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, envNewIndex, "env.__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, envIter, "env.__iter");
    lua_setfield(L, -2, "__iter");
    lua_pushstring(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");

    lua_setmetatable(L, -2);
}

// ---- process.exec ----

// Raises an argument error unless options[field] is nil or a string. Holds no C++ objects.
void checkOptionalString(lua_State* L, int options, const char* field) {
    lua_getfield(L, options, field);
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING) {
        luaL_error(L, "invalid option '%s' (string expected, got %s)", field, luaL_typename(L, -1));
    }
    lua_pop(L, 1);
}

void checkExecArguments(lua_State* L) {
    std::size_t length = 0;
    const char* program = luaL_checklstring(L, 1, &length);
    luaL_argcheck(L, length != 0, 1, "program must not be empty");
    luaL_argcheck(L, hasNoNul(program, length), 1, "program must not contain null bytes");

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);

        const int count = lua_objlen(L, 2);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING, 2, "args must be an array of strings");

            const char* arg = lua_tolstring(L, -1, &length);
            luaL_argcheck(L, hasNoNul(arg, length), 2, "args must not contain null bytes");
            lua_pop(L, 1);
        }
    }

    if (lua_isnoneornil(L, 3)) {
        return;
    }
    luaL_checktype(L, 3, LUA_TTABLE);

    checkOptionalString(L, 3, "cwd");
    checkOptionalString(L, 3, "stdin");

    lua_getfield(L, 3, "cwd");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* cwd = lua_tolstring(L, -1, &length);
        luaL_argcheck(L, length != 0 && hasNoNul(cwd, length), 3, "option 'cwd' must be a non-empty string without null bytes");
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "shell");
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TBOOLEAN) {
        luaL_error(L, "invalid option 'shell' (boolean expected, got %s)", luaL_typename(L, -1));
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "stdio");
    if (!lua_isnil(L, -1)) {
        const char* mode = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        luaL_argcheck(L, mode && (std::strcmp(mode, "pipe") == 0 || std::strcmp(mode, "inherit") == 0), 3,
            "option 'stdio' must be \"pipe\" or \"inherit\"");
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "env");
    if (!lua_isnil(L, -1)) {
        luaL_argcheck(L, lua_istable(L, -1), 3, "option 'env' must be a table of strings");

        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            luaL_argcheck(L, lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING, 3,
                "option 'env' must map strings to strings");

            std::size_t nameLength = 0;
            std::size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            luaL_argcheck(L, nameLength != 0 && hasNoNul(name, nameLength) && !std::strchr(name, '='), 3,
                "option 'env' has an invalid variable name");
            luaL_argcheck(L, hasNoNul(value, valueLength), 3, "option 'env' values must not contain null bytes");

            lua_pop(L, 1); // keep the key for lua_next
        }
    }
    lua_pop(L, 1);
}

// Copies the (already validated) arguments into C++ objects. Raises nothing.
sys::ExecSpec readExecSpec(lua_State* L) {
    sys::ExecSpec spec;
    std::size_t length = 0;

    const char* program = lua_tolstring(L, 1, &length);
    spec.program.assign(program, length);

    if (lua_istable(L, 2)) {
        const int count = lua_objlen(L, 2);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            const char* arg = lua_tolstring(L, -1, &length);
            spec.args.emplace_back(arg, length);
            lua_pop(L, 1);
        }
    }

    if (!lua_istable(L, 3)) {
        return spec;
    }

    lua_getfield(L, 3, "cwd");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* cwd = lua_tolstring(L, -1, &length);
        spec.cwd = std::string(cwd, length);
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "stdin");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* data = lua_tolstring(L, -1, &length);
        spec.stdinData.assign(data, length);
        spec.hasStdin = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "shell");
    spec.shell = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 3, "stdio");
    if (lua_type(L, -1) == LUA_TSTRING) {
        spec.inheritStdio = std::strcmp(lua_tostring(L, -1), "inherit") == 0;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "env");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            std::size_t nameLength = 0;
            std::size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            spec.env.emplace_back(std::string(name, nameLength), std::string(value, valueLength));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

#if !defined(_WIN32)
    if (!spec.env.empty()) {
        sys::listEnv(spec.baseEnv); // here, on the VM's thread: the child is started on a worker
    }
#endif
    return spec;
}

// process.exec(program: string, args: { string }?, options: ExecOptions?): ExecResult
int processExec(lua_State* L) {
    checkExecArguments(L);

    auto spec = std::make_shared<sys::ExecSpec>(readExecSpec(L));
    return sched::Scheduler::of(L).await(L, [spec]() -> sched::Completion {
        auto outcome = std::make_shared<sys::ExecOutcome>(sys::runExec(*spec));
        return [outcome](lua_State* T) {
            if (!outcome->launched) {
                lua_pushlstring(T, outcome->error.data(), outcome->error.size());
                return sched::kRaise;
            }
            lua_createtable(T, 0, 4);
            lua_pushboolean(T, outcome->code == 0);
            lua_setfield(T, -2, "ok");
            lua_pushnumber(T, outcome->code);
            lua_setfield(T, -2, "code");
            lua_pushlstring(T, outcome->out.data(), outcome->out.size());
            lua_setfield(T, -2, "stdout");
            lua_pushlstring(T, outcome->err.data(), outcome->err.size());
            lua_setfield(T, -2, "stderr");
            return 1;
        };
    });
}

// ---- signals ----

struct SignalName {
    const char* name;  // what scripts use
    const char* alias; // the traditional name without "SIG"
    int number;
    bool catchable;
};

#if defined(_WIN32)
// Pseudo numbers for the console events (matching the POSIX signals they resemble).
constexpr int kSigInterrupt = 2;
constexpr int kSigBreak = 21;
constexpr int kSigHangup = 1;

constexpr SignalName kSignals[] = {
    {"interrupt", "INT", kSigInterrupt, true},
    {"break", "BREAK", kSigBreak, true},
    {"hangup", "HUP", kSigHangup, true},
    {"terminate", "TERM", 15, false},
    {"kill", "KILL", 9, false},
};
#else
constexpr SignalName kSignals[] = {
    {"interrupt", "INT", SIGINT, true},   {"terminate", "TERM", SIGTERM, true}, {"hangup", "HUP", SIGHUP, true},
    {"quit", "QUIT", SIGQUIT, true},      {"user1", "USR1", SIGUSR1, true},     {"user2", "USR2", SIGUSR2, true},
    {"resize", "WINCH", SIGWINCH, true},  {"alarm", "ALRM", SIGALRM, true},     {"continue", "CONT", SIGCONT, true},
    {"kill", "KILL", SIGKILL, false},     {"stop", "STOP", SIGSTOP, false},
};
#endif

const SignalName* findSignal(int number) {
    for (const SignalName& signal : kSignals) {
        if (signal.number == number) {
            return &signal;
        }
    }
    return nullptr;
}

void pushSignalName(lua_State* L, int number) {
    if (const SignalName* signal = findSignal(number)) {
        lua_pushstring(L, signal->name);
    } else {
        lua_pushfstring(L, "SIG%d", number);
    }
}

bool sameText(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
    }
    return *a == *b;
}

// A signal argument: a name ("interrupt", "SIGINT", "INT") or a number. Raises if unknown.
int checkSignal(lua_State* L, int index, const char* fallback) {
    if (lua_type(L, index) == LUA_TNUMBER) {
        const int number = luaL_checkinteger(L, index);
        luaL_argcheck(L, number > 0 && number < 128, index, "signal number out of range");
        return number;
    }
    const char* text = luaL_optstring(L, index, fallback);
    const char* bare = (std::strlen(text) > 3 && sameText(std::string(text, 3).c_str(), "sig")) ? text + 3 : text;
    for (const SignalName& signal : kSignals) {
        if (sameText(text, signal.name) || sameText(bare, signal.alias)) {
            return signal.number;
        }
    }
    luaL_argerror(L, index, lua_pushfstring(L, "unknown signal '%s'", text));
}

constexpr const char* kSignalHandlersKey = "sonata.process.signals"; // { [number] = handler }

// Runs on a VM's thread (posted by the hub): starts that VM's handler as a task.
void runSignalHandler(lua_State* L, int number) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSignalHandlersKey);
    if (!lua_istable(L, -1)) {
        return;
    }
    lua_rawgeti(L, -1, number);
    if (!lua_isfunction(L, -1)) {
        return;
    }
    pushSignalName(L, number);
    sched::Scheduler::of(L).spawn(L, lua_gettop(L) - 1);
}

// Process-wide: OS signal handlers can only be installed once per process, so
// every VM that wants a signal subscribes here and gets it posted to its scheduler.
class SignalHub {
  public:
    static SignalHub& instance() {
        static SignalHub* hub = new SignalHub(); // never destroyed: the OS handler may still fire at exit
        return *hub;
    }

    void subscribe(int number, const std::shared_ptr<sched::Remote>& remote) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& list = subscribers_[number];
        prune(list);
        for (const auto& weak : list) {
            if (weak.lock() == remote) {
                return;
            }
        }
        list.push_back(remote);
        if (list.size() == 1) {
            install(number, true);
        }
    }

    void unsubscribe(int number, const std::shared_ptr<sched::Remote>& remote) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& list = subscribers_[number];
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const auto& weak) { return weak.expired() || weak.lock() == remote; }),
                   list.end());
        if (list.empty()) {
            install(number, false);
        }
    }

    // Called (on some thread) when signal "number" arrives. False if nobody listens.
    bool deliver(int number) {
        std::vector<std::shared_ptr<sched::Remote>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& list = subscribers_[number];
            prune(list);
            for (const auto& weak : list) {
                if (auto remote = weak.lock()) {
                    targets.push_back(std::move(remote));
                }
            }
        }
        bool delivered = false;
        for (const auto& remote : targets) {
            delivered = remote->post([number](lua_State* L) { runSignalHandler(L, number); }) || delivered;
        }
        return delivered;
    }

  private:
    static void prune(std::vector<std::weak_ptr<sched::Remote>>& list) {
        list.erase(std::remove_if(list.begin(), list.end(), [](const auto& weak) { return weak.expired(); }),
                   list.end());
    }

#if defined(_WIN32)
    static BOOL WINAPI consoleHandler(DWORD event) {
        int number = 0;
        switch (event) {
        case CTRL_C_EVENT:
            number = kSigInterrupt;
            break;
        case CTRL_BREAK_EVENT:
            number = kSigBreak;
            break;
        case CTRL_CLOSE_EVENT:
            number = kSigHangup;
            break;
        default:
            return FALSE;
        }
        return instance().deliver(number) ? TRUE : FALSE;
    }

    void install(int, bool) {
        if (!consoleInstalled_) {
            consoleInstalled_ = SetConsoleCtrlHandler(consoleHandler, TRUE) != 0;
        }
    }

    bool consoleInstalled_ = false;
#else
    // The OS handler only writes the signal number to a pipe (that is all a signal
    // handler may safely do); a thread of ours reads it and does the rest.
    static int& pipeWriteEnd() {
        static int fd = -1;
        return fd;
    }

    static void onSignal(int number) {
        const int saved = errno;
        const unsigned char byte = static_cast<unsigned char>(number);
        const ssize_t ignored = ::write(pipeWriteEnd(), &byte, 1);
        (void)ignored;
        errno = saved;
    }

    void install(int number, bool on) {
        if (on && pipeWriteEnd() < 0) {
            int fds[2];
            if (::pipe(fds) != 0) {
                return;
            }
            ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
            ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK);
            pipeWriteEnd() = fds[1];
            const int readEnd = fds[0];
            std::thread([readEnd] {
                for (;;) {
                    unsigned char byte = 0;
                    const ssize_t got = ::read(readEnd, &byte, 1);
                    if (got == 1) {
                        instance().deliver(byte);
                    } else if (got < 0 && errno == EINTR) {
                        continue;
                    } else {
                        return;
                    }
                }
            }).detach();
        }

        struct sigaction action{};
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        action.sa_handler = on ? onSignal : SIG_DFL;
        ::sigaction(number, &action, nullptr);
    }
#endif

    std::mutex mutex_;
    std::map<int, std::vector<std::weak_ptr<sched::Remote>>> subscribers_;
};

// process.onSignal(signal: string | number, handler: ((name: string) -> ())?)
int processOnSignal(lua_State* L) {
    const int number = checkSignal(L, 1, nullptr);
    const SignalName* known = findSignal(number);
    luaL_argcheck(L, known == nullptr || known->catchable, 1, "this signal cannot be handled");
#if defined(_WIN32)
    luaL_argcheck(L, known != nullptr && known->catchable, 1,
                  "only \"interrupt\", \"break\" and \"hangup\" can be handled on Windows");
#endif
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
    }

    lua_getfield(L, LUA_REGISTRYINDEX, kSignalHandlersKey);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, number);
    lua_pop(L, 1);

    const std::shared_ptr<sched::Remote> remote = sched::Scheduler::of(L).remote();
    if (lua_isnoneornil(L, 2)) {
        SignalHub::instance().unsubscribe(number, remote);
    } else {
        SignalHub::instance().subscribe(number, remote);
    }
    return 0;
}

// process.kill(pid: number, signal: (string | number)?): true | (nil, string)
int processKill(lua_State* L) {
    const double pid = luaL_checknumber(L, 1);
    luaL_argcheck(L, pid > 0 && pid == std::floor(pid), 1, "pid must be a positive whole number");
    const int number = checkSignal(L, 2, "terminate");

    {
        std::string error;
        if (sys::killProcess(pid, number, error)) {
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushnil(L);
        lua_pushlstring(L, error.data(), error.size());
    }
    return 2;
}

// ---- the running process ----

const std::chrono::steady_clock::time_point kStartTime = std::chrono::steady_clock::now();

// process.uptime(): number
int processUptime(lua_State* L) {
    lua_pushnumber(L, std::chrono::duration<double>(std::chrono::steady_clock::now() - kStartTime).count());
    return 1;
}

// process.memoryUsage(): { rss: number?, luauHeap: number }
int processMemoryUsage(lua_State* L) {
    lua_createtable(L, 0, 2);
    if (const std::optional<double> rss = sys::residentMemory()) {
        lua_pushnumber(L, *rss);
        lua_setfield(L, -2, "rss");
    }
    lua_pushnumber(L, static_cast<double>(lua_gc(L, LUA_GCCOUNT, 0)) * 1024.0 + lua_gc(L, LUA_GCCOUNTB, 0));
    lua_setfield(L, -2, "luauHeap");
    return 1;
}

// ---- process.spawn ----

constexpr const char* kChildMeta = "sonata.process.Child";
constexpr const char* kChildHandlesKey = "sonata.process.children"; // child table -> ChildHandle (weak keys)
constexpr const char* kPipeMeta = "sonata.process.Pipe";

struct ChildHandle {
    sys::ChildPtr child;
};

struct PipeHandle {
    sys::PipePtr pipe;
    bool readable = false;
    const char* name = "";
};

const char* const kStdioModes[] = {"pipe", "inherit", "null", nullptr};
const char* const kStreamFields[] = {"stdin", "stdout", "stderr"};

// Validates program, args and the options process.spawn understands.
void checkSpawnArguments(lua_State* L) {
    std::size_t length = 0;
    const char* program = luaL_checklstring(L, 1, &length);
    luaL_argcheck(L, length != 0 && hasNoNul(program, length), 1, "program must be a non-empty string without null bytes");

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        const int count = lua_objlen(L, 2);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING, 2, "args must be an array of strings");
            const char* arg = lua_tolstring(L, -1, &length);
            luaL_argcheck(L, hasNoNul(arg, length), 2, "args must not contain null bytes");
            lua_pop(L, 1);
        }
    }

    if (lua_isnoneornil(L, 3)) {
        return;
    }
    luaL_checktype(L, 3, LUA_TTABLE);

    lua_getfield(L, 3, "cwd");
    if (!lua_isnil(L, -1)) {
        const char* cwd = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &length) : nullptr;
        luaL_argcheck(L, cwd && length != 0 && hasNoNul(cwd, length), 3, "option 'cwd' must be a non-empty string without null bytes");
    }
    lua_pop(L, 1);

    for (const char* flag : {"shell", "detached"}) {
        lua_getfield(L, 3, flag);
        if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TBOOLEAN) {
            luaL_error(L, "invalid option '%s' (boolean expected, got %s)", flag, luaL_typename(L, -1));
        }
        lua_pop(L, 1);
    }

    for (const char* field : {"stdio", "stdin", "stdout", "stderr"}) {
        lua_getfield(L, 3, field);
        if (!lua_isnil(L, -1)) {
            bool valid = false;
            if (lua_type(L, -1) == LUA_TSTRING) {
                for (const char* const* mode = kStdioModes; *mode; ++mode) {
                    valid = valid || std::strcmp(lua_tostring(L, -1), *mode) == 0;
                }
            }
            if (!valid) {
                luaL_error(L, "invalid option '%s' (expected \"pipe\", \"inherit\" or \"null\")", field);
            }
        }
        lua_pop(L, 1);
    }

    lua_getfield(L, 3, "env");
    if (!lua_isnil(L, -1)) {
        luaL_argcheck(L, lua_istable(L, -1), 3, "option 'env' must be a table of strings");
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            luaL_argcheck(L, lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING, 3,
                          "option 'env' must map strings to strings");
            std::size_t nameLength = 0;
            std::size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            luaL_argcheck(L, nameLength != 0 && hasNoNul(name, nameLength) && !std::strchr(name, '='), 3,
                          "option 'env' has an invalid variable name");
            luaL_argcheck(L, hasNoNul(value, valueLength), 3, "option 'env' values must not contain null bytes");
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

sys::StdioMode modeOf(const char* text) {
    if (std::strcmp(text, "inherit") == 0) {
        return sys::StdioMode::Inherit;
    }
    return std::strcmp(text, "null") == 0 ? sys::StdioMode::Null : sys::StdioMode::Pipe;
}

// Copies the (validated) arguments. Raises nothing.
sys::SpawnSpec readSpawnSpec(lua_State* L) {
    sys::SpawnSpec spec;
    std::size_t length = 0;
    const char* program = lua_tolstring(L, 1, &length);
    spec.exec.program.assign(program, length);

    if (lua_istable(L, 2)) {
        const int count = lua_objlen(L, 2);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            const char* arg = lua_tolstring(L, -1, &length);
            spec.exec.args.emplace_back(arg, length);
            lua_pop(L, 1);
        }
    }
    if (!lua_istable(L, 3)) {
        return spec;
    }

    lua_getfield(L, 3, "cwd");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* cwd = lua_tolstring(L, -1, &length);
        spec.exec.cwd = std::string(cwd, length);
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "shell");
    spec.exec.shell = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 3, "detached");
    spec.detached = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 3, "stdio");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const sys::StdioMode all = modeOf(lua_tostring(L, -1));
        spec.modes[0] = spec.modes[1] = spec.modes[2] = all;
    }
    lua_pop(L, 1);
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, 3, kStreamFields[i]);
        if (lua_type(L, -1) == LUA_TSTRING) {
            spec.modes[i] = modeOf(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }

    lua_getfield(L, 3, "env");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            std::size_t nameLength = 0;
            std::size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            spec.exec.env.emplace_back(std::string(name, nameLength), std::string(value, valueLength));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

#if !defined(_WIN32)
    if (!spec.exec.env.empty()) {
        sys::listEnv(spec.exec.baseEnv);
    }
#endif
    return spec;
}

template <typename T>
T* newHandle(lua_State* L, const char* meta) {
    void* memory = lua_newuserdatadtor(L, sizeof(T), [](lua_State*, void* p) { static_cast<T*>(p)->~T(); });
    T* handle = new (memory) T();
    luaL_getmetatable(L, meta);
    lua_setmetatable(L, -2);
    return handle;
}

void pushPipe(lua_State* L, const sys::PipePtr& pipe, bool readable, const char* name) {
    PipeHandle* handle = newHandle<PipeHandle>(L, kPipeMeta);
    handle->pipe = pipe;
    handle->readable = readable;
    handle->name = name;
}

// process.spawn(program: string, args: { string }?, options: SpawnOptions?): Child
int processSpawn(lua_State* L) {
    checkSpawnArguments(L);
    lua_settop(L, 3);

    {
        sys::Spawned spawned = sys::spawnChild(readSpawnSpec(L));
        if (spawned.error.empty()) {
            lua_createtable(L, 0, 4); // 4: the child
#if defined(_WIN32)
            lua_pushnumber(L, static_cast<double>(spawned.child->pid));
#else
            lua_pushnumber(L, static_cast<double>(spawned.child->pid));
#endif
            lua_setfield(L, 4, "pid");
            for (int i = 0; i < 3; ++i) {
                if (spawned.pipes[i]) {
                    pushPipe(L, spawned.pipes[i], i != 0, kStreamFields[i]);
                    lua_setfield(L, 4, kStreamFields[i]);
                }
            }
            luaL_getmetatable(L, kChildMeta);
            lua_setmetatable(L, 4);

            lua_getfield(L, LUA_REGISTRYINDEX, kChildHandlesKey);
            lua_pushvalue(L, 4);
            newHandle<ChildHandle>(L, kChildMeta)->child = std::move(spawned.child);
            lua_rawset(L, -3);
            lua_pop(L, 1);
            return 1;
        }
        lua_pushlstring(L, spawned.error.data(), spawned.error.size());
    }
    luaL_error(L, "%s", lua_tostring(L, -1));
}

sys::ChildPtr checkChild(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, LUA_REGISTRYINDEX, kChildHandlesKey);
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    auto* handle = static_cast<ChildHandle*>(lua_touserdata(L, -1));
    lua_pop(L, 2);
    luaL_argcheck(L, handle != nullptr, 1, "expected a child process");
    return handle->child;
}

// Pushes { ok, code, signal? } for a child that has exited.
void pushExit(lua_State* L, sys::ChildState& child) {
    std::lock_guard<std::mutex> lock(child.mutex);
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, child.code == 0 && child.signal == 0);
    lua_setfield(L, -2, "ok");
    lua_pushnumber(L, child.code);
    lua_setfield(L, -2, "code");
    if (child.signal != 0) {
        pushSignalName(L, child.signal);
        lua_setfield(L, -2, "signal");
    }
}

// child:wait(timeout: number?): ExitStatus | (nil, string)
int childWait(lua_State* L) {
    const sys::ChildPtr child = checkChild(L);
    double timeout = std::numeric_limits<double>::infinity();
    if (!lua_isnoneornil(L, 2)) {
        timeout = luaL_checknumber(L, 2);
        luaL_argcheck(L, timeout >= 0, 2, "timeout must not be negative");
    }

    return sched::Scheduler::of(L).await(L, [child, timeout]() -> sched::Completion {
        const bool exited = sys::waitExit(*child, timeout);
        return [child, exited](lua_State* T) {
            if (!exited) {
                lua_pushnil(T);
                lua_pushstring(T, "timed out");
                return 2;
            }
            pushExit(T, *child);
            return 1;
        };
    });
}

// child:status(): ExitStatus?
int childStatus(lua_State* L) {
    const sys::ChildPtr child = checkChild(L);
    bool exited = false;
    {
        std::lock_guard<std::mutex> lock(child->mutex);
        exited = sys::pollExit(*child);
    }
    if (exited) {
        pushExit(L, *child);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// child:kill(signal: (string | number)?): boolean
int childKill(lua_State* L) {
    const sys::ChildPtr child = checkChild(L);
    const int number = checkSignal(L, 2, "terminate");
    std::string error;
    lua_pushboolean(L, sys::killChild(*child, number, error));
    return 1;
}

PipeHandle* checkPipe(lua_State* L, int readable) {
    auto* handle = static_cast<PipeHandle*>(luaL_checkudata(L, 1, kPipeMeta));
    if (readable == 1) {
        luaL_argcheck(L, handle->readable, 1, "stdin can only be written to");
    } else if (readable == 0) {
        luaL_argcheck(L, !handle->readable, 1, "stdout and stderr can only be read from");
    }
    return handle;
}

enum class ReadKind { Some, Line, All };

// Takes what "kind" asks for from the pipe's buffer and pushes the results.
// Returns -1 (pushing nothing) if the buffer cannot answer yet.
int consume(lua_State* L, sys::PipeEnd& pipe, ReadKind kind, std::size_t max, bool keepEnding) {
    std::lock_guard<std::mutex> lock(pipe.mutex);
    std::string& buffer = pipe.buffer;
    const bool ended = pipe.eof || pipe.closed;

    if (kind == ReadKind::Some && !buffer.empty()) {
        const std::size_t take = std::min(max, buffer.size());
        lua_pushlstring(L, buffer.data(), take);
        buffer.erase(0, take);
        return 1;
    }
    if (kind == ReadKind::Line) {
        const std::size_t newline = buffer.find('\n');
        if (newline != std::string::npos || (ended && !buffer.empty())) {
            std::size_t length = newline == std::string::npos ? buffer.size() : newline + 1;
            std::size_t keep = length;
            if (!keepEnding && newline != std::string::npos) {
                keep = newline > 0 && buffer[newline - 1] == '\r' ? newline - 1 : newline;
            }
            lua_pushlstring(L, buffer.data(), keep);
            buffer.erase(0, length);
            return 1;
        }
    }
    if (kind == ReadKind::All && ended) {
        if (buffer.empty() && !pipe.error.empty()) {
            lua_pushnil(L);
            lua_pushlstring(L, pipe.error.data(), pipe.error.size());
            return 2;
        }
        lua_pushlstring(L, buffer.data(), buffer.size());
        buffer.clear();
        return 1;
    }
    if (!ended) {
        return -1;
    }
    lua_pushnil(L);
    if (!pipe.error.empty()) {
        lua_pushlstring(L, pipe.error.data(), pipe.error.size());
        return 2;
    }
    return 1;
}

int readPipe(lua_State* L, ReadKind kind, std::size_t max, bool keepEnding) {
    PipeHandle* handle = checkPipe(L, 1);
    const sys::PipePtr pipe = handle->pipe;

    const int ready = consume(L, *pipe, kind, max, keepEnding);
    if (ready >= 0) {
        return ready;
    }

    return sched::Scheduler::of(L).await(L, [pipe, kind, max, keepEnding]() -> sched::Completion {
        sys::fillUntil(*pipe, [kind](const std::string& buffer) {
            if (kind == ReadKind::Some) {
                return !buffer.empty();
            }
            return kind == ReadKind::Line && buffer.find('\n') != std::string::npos;
        });
        return [pipe, kind, max, keepEnding](lua_State* T) {
            const int n = consume(T, *pipe, kind, max, keepEnding);
            if (n >= 0) {
                return n;
            }
            lua_pushnil(T); // closed while we waited
            return 1;
        };
    });
}

// pipe:read(max: number?): string?
int pipeRead(lua_State* L) {
    const int max = luaL_optinteger(L, 2, 64 * 1024);
    luaL_argcheck(L, max >= 1, 2, "max must be at least 1");
    return readPipe(L, ReadKind::Some, static_cast<std::size_t>(max), false);
}

// pipe:readLine(keepEnding: boolean?): string?
int pipeReadLine(lua_State* L) {
    return readPipe(L, ReadKind::Line, 0, lua_toboolean(L, 2) != 0);
}

// pipe:readAll(): string
int pipeReadAll(lua_State* L) {
    return readPipe(L, ReadKind::All, 0, false);
}

// pipe:write(data: string): true | (nil, string)
int pipeWrite(lua_State* L) {
    PipeHandle* handle = checkPipe(L, 0);
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 2, &length);

    const sys::PipePtr pipe = handle->pipe;
    auto text = std::make_shared<std::string>(data, length);
    return sched::Scheduler::of(L).await(L, [pipe, text]() -> sched::Completion {
        auto error = std::make_shared<std::string>(sys::writeTo(*pipe, *text));
        return [error](lua_State* T) {
            if (error->empty()) {
                lua_pushboolean(T, 1);
                return 1;
            }
            lua_pushnil(T);
            lua_pushlstring(T, error->data(), error->size());
            return 2;
        };
    });
}

// pipe:close()
int pipeClose(lua_State* L) {
    checkPipe(L, -1)->pipe->close();
    return 0;
}

// tostring(pipe): "process.Pipe(stdout)"
int pipeToString(lua_State* L) {
    lua_pushfstring(L, "process.Pipe(%s)", checkPipe(L, -1)->name);
    return 1;
}

constexpr NativeFunction kFunctions[] = {
    {"cwd", processCwd},
    {"chdir", processChdir},
    {"exit", processExit},
    {"exec", processExec},
    {"spawn", processSpawn},
    {"kill", processKill},
    {"onSignal", processOnSignal},
    {"uptime", processUptime},
    {"memoryUsage", processMemoryUsage},
};

constexpr NativeFunction kChildMethods[] = {
    {"wait", childWait},
    {"status", childStatus},
    {"kill", childKill},
};

constexpr NativeFunction kPipeMethods[] = {
    {"read", pipeRead},
    {"readLine", pipeReadLine},
    {"readAll", pipeReadAll},
    {"write", pipeWrite},
    {"close", pipeClose},
};

void registerTypes(lua_State* L) {
    if (luaL_newmetatable(L, kChildMeta)) {
        lua_createtable(L, 0, static_cast<int>(std::size(kChildMethods)));
        setFunctions(L, kChildMethods);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, "process.Child");
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, kPipeMeta)) {
        lua_createtable(L, 0, static_cast<int>(std::size(kPipeMethods)));
        setFunctions(L, kPipeMethods);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, pipeToString, "Pipe.__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "process.Pipe");
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kChildHandlesKey);

    lua_getfield(L, LUA_REGISTRYINDEX, kSignalHandlersKey);
    if (!lua_istable(L, -1)) {
        lua_createtable(L, 0, 0);
        lua_setfield(L, LUA_REGISTRYINDEX, kSignalHandlersKey);
    }
    lua_pop(L, 1);
}

} // namespace

// The opener: runs once per VM and returns the library table.
void openProcess(lua_State* L) {
    registerTypes(L);

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 5);
    setFunctions(L, kFunctions);

    lua_pushnumber(L, sys::processId());
    lua_setfield(L, -2, "pid");

    if (const std::optional<double> parent = sys::parentProcessId()) {
        lua_pushnumber(L, *parent);
        lua_setfield(L, -2, "ppid");
    }

    {
        std::string path;
        if (sys::executablePath(path)) {
            lua_pushlstring(L, path.data(), path.size());
            lua_setfield(L, -2, "execPath");
        }
    }

    // The program's arguments, if the host provided them.
    lua_getfield(L, LUA_REGISTRYINDEX, kArgsRegistryKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 0);
    }
    lua_setfield(L, -2, "args");

    pushEnvironmentProxy(L);
    lua_setfield(L, -2, "env");
}

} // namespace sonata::lib::libs
