// @sonata/fs: Filesystem access
//
//     local fs = require("@sonata/fs")
//
// Every function raises a Lua error on failure (use pcall to catch it), except
// the exists / isFile / isDir / isSymlink queries, which just answer false.
// Paths are UTF-8 strings. File contents are binary-safe Lua strings.
//
// Variables
//     fs.maxReadSize                         --> largest file readFile() accepts, in bytes
//     fs.maxChunkSize                        --> largest chunk a reader can produce, in bytes
//     fs.modes                               --> permission presets for chmod():
//         .file 420 (0644)      .executable 493 (0755)   .directory 493 (0755)
//         .private 384 (0600)   .privateDirectory 448 (0700)   .readOnly 292 (0444)
//
// Files
//     fs.readFile(path)                      --> contents: string
//     fs.writeFile(path, contents)           creates or truncates
//     fs.appendFile(path, contents)          creates if missing
//     fs.copyFile(from, to, overwrite?)      overwrite defaults to true
//     fs.removeFile(path)
//     fs.move(from, to)                      rename; copies + deletes across devices
//
// Chunked reading (big files: only one chunk is in memory at a time)
//     fs.chunkPlan(sizeOrPath, spec?)        --> { size, chunkSize, chunkCount, lastChunkSize }
//     fs.openReader(path, spec?)             --> reader (regular files only)
//         reader:read()                      --> next chunk, or nil after the last one
//         reader:readChunk(index)            --> chunk number `index` (1-based), in any order
//         reader:chunks()                    for index, chunk in reader:chunks() do ... end
//         reader:rewind()                    makes read() start again at chunk 1
//         reader:plan()                      --> the same table fs.chunkPlan returns
//         reader:close()                     releases the file now; calling it twice is fine
//
//     spec decides the chunk size:
//         nil or "auto"   size / 64, rounded up to a multiple of 4 KiB, kept within
//                         64 KiB .. 4 MiB. A heuristic that bounds memory per chunk;
//                         it is not tuned to any particular disk or workload.
//         { count = n }   aim for n chunks: chunkSize = ceil(size / n). Every chunk has the
//                         same size except the last (shorter), so the real count can be
//                         lower than n (10 bytes with count = 6 -> 5 chunks of 2 bytes),
//                         never higher.
//         { size = n }    exactly n bytes per chunk (1 .. fs.maxChunkSize); last may be shorter.
//
//     chunkPlan accepts a size in bytes (pure arithmetic, touches no file) or a path.
//     An empty file has 0 chunks. The plan is fixed when the reader opens: if the file
//     shrinks afterwards read() raises; bytes appended afterwards are not seen.
//     read(), readChunk() and chunks() do not disturb each other: only read() moves the
//     read() position. A reader keeps its file open until close() or until it is collected.
//
// Directories
//     fs.makeDir(path, recursive?)           non-recursive fails if it already exists
//     fs.removeDir(path, recursive?)         non-recursive fails if it is not empty
//     fs.listDir(path, recursive?)           --> { "a.txt", "sub" }  (sorted)
//     fs.readDir(path, recursive?)           --> { { name = "a.txt", kind = "file" }, ... }
//     fs.copyDir(from, to, overwrite?)       merges into `to`; overwrite defaults to true
//
//     With recursive = true, listDir / readDir return paths relative to `path`
//     with "/" separators ("sub/a.txt"). Symlinks are reported, not followed.
//
// Queries
//     fs.exists(path)                        --> boolean (follows symlinks)
//     fs.isFile(path), fs.isDir(path)        --> boolean (follow symlinks)
//     fs.isSymlink(path)                     --> boolean
//     fs.stat(path), fs.lstat(path)          --> { kind = "file" | "directory" | "symlink" | "other",
//                                                  size = bytes, modified = unix seconds, mode = number }
//                                                (lstat does not follow symlinks; size is 0 for
//                                                 anything but regular files)
//     fs.realPath(path)                      --> absolute path with symlinks resolved (must exist)
//
// Links and permissions
//     fs.symlink(target, link)
//     fs.readLink(path)                      --> target: string
//     fs.chmod(path, mode)                   e.g. fs.chmod(p, fs.modes.executable)
//
// Process-level locations
//     fs.cwd()                               --> string
//     fs.chdir(path)
//     fs.tempDir()                           --> the system temp directory
//     fs.makeTempDir(prefix?)                --> a new, empty, private directory inside it
//
// Finding files
//     fs.walk(dir, options?)                 --> iterator: for entry in fs.walk("src") do ... end
//         entry = { path = "sub/a.txt", fullPath = "src/sub/a.txt", name = "a.txt",
//                   kind = "file" | "directory" | "symlink" | "other", depth = 2 }
//         Depth first, a directory before what is in it, names sorted within each directory.
//         options: maxDepth (number, default unlimited), followSymlinks (false: links are
//                  reported, not entered), filter = function(entry): boolean (false skips the
//                  entry, and for a directory everything in it), dirs (true: report directories)
//     fs.glob(pattern, options?)             --> { string }  matching paths, sorted
//         "*" any characters but "/", "?" one character, "[abc]" "[a-z]" "[!abc]" one of a set,
//         "**" any number of directories, "{a,b}" alternatives, "\\" escapes the next character.
//         Names starting with "." only match patterns that spell the dot, unless dot = true.
//         options: cwd (where relative patterns start, default "."), dot (false),
//                  kind ("file" | "directory": only those), absolute (false: return paths as
//                  the pattern wrote them, relative to cwd)
//     fs.match(pattern, path, options?)      --> boolean  the same rules, without touching the disk
//                                                (options: dot)
//
// Watching for changes
//     fs.watch(path, options?)               --> Watcher  (a file, or a directory and what is in it)
//         options: recursive (true), interval (seconds between checks, default 0.25)
//         watcher:next(timeout?)             --> Event?   waits for the next change; nil on timeout
//                                                         or once the watcher is closed
//         watcher:poll()                     --> { Event }  the changes since the last look, now
//         watcher:close()
//         Event = { kind = "create" | "modify" | "remove", path = string, isDir = boolean }
//     Changes are found by comparing what the files look like (size, modification time)
//     every "interval", so very fast edits in between can show up as one "modify".
//     next() lets other tasks run while it waits (see @sonata/task).
//
// Locking (advisory: only between programs that also lock)
//     fs.lock(path, options?)                --> Lock | nil   waits for the lock; nil after "timeout"
//         options: shared (false: exclusive), timeout (seconds, default: forever)
//     fs.tryLock(path, options?)             --> Lock | nil   nil if someone else holds it
//         lock:unlock()                      lock:isLocked() --> boolean
//     The file is created if it does not exist, and left in place afterwards. A lock is
//     released by unlock(), when it is collected, and when the program exits. Waiting in
//     fs.lock lets other tasks run.
//
// Same behavior on every OS
//     Paths you pass in may use "/" everywhere (Windows also accepts a backslash).
//     Paths this library returns (realPath, cwd, tempDir, makeTempDir, readLink)
//     always use "/", so they can be joined with "/" and compared as strings.
//     Windows keeps its drive letter ("C:/Users/me"); nothing can hide that.
//     Error messages use the same wording on every OS whenever the OS has a
//     portable name for the error ("no such file or directory", "permission
//     denied", ...). Do not parse anything beyond that.
//     removeFile / removeDir delete read-only entries on Windows too, like POSIX.
//
// Permissions on Windows
//     Windows has no permission bits, only a read-only attribute. chmod looks at
//     the owner-write bit (0200) only: set -> writable, clear -> read-only.
//     stat().mode is derived from that attribute: 0644 / 0444 for files and
//     0755 / 0555 for directories. Execute, group and other bits are ignored, so
//     chmod(p, fs.modes.private) reads back as 0644 on Windows. fs.modes.file
//     and fs.modes.readOnly round-trip on every OS.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if __has_include(<version>)
#include <version>
#endif

#include <sonata/core/library.hpp>
#include <sonata/core/scheduler.hpp>
#include "lua.h"
#include "lualib.h"

#include "fs/oslock.hpp"
#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

namespace stdfs = std::filesystem;

// Rule of thumb (same as example.cpp): validate ALL arguments first, then
// create C++ objects. Failures that only show up while the work is being done
// are not raised from inside the functions that own C++ objects. They push
// their message and return kFailed; finish() raises it once those functions
// have returned and their destructors have run.
//
//     int fsThing(lua_State* L) {
//         const std::string_view path = checkPath(L, 1);   // may raise: no objects yet
//         return finish(L, thingImpl(L, path));            // impl never raises
//     }

constexpr int kFailed = -1;

// Luau strings are limited to 1 GiB.
constexpr std::uintmax_t kMaxReadSize = (std::uintmax_t{1} << 30) - 1;

int fail(lua_State* L, const char* op, const std::string& subject, std::string_view reason) {
    std::string message = "fs.";
    message += op;
    if (!subject.empty()) {
        message += " ";
        message += subject;
    }
    message += ": ";
    message.append(reason);
    lua_pushlstring(L, message.data(), message.size());
    return kFailed;
}

int finish(lua_State* L, int results) {
    if (results < 0) {
        luaL_error(L, "%s", lua_tostring(L, -1));
    }
    return results;
}

std::string quote(std::string_view path) {
    std::string out = "'";
    out.append(path);
    out += "'";
    return out;
}

std::string quote(std::string_view from, std::string_view to) {
    return quote(from) + " -> " + quote(to);
}

// Error text that reads the same on every OS. Mapping to the portable condition
// (ENOENT, EACCES, ...) makes Windows say "no such file or directory" instead of a
// localized Win32 message; lower-casing the first letter reconciles glibc/macOS
// ("No such file or directory") with MSVC ("no such file or directory").
std::string describe(const std::error_code& ec) {
    std::string text = ec.default_error_condition().message();
    if (text.size() > 1 && std::isupper(static_cast<unsigned char>(text[0])) &&
        std::islower(static_cast<unsigned char>(text[1]))) {
        text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
    }
    return text;
}

std::string reasonOf(std::errc code) {
    return describe(std::make_error_code(code));
}

