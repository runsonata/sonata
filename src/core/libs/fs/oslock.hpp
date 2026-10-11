#pragma once

#include <filesystem>
#include <memory>
#include <string>

// Advisory file locks for @sonata/fs (flock on POSIX, LockFileEx on Windows).
// Kept apart from fs.cpp so the platform headers stay out of it. Never touches Lua.
namespace sonata::lib::libs::fslock {

class FileLock {
  public:
    virtual ~FileLock() = default; // releases the lock and closes the file
    virtual void release() = 0;
    virtual bool held() const = 0;
};

// Opens (creating it if needed) "path" and locks it. "timeout" in seconds:
// < 0 waits forever, 0 only tries once. Returns nullptr when the lock could not
// be taken in time ("error" stays empty) or on failure ("error" says why).
std::unique_ptr<FileLock> acquire(const std::filesystem::path& path, bool shared, double timeout, std::string& error);

} // namespace sonata::lib::libs::fslock
