#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string_view>

#include <sonata/core/system.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
extern char **environ;
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace sonata::os {

namespace {

constexpr std::size_t kMaxCapturedOutput = 64 * 1024;

// Keeps the end of the output: that's where a failing tool says why.
void capture(std::string &output, const char *data, std::size_t size) {
    output.append(data, size);
    if (output.size() > 2 * kMaxCapturedOutput)
        output.erase(0, output.size() - kMaxCapturedOutput);
}

void trimCaptured(std::string &output) {
    if (output.size() > kMaxCapturedOutput)
        output.erase(0, output.size() - kMaxCapturedOutput);
}

#if defined(_WIN32)

std::wstring widen(const std::string &s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

/* The standard CommandLineToArgvW-compatible quoting rules. */
std::wstring quoteArg(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

#endif

} // namespace

#if defined(_WIN32)

ProcessResult runProcess(const std::vector<std::string> &args,
                         const EnvList &env, bool inheritOutput) {
    ProcessResult result;

    std::wstring commandLine;
    for (const auto &a : args) {
        if (!commandLine.empty())
            commandLine.push_back(L' ');
        commandLine += quoteArg(widen(a));
    }

    // Environment block: current environment, with our overrides replacing any
    // duplicates.
    std::vector<std::wstring> overrideNames;
    for (const auto &kv : env)
        overrideNames.push_back(widen(kv.first));

    std::wstring block;
    if (LPWCH current = GetEnvironmentStringsW()) {
        for (const wchar_t *p = current; *p; p += std::wcslen(p) + 1) {
            const std::wstring entry(p);
            const auto eq = entry.find(L'=', 1);
            const std::wstring name = entry.substr(0, eq);
            const bool overridden =
                std::any_of(overrideNames.begin(), overrideNames.end(),
                            [&](const std::wstring &o) {
                                return _wcsicmp(o.c_str(), name.c_str()) == 0;
                            });
            if (!overridden) {
                block += entry;
                block.push_back(L'\0');
            }
        }
        FreeEnvironmentStringsW(current);
    }
    for (const auto &kv : env) {
        block += widen(kv.first + "=" + kv.second);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!inheritOutput) {
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
            return result;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    }

    HANDLE nul =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = inheritOutput ? GetStdHandle(STD_OUTPUT_HANDLE) : writeEnd;
    si.hStdError = inheritOutput ? GetStdHandle(STD_ERROR_HANDLE) : writeEnd;

    PROCESS_INFORMATION pi{};
    const BOOL ok =
        CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                       block.data(), nullptr, &si, &pi);

    if (writeEnd)
        CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ok) {
        if (readEnd)
            CloseHandle(readEnd);
        return result;
    }
    result.started = true;

    if (readEnd) {
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) &&
               got > 0) {
            capture(result.output, buffer, got);
        }
        CloseHandle(readEnd);
        trimCaptured(result.output);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    result.exitCode = static_cast<int>(code);
    return result;
}

std::optional<fs::path> executablePath() {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
        if (n == 0)
            return std::nullopt;
        if (n < buffer.size())
            return fs::path(buffer.substr(0, n));
        buffer.resize(buffer.size() * 2); // truncated
    }
}

std::optional<fs::path> homeDirectory() {
    const wchar_t *home = _wgetenv(L"USERPROFILE");
    if (home && *home)
        return fs::path(home);
    return std::nullopt;
}

#else // POSIX

ProcessResult runProcess(const std::vector<std::string> &args,
                         const EnvList &env, bool inheritOutput) {
    ProcessResult result;
    if (args.empty())
        return result;

    int fds[2] = {-1, -1};
    if (!inheritOutput) {
        if (pipe(fds) != 0)
            return result;
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (!inheritOutput) {
        // dup2 clears CLOEXEC on the copy
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    }

    std::vector<std::string> envStrings;
    for (char **e = environ; e && *e; ++e) {
        const std::string_view entry(*e);
        const std::string_view name = entry.substr(0, entry.find('='));
        const bool overridden =
            std::any_of(env.begin(), env.end(), [&](const auto &kv) {
                return std::string_view(kv.first) == name;
            });
        if (!overridden)
            envStrings.emplace_back(entry);
    }
    for (const auto &kv : env)
        envStrings.push_back(kv.first + "=" + kv.second);

    std::vector<char *> envp;
    for (auto &s : envStrings)
        envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> argStorage = args;
    std::vector<char *> argv;
    for (auto &s : argStorage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(),
                                envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (!inheritOutput)
        close(fds[1]);
    if (rc != 0) {
        if (!inheritOutput)
            close(fds[0]);
        return result;
    }
    result.started = true;

    if (!inheritOutput) {
        char buffer[4096];
        for (;;) {
            const ssize_t n = read(fds[0], buffer, sizeof(buffer));
            if (n > 0) {
                capture(result.output, buffer, static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fds[0]);
        trimCaptured(result.output);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#if defined(__linux__)

std::optional<fs::path> executablePath() {
    std::error_code ec;
    fs::path path = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return std::nullopt;
    return path;
}

#elif defined(__APPLE__)

std::optional<fs::path> executablePath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // fails, but reports the size needed

    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return std::nullopt;

    std::error_code ec;
    fs::path resolved = fs::canonical(buffer.data(), ec);
    return ec ? fs::path(buffer.data()) : resolved;
}

#else

std::optional<fs::path> executablePath() { return std::nullopt; }

#endif

std::optional<fs::path> homeDirectory() {
    const char *home = std::getenv("HOME");
    if (home && *home)
        return fs::path(home);
    return std::nullopt;
}

#endif

} // namespace sonata::os