std::string errnoReason(int error) {
    if (error == 0) {
        return "operation failed";
    }
    return describe(std::error_code(error, std::generic_category()));
}

std::string_view checkPath(lua_State* L, int arg) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, arg, &length);
    luaL_argcheck(L, std::strlen(text) == length, arg, "path must not contain NUL bytes");
    return std::string_view(text, length);
}

std::string_view checkData(lua_State* L, int arg) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, arg, &length);
    return std::string_view(text, length);
}

bool optBoolean(lua_State* L, int arg, bool fallback) {
    if (lua_isnoneornil(L, arg)) {
        return fallback;
    }
    luaL_checktype(L, arg, LUA_TBOOLEAN);
    return lua_toboolean(L, arg) != 0;
}

void pushString(lua_State* L, std::string_view text) {
    lua_pushlstring(L, text.data(), text.size());
}

void setNumberField(lua_State* L, const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

void setStringField(lua_State* L, const char* key, std::string_view text) {
    pushString(L, text);
    lua_setfield(L, -2, key);
}

// UTF-8 <-> std::filesystem::path (std::string would be the ANSI code page on
// Windows)
////////////////////////////////////////////////

stdfs::path toPath(std::string_view text) {
#if defined(__cpp_lib_char8_t)
    return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
    return stdfs::u8path(text.begin(), text.end());
#endif
}

std::string toUtf8(const stdfs::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string text = p.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return p.u8string();
#endif
}

// Same, but with "/" as the separator on every platform.
std::string toGenericUtf8(const stdfs::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string text = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return p.generic_u8string();
#endif
}

// Paths handed back to Lua always use "/" (see the header comment).
void pushPath(lua_State* L, const stdfs::path& p) {
    pushString(L, toGenericUtf8(p));
}

// Drops a trailing separator ("/var/T/" -> "/var/T", "C:/Temp/" -> "C:/Temp"); roots stay.
stdfs::path withoutTrailingSeparator(const stdfs::path& p) {
    if (!p.has_filename() && p.has_relative_path()) {
        return p.parent_path();
    }
    return p;
}

const char* kindOf(const stdfs::file_status& status) {
    switch (status.type()) {
    case stdfs::file_type::regular:
        return "file";
    case stdfs::file_type::directory:
        return "directory";
    case stdfs::file_type::symlink:
        return "symlink";
    default:
        return "other";
    }
}

// Permission bits as reported by stat(). POSIX reports what the filesystem
// stores. Windows only has a read-only attribute, and the standard library
// reports it differently from one implementation to the next, so build the
// answer from that one bit.
unsigned modeOf(const stdfs::file_status& status) {
#ifdef _WIN32
    const bool writable =
        (status.permissions() & stdfs::perms::owner_write) != stdfs::perms::none;
    if (status.type() == stdfs::file_type::directory) {
        return writable ? 0755 : 0555;
    }
    return writable ? 0644 : 0444;
#else
    return static_cast<unsigned>(status.permissions() & stdfs::perms::mask);
#endif
}

#ifdef _WIN32
// POSIX lets you delete a read-only file; Windows refuses while the read-only
// attribute is set. Used only to retry after a "permission denied".
void clearReadOnly(const stdfs::path& path) {
    std::error_code ignored;
    stdfs::permissions(path, stdfs::perms::owner_write, stdfs::perm_options::add, ignored);
}

void clearReadOnlyTree(const stdfs::path& dir) {
    clearReadOnly(dir);
    std::error_code ec;
    for (stdfs::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code linkEc;
        if (!it->is_symlink(linkEc)) { // never touch a link's target
            clearReadOnly(it->path());
        }
    }
}
#endif

double toUnixSeconds(stdfs::file_time_type time) {
    // C++17 has no portable file_clock -> system_clock conversion; measure the
    // distance to "now" on both clocks instead (accurate to microseconds).
    const auto delta = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        time - stdfs::file_time_type::clock::now());
    const auto system = std::chrono::system_clock::now() + delta;
    return std::chrono::duration<double>(system.time_since_epoch()).count();
}

// Files ////////////////////////

int readFileImpl(lua_State* L, std::string_view pathText) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    const stdfs::file_status status = stdfs::status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, "readFile", quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (stdfs::is_directory(status)) {
        return fail(L, "readFile", quote(pathText), reasonOf(std::errc::is_a_directory));
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(L, "readFile", quote(pathText), errnoReason(errno));
    }

    // Do not trust the size alone: pipes and /proc files report 0.
    std::string data;
    if (stdfs::is_regular_file(status)) {
        const std::uintmax_t size = stdfs::file_size(path, ec);
        if (!ec) {
            if (size > kMaxReadSize) {
                return fail(L, "readFile", quote(pathText), "file is too large to read into a string");
            }
            data.reserve(static_cast<std::size_t>(size));
        }
    }

    std::vector<char> chunk(1 << 16);
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0) {
        data.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
        if (data.size() > kMaxReadSize) {
            return fail(L, "readFile", quote(pathText), "file is too large to read into a string");
        }
    }
    if (in.bad()) {
        return fail(L, "readFile", quote(pathText), "read error");
    }

    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int writeImpl(lua_State* L, const char* op, std::string_view pathText, std::string_view data,
              bool append) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    if (stdfs::is_directory(path, ec)) {
        return fail(L, op, quote(pathText), reasonOf(std::errc::is_a_directory));
    }

    std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) {
        return fail(L, op, quote(pathText), errnoReason(errno));
    }

    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (out.fail()) {
        return fail(L, op, quote(pathText), "write error");
    }
    return 0;
}

int copyFileImpl(lua_State* L, std::string_view fromText, std::string_view toText, bool overwrite) {
    const stdfs::path from = toPath(fromText);
    const stdfs::path to = toPath(toText);

    std::error_code ec;
    stdfs::copy_file(from, to,
                     overwrite ? stdfs::copy_options::overwrite_existing : stdfs::copy_options::none,
                     ec);
    if (ec) {
        return fail(L, "copyFile", quote(fromText, toText), describe(ec));
    }
    return 0;
}

int removeFileImpl(lua_State* L, std::string_view pathText) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    const stdfs::file_status status = stdfs::symlink_status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, "removeFile", quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (ec) {
        return fail(L, "removeFile", quote(pathText), describe(ec));
    }
    if (stdfs::is_directory(status)) {
        return fail(L, "removeFile", quote(pathText), reasonOf(std::errc::is_a_directory));
    }

    stdfs::remove(path, ec);
#ifdef _WIN32
    if (ec == std::errc::permission_denied && !stdfs::is_symlink(status)) {
        clearReadOnly(path);
        ec.clear();
        stdfs::remove(path, ec);
    }
#endif
    if (ec) {
        return fail(L, "removeFile", quote(pathText), describe(ec));
    }
    return 0;
}

int moveImpl(lua_State* L, std::string_view fromText, std::string_view toText) {
    const stdfs::path from = toPath(fromText);
    const stdfs::path to = toPath(toText);

    std::error_code ec;
    stdfs::rename(from, to, ec);
    if (!ec) {
        return 0;
    }
    if (ec != std::errc::cross_device_link) {
        return fail(L, "move", quote(fromText, toText), describe(ec));
    }

    // Different filesystems: copy, then delete the source.
    std::error_code ignored;
    if (stdfs::is_directory(stdfs::symlink_status(to, ignored))) {
        return fail(L, "move", quote(fromText, toText), "destination is a directory");
    }
    stdfs::copy(from, to,
                stdfs::copy_options::recursive | stdfs::copy_options::copy_symlinks |
                    stdfs::copy_options::overwrite_existing,
                ec);
    if (ec) {
        return fail(L, "move", quote(fromText, toText), describe(ec));
    }
    stdfs::remove_all(from, ec);
    if (ec) {
        return fail(L, "move", quote(fromText, toText), describe(ec));
    }
    return 0;
}

// Directories ////////////////////

int makeDirImpl(lua_State* L, std::string_view pathText, bool recursive) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    if (recursive) {
        stdfs::create_directories(path, ec);
        if (!ec && !stdfs::is_directory(path, ec)) {
            return fail(L, "makeDir", quote(pathText), reasonOf(std::errc::not_a_directory));
        }
        if (ec) {
            return fail(L, "makeDir", quote(pathText), describe(ec));
        }
        return 0;
    }

    const bool created = stdfs::create_directory(path, ec);
    if (ec) {
        return fail(L, "makeDir", quote(pathText), describe(ec));
    }
    if (!created) {
        return fail(L, "makeDir", quote(pathText), reasonOf(std::errc::file_exists));
    }
    return 0;
}

int removeDirImpl(lua_State* L, std::string_view pathText, bool recursive) {
    const stdfs::path path = toPath(pathText);

    // Never delete a filesystem root, whatever the flags say.
    if (!path.has_relative_path()) {
        return fail(L, "removeDir", quote(pathText), "refusing to remove a filesystem root");
    }

    std::error_code ec;
    const stdfs::file_status status = stdfs::symlink_status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, "removeDir", quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (ec) {
        return fail(L, "removeDir", quote(pathText), describe(ec));
    }
    // A symlink to a directory is not a directory here: it is never followed.
    if (!stdfs::is_directory(status)) {
        return fail(L, "removeDir", quote(pathText), reasonOf(std::errc::not_a_directory));
    }

    const auto attempt = [&] {
        ec.clear();
        if (recursive) {
            stdfs::remove_all(path, ec);
        } else {
            stdfs::remove(path, ec);
        }
    };
    attempt();
#ifdef _WIN32
    if (ec == std::errc::permission_denied) {
        if (recursive) {
            clearReadOnlyTree(path);
        } else {
            clearReadOnly(path);
        }
        attempt();
    }
#endif
    if (ec) {
        return fail(L, "removeDir", quote(pathText), describe(ec));
    }
    return 0;
}

struct Entry {
    std::string name;
    const char* kind;
};

