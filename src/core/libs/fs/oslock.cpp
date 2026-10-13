#include "oslock.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace sonata::lib::libs::fslock {

namespace {

std::string describe(int code) {
#if defined(_WIN32)
    std::string text = std::system_category().message(code);
#else
    std::string text = std::generic_category().message(code);
#endif
    if (!text.empty()) {
        text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
    }
    return text;
}

// Calls "attempt" (true = locked, false = busy; it sets "failed" on a real error)
// until it succeeds or "timeout" seconds have passed.
template <typename Attempt>
bool retry(double timeout, Attempt attempt) {
    const auto start = std::chrono::steady_clock::now();
    auto pause = std::chrono::milliseconds(1);
    for (;;) {
        if (attempt()) {
            return true;
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout) {
            return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(timeout - elapsed));
        std::this_thread::sleep_for(std::min(pause, std::max(left, std::chrono::milliseconds(1))));
        pause = std::min(pause * 2, std::chrono::milliseconds(50));
    }
}

#if defined(_WIN32)

class WindowsLock final : public FileLock {
  public:
    explicit WindowsLock(HANDLE file) : file_(file) {}
    ~WindowsLock() override { release(); }

    void release() override {
        if (file_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            UnlockFileEx(file_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }
    bool held() const override { return file_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE file_;
};

#else

class PosixLock final : public FileLock {
  public:
    explicit PosixLock(int fd) : fd_(fd) {}
    ~PosixLock() override { release(); }

    void release() override {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
    }
    bool held() const override { return fd_ >= 0; }

  private:
    int fd_;
};

#endif

} // namespace

std::unique_ptr<FileLock> acquire(const std::filesystem::path& path, bool shared, double timeout, std::string& error) {
    error.clear();
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE && shared) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = describe(static_cast<int>(GetLastError()));
        return nullptr;
    }

    const DWORD mode = shared ? 0 : LOCKFILE_EXCLUSIVE_LOCK;
    bool failed = false;
    const auto attempt = [&](DWORD extra) {
        OVERLAPPED overlapped{};
        if (LockFileEx(file, mode | extra, 0, MAXDWORD, MAXDWORD, &overlapped)) {
            return true;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING) {
            error = describe(static_cast<int>(code));
            failed = true;
            return true; // stop retrying
        }
        return false;
    };

    const bool locked = timeout < 0 ? attempt(0) : retry(timeout, [&] { return attempt(LOCKFILE_FAIL_IMMEDIATELY); });
    if (!locked || failed) {
        CloseHandle(file);
        return nullptr;
    }
    return std::make_unique<WindowsLock>(file);
#else
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0 && shared && (errno == EACCES || errno == EROFS)) {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        error = describe(errno);
        return nullptr;
    }

    const int operation = shared ? LOCK_SH : LOCK_EX;
    bool failed = false;
    const auto attempt = [&](int extra) {
        for (;;) {
            if (::flock(fd, operation | extra) == 0) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EWOULDBLOCK) {
                error = describe(errno);
                failed = true;
                return true; // stop retrying
            }
            return false;
        }
    };

    const bool locked = timeout < 0 ? attempt(0) : retry(timeout, [&] { return attempt(LOCK_NB); });
    if (!locked || failed) {
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<PosixLock>(fd);
#endif
}

} // namespace sonata::lib::libs::fslock