template <typename Iterator>
bool scanEntries(const stdfs::path& dir, bool relativeToDir, std::vector<Entry>& out,
                 std::error_code& ec) {
    for (Iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const stdfs::directory_entry& entry = *it;

        std::error_code statusEc;
        const stdfs::file_status status = entry.symlink_status(statusEc);

        Entry item;
        item.name = relativeToDir ? toGenericUtf8(entry.path().lexically_relative(dir))
                                  : toUtf8(entry.path().filename());
        item.kind = statusEc ? "other" : kindOf(status);
        out.push_back(std::move(item));
    }
    return !ec;
}

// Collects the entries of a directory, sorted by name. Returns false with ec set on failure.
bool collectEntries(const stdfs::path& dir, bool recursive, std::vector<Entry>& out,
                    std::error_code& ec) {
    const bool ok = recursive ? scanEntries<stdfs::recursive_directory_iterator>(dir, true, out, ec)
                              : scanEntries<stdfs::directory_iterator>(dir, false, out, ec);
    if (ok) {
        std::sort(out.begin(), out.end(),
                  [](const Entry& a, const Entry& b) { return a.name < b.name; });
    }
    return ok;
}

int listDirImpl(lua_State* L, const char* op, std::string_view pathText, bool recursive,
                bool withKinds) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    if (!stdfs::is_directory(path, ec)) {
        return fail(L, op, quote(pathText),
                    stdfs::exists(path, ec) ? reasonOf(std::errc::not_a_directory)
                                            : reasonOf(std::errc::no_such_file_or_directory));
    }

    std::vector<Entry> entries;
    if (!collectEntries(path, recursive, entries, ec)) {
        return fail(L, op, quote(pathText), describe(ec));
    }

    lua_createtable(L, static_cast<int>(entries.size()), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (withKinds) {
            lua_createtable(L, 0, 2);
            setStringField(L, "name", entries[i].name);
            setStringField(L, "kind", entries[i].kind);
        } else {
            pushString(L, entries[i].name);
        }
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int copyDirImpl(lua_State* L, std::string_view fromText, std::string_view toText, bool overwrite) {
    const stdfs::path from = toPath(fromText);
    const stdfs::path to = toPath(toText);

    std::error_code ec;
    if (!stdfs::is_directory(from, ec)) {
        return fail(L, "copyDir", quote(fromText, toText), "source is not a directory");
    }

    // Copying a directory into itself would never end.
    const stdfs::path source = stdfs::weakly_canonical(from, ec);
    if (ec) {
        return fail(L, "copyDir", quote(fromText, toText), describe(ec));
    }
    const stdfs::path target = stdfs::weakly_canonical(to, ec);
    if (ec) {
        return fail(L, "copyDir", quote(fromText, toText), describe(ec));
    }
    const stdfs::path relation = target.lexically_relative(source);
    if (!relation.empty() && *relation.begin() != stdfs::path("..")) {
        return fail(L, "copyDir", quote(fromText, toText),
                    "destination is the source directory or inside it");
    }

    stdfs::copy_options options = stdfs::copy_options::recursive | stdfs::copy_options::copy_symlinks;
    if (overwrite) {
        options |= stdfs::copy_options::overwrite_existing;
    }
    stdfs::copy(from, to, options, ec);
    if (ec) {
        return fail(L, "copyDir", quote(fromText, toText), describe(ec));
    }
    return 0;
}

int tempDirImpl(lua_State* L) {
    std::error_code ec;
    const stdfs::path dir = stdfs::temp_directory_path(ec);
    if (ec) {
        return fail(L, "tempDir", "", describe(ec));
    }
    pushPath(L, withoutTrailingSeparator(dir));
    return 1;
}

int makeTempDirImpl(lua_State* L, std::string_view prefix) {
    std::error_code ec;
    const stdfs::path base = stdfs::temp_directory_path(ec);
    if (ec) {
        return fail(L, "makeTempDir", "", describe(ec));
    }

    static const char kDigits[] = "0123456789abcdef";
    std::random_device device;
    std::mt19937_64 random(
        (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device()));

    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name(prefix);
        for (int i = 0; i < 12; ++i) {
            name.push_back(kDigits[random() & 0xf]);
        }

        const stdfs::path candidate = base / toPath(name);
        if (stdfs::create_directory(candidate, ec)) {
            stdfs::permissions(candidate, stdfs::perms::owner_all, ec); // best effort: 0700
            pushPath(L, candidate);
            return 1;
        }
        if (ec) {
            return fail(L, "makeTempDir", quote(toUtf8(candidate)), describe(ec));
        }
        // The name was taken: try another one.
    }
    return fail(L, "makeTempDir", quote(toUtf8(base)), "could not find an unused name");
}

int cwdImpl(lua_State* L) {
    std::error_code ec;
    const stdfs::path dir = stdfs::current_path(ec);
    if (ec) {
        return fail(L, "cwd", "", describe(ec));
    }
    pushPath(L, dir);
    return 1;
}

int chdirImpl(lua_State* L, std::string_view pathText) {
    std::error_code ec;
    stdfs::current_path(toPath(pathText), ec);
    if (ec) {
        return fail(L, "chdir", quote(pathText), describe(ec));
    }
    return 0;
}

// Queries, links, permissions ///////////////

int statImpl(lua_State* L, const char* op, std::string_view pathText, bool follow) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    const stdfs::file_status status =
        follow ? stdfs::status(path, ec) : stdfs::symlink_status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, op, quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (ec) {
        return fail(L, op, quote(pathText), describe(ec));
    }

    double size = 0;
    if (stdfs::is_regular_file(status)) {
        std::error_code sizeEc;
        const std::uintmax_t bytes = stdfs::file_size(path, sizeEc);
        if (!sizeEc) {
            size = static_cast<double>(bytes);
        }
    }

    double modified = 0;
    std::error_code timeEc;
    const stdfs::file_time_type time = stdfs::last_write_time(path, timeEc);
    if (!timeEc) {
        modified = toUnixSeconds(time);
    }

    lua_createtable(L, 0, 4);
    setStringField(L, "kind", kindOf(status));
    setNumberField(L, "size", size);
    setNumberField(L, "modified", modified);
    setNumberField(L, "mode", static_cast<double>(modeOf(status)));
    return 1;
}

int realPathImpl(lua_State* L, std::string_view pathText) {
    std::error_code ec;
    const stdfs::path resolved = stdfs::canonical(toPath(pathText), ec);
    if (ec) {
        return fail(L, "realPath", quote(pathText), describe(ec));
    }
    pushPath(L, resolved);
    return 1;
}

int symlinkImpl(lua_State* L, std::string_view targetText, std::string_view linkText) {
    stdfs::path target = toPath(targetText);
    const stdfs::path link = toPath(linkText);
#ifdef _WIN32
    // Windows stores a relative target verbatim and does not understand "/" in it.
    target.make_preferred();
#endif

    // Windows distinguishes links to directories from links to files.
    std::error_code ec;
    const stdfs::path anchored = target.is_absolute() ? target : link.parent_path() / target;
    const bool toDirectory = stdfs::is_directory(anchored, ec);

    if (toDirectory) {
        stdfs::create_directory_symlink(target, link, ec);
    } else {
        stdfs::create_symlink(target, link, ec);
    }
    if (ec) {
        return fail(L, "symlink", quote(linkText, targetText), describe(ec));
    }
    return 0;
}

int readLinkImpl(lua_State* L, std::string_view pathText) {
    std::error_code ec;
    const stdfs::path target = stdfs::read_symlink(toPath(pathText), ec);
    if (ec) {
        return fail(L, "readLink", quote(pathText), describe(ec));
    }
    pushPath(L, target);
    return 1;
}

int chmodImpl(lua_State* L, std::string_view pathText, int mode) {
    stdfs::perms requested = static_cast<stdfs::perms>(mode);
#ifdef _WIN32
    // Only the read-only attribute exists: owner-write set -> writable, else read-only.
    requested = (mode & 0200) != 0 ? (stdfs::perms::owner_read | stdfs::perms::owner_write)
                                   : stdfs::perms::owner_read;
#endif
    std::error_code ec;
    stdfs::permissions(toPath(pathText), requested, ec);
    if (ec) {
        return fail(L, "chmod", quote(pathText), describe(ec));
    }
    return 0;
}

// Chunked reading ///////////////////////////

// A chunk becomes one Lua string, so it can be no larger than one.
constexpr std::uintmax_t kMaxChunkSize = kMaxReadSize;

// "auto": about kAutoTargetChunks chunks, each a multiple of kAutoAlignment bytes and
// within kAutoMinChunk .. kAutoMaxChunk. A heuristic, not tuned to any particular disk.
constexpr std::uintmax_t kAutoTargetChunks = 64;
constexpr std::uintmax_t kAutoAlignment = 4096;
constexpr std::uintmax_t kAutoMinChunk = std::uintmax_t{64} << 10;
constexpr std::uintmax_t kAutoMaxChunk = std::uintmax_t{4} << 20;

// Lua numbers are doubles; whole numbers are exact up to 2^53.
constexpr double kMaxExactInteger = 9007199254740992.0;

struct ChunkSpec {
    enum class Kind { Auto, Count, Size };
    Kind kind = Kind::Auto;
    std::uintmax_t value = 0; // the count or the size, depending on kind
};

struct ChunkPlan {
    std::uintmax_t size = 0;          // file size in bytes
    std::uintmax_t chunkSize = 1;     // bytes per chunk; the last one may be shorter
    std::uintmax_t chunkCount = 0;
    std::uintmax_t lastChunkSize = 0; // 0 for an empty file
};

std::uintmax_t ceilDiv(std::uintmax_t a, std::uintmax_t b) {
    return a / b + (a % b != 0 ? 1 : 0);
}

// The ways to pick a chunk size. They only do arithmetic. A literal size needs no
// function: checkChunkSpec() validates it and planChunks() uses it as it is.

// Overall size divided by the target count, rounded up so that the result never has
// more chunks than asked for. count must be >= 1. May return 0 (empty file).
std::uintmax_t chunkSizeForCount(std::uintmax_t fileSize, std::uintmax_t count) {
    return ceilDiv(fileSize, count);
}

// Roughly fileSize / kAutoTargetChunks, rounded up to the alignment, then clamped.
// Small files get one chunk of 64 KiB (or fewer); huge files get 4 MiB chunks.
std::uintmax_t chunkSizeAuto(std::uintmax_t fileSize) {
    const std::uintmax_t wanted = ceilDiv(fileSize / kAutoTargetChunks, kAutoAlignment) * kAutoAlignment;
    return std::clamp(wanted, kAutoMinChunk, kAutoMaxChunk);
}

// Turns a spec into a plan for a file of fileSize bytes. Returns false and sets reason if
// the spec cannot be honoured.
bool planChunks(std::uintmax_t fileSize, const ChunkSpec& spec, ChunkPlan& plan,
                std::string& reason) {
    std::uintmax_t chunk = 0;
    switch (spec.kind) {
    case ChunkSpec::Kind::Count:
        chunk = std::max<std::uintmax_t>(chunkSizeForCount(fileSize, spec.value), 1);
        break;
    case ChunkSpec::Kind::Size:
        chunk = spec.value;
        break;
    default:
        chunk = chunkSizeAuto(fileSize);
        break;
    }

    // Only reachable with { count = n } on a very large file: a literal size is checked
    // when the spec is read, and "auto" stays far below the limit.
    if (chunk > kMaxChunkSize) {
        reason = "a file of " + std::to_string(fileSize) + " bytes needs at least " +
                 std::to_string(ceilDiv(fileSize, kMaxChunkSize)) +
                 " chunks (one chunk cannot exceed " + std::to_string(kMaxChunkSize) + " bytes)";
        return false;
    }

    plan.size = fileSize;
    plan.chunkSize = chunk;
    plan.chunkCount = ceilDiv(fileSize, chunk);
    plan.lastChunkSize = plan.chunkCount == 0 ? 0 : fileSize - (plan.chunkCount - 1) * chunk;
    return true;
}

// The size of a regular file. Pipes, devices and sockets have no meaningful size, so
// they are refused instead of being "streamed" with a made-up plan.
bool regularFileSize(const stdfs::path& path, std::uintmax_t& size, std::string& reason) {
    std::error_code ec;
    const stdfs::file_status status = stdfs::status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        reason = reasonOf(std::errc::no_such_file_or_directory);
        return false;
    }
    if (stdfs::is_directory(status)) {
        reason = reasonOf(std::errc::is_a_directory);
        return false;
    }
    if (ec) {
        reason = describe(ec);
        return false;
    }
    if (!stdfs::is_regular_file(status)) {
        reason = "not a regular file";
        return false;
    }
    size = stdfs::file_size(path, ec);
    if (ec) {
        reason = describe(ec);
        return false;
    }
    return true;
}

struct Reader {
    std::ifstream in;
    std::string path;        // as the script wrote it, for error messages
    ChunkPlan plan;
    std::uintmax_t next = 0; // 0-based index of the chunk read() returns next
    std::string buffer;      // holds the chunk just read; reused, so no allocation per chunk
};

// What the Lua userdata holds. The Reader lives on the heap and the userdata only has a
// pointer to it (null until the Reader exists), so the userdata is destroyed correctly
// whatever happens while it is being set up.
struct ReaderHandle {
    Reader* reader;
};

constexpr const char* kReaderMeta = "sonata.fs.Reader";

void destroyReaderHandle(lua_State* L, void* memory) {
    ReaderHandle* handle = static_cast<ReaderHandle*>(memory);
    delete handle->reader;
    handle->reader = nullptr;
}

bool openReader(Reader& reader, std::string_view pathText, const ChunkSpec& spec,
                std::string& reason) {
    const stdfs::path path = toPath(pathText);

    std::uintmax_t size = 0;
    if (!regularFileSize(path, size, reason)) {
        return false;
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
        reason = "file is too large";
        return false;
    }

    ChunkPlan plan;
    if (!planChunks(size, spec, plan, reason)) {
        return false;
    }

    reader.in.open(path, std::ios::binary);
    if (!reader.in) {
        reason = errnoReason(errno);
        return false;
    }
    reader.path.assign(pathText);
    reader.plan = plan;
    reader.next = 0;
    return true;
}

// Reads chunk `index` (0-based, below plan.chunkCount) into reader.buffer. Every chunk is
// read from its own offset, so the order in which chunks are asked for does not matter.
bool readChunkBytes(Reader& reader, std::uintmax_t index, std::string& reason) {
    const std::uintmax_t offset = index * reader.plan.chunkSize; // < size, cannot overflow
    const std::uintmax_t length = std::min(reader.plan.chunkSize, reader.plan.size - offset);

    try {
        reader.buffer.resize(static_cast<std::size_t>(length));
    } catch (const std::exception&) {
        reason = "out of memory";
        return false;
    }

    reader.in.clear();
    reader.in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!reader.in) {
        reason = "seek error";
        return false;
    }
    reader.in.read(reader.buffer.data(), static_cast<std::streamsize>(length));
    if (static_cast<std::uintmax_t>(reader.in.gcount()) != length) {
        reason = reader.in.bad() ? "read error" : "file was truncated while being read";
        return false;
    }
    return true;
}

bool wholeNumber(double value, double min, double max) {
    return value >= min && value <= max && value == std::floor(value); // false for NaN
}

// nil / "auto" / { count = n } / { size = n }. Raises on anything else.
ChunkSpec checkChunkSpec(lua_State* L, int arg) {
    ChunkSpec spec;
    if (lua_isnoneornil(L, arg)) {
        return spec;
    }
    if (lua_type(L, arg) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = luaL_checklstring(L, arg, &length);
        luaL_argcheck(L, std::string_view(text, length) == "auto", arg,
                      "chunk spec must be \"auto\", { count = n } or { size = n }");
        return spec;
    }

    luaL_checktype(L, arg, LUA_TTABLE);
    lua_getfield(L, arg, "count");
    lua_getfield(L, arg, "size");
    const bool hasCount = !lua_isnil(L, -2);
    const bool hasSize = !lua_isnil(L, -1);
    luaL_argcheck(L, hasCount != hasSize, arg,
                  "chunk spec needs exactly one of 'count' or 'size'");

    const int valueIndex = hasCount ? -2 : -1;
    luaL_argcheck(L, lua_type(L, valueIndex) == LUA_TNUMBER, arg,
                  "chunk spec 'count' or 'size' must be a number");
    const double value = luaL_checknumber(L, valueIndex);
    if (hasCount) {
        luaL_argcheck(L, wholeNumber(value, 1, kMaxExactInteger), arg,
                      "'count' must be a whole number of at least 1");
        spec.kind = ChunkSpec::Kind::Count;
    } else {
        luaL_argcheck(L, wholeNumber(value, 1, static_cast<double>(kMaxChunkSize)), arg,
                      "'size' must be a whole number from 1 to fs.maxChunkSize");
        spec.kind = ChunkSpec::Kind::Size;
    }
    spec.value = static_cast<std::uintmax_t>(value);
    lua_pop(L, 2);
    return spec;
}

void pushPlan(lua_State* L, const ChunkPlan& plan) {
    lua_createtable(L, 0, 4);
    setNumberField(L, "size", static_cast<double>(plan.size));
    setNumberField(L, "chunkSize", static_cast<double>(plan.chunkSize));
    setNumberField(L, "chunkCount", static_cast<double>(plan.chunkCount));
    setNumberField(L, "lastChunkSize", static_cast<double>(plan.lastChunkSize));
}

int chunkPlanImpl(lua_State* L, bool fromPath, std::string_view pathText, std::uintmax_t size,
                  const ChunkSpec& spec) {
    std::string reason;
    if (fromPath && !regularFileSize(toPath(pathText), size, reason)) {
        return fail(L, "chunkPlan", quote(pathText), reason);
    }

    ChunkPlan plan;
    if (!planChunks(size, spec, plan, reason)) {
        return fail(L, "chunkPlan", fromPath ? quote(pathText) : std::string(), reason);
    }
    pushPlan(L, plan);
    return 1;
}

int openReaderImpl(lua_State* L, std::string_view pathText, const ChunkSpec& spec) {
    // The userdata goes first and starts out empty and valid; it is left on the stack below
    // an error message if anything later fails, and the garbage collector cleans it up.
    ReaderHandle* handle = static_cast<ReaderHandle*>(
        lua_newuserdatadtor(L, sizeof(ReaderHandle), destroyReaderHandle));
    handle->reader = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, kReaderMeta);
    lua_setmetatable(L, -2);

    try {
        handle->reader = new Reader();
    } catch (const std::bad_alloc&) {
        return fail(L, "openReader", quote(pathText), "out of memory");
    }

    std::string reason;
    if (!openReader(*handle->reader, pathText, spec, reason)) {
        return fail(L, "openReader", quote(pathText), reason);
    }
    return 1; // the reader is on top of the stack
}

int failClosed(lua_State* L, const char* op, const Reader& reader) {
    return fail(L, op, quote(reader.path), "reader is closed");
}

// Pushes chunk `index` (0-based). The reader must be open and the index in range.
int pushChunk(lua_State* L, const char* op, Reader& reader, std::uintmax_t index) {
    std::string reason;
    if (!readChunkBytes(reader, index, reason)) {
        return fail(L, op, quote(reader.path), reason);
    }
    pushString(L, reader.buffer);
    return 1;
}

int readNextImpl(lua_State* L, Reader& reader) {
    if (!reader.in.is_open()) {
        return failClosed(L, "read", reader);
    }
    if (reader.next >= reader.plan.chunkCount) {
        lua_pushnil(L);
        return 1;
    }
    const int results = pushChunk(L, "read", reader, reader.next);
    if (results >= 0) {
        ++reader.next; // a failed read can be retried
    }
    return results;
}

int readChunkImpl(lua_State* L, Reader& reader, std::uintmax_t index) {
    if (!reader.in.is_open()) {
        return failClosed(L, "readChunk", reader);
    }
    return pushChunk(L, "readChunk", reader, index);
}

// Stateless iterator behind reader:chunks(): given the previous index it returns the next
// (index, chunk) pair, or nil. All progress lives in the control variable of the loop.
int nextChunkImpl(lua_State* L, Reader& reader, double previous) {
    if (!reader.in.is_open()) {
        return failClosed(L, "chunks", reader);
    }
    if (previous >= static_cast<double>(reader.plan.chunkCount)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, previous + 1);
    const int results = pushChunk(L, "chunks", reader, static_cast<std::uintmax_t>(previous));
    return results < 0 ? results : 2;
}

// Bindings /////////////////////////////////

// fs.readFile(path: string): string
int fsReadFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, readFileImpl(L, path));
}

// fs.writeFile(path: string, contents: string)
int fsWriteFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const std::string_view data = checkData(L, 2);
    return finish(L, writeImpl(L, "writeFile", path, data, false));
}

// fs.appendFile(path: string, contents: string)
int fsAppendFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const std::string_view data = checkData(L, 2);
    return finish(L, writeImpl(L, "appendFile", path, data, true));
}

// fs.copyFile(from: string, to: string, overwrite: boolean?)
int fsCopyFile(lua_State* L) {
    const std::string_view from = checkPath(L, 1);
    const std::string_view to = checkPath(L, 2);
    const bool overwrite = optBoolean(L, 3, true);
    return finish(L, copyFileImpl(L, from, to, overwrite));
}

// fs.removeFile(path: string)
int fsRemoveFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, removeFileImpl(L, path));
}

// fs.move(from: string, to: string)
int fsMove(lua_State* L) {
    const std::string_view from = checkPath(L, 1);
    const std::string_view to = checkPath(L, 2);
    return finish(L, moveImpl(L, from, to));
}

// fs.makeDir(path: string, recursive: boolean?)
int fsMakeDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, makeDirImpl(L, path, recursive));
}

// fs.removeDir(path: string, recursive: boolean?)
int fsRemoveDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, removeDirImpl(L, path, recursive));
}

// fs.listDir(path: string, recursive: boolean?): { string }
int fsListDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, listDirImpl(L, "listDir", path, recursive, false));
}

// fs.readDir(path: string, recursive: boolean?): { { name: string, kind: string } }
int fsReadDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, listDirImpl(L, "readDir", path, recursive, true));
}

// fs.copyDir(from: string, to: string, overwrite: boolean?)
int fsCopyDir(lua_State* L) {
    const std::string_view from = checkPath(L, 1);
    const std::string_view to = checkPath(L, 2);
    const bool overwrite = optBoolean(L, 3, true);
    return finish(L, copyDirImpl(L, from, to, overwrite));
}

// fs.exists(path: string): boolean
int fsExists(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::exists(stdfs::status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.isFile(path: string): boolean
int fsIsFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::is_regular_file(stdfs::status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.isDir(path: string): boolean
int fsIsDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::is_directory(stdfs::status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.isSymlink(path: string): boolean
int fsIsSymlink(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::is_symlink(stdfs::symlink_status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.stat(path: string): { kind: string, size: number, modified: number, mode: number }
int fsStat(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, statImpl(L, "stat", path, true));
}

// fs.lstat(path: string): { kind: string, size: number, modified: number, mode: number }
int fsLstat(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, statImpl(L, "lstat", path, false));
}

// fs.realPath(path: string): string
int fsRealPath(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, realPathImpl(L, path));
}

// fs.symlink(target: string, link: string)
int fsSymlink(lua_State* L) {
    const std::string_view target = checkPath(L, 1);
    const std::string_view link = checkPath(L, 2);
    return finish(L, symlinkImpl(L, target, link));
}

// fs.readLink(path: string): string
int fsReadLink(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, readLinkImpl(L, path));
}

// fs.chmod(path: string, mode: number)
int fsChmod(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const int mode = luaL_checkinteger(L, 2);
    luaL_argcheck(L, mode >= 0 && mode <= 4095, 2, "mode must be an integer between 0 and 4095");
    return finish(L, chmodImpl(L, path, mode));
}

// fs.cwd(): string
int fsCwd(lua_State* L) {
    return finish(L, cwdImpl(L));
}

// fs.chdir(path: string)
int fsChdir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, chdirImpl(L, path));
}

// fs.tempDir(): string
int fsTempDir(lua_State* L) {
    return finish(L, tempDirImpl(L));
}

// fs.makeTempDir(prefix: string?): string
int fsMakeTempDir(lua_State* L) {
    std::size_t length = 0;
    const char* prefix = luaL_optlstring(L, 1, "sonata-", &length);
    const std::string_view text(prefix, length);
    luaL_argcheck(L, text.find_first_of("/\\") == std::string_view::npos &&
                         std::strlen(prefix) == length,
                  1, "prefix must not contain path separators or NUL bytes");
    return finish(L, makeTempDirImpl(L, text));
}

// fs.chunkPlan(sizeOrPath: number | string, spec: ChunkSpec?):
//     { size: number, chunkSize: number, chunkCount: number, lastChunkSize: number }
int fsChunkPlan(lua_State* L) {
    const bool fromPath = lua_type(L, 1) != LUA_TNUMBER;
    std::string_view path;
    std::uintmax_t size = 0;
    if (fromPath) {
        path = checkPath(L, 1);
    } else {
        const double bytes = luaL_checknumber(L, 1);
        luaL_argcheck(L, wholeNumber(bytes, 0, kMaxExactInteger), 1,
                      "file size must be a whole number of bytes");
        size = static_cast<std::uintmax_t>(bytes);
    }
    const ChunkSpec spec = checkChunkSpec(L, 2);
    return finish(L, chunkPlanImpl(L, fromPath, path, size, spec));
}

// fs.openReader(path: string, spec: ChunkSpec?): Reader
int fsOpenReader(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const ChunkSpec spec = checkChunkSpec(L, 2);
    return finish(L, openReaderImpl(L, path, spec));
}

// Reader methods ///////////////////////////

ReaderHandle* checkHandle(lua_State* L, int arg) {
    return static_cast<ReaderHandle*>(luaL_checkudata(L, arg, kReaderMeta));
}

// reader:read(): string?
int fsReaderRead(lua_State* L) {
    ReaderHandle* handle = checkHandle(L, 1);
    return finish(L, readNextImpl(L, *handle->reader));
}

// reader:readChunk(index: number): string
int fsReaderReadChunk(lua_State* L) {
    ReaderHandle* handle = checkHandle(L, 1);
    const double index = luaL_checknumber(L, 2);
    luaL_argcheck(L, wholeNumber(index, 1, static_cast<double>(handle->reader->plan.chunkCount)),
                  2, "chunk index out of range (chunks are numbered from 1)");
    return finish(L, readChunkImpl(L, *handle->reader, static_cast<std::uintmax_t>(index) - 1));
}

// The iterator function returned by reader:chunks(): (reader, previousIndex) -> (index, chunk)
int fsChunkIterator(lua_State* L) {
    ReaderHandle* handle = checkHandle(L, 1);
    const double previous = luaL_checknumber(L, 2);
    luaL_argcheck(L, wholeNumber(previous, 0, kMaxExactInteger), 2,
                  "previous chunk index must be a whole number");
    return finish(L, nextChunkImpl(L, *handle->reader, previous));
}

// reader:chunks(): (iterator, reader, 0), for `for index, chunk in reader:chunks() do`
int fsReaderChunks(lua_State* L) {
    checkHandle(L, 1);
    lua_pushcfunction(L, fsChunkIterator, "chunkIterator");
    lua_pushvalue(L, 1);
    lua_pushnumber(L, 0);
    return 3;
}

// reader:rewind()
int fsReaderRewind(lua_State* L) {
    ReaderHandle* handle = checkHandle(L, 1);
    handle->reader->next = 0;
    return 0;
}

// reader:plan(): { size, chunkSize, chunkCount, lastChunkSize }
int fsReaderPlan(lua_State* L) {
    ReaderHandle* handle = checkHandle(L, 1);
    pushPlan(L, handle->reader->plan);
    return 1;
}

// reader:close()
int fsReaderClose(lua_State* L) {
    ReaderHandle* handle = checkHandle(L, 1);
    Reader& reader = *handle->reader;
    if (reader.in.is_open()) {
        reader.in.close();
    }
    std::string().swap(reader.buffer); // give the chunk memory back too
    return 0;
}

constexpr NativeFunction kReaderMethods[] = {
    {"read", fsReaderRead},
    {"readChunk", fsReaderReadChunk},
    {"chunks", fsReaderChunks},
    {"rewind", fsReaderRewind},
    {"plan", fsReaderPlan},
    {"close", fsReaderClose},
};

// Creates the reader's metatable once per Lua state; leaves the stack as it found it.
void registerReaderType(lua_State* L) {
    if (luaL_newmetatable(L, kReaderMeta) != 0) {
        lua_createtable(L, 0, static_cast<int>(std::size(kReaderMethods)));
        setFunctions(L, kReaderMethods);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        setStringField(L, "__type", "FsReader"); // what typeof(reader) reports
        lua_setreadonly(L, -1, 1);
    }
    lua_pop(L, 1);
}

// Finding files ////////////////////////////

// One entry of a directory, as the walkers see it.
struct DirEntry {
    std::string name;
    const char* kind;
    stdfs::path path;
};

// The entries of "dir", sorted by name. Unreadable directories give none.
std::vector<DirEntry> listSorted(const stdfs::path& dir) {
    std::vector<DirEntry> entries;
    std::error_code ec;
    for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code statusEc;
        const stdfs::file_status status = it->symlink_status(statusEc);
        entries.push_back(DirEntry{toUtf8(it->path().filename()), statusEc ? "other" : kindOf(status), it->path()});
    }
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return entries;
}

bool isDirectoryTarget(const stdfs::path& path) {
    std::error_code ec;
    return stdfs::is_directory(path, ec); // follows links
}

// ---- glob patterns

bool hasMagic(std::string_view segment) {
    return segment.find_first_of("*?[\\") != std::string_view::npos;
}

std::size_t utf8Size(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

std::uint32_t decodeAt(std::string_view text, std::size_t at, std::size_t& length) {
    const auto lead = static_cast<unsigned char>(text[at]);
    length = std::min(utf8Size(lead), text.size() - at);
    if (length == 1) {
        return lead;
    }
    std::uint32_t code = lead & (0xFF >> (length + 1));
    for (std::size_t k = 1; k < length; ++k) {
        code = (code << 6) | (static_cast<unsigned char>(text[at + k]) & 0x3F);
    }
    return code;
}

// Matches the character class starting at pat[p] ('[') against "c". Sets "end" past
// the ']'. Returns -1 if the class has no closing ']' (then '[' is an ordinary character).
int matchClass(std::string_view pat, std::size_t p, std::uint32_t c, std::size_t& end) {
    std::size_t i = p + 1;
    const bool negate = i < pat.size() && (pat[i] == '!' || pat[i] == '^');
    if (negate) {
        ++i;
    }
    bool matched = false;
    bool first = true;
    while (i < pat.size() && (pat[i] != ']' || first)) {
        first = false;
        std::size_t length = 0;
        if (pat[i] == '\\' && i + 1 < pat.size()) {
            ++i;
        }
        const std::uint32_t low = decodeAt(pat, i, length);
        i += length;
        std::uint32_t high = low;
        if (i + 1 < pat.size() && pat[i] == '-' && pat[i + 1] != ']') {
            ++i;
            if (pat[i] == '\\' && i + 1 < pat.size()) {
                ++i;
            }
            high = decodeAt(pat, i, length);
            i += length;
        }
        matched = matched || (c >= low && c <= high);
    }
    if (i >= pat.size()) {
        return -1;
    }
    end = i + 1;
    return matched != negate ? 1 : 0;
}

// Matches one path segment: "*", "?", "[...]" and backslash escapes.
bool matchSegment(std::string_view pat, std::string_view name) {
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t starP = std::string_view::npos;
    std::size_t starN = 0;

    while (n < name.size()) {
        if (p < pat.size()) {
            const char c = pat[p];
            std::size_t length = 0;
            if (c == '*') {
                starP = p++;
                starN = n;
                continue;
            }
            if (c == '?') {
                decodeAt(name, n, length);
                ++p;
                n += length;
                continue;
            }
            if (c == '[') {
                std::size_t end = 0;
                const std::uint32_t code = decodeAt(name, n, length);
                const int result = matchClass(pat, p, code, end);
                if (result == 1) {
                    p = end;
                    n += length;
                    continue;
                }
                if (result == -1 && name[n] == '[') { // no closing ']': a literal '['
                    ++p;
                    ++n;
                    continue;
                }
            } else if (c == '\\' && p + 1 < pat.size()) {
                if (pat[p + 1] == name[n]) {
                    p += 2;
                    ++n;
                    continue;
                }
            } else if (c == name[n]) {
                ++p;
                ++n;
                continue;
            }
        }
        if (starP == std::string_view::npos) {
            return false;
        }
        p = starP + 1;
        std::size_t length = 0;
        decodeAt(name, starN, length);
        starN += length;
        n = starN;
    }
    while (p < pat.size() && pat[p] == '*') {
        ++p;
    }
    return p == pat.size();
}

std::vector<std::string_view> splitSegments(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

bool hidden(std::string_view segment) {
    return !segment.empty() && segment[0] == '.';
}

bool matchSegments(const std::vector<std::string_view>& pat, std::size_t pi, const std::vector<std::string_view>& path,
                   std::size_t ni, bool dot) {
    if (pi == pat.size()) {
        return ni == path.size();
    }
    if (pat[pi] == "**") {
        for (std::size_t k = ni; k <= path.size(); ++k) {
            if (matchSegments(pat, pi + 1, path, k, dot)) {
                return true;
            }
            if (k < path.size() && !dot && hidden(path[k])) {
                return false; // "**" does not enter hidden directories
            }
        }
        return false;
    }
    if (ni == path.size()) {
        return false;
    }
    if (!dot && hidden(path[ni]) && !hidden(pat[pi])) {
        return false;
    }
    return matchSegment(pat[pi], path[ni]) && matchSegments(pat, pi + 1, path, ni + 1, dot);
}

// "a{b,c{d,e}}f" -> "abf", "acdf", "acef". Braces without a matching '}' stay as they are.
void expandBraces(const std::string& pattern, std::vector<std::string>& out) {
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            ++i;
            continue;
        }
        if (pattern[i] != '{') {
            continue;
        }
        int depth = 0;
        std::vector<std::size_t> commas;
        std::size_t close = std::string::npos;
        for (std::size_t j = i; j < pattern.size(); ++j) {
            if (pattern[j] == '\\') {
                ++j;
            } else if (pattern[j] == '{') {
                ++depth;
            } else if (pattern[j] == '}' && --depth == 0) {
                close = j;
                break;
            } else if (pattern[j] == ',' && depth == 1) {
                commas.push_back(j);
            }
        }
        if (close == std::string::npos) {
            break;
        }
        const std::string prefix = pattern.substr(0, i);
        const std::string suffix = pattern.substr(close + 1);
        std::size_t start = i + 1;
        commas.push_back(close);
        for (const std::size_t comma : commas) {
            expandBraces(prefix + pattern.substr(start, comma - start) + suffix, out);
            start = comma + 1;
        }
        return;
    }
    out.push_back(pattern);
}

// Paths compared with patterns use "/" (and no leading "./").
std::string normalizeSlashes(std::string_view text) {
    std::string out(text);
#ifdef _WIN32
    std::replace(out.begin(), out.end(), '\\', '/');
#endif
    while (out.size() >= 2 && out[0] == '.' && out[1] == '/') {
        out.erase(0, 2);
    }
    return out;
}

// Splits an absolute pattern's root ("/", "C:/") off. Empty for relative patterns.
std::string patternRoot(const std::string& pattern) {
    if (!pattern.empty() && pattern[0] == '/') {
        return "/";
    }
    if (pattern.size() >= 3 && std::isalpha(static_cast<unsigned char>(pattern[0])) && pattern[1] == ':' && pattern[2] == '/') {
        return pattern.substr(0, 3);
    }
    return {};
}

struct GlobOptions {
    stdfs::path cwd = ".";
    bool dot = false;
    bool absolute = false;
    const char* kind = nullptr; // "file" / "directory" / nullptr = anything
};

// Walks "dir" (relative path "rel" from the glob's base), testing every entry.
void globWalk(const stdfs::path& dir, const std::string& rel, std::size_t depth, std::size_t maxDepth,
              const std::vector<std::string_view>& segments, const GlobOptions& options, const std::string& prefix,
              std::set<std::string>& found) {
    for (const DirEntry& entry : listSorted(dir)) {
        const std::string path = rel.empty() ? entry.name : rel + "/" + entry.name;
        const std::vector<std::string_view> parts = splitSegments(path);
        const bool isDir = std::strcmp(entry.kind, "directory") == 0 ||
                           (std::strcmp(entry.kind, "symlink") == 0 && isDirectoryTarget(entry.path));

        if (matchSegments(segments, 0, parts, 0, options.dot)) {
            const bool kindOk = options.kind == nullptr || (std::strcmp(options.kind, "directory") == 0 ? isDir : !isDir && std::strcmp(entry.kind, "other") != 0);
            if (kindOk) {
                found.insert(prefix + path);
            }
        }
        const bool enter = std::strcmp(entry.kind, "directory") == 0 && depth + 1 < maxDepth &&
                           (options.dot || !hidden(entry.name));
        if (enter) {
            globWalk(entry.path, path, depth + 1, maxDepth, segments, options, prefix, found);
        }
    }
}

void globOne(const std::string& rawPattern, const GlobOptions& options, std::set<std::string>& found) {
    const std::string pattern = normalizeSlashes(rawPattern);
    const std::string root = patternRoot(pattern);
    const std::vector<std::string_view> all = splitSegments(std::string_view(pattern).substr(root.size()));

    // Leading segments without wildcards are a plain directory to start from.
    std::size_t literal = 0;
    while (literal < all.size() && !hasMagic(all[literal]) && all[literal] != "**") {
        ++literal;
    }

    std::string prefix = root;
    for (std::size_t i = 0; i < literal; ++i) {
        prefix += std::string(all[i]);
        prefix += '/';
    }

    const stdfs::path base = root.empty() ? options.cwd / toPath(prefix) : toPath(prefix);
    std::string shownPrefix = prefix;
    if (options.absolute && root.empty()) {
        std::error_code ec;
        const stdfs::path absoluteBase = stdfs::absolute(options.cwd, ec).lexically_normal();
        shownPrefix = toGenericUtf8(withoutTrailingSeparator(absoluteBase)) + "/" + prefix;
    }

    if (literal == all.size()) { // no wildcard at all: it either exists or not
        std::error_code ec;
        const stdfs::path target = root.empty() ? options.cwd / toPath(pattern) : toPath(pattern);
        if (stdfs::exists(stdfs::symlink_status(target, ec))) {
            const bool isDir = stdfs::is_directory(target, ec);
            if (options.kind == nullptr || (std::strcmp(options.kind, "directory") == 0) == isDir) {
                std::string shown = shownPrefix;
                if (!shown.empty() && shown.back() == '/' && shown != root) {
                    shown.pop_back();
                }
                found.insert(shown.empty() ? pattern : shown);
            }
        }
        return;
    }

    const std::vector<std::string_view> rest(all.begin() + static_cast<std::ptrdiff_t>(literal), all.end());
    const bool deep = std::find(rest.begin(), rest.end(), std::string_view("**")) != rest.end();
    const std::size_t maxDepth = deep ? std::numeric_limits<std::size_t>::max() : rest.size();

    std::error_code ec;
    if (stdfs::is_directory(base, ec)) {
        globWalk(base, "", 0, maxDepth, rest, options, shownPrefix, found);
    }
}

// ---- fs.walk

struct WalkFrame {
    std::vector<DirEntry> entries;
    std::size_t next = 0;
    std::string rel; // of the directory, "" for the root
    int depth = 0;   // of its entries
};

struct Walker {
    stdfs::path root;
    std::string rootText;
    std::vector<WalkFrame> stack;
    double maxDepth = std::numeric_limits<double>::infinity();
    bool follow = false;
    bool dirs = true;
    std::set<stdfs::path> visited; // real paths of the directories entered (with follow: no loops)
};

constexpr const char* kWalkerMeta = "sonata.fs.Walker";

void pushWalkEntry(lua_State* L, const Walker& walker, const DirEntry& entry, const std::string& rel, int depth) {
    lua_createtable(L, 0, 5);
    setStringField(L, "path", rel);
    std::string full = walker.rootText;
    if (!full.empty() && full.back() != '/') {
        full += '/';
    }
    full += rel;
    setStringField(L, "fullPath", full);
    setStringField(L, "name", entry.name);
    setStringField(L, "kind", entry.kind);
    setNumberField(L, "depth", depth);
}

// The iterator behind fs.walk. Upvalue 1: the Walker userdata; upvalue 2: the filter (or nil).
int walkNext(lua_State* L) {
    auto* walker = static_cast<Walker*>(lua_touserdata(L, lua_upvalueindex(1)));
    const bool hasFilter = lua_isfunction(L, lua_upvalueindex(2));

    while (!walker->stack.empty()) {
        WalkFrame& frame = walker->stack.back();
        if (frame.next >= frame.entries.size()) {
            walker->stack.pop_back();
            continue;
        }
        const DirEntry entry = frame.entries[frame.next++];
        const std::string rel = frame.rel.empty() ? entry.name : frame.rel + "/" + entry.name;
        const int depth = frame.depth;

        const bool isDir = std::strcmp(entry.kind, "directory") == 0 ||
                           (walker->follow && std::strcmp(entry.kind, "symlink") == 0 && isDirectoryTarget(entry.path));

        pushWalkEntry(L, *walker, entry, rel, depth);
        if (hasFilter) {
            lua_pushvalue(L, lua_upvalueindex(2));
            lua_pushvalue(L, -2);
            lua_call(L, 1, 1);
            const bool keep = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            if (!keep) {
                lua_pop(L, 1);
                continue;
            }
        }

        if (isDir && depth < walker->maxDepth) {
            std::error_code ec;
            const stdfs::path real = stdfs::canonical(entry.path, ec);
            if (!ec && walker->visited.insert(real).second) {
                walker->stack.push_back(WalkFrame{listSorted(entry.path), 0, rel, depth + 1});
            }
        }
        if (isDir && !walker->dirs) {
            lua_pop(L, 1);
            continue;
        }
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// ---- Watching

struct FileState {
    const char* kind;
    std::uintmax_t size;
    stdfs::file_time_type modified;
};

using Snapshot = std::map<std::string, FileState>; // relative path ("" = the root itself) -> state

struct WatchEvent {
    const char* kind;
    std::string path;
    bool isDir;
};

struct WatchState {
    std::mutex mutex;
    stdfs::path root;
    std::string rootText;
    bool recursive = true;
    double interval = 0.25;
    Snapshot last;
    std::deque<WatchEvent> pending;
    bool closed = false;
};

struct Watcher {
    std::shared_ptr<WatchState> state;
};

constexpr const char* kWatcherMeta = "sonata.fs.Watcher";

FileState stateOf(const stdfs::path& path, const stdfs::file_status& status) {
    std::error_code ec;
    FileState state{kindOf(status), 0, stdfs::file_time_type{}};
    if (status.type() == stdfs::file_type::regular) {
        state.size = stdfs::file_size(path, ec);
    }
    state.modified = stdfs::last_write_time(path, ec);
    return state;
}

void scanInto(const stdfs::path& dir, const std::string& rel, bool recursive, Snapshot& out) {
    std::error_code ec;
    for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code statusEc;
        const stdfs::file_status status = it->symlink_status(statusEc);
        if (statusEc) {
            continue;
        }
        const std::string path = rel.empty() ? toUtf8(it->path().filename()) : rel + "/" + toUtf8(it->path().filename());
        out[path] = stateOf(it->path(), status);
        if (recursive && status.type() == stdfs::file_type::directory) {
            scanInto(it->path(), path, true, out);
        }
    }
}

Snapshot scan(const WatchState& state) {
    Snapshot snapshot;
    std::error_code ec;
    const stdfs::file_status status = stdfs::symlink_status(state.root, ec);
    if (ec || !stdfs::exists(status)) {
        return snapshot;
    }
    snapshot[""] = stateOf(state.root, status);
    if (status.type() == stdfs::file_type::directory) {
        scanInto(state.root, "", state.recursive, snapshot);
    }
    return snapshot;
}

std::string eventPath(const WatchState& state, const std::string& rel) {
    if (rel.empty()) {
        return state.rootText;
    }
    std::string path = state.rootText;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    return path + rel;
}

// Compares a new snapshot with the last one and queues the differences. Caller holds the mutex.
void diffInto(WatchState& state, Snapshot next) {
    for (const auto& [rel, now] : next) {
        const auto old = state.last.find(rel);
        const bool isDir = std::strcmp(now.kind, "directory") == 0;
        if (old == state.last.end()) {
            state.pending.push_back(WatchEvent{"create", eventPath(state, rel), isDir});
        } else if (std::strcmp(old->second.kind, now.kind) != 0) {
            state.pending.push_back(WatchEvent{"remove", eventPath(state, rel), std::strcmp(old->second.kind, "directory") == 0});
            state.pending.push_back(WatchEvent{"create", eventPath(state, rel), isDir});
        } else if (!isDir && (old->second.size != now.size || old->second.modified != now.modified)) {
            state.pending.push_back(WatchEvent{"modify", eventPath(state, rel), false});
        }
    }
    for (const auto& [rel, old] : state.last) {
        if (next.find(rel) == next.end()) {
            state.pending.push_back(WatchEvent{"remove", eventPath(state, rel), std::strcmp(old.kind, "directory") == 0});
        }
    }
    state.last = std::move(next);
}

void pushEvent(lua_State* L, const WatchEvent& event) {
    lua_createtable(L, 0, 3);
    setStringField(L, "kind", event.kind);
    setStringField(L, "path", event.path);
    lua_pushboolean(L, event.isDir);
    lua_setfield(L, -2, "isDir");
}

Watcher* checkWatcher(lua_State* L) {
    return static_cast<Watcher*>(luaL_checkudata(L, 1, kWatcherMeta));
}

// watcher:next: stack 1 watcher, 2 deadline (scheduler clock)
int watchNextStep(lua_State* L);

int watchNextK(lua_State* L, int) {
    return watchNextStep(L);
}

int watchNextStep(lua_State* L) {
    lua_settop(L, 2);
    const std::shared_ptr<WatchState> state = checkWatcher(L)->state;
    sched::Scheduler& scheduler = sched::Scheduler::of(L);

    for (;;) {
        double wait = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->pending.empty()) {
                const WatchEvent event = state->pending.front();
                state->pending.pop_front();
                pushEvent(L, event);
                return 1;
            }
            if (state->closed) {
                lua_pushnil(L);
                return 1;
            }
            const double left = lua_tonumber(L, 2) - scheduler.now();
            if (left <= 0) {
                lua_pushnil(L);
                return 1;
            }
            wait = std::min(state->interval, left);
        }

        const int r = scheduler.await(L, [state, wait]() -> sched::Completion {
            std::this_thread::sleep_for(std::chrono::duration<double>(wait));
            Snapshot next = scan(*state);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->closed) {
                diffInto(*state, std::move(next));
            }
            return [](lua_State*) { return 0; };
        }, watchNextK);
        if (r != sched::kInline) {
            return r;
        }
        lua_settop(L, 2);
    }
}

// watcher:next(timeout: number?): Event?
int fsWatcherNext(lua_State* L) {
    lua_settop(L, 2);
    checkWatcher(L);
    double deadline = std::numeric_limits<double>::infinity();
    if (!lua_isnil(L, 2)) {
        const double timeout = luaL_checknumber(L, 2);
        luaL_argcheck(L, timeout >= 0, 2, "timeout must not be negative");
        deadline = sched::Scheduler::of(L).now() + timeout;
    }
    lua_pushnumber(L, deadline);
    lua_replace(L, 2);
    return watchNextStep(L);
}

// watcher:poll(): { Event }
int fsWatcherPoll(lua_State* L) {
    const std::shared_ptr<WatchState> state = checkWatcher(L)->state;
    std::deque<WatchEvent> events;
    {
        Snapshot next = scan(*state);
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->closed) {
            diffInto(*state, std::move(next));
        }
        events.swap(state->pending);
    }
    lua_createtable(L, static_cast<int>(events.size()), 0);
    for (std::size_t i = 0; i < events.size(); ++i) {
        pushEvent(L, events[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// watcher:close()
int fsWatcherClose(lua_State* L) {
    const std::shared_ptr<WatchState> state = checkWatcher(L)->state;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    state->pending.clear();
    return 0;
}

// ---- Locks

struct LockHandle {
    std::shared_ptr<fslock::FileLock> lock;
};

constexpr const char* kLockMeta = "sonata.fs.Lock";

LockHandle* checkLock(lua_State* L) {
    return static_cast<LockHandle*>(luaL_checkudata(L, 1, kLockMeta));
}

// lock:unlock()
int fsLockUnlock(lua_State* L) {
    LockHandle* handle = checkLock(L);
    if (handle->lock) {
        handle->lock->release();
    }
    return 0;
}

// lock:isLocked(): boolean
int fsLockIsLocked(lua_State* L) {
    LockHandle* handle = checkLock(L);
    lua_pushboolean(L, handle->lock && handle->lock->held());
    return 1;
}

template <typename T>
T* newUserdata(lua_State* L, const char* meta) {
    void* memory = lua_newuserdatadtor(L, sizeof(T), [](lua_State*, void* p) { static_cast<T*>(p)->~T(); });
    T* object = new (memory) T();
    luaL_getmetatable(L, meta);
    lua_setmetatable(L, -2);
    return object;
}

// fs.lock / fs.tryLock. "tryOnly": never wait.
int lockImpl(lua_State* L, bool tryOnly) {
    const std::string_view pathText = checkPath(L, 1);
    bool shared = false;
    double timeout = tryOnly ? 0.0 : -1.0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "shared");
        shared = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (!tryOnly) {
            lua_getfield(L, 2, "timeout");
            if (!lua_isnil(L, -1)) {
                timeout = luaL_checknumber(L, -1);
                luaL_argcheck(L, timeout >= 0, 2, "timeout must not be negative");
            }
            lua_pop(L, 1);
        }
    }

    const char* op = tryOnly ? "tryLock" : "lock";
    auto path = std::make_shared<stdfs::path>(toPath(pathText));
    auto label = std::make_shared<std::string>(quote(pathText));
    return sched::Scheduler::of(L).await(L, [path, label, shared, timeout, op]() -> sched::Completion {
        auto error = std::make_shared<std::string>();
        std::shared_ptr<fslock::FileLock> lock = fslock::acquire(*path, shared, timeout, *error);
        return [lock, error, label, op](lua_State* T) {
            if (!error->empty()) {
                lua_pushfstring(T, "fs.%s %s: %s", op, label->c_str(), error->c_str());
                return sched::kRaise;
            }
            if (!lock) {
                lua_pushnil(T);
                return 1;
            }
            newUserdata<LockHandle>(T, kLockMeta)->lock = lock;
            return 1;
        };
    });
}

// fs.lock(path: string, options: { shared: boolean?, timeout: number? }?): Lock?
int fsLock(lua_State* L) {
    return lockImpl(L, false);
}

// fs.tryLock(path: string, options: { shared: boolean? }?): Lock?
int fsTryLock(lua_State* L) {
    return lockImpl(L, true);
}

// fs.walk(dir: string, options: WalkOptions?): () -> Entry?
int fsWalk(lua_State* L) {
    const std::string_view dirText = checkPath(L, 1);
    double maxDepth = std::numeric_limits<double>::infinity();
    bool follow = false;
    bool dirs = true;
    lua_settop(L, 2);
    lua_pushnil(L); // 3: filter
    if (!lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "maxDepth");
        if (!lua_isnil(L, -1)) {
            maxDepth = luaL_checknumber(L, -1);
            luaL_argcheck(L, maxDepth >= 1, 2, "maxDepth must be at least 1");
        }
        lua_getfield(L, 2, "followSymlinks");
        follow = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 2, "dirs");
        dirs = lua_isnil(L, -1) || lua_toboolean(L, -1) != 0;
        lua_getfield(L, 2, "filter");
        luaL_argcheck(L, lua_isnil(L, -1) || lua_isfunction(L, -1), 2, "filter must be a function");
        lua_replace(L, 3);
        lua_settop(L, 3);
    }

    const stdfs::path dir = toPath(dirText);
    {
        std::error_code ec;
        if (!stdfs::is_directory(dir, ec)) {
            const bool exists = stdfs::exists(dir, ec);
            fail(L, "walk", quote(dirText), exists ? reasonOf(std::errc::not_a_directory) : reasonOf(std::errc::no_such_file_or_directory));
            return finish(L, kFailed);
        }
    }

    Walker* walker = newUserdata<Walker>(L, kWalkerMeta);
    walker->root = dir;
    walker->rootText = normalizeSlashes(dirText);
    walker->maxDepth = maxDepth;
    walker->follow = follow;
    walker->dirs = dirs;
    std::error_code ec;
    walker->visited.insert(stdfs::canonical(dir, ec));
    walker->stack.push_back(WalkFrame{listSorted(dir), 0, "", 1});

    lua_pushvalue(L, 3);
    lua_pushcclosure(L, walkNext, "walk", 2);
    return 1;
}

// fs.glob(pattern: string, options: GlobOptions?): { string }
int fsGlob(lua_State* L) {
    std::size_t length = 0;
    const char* patternText = luaL_checklstring(L, 1, &length);
    GlobOptions options;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "cwd");
        if (!lua_isnil(L, -1)) {
            options.cwd = toPath(checkPath(L, lua_gettop(L)));
        }
        lua_getfield(L, 2, "dot");
        options.dot = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 2, "absolute");
        options.absolute = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 2, "kind");
        if (!lua_isnil(L, -1)) {
            static const char* const kKinds[] = {"file", "directory", nullptr};
            options.kind = kKinds[luaL_checkoption(L, -1, nullptr, kKinds)];
        }
    }

    {
        std::vector<std::string> patterns;
        expandBraces(std::string(patternText, length), patterns);
        std::set<std::string> found;
        for (const std::string& pattern : patterns) {
            globOne(pattern, options, found);
        }

        lua_createtable(L, static_cast<int>(found.size()), 0);
        int i = 0;
        for (const std::string& path : found) {
            pushString(L, path);
            lua_rawseti(L, -2, ++i);
        }
    }
    return 1;
}

// fs.match(pattern: string, path: string, options: { dot: boolean? }?): boolean
int fsMatch(lua_State* L) {
    std::size_t patternLength = 0;
    std::size_t pathLength = 0;
    const char* patternText = luaL_checklstring(L, 1, &patternLength);
    const char* pathText = luaL_checklstring(L, 2, &pathLength);
    bool dot = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "dot");
        dot = lua_toboolean(L, -1) != 0;
    }

    bool matched = false;
    {
        const std::string path = normalizeSlashes(std::string_view(pathText, pathLength));
        const std::string pathRoot = patternRoot(path);
        const std::vector<std::string_view> parts = splitSegments(std::string_view(path).substr(pathRoot.size()));

        std::vector<std::string> patterns;
        expandBraces(std::string(patternText, patternLength), patterns);
        for (const std::string& raw : patterns) {
            const std::string pattern = normalizeSlashes(raw);
            const std::string root = patternRoot(pattern);
            if (root != pathRoot) {
                continue;
            }
            const std::vector<std::string_view> segments = splitSegments(std::string_view(pattern).substr(root.size()));
            if (matchSegments(segments, 0, parts, 0, dot)) {
                matched = true;
                break;
            }
        }
    }
    lua_pushboolean(L, matched);
    return 1;
}

// fs.watch(path: string, options: { recursive: boolean?, interval: number? }?): Watcher
int fsWatch(lua_State* L) {
    const std::string_view pathText = checkPath(L, 1);
    bool recursive = true;
    double interval = 0.25;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "recursive");
        recursive = lua_isnil(L, -1) || lua_toboolean(L, -1) != 0;
        lua_getfield(L, 2, "interval");
        if (!lua_isnil(L, -1)) {
            interval = luaL_checknumber(L, -1);
            luaL_argcheck(L, interval >= 0.01, 2, "interval must be at least 0.01 seconds");
        }
    }

    {
        std::error_code ec;
        if (!stdfs::exists(toPath(pathText), ec)) {
            fail(L, "watch", quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
            return finish(L, kFailed);
        }
    }

    Watcher* watcher = newUserdata<Watcher>(L, kWatcherMeta);
    watcher->state = std::make_shared<WatchState>();
    WatchState& state = *watcher->state;
    state.root = toPath(pathText);
    state.rootText = normalizeSlashes(pathText);
    state.recursive = recursive;
    state.interval = interval;
    state.last = scan(state);
    return 1;
}

constexpr NativeFunction kWatcherMethods[] = {
    {"next", fsWatcherNext, watchNextK},
    {"poll", fsWatcherPoll},
    {"close", fsWatcherClose},
};

constexpr NativeFunction kLockMethods[] = {
    {"unlock", fsLockUnlock},
    {"isLocked", fsLockIsLocked},
};

template <std::size_t N>
void registerType(lua_State* L, const char* name, const char* typeName, const NativeFunction (&methods)[N]) {
    if (luaL_newmetatable(L, name)) {
        lua_createtable(L, 0, static_cast<int>(N));
        setFunctions(L, methods);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, typeName);
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
}

constexpr NativeFunction kFunctions[] = {
    {"readFile", fsReadFile},
    {"writeFile", fsWriteFile},
    {"appendFile", fsAppendFile},
    {"copyFile", fsCopyFile},
    {"removeFile", fsRemoveFile},
    {"move", fsMove},
    {"makeDir", fsMakeDir},
    {"removeDir", fsRemoveDir},
    {"listDir", fsListDir},
    {"readDir", fsReadDir},
    {"copyDir", fsCopyDir},
    {"exists", fsExists},
    {"isFile", fsIsFile},
    {"isDir", fsIsDir},
    {"isSymlink", fsIsSymlink},
    {"stat", fsStat},
    {"lstat", fsLstat},
    {"realPath", fsRealPath},
    {"symlink", fsSymlink},
    {"readLink", fsReadLink},
    {"chmod", fsChmod},
    {"cwd", fsCwd},
    {"chdir", fsChdir},
    {"tempDir", fsTempDir},
    {"makeTempDir", fsMakeTempDir},
    {"chunkPlan", fsChunkPlan},
    {"openReader", fsOpenReader},
    {"walk", fsWalk},
    {"glob", fsGlob},
    {"match", fsMatch},
    {"watch", fsWatch},
    {"lock", fsLock},
    {"tryLock", fsTryLock},
};

} // namespace


void openFs(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 4);
    setFunctions(L, kFunctions);

    lua_pushnumber(L, static_cast<double>(kMaxReadSize));
    lua_setfield(L, -2, "maxReadSize");

    lua_pushnumber(L, static_cast<double>(kMaxChunkSize));
    lua_setfield(L, -2, "maxChunkSize");

    registerReaderType(L);
    registerType(L, kWatcherMeta, "fs.Watcher", kWatcherMethods);
    registerType(L, kLockMeta, "fs.Lock", kLockMethods);
    if (luaL_newmetatable(L, kWalkerMeta)) {
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, 6);
    setNumberField(L, "file", 0644);
    setNumberField(L, "executable", 0755);
    setNumberField(L, "directory", 0755);
    setNumberField(L, "private", 0600);
    setNumberField(L, "privateDirectory", 0700);
    setNumberField(L, "readOnly", 0444);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "modes");
}

} // namespace sonata::lib::libs